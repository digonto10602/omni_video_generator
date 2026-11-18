#include "runner.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTimer>
#include <QUuid>
#include <QCryptographicHash>

QJsonObject Runner::Result::toJson() const {
    return {{"ok", ok}, {"error", error}, {"file", file}, {"prompt_id", promptId},
            {"seconds", seconds}, {"sampling_seconds", samplingSeconds},
            {"vram_peak_mib", vramPeakMiB}, {"vram_baseline_mib", vramBaselineMiB},
            {"template", templateSource}, {"comfyui_version", comfyVersion}};
}

Runner::Runner(const AppPaths &paths, EventFn onEvent, QObject *parent)
    : QObject(parent), m_paths(paths), m_onEvent(std::move(onEvent)), m_lock(paths.gpuLock) {
    m_clientId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    connect(&m_ws, &WsReader::textMessage, this, &Runner::onWsMessage);
}

Runner::~Runner() { m_lock.unlock(); }

void Runner::emitEvent(QJsonObject e) {
    e["time"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    if (m_onEvent) m_onEvent(e);
}

static void sleepEventLoop(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

int Runner::gpuMemoryUsedMiB() {
    QProcess p;
    p.start("nvidia-smi", {"--query-gpu=memory.used", "--format=csv,noheader,nounits"});
    if (!p.waitForFinished(5000)) return -1;
    return QString::fromUtf8(p.readAllStandardOutput()).split('\n').first().trimmed().toInt();
}

bool Runner::begin(QString *error) {
    QString err;
    bool announced = false;
    // Wait for the shared GPU lock; another app may be using the card.
    while (!m_lock.tryLock(&err)) {
        if (!err.isEmpty()) {
            if (error) *error = err;
            return false;
        }
        if (m_cancelled) {
            if (error) *error = "cancelled while waiting for the GPU";
            return false;
        }
        if (!announced) {
            emitEvent({{"event", "waiting_gpu"}, {"message", "waiting for GPU (in use by another app)"},
                       {"lock", m_paths.gpuLock}});
            announced = true;
        }
        sleepEventLoop(1000);
    }
    emitEvent({{"event", "gpu_locked"}, {"lock", m_paths.gpuLock}});

    m_status = ComfyServer::detect(m_paths);
    if (!m_status.running) {
        emitEvent({{"event", "starting_comfy"}});
        m_status = ComfyServer::start(m_paths, &err, [this](const QString &m) {
            emitEvent({{"event", "log"}, {"message", m}});
        });
        if (!m_status.running) {
            m_lock.unlock();
            if (error) *error = err;
            return false;
        }
        m_startedComfy = true;
    }
    emitEvent({{"event", "comfy_ready"}, {"comfy", m_status.toJson()}});
    m_client = std::make_unique<ComfyClient>("127.0.0.1", m_status.port);
    m_ws.open("127.0.0.1", quint16(m_status.port), "/ws?clientId=" + m_clientId);
    return true;
}

void Runner::onWsMessage(const QString &text) {
    const QJsonObject msg = QJsonDocument::fromJson(text.toUtf8()).object();
    const QString type = msg.value("type").toString();
    const QJsonObject data = msg.value("data").toObject();
    if (!m_promptId.isEmpty() && data.contains("prompt_id") && data.value("prompt_id").toString() != m_promptId)
        return;
    if (type == "executing") {
        if (data.value("node").isNull()) {
            if (!m_promptId.isEmpty()) m_finished = true;
            return;
        }
        m_currentNode = data.value("node").toString();
        emitEvent({{"event", "stage"}, {"node", m_currentNode}});
    } else if (type == "progress") {
        const int value = data.value("value").toInt(), max = data.value("max").toInt();
        QJsonObject e{{"event", "progress"}, {"node", data.value("node").toString()}, {"value", value}, {"max", max}};
        // The sampler is the long part; its step rate gives the ETA.
        if (m_samplingStartValue < 0 || value < m_samplingStartValue) {
            m_samplingStartValue = value;
            m_samplingTimer.start();
        } else if (value > m_samplingStartValue) {
            const double perStep = m_samplingTimer.elapsed() / 1000.0 / (value - m_samplingStartValue);
            e["seconds_per_step"] = perStep;
            e["eta_seconds"] = perStep * (max - value);
            m_samplingSeconds = m_samplingTimer.elapsed() / 1000.0;
        }
        emitEvent(e);
    } else if (type == "execution_error") {
        m_execError = QString("%1: %2").arg(data.value("exception_type").toString(), data.value("exception_message").toString());
        m_finished = true;
    } else if (type == "execution_interrupted") {
        m_execError = "interrupted";
        m_finished = true;
    }
}

Runner::Result Runner::run(const Job &job) {
    Result r;
    QElapsedTimer total;
    total.start();
    if (!m_client) {
        r.error = "runner not started";
        return r;
    }
    if (m_cancelled) {
        r.error = "cancelled";
        return r;
    }
    H3::GenParams params = job.params;
    WorkflowBuilder builder(m_paths.templatesDir);
    builder.loadTemplate(params.mode, &r.templateSource);

    // Upload reference / first-frame images under content-addressed names, so
    // two different files never collide and re-uploads are harmless.
    params.images.clear();
    for (const QString &file : job.imageFiles) {
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly)) {
            r.error = "cannot read image " + file;
            return r;
        }
        const QString hash = QString::fromLatin1(QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex().left(12));
        const QString name = QString("ovg_%1.%2").arg(hash, QFileInfo(file).suffix().toLower());
        emitEvent({{"event", "uploading"}, {"file", file}, {"name", name}});
        if (!m_client->uploadImage(file, name, &r.error)) return r;
        params.images << name;
    }

    r.graph = builder.build(params, &r.error);
    if (r.graph.isEmpty()) return r;

    // Every model file the graph names must be present; say which is missing
    // instead of letting ComfyUI fail halfway.
    const QJsonObject models = WorkflowBuilder::modelFiles(r.graph);
    const QStringList unets = m_client->models("diffusion_models");
    const QStringList tes = m_client->models("text_encoders");
    const QStringList vaes = m_client->models("vae");
    QStringList missing;
    if (!unets.contains(models.value("diffusion_model").toString())) missing << "diffusion_models/" + models.value("diffusion_model").toString();
    if (!tes.contains(models.value("text_encoder").toString())) missing << "text_encoders/" + models.value("text_encoder").toString();
    for (const auto &v : models.value("vae").toArray())
        if (!vaes.contains(v.toString())) missing << "vae/" + v.toString();
    if (!missing.isEmpty()) {
        r.error = "missing model files (not downloaded by this app): " + missing.join(", ");
        return r;
    }

    m_promptId.clear();
    m_finished = false;
    m_execError.clear();
    m_samplingStartValue = -1;
    m_samplingSeconds = 0;
    r.vramBaselineMiB = gpuMemoryUsedMiB();
    r.vramPeakMiB = r.vramBaselineMiB;

    r.promptId = m_client->queuePrompt(r.graph, m_clientId, &r.error);
    if (r.promptId.isEmpty()) return r;
    m_promptId = r.promptId;
    emitEvent({{"event", "queued"}, {"prompt_id", r.promptId}, {"label", job.label},
               {"width", params.width}, {"height", params.height},
               {"frames", H3::framesForSeconds(params.seconds)}, {"seed", params.seed}, {"steps", params.steps}});

    // Wait for the finish message, and also poll /history, which survives a
    // dropped WebSocket.
    QJsonObject hist;
    QElapsedTimer poll;
    poll.start();
    while (!m_finished) {
        if (m_cancelled) {
            m_client->interrupt();
            r.error = "cancelled";
            return r;
        }
        sleepEventLoop(250);
        if (poll.elapsed() > 2000) {
            poll.restart();
            const int used = gpuMemoryUsedMiB();
            r.vramPeakMiB = std::max(r.vramPeakMiB, used);
            hist = m_client->history(r.promptId);
            const QJsonObject st = hist.value("status").toObject();
            if (st.value("completed").toBool() || st.value("status_str") == "error") {
                if (st.value("status_str") == "error" && m_execError.isEmpty()) {
                    for (const auto &m : st.value("messages").toArray())
                        if (m.toArray().at(0).toString() == "execution_error") {
                            const QJsonObject d = m.toArray().at(1).toObject();
                            m_execError = QString("%1: %2").arg(d.value("exception_type").toString(),
                                                                d.value("exception_message").toString());
                        }
                    if (m_execError.isEmpty()) m_execError = "ComfyUI reported an error";
                }
                m_finished = true;
            }
        }
    }
    r.samplingSeconds = m_samplingSeconds;
    if (!m_execError.isEmpty()) {
        r.error = m_execError;
        return r;
    }
    if (!hist.value("status").toObject().value("completed").toBool())
        for (int i = 0; i < 20 && hist.value("outputs").toObject().isEmpty(); ++i) {
            sleepEventLoop(250);
            hist = m_client->history(r.promptId);
        }

    // SaveVideo reports its file under "images" (with "animated"), older
    // nodes under "videos" or "gifs".
    QJsonObject out;
    const QJsonObject outputs = hist.value("outputs").toObject();
    for (auto it = outputs.begin(); it != outputs.end() && out.isEmpty(); ++it)
        for (const char *key : {"videos", "images", "gifs"})
            for (const auto &v : it.value().toObject().value(key).toArray())
                if (v.toObject().value("filename").toString().endsWith(".mp4")) out = v.toObject();
    if (out.isEmpty()) {
        r.error = "ComfyUI finished but reported no video file";
        return r;
    }
    const QString filename = out.value("filename").toString(), subfolder = out.value("subfolder").toString();
    const QString local = QDir(m_paths.outputDir).filePath(subfolder.isEmpty() ? filename : subfolder + "/" + filename);
    QDir().mkpath(QFileInfo(job.destFile).absolutePath());
    if (m_status.ours && QFileInfo::exists(local)) {
        if (QFileInfo::exists(job.destFile)) QFile::remove(job.destFile);
        if (!QFile::rename(local, job.destFile)) {
            r.error = "cannot move " + local + " to " + job.destFile;
            return r;
        }
    } else if (!m_client->download(filename, subfolder, out.value("type").toString("output"), job.destFile, &r.error)) {
        return r;
    }
    r.file = job.destFile;
    r.comfyVersion = m_status.version;
    r.seconds = total.elapsed() / 1000.0;
    r.ok = true;
    emitEvent({{"event", "done"}, {"result", r.toJson()}});
    return r;
}

