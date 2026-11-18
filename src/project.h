#pragma once

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include "h3.h"

// A multi-shot project, like the earlier proven workflow: projects/<NAME>/shots.json
// lists the shots; clips/ holds the rendered shots; video_N.mp4 and
// video_N_manifest.json are the stitched results; contact/ holds frame grids.
struct ShotSpec {
    int id = 0;
    QString mode;          // t2v | i2v | r2v (falls back to the project's mode)
    QString prompt;        // may name refs as {KEY}
    QStringList refs;      // keys into ProjectSpec::refs, in <Picture i> order
    QString image;         // i2v first frame (path relative to the project)
    double seconds = 3.0;
    qint64 seed = 0;
};

struct ProjectSpec {
    QString name, dir, title, mode = "t2v", style;
    int width = 608, height = 352, steps = 20;
    bool audio = true;
    QMap<QString, QString> refs;  // KEY -> path relative to the project
    QList<ShotSpec> shots;

    static bool validName(const QString &name);
    static bool load(const QString &dir, ProjectSpec *out, QString *error);
    // Writes a starter shots.json (two short text-to-video shots) into a new folder.
    static bool create(const QString &dir, const QString &name, QString *error);

    const ShotSpec *shot(int id) const;
    H3::Mode shotMode(const ShotSpec &s) const;
    // {KEY} -> <Picture i> (render_shots.py rule), then the shared style text.
    QString finalPrompt(const ShotSpec &s) const;
    QStringList imagePaths(const ShotSpec &s) const;  // absolute
    QString paramsHash(const ShotSpec &s, qint64 seed) const;

    QJsonObject loadState() const;
    bool saveState(const QJsonObject &state, QString *error = nullptr) const;
    qint64 effectiveSeed(const ShotSpec &s, const QJsonObject &state) const;
    // Rendered and still matching the shot's current settings.
    bool shotUpToDate(const ShotSpec &s, const QJsonObject &state) const;
    QString clipPath(const ShotSpec &s, const QJsonObject &state) const;
};

namespace Manifest {
// Next unused N for video_N.mp4 in dir: one past the highest N used by any
// video_N.mp4 or video_N_manifest.json, so numbers are never reused.
int nextVideoNumber(const QString &dir);
// Atomic write that refuses to overwrite an existing file.
bool write(const QString &path, const QJsonObject &manifest, QString *error = nullptr);
}
