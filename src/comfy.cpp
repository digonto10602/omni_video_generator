#include "comfy.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QNetworkReply>
#include <QProcess>
#include <QSaveFile>
#include <QTcpServer>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <csignal>
#include <sys/types.h>
#include <unistd.h>

ComfyClient::ComfyClient(const QString &host, int port) : m_host(host), m_port(port) {}

QByteArray ComfyClient::request(const QString &method, const QString &path, const QByteArray &body,
                                const QByteArray &contentType, QString *error, int timeoutMs, int *status) {
    QNetworkRequest req(QUrl(baseUrl() + path));
    req.setTransferTimeout(timeoutMs);
    if (!contentType.isEmpty())
        req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    QNetworkReply *reply = method == "GET" ? m_nam.get(req) : m_nam.post(req, body);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status) *status = code;
    const QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError && error)
        *error = QString("%1 %2: %3 %4").arg(method, path, reply->errorString(), QString::fromUtf8(data.left(4000)));
    reply->deleteLater();
    return reply->error() == QNetworkReply::NoError || code >= 400 ? data : QByteArray();
}

QJsonObject ComfyClient::getJson(const QString &path, QString *error, int timeoutMs) {
    return QJsonDocument::fromJson(request("GET", path, {}, {}, error, timeoutMs, nullptr)).object();
}

QJsonObject ComfyClient::postJson(const QString &path, const QJsonObject &body, QString *error, int timeoutMs,
                                  int *status) {
    return QJsonDocument::fromJson(request("POST", path, QJsonDocument(body).toJson(QJsonDocument::Compact),
                                           "application/json", error, timeoutMs, status)).object();
}

QJsonObject ComfyClient::systemStats(QString *error, int timeoutMs) {
    return getJson("/system_stats", error, timeoutMs);
}

QStringList ComfyClient::models(const QString &folder, QString *error) {
    const QByteArray data = request("GET", "/models/" + folder, {}, {}, error, 10000, nullptr);
    QStringList out;
    for (const auto &v : QJsonDocument::fromJson(data).array())
        out << v.toString();
    return out;
}

bool ComfyClient::uploadImage(const QString &localPath, const QString &name, QString *error) {
    auto *file = new QFile(localPath);
    if (!file->open(QIODevice::ReadOnly)) {
        if (error) *error = QString("cannot read %1").arg(localPath);
        delete file;
        return false;
    }
    auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart image;
    image.setHeader(QNetworkRequest::ContentDispositionHeader,
                    QString("form-data; name=\"image\"; filename=\"%1\"").arg(name));
    image.setHeader(QNetworkRequest::ContentTypeHeader, QMimeDatabase().mimeTypeForFile(localPath).name());
    image.setBodyDevice(file);
    file->setParent(multi);
    multi->append(image);
    for (const auto &[k, v] : {std::pair{"type", "input"}, std::pair{"overwrite", "true"}}) {
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader, QString("form-data; name=\"%1\"").arg(k));
        part.setBody(v);
        multi->append(part);
    }
    QNetworkRequest req(QUrl(baseUrl() + "/upload/image"));
    QNetworkReply *reply = m_nam.post(req, multi);
    multi->setParent(reply);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const bool ok = reply->error() == QNetworkReply::NoError;
    if (!ok && error) *error = QString("upload %1: %2").arg(localPath, reply->errorString());
    reply->deleteLater();
    return ok;
}

QString ComfyClient::queuePrompt(const QJsonObject &graph, const QString &clientId, QString *error) {
    int status = 0;
    QString err;
    const QJsonObject r = postJson("/prompt", QJsonObject{{"prompt", graph}, {"client_id", clientId}}, &err, 30000, &status);
    if (r.contains("prompt_id"))
        return r.value("prompt_id").toString();
    if (error) {
        // ComfyUI explains validation failures per node; keep it readable.
        QString detail = QString::fromUtf8(QJsonDocument(r.value("error").toObject()).toJson(QJsonDocument::Compact));
        const QJsonObject nodeErrors = r.value("node_errors").toObject();
        for (auto it = nodeErrors.begin(); it != nodeErrors.end(); ++it)
            detail += QString("\nnode %1: %2").arg(it.key(), QString::fromUtf8(QJsonDocument(it.value().toObject()).toJson(QJsonDocument::Compact)));
        *error = r.isEmpty() ? err : "ComfyUI rejected the workflow: " + detail;
    }
    return {};
}

QJsonObject ComfyClient::history(const QString &promptId, QString *error) {
    return getJson("/history/" + promptId, error).value(promptId).toObject();
}

bool ComfyClient::freeMemory(QString *error) {
    QString err;
    int status = 0;
    postJson("/free", QJsonObject{{"unload_models", true}, {"free_memory", true}}, &err, 10000, &status);
    if (status != 200 && error) *error = err;
    return status == 200;
}

