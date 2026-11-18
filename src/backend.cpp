#include "backend.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QUrl>

#include "comfy.h"
#include "gpulock.h"
#include "h3.h"

namespace {
// ComfyUI started by this app is stopped after this long without a job, so
// its CUDA context does not sit on the shared card.
constexpr int IdleStopMs = 10 * 60 * 1000;

QString fmtEta(double s) {
    if (s < 0) return {};
    const int t = int(s + 0.5);
    return t >= 60 ? QObject::tr("ETA %1 min %2 s").arg(t / 60).arg(t % 60) : QObject::tr("ETA %1 s").arg(t);
}
}  // namespace

Backend::Backend(const AppPaths &paths, FilePicker *picker, QObject *parent)
    : QObject(parent), m_paths(paths), m_picker(picker) {
    m_installProblem = ComfyServer::checkInstall(paths).join("\n");
    connect(&m_poll, &QTimer::timeout, this, &Backend::pollComfy);
    m_poll.start(4000);
    m_idleStop.setSingleShot(true);
    m_idleStop.setInterval(IdleStopMs);
    connect(&m_idleStop, &QTimer::timeout, this, [this] {
        if (!busy() && m_comfyOurs) {
            appendLog(tr("ComfyUI idle for 10 minutes; stopping it to free the GPU"));
            stopComfy();
        }
    });
    if (m_picker)
        connect(m_picker, &FilePicker::picked, this, &Backend::imagesPicked);
    connect(&m_job, &QProcess::readyReadStandardOutput, this, [this] {
        m_jobBuffer += m_job.readAllStandardOutput();
        int nl;
        while ((nl = m_jobBuffer.indexOf('\n')) >= 0) {
            onJobLine(m_jobBuffer.left(nl));
            m_jobBuffer.remove(0, nl + 1);
        }
    });
    connect(&m_job, &QProcess::readyReadStandardError, this, [this] {
        for (const QByteArray &l : m_job.readAllStandardError().split('\n'))
            if (!l.trimmed().isEmpty()) appendLog(QString::fromUtf8(l));
    });
    connect(&m_job, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        m_waitingForGpu = false;
        if (code != 0 && m_lastError.isEmpty()) m_lastError = tr("job failed (exit %1)").arg(code);
        m_statusText = code == 0 ? tr("%1 finished").arg(m_jobTitle) : tr("%1 failed").arg(m_jobTitle);
        m_progress = code == 0 ? 1.0 : m_progress;
        m_etaText.clear();
        emit jobChanged();
        emit jobFinished(code == 0, m_lastOutput);
        refreshGallery();
        if (!m_currentProject.isEmpty()) loadProject(m_currentProject);
        m_idleStop.start();
        pollComfy();
    });
    pollComfy();
    refreshProjects();
    refreshGallery();
}

Backend::~Backend() {
    if (m_job.state() != QProcess::NotRunning) {
        // SIGTERM lets the CLI interrupt ComfyUI, unload and release the lock.
        m_job.terminate();
        if (!m_job.waitForFinished(30000)) m_job.kill();
    }
}

QVariantList Backend::presets() const {
    QVariantList list;
    for (const auto &p : H3::presets())
        list << QVariantMap{{"label", p.label}, {"width", p.width}, {"height", p.height}};
    return list;
}
QString Backend::version() const { return appVersion(); }
QString Backend::gitCommit() const { return appGitCommit(); }
int Backend::maxPixels() const { return H3::MaxPixels; }
int Backend::snapSize(double v) const { return H3::snapSize(v); }
int Backend::framesFor(double s) const { return H3::framesForSeconds(s); }
double Backend::secondsFor(int f) const { return H3::secondsForFrames(f); }

void Backend::appendLog(const QString &line) {
    m_log << line;
    while (m_log.size() > 300) m_log.removeFirst();
    emit logChanged();
}

void Backend::pollComfy() {
    QFile pf(m_paths.pidFile());
    int port = 8188;
    bool ours = false;
    if (pf.open(QIODevice::ReadOnly)) {
        port = QJsonDocument::fromJson(pf.readAll()).object().value("port").toInt(8188);
        ours = true;
    }
    QNetworkRequest req(QUrl(QString("http://127.0.0.1:%1/system_stats").arg(port)));
    req.setTransferTimeout(2000);
    QNetworkReply *reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, port, ours] {
        reply->deleteLater();
        const QJsonObject stats = QJsonDocument::fromJson(reply->readAll()).object();
        const bool running = reply->error() == QNetworkReply::NoError && !stats.isEmpty();
        const bool gpuBusy = !busy() && GpuLock::isBusy(m_paths.gpuLock);
        QString text;
        if (running) {
            const QJsonObject dev = stats.value("devices").toArray().first().toObject();
            text = tr("ComfyUI %1 on port %2%3 · VRAM free %4 MiB")
                       .arg(stats.value("system").toObject().value("comfyui_version").toString())
                       .arg(port).arg(ours ? QString() : tr(" (external)"))
                       .arg(qint64(dev.value("vram_free").toDouble() / 1048576));
        } else {
            text = ours ? tr("ComfyUI starting...") : tr("ComfyUI stopped");
        }
        if (running != m_comfyRunning || ours != m_comfyOurs || text != m_comfyText || gpuBusy != m_gpuBusy) {
            m_comfyRunning = running;
            m_comfyOurs = ours && running;
            m_comfyText = text;
            m_gpuBusy = gpuBusy;
            emit comfyChanged();
        }
        if (m_comfyOurs && !busy() && !m_idleStop.isActive()) m_idleStop.start();
    });
}