void Runner::end(bool keepComfy) {
    if (m_client && m_status.running) {
        // Unload the model so the VRAM is free for other apps.
        m_client->freeMemory();
        if (m_status.ours) {
            const int held = ComfyServer::waitForVramRelease(m_status.pid);
            emitEvent({{"event", "models_unloaded"}, {"comfy_vram_mib", held}});
            // The GPU lock must not be released while ComfyUI still sits on
            // gigabytes of VRAM, so a server that keeps its pool is stopped
            // even when asked to stay warm.
            const bool stillHolding = held > 1024;
            if (m_startedComfy || stillHolding) {
                if (!keepComfy || stillHolding) {
                    if (stillHolding && keepComfy)
                        emitEvent({{"event", "log"}, {"message", QString("ComfyUI still held %1 MiB after unloading; stopping it to release the GPU").arg(held)}});
                    QString err;
                    ComfyServer::stop(m_paths, &err);
                    emitEvent({{"event", "comfy_stopped"}, {"error", err}});
                }
            }
        } else {
            emitEvent({{"event", "models_unloaded"}});
            emitEvent({{"event", "log"}, {"message", "ComfyUI was started elsewhere; asked it to unload but cannot stop it"}});
        }
    }
    m_ws.close();
    m_client.reset();
    m_lock.unlock();
    emitEvent({{"event", "gpu_released"}});
}

void Runner::cancel() { m_cancelled = true; }