bool ComfyClient::interrupt(QString *error) {
    int status = 0;
    postJson("/interrupt", QJsonObject{}, error, 10000, &status);
    return status == 200;
}

bool ComfyClient::download(const QString &filename, const QString &subfolder, const QString &type,
                           const QString &dest, QString *error) {
    QUrlQuery q;
    q.addQueryItem("filename", filename);
    q.addQueryItem("subfolder", subfolder);
    q.addQueryItem("type", type);
    const QByteArray data = request("GET", "/view?" + q.toString(QUrl::FullyEncoded), {}, {}, error, 120000, nullptr);
    if (data.isEmpty()) return false;
    QDir().mkpath(QFileInfo(dest).absolutePath());
    QSaveFile f(dest);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
        if (error) *error = "cannot write " + dest;
        return false;
    }
    return true;
}

QString ComfyStatus::describe() const {
    if (!running) return "ComfyUI is not running";
    return QString("ComfyUI %1 running on 127.0.0.1:%2 (%3)")
        .arg(version, QString::number(port), ours ? QString("started by omni-video-generator, pid %1").arg(pid)
                                                  : QString("started elsewhere; reused, never stopped by this app"));
}

QJsonObject ComfyStatus::toJson() const {
    QJsonObject o{{"running", running}, {"ours", ours}, {"port", port}, {"pid", pid}, {"version", version}};
    const QJsonArray devices = stats.value("devices").toArray();
    if (!devices.isEmpty()) {
        const QJsonObject d = devices.first().toObject();
        o["device"] = d.value("name");
        o["vram_total_mib"] = qint64(d.value("vram_total").toDouble() / 1048576);
        o["vram_free_mib"] = qint64(d.value("vram_free").toDouble() / 1048576);
        o["torch_vram_reserved_mib"] = qint64(d.value("torch_vram_total").toDouble() / 1048576);
    }
    return o;
}