void Backend::runQuick(const QStringList &args, std::function<void(const QJsonObject &)> done) {
    auto *p = new QProcess(this);
    connect(p, &QProcess::finished, this, [p, done] {
        const QList<QByteArray> lines = p->readAllStandardOutput().trimmed().split('\n');
        done(QJsonDocument::fromJson(lines.last()).object());
        p->deleteLater();
    });
    p->start(QCoreApplication::applicationFilePath(), args + QStringList{"--json"});
}

void Backend::startComfy() {
    appendLog(tr("starting ComfyUI..."));
    m_comfyText = tr("ComfyUI starting...");
    emit comfyChanged();
    runQuick({"comfy", "start"}, [this](const QJsonObject &r) {
        appendLog(r.value("ok").toBool() ? tr("ComfyUI ready") : r.value("error").toString());
        pollComfy();
    });
}

void Backend::stopComfy() {
    runQuick({"comfy", "stop"}, [this](const QJsonObject &r) {
        appendLog(r.value("ok").toBool() ? tr("ComfyUI stopped") : r.value("error").toString());
        pollComfy();
    });
}

void Backend::startJob(const QString &title, const QStringList &args) {
    if (busy()) return;
    m_idleStop.stop();
    m_jobTitle = title;
    m_statusText = tr("starting %1").arg(title);
    m_lastError.clear();
    m_lastOutput.clear();
    m_progress = 0;
    m_etaText.clear();
    m_shotIndex = m_shotCount = 0;
    m_projectEta = -1;
    m_jobBuffer.clear();
    appendLog("$ omni-video-generator " + args.join(' '));
    // The GUI keeps ComfyUI warm between jobs and stops it when idle.
    m_job.start(QCoreApplication::applicationFilePath(), args + QStringList{"--progress-json", "--keep-comfy"});
    emit jobChanged();
}

void Backend::onJobLine(const QByteArray &line) {
    const QJsonObject e = QJsonDocument::fromJson(line).object();
    const QString ev = e.value("event").toString();
    if (ev.isEmpty()) return;
    if (ev == "waiting_gpu") {
        m_waitingForGpu = true;
        m_statusText = tr("waiting for GPU (in use by another app)");
        appendLog(m_statusText);
    } else if (ev == "gpu_locked") {
        m_waitingForGpu = false;
        m_statusText = tr("GPU reserved");
    } else if (ev == "starting_comfy") {
        m_statusText = tr("starting ComfyUI (about 30 s)");
    } else if (ev == "comfy_ready") {
        m_statusText = tr("ComfyUI ready");
        pollComfy();
    } else if (ev == "log") {
        appendLog(e.value("message").toString());
    } else if (ev == "uploading") {
        m_statusText = tr("uploading %1").arg(QFileInfo(e.value("file").toString()).fileName());
    } else if (ev == "queued") {
        m_statusText = tr("loading models (%1x%2, %3 frames)").arg(e.value("width").toInt()).arg(e.value("height").toInt())
                           .arg(e.value("frames").toInt());
        appendLog(QString("queued %1, seed %2").arg(e.value("label").toString()).arg(e.value("seed").toInteger()));
    } else if (ev == "project_progress") {
        m_shotIndex = e.value("index").toInt();
        m_shotCount = e.value("count").toInt();
        m_projectEta = e.contains("eta_seconds") ? e.value("eta_seconds").toDouble() : -1;
        m_progress = double(m_shotIndex - 1) / std::max(1, m_shotCount);
    } else if (ev == "progress") {
        const int v = e.value("value").toInt(), max = std::max(1, e.value("max").toInt());
        const double shotFrac = double(v) / max;
        m_progress = m_shotCount ? (m_shotIndex - 1 + shotFrac) / m_shotCount : shotFrac;
        m_statusText = m_shotCount ? tr("shot %1 of %2 · step %3/%4").arg(m_shotIndex).arg(m_shotCount).arg(v).arg(max)
                                   : tr("sampling · step %1/%2").arg(v).arg(max);
        double eta = e.contains("eta_seconds") ? e.value("eta_seconds").toDouble() : -1;
        if (eta >= 0 && m_projectEta >= 0 && m_shotCount) {
            // Remaining shots after this one, from the project estimate.
            const double perShot = m_projectEta / std::max(1, m_shotCount - m_shotIndex + 1);
            eta += perShot * (m_shotCount - m_shotIndex);
        }
        m_etaText = fmtEta(eta);
        if (v == max) m_statusText = tr("decoding video and audio");
    } else if (ev == "done") {
        const QJsonObject r = e.value("result").toObject();
        m_lastOutput = r.value("file").toString();
        appendLog(tr("saved %1 in %2 s, VRAM peak %3 MiB").arg(m_lastOutput).arg(r.value("seconds").toDouble(), 0, 'f', 0)
                      .arg(r.value("vram_peak_mib").toInt()));
    } else if (ev == "models_unloaded") {
        appendLog(tr("models unloaded; GPU memory released"));
    } else if (ev == "gpu_released") {
        appendLog(tr("GPU lock released"));
    } else if (ev == "result") {
        if (!e.value("ok").toBool()) {
            m_lastError = e.value("error").toString();
            appendLog(tr("error: %1").arg(m_lastError));
        } else if (e.contains("file")) {
            m_lastOutput = e.value("file").toString();
            appendLog(tr("output: %1").arg(m_lastOutput));
        }
    }
    emit jobChanged();
}

