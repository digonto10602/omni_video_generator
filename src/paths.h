#pragma once

#include <QString>

// Where everything lives. The app keeps all its state in one folder
// (~/omni_video_generator, or $OMNI_VIDEO_HOME), next to the ComfyUI copy and
// the Python environment, so nothing is scattered across the home directory.
struct AppPaths {
    QString home;          // ~/omni_video_generator
    QString comfyDir;      // home/comfyui (ComfyUI code + models)
    QString python;        // home/.venv/bin/python
    QString outputDir;     // home/output (ComfyUI --output-directory)
    QString inputDir;      // home/input  (ComfyUI --input-directory, uploads land here)
    QString tmpDir;        // home/.tmp
    QString cacheDir;      // home/.cache
    QString comfyUserDir;  // home/.comfy-user
    QString projectsDir;   // home/projects
    QString templatesDir;  // home/templates (editable workflow templates)
    QString gpuLock;       // $XDG_RUNTIME_DIR/omni-gpu.lock

    static AppPaths fromEnvironment();
    static AppPaths forHome(const QString &home);
    QString pidFile() const { return tmpDir + "/comfyui.json"; }
    QString comfyLog() const { return tmpDir + "/comfyui.log"; }
    bool ensureDirs(QString *error = nullptr) const;
};

QString appGitCommit();
QString appVersion();