namespace ComfyServer {

static bool pidAlive(qint64 pid) { return pid > 0 && ::kill(pid_t(pid), 0) == 0; }

static QJsonObject readPidFile(const AppPaths &paths) {
    QFile f(paths.pidFile());
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

ComfyStatus detect(const AppPaths &paths) {
    ComfyStatus st;
    const QJsonObject pf = readPidFile(paths);
    if (!pf.isEmpty()) {
        const qint64 pid = pf.value("pid").toInteger();
        const int port = pf.value("port").toInt();
        if (pidAlive(pid)) {
            ComfyClient c("127.0.0.1", port);
            const QJsonObject stats = c.systemStats();
            st.pid = pid;
            st.port = port;
            st.ours = true;
            if (!stats.isEmpty()) {
                st.running = true;
                st.stats = stats;
                st.version = stats.value("system").toObject().value("comfyui_version").toString();
            }
            return st;  // ours but still starting when not running
        }
        QFile::remove(paths.pidFile());
    }
    // Someone else's ComfyUI on the default port is reused, never stopped.
    for (int port : {8188}) {
        ComfyClient c("127.0.0.1", port);
        const QJsonObject stats = c.systemStats(nullptr, 1500);
        if (stats.value("system").toObject().contains("comfyui_version")) {
            st.running = true;
            st.port = port;
            st.stats = stats;
            st.version = stats.value("system").toObject().value("comfyui_version").toString();
            return st;
        }
    }
    return st;
}

int findFreePort(int first, int last) {
    for (int port = first; port <= last; ++port) {
        QTcpServer probe;
        if (probe.listen(QHostAddress::LocalHost, quint16(port))) {
            probe.close();
            return port;
        }
    }
    return 0;
}

QStringList launchArguments(const AppPaths &paths, int port) {
    return {"main.py",
            "--listen", "127.0.0.1", "--port", QString::number(port),
            // Proven on this RTX 3070 with 31 GB RAM (hellfire start_comfyui.sh):
            // the ~42 GB of weights exceed RAM, so stream them from NVMe and
            // never pin RAM; keep 1 GB of VRAM for the desktop.
            "--reserve-vram", "1.0", "--disable-pinned-memory", "--fast-disk",
            "--output-directory", paths.outputDir,
            "--input-directory", paths.inputDir,
            "--temp-directory", paths.tmpDir + "/comfy",
            "--user-directory", paths.comfyUserDir,
            "--database-url", "sqlite:///" + paths.comfyUserDir + "/comfyui.db",
            // Fully local: no partner/cloud API nodes, no frontend network calls.
            "--disable-api-nodes", "--disable-auto-launch"};
}

QStringList checkInstall(const AppPaths &paths) {
    QStringList missing;
    if (!QFileInfo::exists(paths.comfyDir + "/main.py"))
        missing << QString("ComfyUI not found at %1").arg(paths.comfyDir);
    if (!QFileInfo(paths.python).isExecutable())
        missing << QString("Python environment not found at %1 (build it with scripts/setup_venv.sh)").arg(paths.python);
    return missing;
}

ComfyStatus start(const AppPaths &paths, QString *error, const std::function<void(const QString &)> &log) {
    ComfyStatus st = detect(paths);
    if (st.running) return st;
    const QStringList missing = checkInstall(paths);
    if (!missing.isEmpty()) {
        if (error) *error = missing.join("\n");
        return st;
    }
    QString err;
    if (!paths.ensureDirs(&err)) {
        if (error) *error = err;
        return st;
    }
    QDir().mkpath(paths.tmpDir + "/comfy");
    if (!st.ours) {
        const int port = findFreePort();
        if (!port) {
            if (error) *error = "no free port between 8188 and 8199";
            return st;
        }
        QProcess proc;
        proc.setProgram(paths.python);
        proc.setArguments(launchArguments(paths, port));
        proc.setWorkingDirectory(paths.comfyDir);
        auto env = QProcessEnvironment::systemEnvironment();
        // Keep caches inside the project and forbid model downloads at run time.
        env.insert("HF_HOME", paths.cacheDir + "/hf");
        env.insert("XDG_CACHE_HOME", paths.cacheDir);
        env.insert("TMPDIR", paths.tmpDir);
        env.insert("HF_HUB_OFFLINE", "1");
        env.insert("TRANSFORMERS_OFFLINE", "1");
        env.insert("DO_NOT_TRACK", "1");
        env.insert("PYTHONUNBUFFERED", "1");
        proc.setProcessEnvironment(env);
        proc.setStandardOutputFile(paths.comfyLog(), QIODevice::Append);
        proc.setStandardErrorFile(paths.comfyLog(), QIODevice::Append);
        proc.setStandardInputFile(QProcess::nullDevice());
        qint64 pid = 0;
        if (!proc.startDetached(&pid)) {
            if (error) *error = "cannot start " + paths.python;
            return st;
        }
        QSaveFile pf(paths.pidFile());
        pf.open(QIODevice::WriteOnly);
        pf.write(QJsonDocument(QJsonObject{{"pid", pid}, {"port", port},
                                           {"started", QDateTime::currentDateTime().toString(Qt::ISODate)},
                                           {"python", paths.python}, {"comfy_dir", paths.comfyDir},
                                           {"args", QJsonArray::fromStringList(launchArguments(paths, port))}})
                     .toJson());
        pf.commit();
        st.pid = pid;
        st.port = port;
        st.ours = true;
        if (log) log(QString("starting ComfyUI (pid %1, port %2, log %3)").arg(pid).arg(port).arg(paths.comfyLog()));
    }
    ComfyClient c("127.0.0.1", st.port);
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 300000) {
        const QJsonObject stats = c.systemStats(nullptr, 2000);
        if (!stats.isEmpty()) {
            st.running = true;
            st.stats = stats;
            st.version = stats.value("system").toObject().value("comfyui_version").toString();
            if (log) log(QString("ComfyUI %1 ready after %2 s").arg(st.version).arg(t.elapsed() / 1000));
            return st;
        }
        if (!pidAlive(st.pid)) {
            QFile::remove(paths.pidFile());
            if (error) *error = "ComfyUI exited during startup; see " + paths.comfyLog();
            return st;
        }
        QEventLoop loop;
        QTimer::singleShot(1000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    if (error) *error = "ComfyUI did not answer within 5 minutes; see " + paths.comfyLog();
    return st;
}

bool stop(const AppPaths &paths, QString *error) {
    const QJsonObject pf = readPidFile(paths);
    const qint64 pid = pf.value("pid").toInteger();
    if (!pidAlive(pid)) {
        QFile::remove(paths.pidFile());
        const ComfyStatus st = detect(paths);
        if (st.running && error)
            *error = QString("ComfyUI on port %1 was not started by omni-video-generator; leaving it running").arg(st.port);
        return !st.running;
    }
    ::kill(pid_t(pid), SIGTERM);
    for (int i = 0; i < 150 && pidAlive(pid); ++i)
        QThread::msleep(100);
    if (pidAlive(pid)) {
        ::kill(pid_t(pid), SIGKILL);
        for (int i = 0; i < 50 && pidAlive(pid); ++i)
            QThread::msleep(100);
    }
    QFile::remove(paths.pidFile());
    if (pidAlive(pid)) {
        if (error) *error = QString("ComfyUI pid %1 did not stop").arg(pid);
        return false;
    }
    return true;
}

void waitForVramRelease(ComfyClient &client, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        const QJsonArray devices = client.systemStats().value("devices").toArray();
        if (devices.isEmpty()) return;
        const double reserved = devices.first().toObject().value("torch_vram_total").toDouble();
        if (reserved < 512.0 * 1048576) return;
        QEventLoop loop;
        QTimer::singleShot(1000, &loop, &QEventLoop::quit);
        loop.exec();
    }
}

}  // namespace ComfyServer
