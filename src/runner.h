#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <functional>
#include <memory>

#include "comfy.h"
#include "gpulock.h"
#include "h3.h"
#include "paths.h"
#include "wsreader.h"

// Runs H3 generations one at a time on the local ComfyUI. A session holds the
// shared GPU lock from begin() to end(): the whole time the model may sit in
// VRAM. end() unloads the models (/free) and, if this session started ComfyUI,
// stops it, so the card is empty for the next app.
class Runner : public QObject {
    Q_OBJECT
public:
    struct Job {
        H3::GenParams params;     // params.images is filled in by run()
        QStringList imageFiles;   // local files to upload, in order
        QString destFile;         // final .mp4 location
        QString label;
    };
    struct Result {
        bool ok = false;
        QString error, file, promptId, templateSource;
        double seconds = 0;          // queue to file on disk
        double samplingSeconds = 0;
        int vramPeakMiB = 0;         // whole card, from nvidia-smi
        int vramBaselineMiB = 0;     // before the job (desktop and other apps)
        QJsonObject graph;
        QJsonObject toJson() const;
    };
    using EventFn = std::function<void(const QJsonObject &)>;

    Runner(const AppPaths &paths, EventFn onEvent, QObject *parent = nullptr);
    ~Runner() override;

    bool begin(QString *error);
    Result run(const Job &job);
    void end(bool keepComfy);
    void cancel();
    bool cancelled() const { return m_cancelled; }
    ComfyStatus comfyStatus() const { return m_status; }

    static int gpuMemoryUsedMiB();

private:
    void emitEvent(QJsonObject e);
    void onWsMessage(const QString &text);

    AppPaths m_paths;
    EventFn m_onEvent;
    GpuLock m_lock;
    ComfyStatus m_status;
    bool m_startedComfy = false;
    bool m_cancelled = false;
    std::unique_ptr<ComfyClient> m_client;
    WsReader m_ws;
    QString m_clientId;
    // Per-job tracking from WebSocket messages.
    QString m_promptId;
    bool m_finished = false;
    QString m_execError;
    QString m_currentNode;
    QElapsedTimer m_samplingTimer;
    int m_samplingStartValue = -1;
    double m_samplingSeconds = 0;
};
