#include "media.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace Media {

QJsonObject ProbeResult::toJson() const {
    return {{"width", width}, {"height", height}, {"fps", fps}, {"frames", frames}, {"duration_seconds", duration},
            {"has_audio", hasAudio}, {"audio_sample_rate", audioRate}, {"audio_channels", audioChannels},
            {"video_codec", videoCodec}, {"audio_codec", audioCodec}};
}

bool run(const QString &program, const QStringList &args, QString *error, QByteArray *output, int timeoutMs) {
    QProcess p;
    p.start(program, args);
    if (!p.waitForStarted(10000)) {
        if (error) *error = QString("%1 is not installed or cannot start").arg(program);
        return false;
    }
    p.closeWriteChannel();
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        if (error) *error = QString("%1 timed out").arg(program);
        return false;
    }
    if (output) *output = p.readAllStandardOutput();
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (error) *error = QString("%1 failed: %2").arg(program, QString::fromUtf8(p.readAllStandardError().right(2000)));
        return false;
    }
    return true;
}

bool toolsAvailable(QString *missing) {
    QStringList m;
    for (const char *tool : {"ffmpeg", "ffprobe"})
        if (QStandardPaths::findExecutable(tool).isEmpty()) m << tool;
    if (missing) *missing = m.join(", ");
    return m.isEmpty();
}

static double rate(const QString &r) {
    const auto parts = r.split('/');
    if (parts.size() == 2 && parts[1].toDouble() != 0) return parts[0].toDouble() / parts[1].toDouble();
    return r.toDouble();
}

ProbeResult probe(const QString &file) {
    ProbeResult r;
    QByteArray out;
    if (!run("ffprobe", {"-v", "error", "-count_packets", "-show_streams", "-show_format", "-of", "json", file},
             &r.error, &out, 60000))
        return r;
    const QJsonObject o = QJsonDocument::fromJson(out).object();
    for (const auto &sv : o.value("streams").toArray()) {
        const QJsonObject s = sv.toObject();
        if (s.value("codec_type") == "video" && !r.width) {
            r.width = s.value("width").toInt();
            r.height = s.value("height").toInt();
            r.fps = rate(s.value("avg_frame_rate").toString());
            r.frames = s.value("nb_read_packets").toString().toInt();
            if (!r.frames) r.frames = s.value("nb_frames").toString().toInt();
            r.videoCodec = s.value("codec_name").toString();
        } else if (s.value("codec_type") == "audio" && !r.hasAudio) {
            r.hasAudio = true;
            r.audioRate = s.value("sample_rate").toString().toInt();
            r.audioChannels = s.value("channels").toInt();
            r.audioCodec = s.value("codec_name").toString();
        }
    }
    r.duration = o.value("format").toObject().value("duration").toString().toDouble();
    r.ok = r.width > 0;
    if (!r.ok && r.error.isEmpty()) r.error = "no video stream in " + file;
    return r;
}

bool extractFrame(const QString &video, double seconds, const QString &png, QString *error) {
    QDir().mkpath(QFileInfo(png).absolutePath());
    return run("ffmpeg", {"-v", "error", "-nostdin", "-y", "-ss", QString::number(seconds, 'f', 3), "-i", video,
                          "-frames:v", "1", png}, error);
}

bool contactSheet(const QString &video, const QString &png, QString *error) {
    const ProbeResult p = probe(video);
    if (!p.ok) {
        if (error) *error = p.error;
        return false;
    }
    QTemporaryDir tmp(QFileInfo(png).absolutePath() + "/.sheet-XXXXXX");
    QStringList frames;
    const double fractions[] = {0.05, 0.5, 0.95};
    for (int k = 0; k < 3; ++k) {
        const QString f = tmp.filePath(QString("f%1.png").arg(k));
        if (!extractFrame(video, p.duration * fractions[k], f, error)) return false;
        frames << f;
    }
    return run("ffmpeg", {"-v", "error", "-nostdin", "-y", "-i", frames[0], "-i", frames[1], "-i", frames[2],
                          "-filter_complex", "hstack=3", png}, error);
}

bool stackImages(const QStringList &pngs, const QString &out, int width, QString *error) {
    if (pngs.isEmpty()) {
        if (error) *error = "no images to stack";
        return false;
    }
    QStringList args{"-v", "error", "-nostdin", "-y"};
    QString graph;
    for (int i = 0; i < pngs.size(); ++i) {
        args << "-i" << pngs[i];
        graph += QString("[%1:v]scale=%2:-2[s%1];").arg(i).arg(width);
    }
    for (int i = 0; i < pngs.size(); ++i) graph += QString("[s%1]").arg(i);
    graph += pngs.size() > 1 ? QString("vstack=%1").arg(pngs.size()) : QString("null");
    args << "-filter_complex" << graph << out;
    return run("ffmpeg", args, error);
}

QStringList stitchArguments(const QStringList &clips, int width, int height, bool audio, const QString &out,
                            const QString &comment) {
    QStringList args{"-v", "error", "-nostdin", "-n"};
    QString graph;
    for (int i = 0; i < clips.size(); ++i) {
        args << "-i" << clips[i];
        graph += QString("[%1:v]scale=%2:%3:force_original_aspect_ratio=increase,crop=%2:%3,fps=24,setsar=1,format=yuv420p[v%1];")
                     .arg(i).arg(width).arg(height);
        if (audio)
            graph += QString("[%1:a]aresample=48000,aformat=channel_layouts=stereo[a%1];").arg(i);
    }
    for (int i = 0; i < clips.size(); ++i)
        graph += audio ? QString("[v%1][a%1]").arg(i) : QString("[v%1]").arg(i);
    graph += QString("concat=n=%1:v=1:a=%2[v]%3").arg(clips.size()).arg(audio ? 1 : 0).arg(audio ? "[a]" : "");
    args << "-filter_complex" << graph << "-map" << "[v]";
    if (audio) args << "-map" << "[a]";
    args << "-c:v" << "libx264" << "-crf" << "18" << "-preset" << "medium" << "-pix_fmt" << "yuv420p";
    if (audio) args << "-c:a" << "aac" << "-b:a" << "192k" << "-ar" << "48000" << "-ac" << "2";
    // The H3 licence encourages marking AI-generated files.
    args << "-metadata" << ("comment=" + comment) << "-movflags" << "+faststart" << out;
    return args;
}

bool stitch(const QStringList &clips, int width, int height, bool audio, const QString &out, const QString &comment,
            QString *error) {
    return run("ffmpeg", stitchArguments(clips, width, height, audio, out, comment), error, nullptr, 3600000);
}

}  // namespace Media
