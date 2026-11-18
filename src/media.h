#pragma once

#include <QJsonObject>
#include <QStringList>

// ffmpeg / ffprobe through QProcess (never linked), as Omarchy apps do.
namespace Media {
struct ProbeResult {
    bool ok = false;
    int width = 0, height = 0;
    double fps = 0;
    int frames = 0;
    double duration = 0;
    bool hasAudio = false;
    int audioRate = 0, audioChannels = 0;
    QString videoCodec, audioCodec, error;
    QJsonObject toJson() const;
};
ProbeResult probe(const QString &file);
bool toolsAvailable(QString *missing = nullptr);
bool extractFrame(const QString &video, double seconds, const QString &png, QString *error = nullptr);
// Start / middle / end frames side by side (like hellfire contact_sheets.sh).
bool contactSheet(const QString &video, const QString &png, QString *error = nullptr);
// Several sheets stacked into one image, each scaled to `width`.
bool stackImages(const QStringList &pngs, const QString &out, int width, QString *error = nullptr);
// The ffmpeg arguments for joining clips with hard cuts, normalised to one
// size, 24 fps and 48 kHz stereo (render_shots.py do_stitch, plus scaling).
QStringList stitchArguments(const QStringList &clips, int width, int height, bool audio, const QString &out,
                            const QString &comment);
bool stitch(const QStringList &clips, int width, int height, bool audio, const QString &out,
            const QString &comment, QString *error = nullptr);
bool run(const QString &program, const QStringList &args, QString *error = nullptr, QByteArray *output = nullptr,
         int timeoutMs = 600000);
}