void Backend::generate(const QVariantMap &s) {
    QStringList args{"gen", "--mode", s.value("mode").toString(), "--prompt", s.value("prompt").toString(),
                     "--width", QString::number(s.value("width").toInt()), "--height", QString::number(s.value("height").toInt()),
                     "--seconds", QString::number(s.value("seconds").toDouble()), "--steps", QString::number(s.value("steps").toInt())};
    if (s.value("seed").toLongLong() >= 0) args << "--seed" << QString::number(s.value("seed").toLongLong());
    if (!s.value("audio", true).toBool()) args << "--no-audio";
    for (const QVariant &r : s.value("refs").toList()) args << "--ref" << r.toString();
    startJob(tr("generation"), args);
}

void Backend::cancel() {
    if (busy()) {
        appendLog(tr("cancelling..."));
        m_job.terminate();
    }
}

void Backend::pickImages(bool multiple) {
    if (m_picker) m_picker->pickImages(tr("Choose images"), multiple);
}

void Backend::refreshProjects() {
    QStringList list = QDir(m_paths.projectsDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    if (list != m_projects) {
        m_projects = list;
        emit projectsChanged();
    }
}

void Backend::loadProject(const QString &name) {
    m_currentProject = name;
    if (name.isEmpty()) {
        m_project.clear();
        emit projectChanged();
        return;
    }
    runQuick({"project", "status", name}, [this, name](const QJsonObject &r) {
        if (name != m_currentProject) return;
        QVariantMap map = r.toVariantMap();
        const QString grid = m_paths.projectsDir + "/" + name + "/contact/all.png";
        map["contact_grid"] = QFileInfo::exists(grid) ? grid : QString();
        map["shots_file"] = m_paths.projectsDir + "/" + name + "/shots.json";
        m_project = map;
        emit projectChanged();
    });
}

void Backend::newProject(const QString &name) {
    runQuick({"project", "new", name}, [this, name](const QJsonObject &r) {
        if (!r.value("ok").toBool()) {
            m_lastError = r.value("error").toString();
            appendLog(m_lastError);
            emit jobChanged();
            return;
        }
        refreshProjects();
        loadProject(name);
    });
}

void Backend::renderProject(const QString &name) { startJob(tr("render %1").arg(name), {"project", "render", name}); }

void Backend::retryShot(const QString &name, int shot, bool newSeed) {
    QStringList args{"project", "render", name, "--shot", QString::number(shot)};
    if (newSeed) args << "--new-seed";
    startJob(tr("shot %1").arg(shot), args);
}

void Backend::stitchProject(const QString &name) { startJob(tr("stitch %1").arg(name), {"project", "stitch", name}); }
void Backend::contactSheets(const QString &name) { startJob(tr("contact sheets"), {"project", "contact", name}); }

void Backend::refreshGallery() {
    runQuick({"gallery"}, [this](const QJsonObject &r) {
        QVariantList items = r.value("items").toArray().toVariantList();
        for (QVariant &v : items) {
            QVariantMap m = v.toMap();
            const QFileInfo fi(m.value("file").toString());
            QFile side(fi.absolutePath() + "/" + fi.completeBaseName() + ".json");
            if (side.open(QIODevice::ReadOnly)) {
                const QJsonObject o = QJsonDocument::fromJson(side.readAll()).object();
                m["prompt"] = o.value("prompt").toString();
                m["seed"] = o.value("seed").toInteger();
                m["mode"] = o.value("mode").toString();
            }
            m["name"] = fi.fileName();
            v = m;
        }
        m_gallery = items;
        emit galleryChanged();
    });
}

void Backend::openPath(const QString &path) { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); }
QString Backend::fileUrl(const QString &path) const { return QUrl::fromLocalFile(path).toString(); }
bool Backend::exists(const QString &path) const { return QFileInfo::exists(path); }
