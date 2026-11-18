#include "paths.h"

#include <QDir>
#include <QStandardPaths>
#include <unistd.h>

#ifndef APP_GIT_COMMIT
#define APP_GIT_COMMIT "unknown"
#endif
#ifndef APP_VERSION
#define APP_VERSION "0.1.0"
#endif

QString appGitCommit() { return QStringLiteral(APP_GIT_COMMIT); }
QString appVersion() { return QStringLiteral(APP_VERSION); }

AppPaths AppPaths::forHome(const QString &home) {
    AppPaths p;
    p.home = QDir::cleanPath(home);
    p.comfyDir = p.home + "/comfyui";
    p.python = p.home + "/.venv/bin/python";
    p.outputDir = p.home + "/output";
    p.inputDir = p.home + "/input";
    p.tmpDir = p.home + "/.tmp";
    p.cacheDir = p.home + "/.cache";
    p.comfyUserDir = p.home + "/.comfy-user";
    p.projectsDir = p.home + "/projects";
    p.templatesDir = p.home + "/templates";
    // The lock file is shared with other GPU apps on this machine, so its
    // path is fixed by convention rather than by this app's home.
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty())
        runtime = QString("/tmp/omni-gpu-%1").arg(getuid());
    p.gpuLock = qEnvironmentVariable("OMNI_GPU_LOCK", runtime + "/omni-gpu.lock");
    return p;
}

AppPaths AppPaths::fromEnvironment() {
    const QString home = qEnvironmentVariable("OMNI_VIDEO_HOME", QDir::homePath() + "/omni_video_generator");
    AppPaths p = forHome(home);
    if (qEnvironmentVariableIsSet("OMNI_VIDEO_COMFY_DIR"))
        p.comfyDir = qEnvironmentVariable("OMNI_VIDEO_COMFY_DIR");
    if (qEnvironmentVariableIsSet("OMNI_VIDEO_PYTHON"))
        p.python = qEnvironmentVariable("OMNI_VIDEO_PYTHON");
    return p;
}

bool AppPaths::ensureDirs(QString *error) const {
    for (const QString &d : {outputDir, inputDir, tmpDir, cacheDir, comfyUserDir, projectsDir}) {
        if (!QDir().mkpath(d)) {
            if (error) *error = QString("cannot create %1").arg(d);
            return false;
        }
    }
    return true;
}
