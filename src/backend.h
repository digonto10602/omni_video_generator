#pragma once

#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include "filepicker.h"
#include "paths.h"

// The window's state. Long jobs (generation, project render, stitch) run as
// this same binary's CLI in a child process with --progress-json, so the GUI
// and the command line share one code path, one GPU lock rule, and the job
// survives nothing it should not: closing the window cancels it cleanly.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool comfyRunning READ comfyRunning NOTIFY comfyChanged)
    Q_PROPERTY(bool comfyOurs READ comfyOurs NOTIFY comfyChanged)
    Q_PROPERTY(QString comfyText READ comfyText NOTIFY comfyChanged)
    Q_PROPERTY(bool gpuBusy READ gpuBusy NOTIFY comfyChanged)
    Q_PROPERTY(QString installProblem READ installProblem CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY jobChanged)
    Q_PROPERTY(bool waitingForGpu READ waitingForGpu NOTIFY jobChanged)
    Q_PROPERTY(QString jobTitle READ jobTitle NOTIFY jobChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY jobChanged)
    Q_PROPERTY(double progress READ progress NOTIFY jobChanged)
    Q_PROPERTY(QString etaText READ etaText NOTIFY jobChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY jobChanged)
    Q_PROPERTY(QString lastOutput READ lastOutput NOTIFY jobChanged)
    Q_PROPERTY(QString log READ log NOTIFY logChanged)
    Q_PROPERTY(QStringList projects READ projects NOTIFY projectsChanged)
    Q_PROPERTY(QVariantMap project READ project NOTIFY projectChanged)
    Q_PROPERTY(QVariantList gallery READ gallery NOTIFY galleryChanged)
    Q_PROPERTY(QVariantList presets READ presets CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString gitCommit READ gitCommit CONSTANT)
    Q_PROPERTY(QString homeDir READ homeDir CONSTANT)
    Q_PROPERTY(int maxPixels READ maxPixels CONSTANT)

public:
    explicit Backend(const AppPaths &paths, FilePicker *picker, QObject *parent = nullptr);
    ~Backend() override;

    bool comfyRunning() const { return m_comfyRunning; }
    bool comfyOurs() const { return m_comfyOurs; }
    QString comfyText() const { return m_comfyText; }
    bool gpuBusy() const { return m_gpuBusy; }
    QString installProblem() const { return m_installProblem; }
    bool busy() const { return m_job.state() != QProcess::NotRunning; }
    bool waitingForGpu() const { return m_waitingForGpu; }
    QString jobTitle() const { return m_jobTitle; }
    QString statusText() const { return m_statusText; }
    double progress() const { return m_progress; }
    QString etaText() const { return m_etaText; }
    QString lastError() const { return m_lastError; }
    QString lastOutput() const { return m_lastOutput; }
    QString log() const { return m_log.join('\n'); }
    QStringList projects() const { return m_projects; }
    QVariantMap project() const { return m_project; }
    QVariantList gallery() const { return m_gallery; }
    QVariantList presets() const;
    QString version() const;
    QString gitCommit() const;
    QString homeDir() const { return m_paths.home; }
    int maxPixels() const;

    Q_INVOKABLE int snapSize(double value) const;
    Q_INVOKABLE int framesFor(double seconds) const;
    Q_INVOKABLE double secondsFor(int frames) const;

    Q_INVOKABLE void startComfy();
    Q_INVOKABLE void stopComfy();
    Q_INVOKABLE void generate(const QVariantMap &settings);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void pickImages(bool multiple);

    Q_INVOKABLE void refreshProjects();
    Q_INVOKABLE void loadProject(const QString &name);
    Q_INVOKABLE void newProject(const QString &name);
    Q_INVOKABLE void renderProject(const QString &name);
    Q_INVOKABLE void retryShot(const QString &name, int shot, bool newSeed);
    Q_INVOKABLE void stitchProject(const QString &name);
    Q_INVOKABLE void contactSheets(const QString &name);
    Q_INVOKABLE void refreshGallery();
    Q_INVOKABLE void openPath(const QString &path);
    Q_INVOKABLE QString fileUrl(const QString &path) const;
    Q_INVOKABLE bool exists(const QString &path) const;

signals:
    void comfyChanged();
    void jobChanged();
    void logChanged();
    void projectsChanged();
    void projectChanged();
    void galleryChanged();
    void imagesPicked(const QStringList &paths);
    void jobFinished(bool ok, const QString &output);

private:
    void pollComfy();
    void startJob(const QString &title, const QStringList &args);
    void onJobLine(const QByteArray &line);
    void appendLog(const QString &line);
    void runQuick(const QStringList &args, std::function<void(const QJsonObject &)> done);

    AppPaths m_paths;
    FilePicker *m_picker;
    QNetworkAccessManager m_nam;
    QTimer m_poll;
    QTimer m_idleStop;
    QProcess m_job;
    QByteArray m_jobBuffer;
    QString m_jobTitle, m_statusText, m_etaText, m_lastError, m_lastOutput, m_currentProject;
    double m_progress = 0;
    bool m_waitingForGpu = false;
    int m_shotIndex = 0, m_shotCount = 0;
    double m_projectEta = -1;
    bool m_comfyRunning = false, m_comfyOurs = false, m_gpuBusy = false;
    QString m_comfyText = "checking ComfyUI...";
    QString m_installProblem;
    QStringList m_log;
    QStringList m_projects;
    QVariantMap m_project;
    QVariantList m_gallery;
};
