#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QString>
#include <functional>

#include "paths.h"

// Blocking HTTP calls to a ComfyUI server, for the command-line side. They
// spin a local event loop, so signals (like WebSocket progress) keep flowing.
class ComfyClient {
public:
    ComfyClient(const QString &host = "127.0.0.1", int port = 8188);
    QString baseUrl() const { return QString("http://%1:%2").arg(m_host).arg(m_port); }
    QString host() const { return m_host; }
    int port() const { return m_port; }

    QJsonObject getJson(const QString &path, QString *error = nullptr, int timeoutMs = 10000);
    QJsonObject postJson(const QString &path, const QJsonObject &body, QString *error = nullptr,
                         int timeoutMs = 30000, int *status = nullptr);
    QJsonObject systemStats(QString *error = nullptr, int timeoutMs = 3000);
    QStringList models(const QString &folder, QString *error = nullptr);
    // Uploads a local image to ComfyUI's input folder under `name`.
    bool uploadImage(const QString &localPath, const QString &name, QString *error = nullptr);
    QString queuePrompt(const QJsonObject &graph, const QString &clientId, QString *error = nullptr);
    QJsonObject history(const QString &promptId, QString *error = nullptr);
    bool freeMemory(QString *error = nullptr);
    bool interrupt(QString *error = nullptr);
    bool download(const QString &filename, const QString &subfolder, const QString &type,
                  const QString &dest, QString *error = nullptr);

private:
    QByteArray request(const QString &method, const QString &path, const QByteArray &body,
                       const QByteArray &contentType, QString *error, int timeoutMs, int *status);
    QString m_host;
    int m_port;
    QNetworkAccessManager m_nam;
};

// Starting, finding and stopping the ComfyUI server.
struct ComfyStatus {
    bool running = false;
    bool ours = false;        // started by this app (pid file), so we may stop it
    int port = 0;
    qint64 pid = 0;
    QString version;
    QJsonObject stats;
    QString describe() const;
    QJsonObject toJson() const;
};

namespace ComfyServer {
ComfyStatus detect(const AppPaths &paths);
// The proven 8 GB launch command (hellfire start_comfyui.sh) plus local-only,
// no-cloud and in-project folders.
QStringList launchArguments(const AppPaths &paths, int port);
// Missing pieces (ComfyUI code, Python env, model files) as readable lines.
QStringList checkInstall(const AppPaths &paths);
ComfyStatus start(const AppPaths &paths, QString *error, const std::function<void(const QString &)> &log = {});
bool stop(const AppPaths &paths, QString *error = nullptr);
int findFreePort(int first = 8188, int last = 8199);
// VRAM held by one process, from nvidia-smi (MiB; 0 if none, -1 if unknown).
int processVramMiB(qint64 pid);
// After /free, polls until the process holds at most `thresholdMiB` or the
// timeout passes; returns what it still holds.
int waitForVramRelease(qint64 pid, int thresholdMiB = 1024, int timeoutMs = 10000);
}
