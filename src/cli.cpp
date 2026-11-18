// Headless commands. Each exits 0 or 1, prints errors on stderr, and with
// --json prints one JSON object on stdout. --progress-json streams one JSON
// event per line on stdout instead (the GUI drives jobs this way).
#include "cli.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSocketNotifier>
#include <QTextStream>
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

#include "comfy.h"
#include "gpulock.h"
#include "h3.h"
#include "media.h"
#include "paths.h"
#include "project.h"
#include "runner.h"

namespace {

QTextStream out(stdout), err(stderr);

struct Options {
    QStringList positional;
    QMap<QString, QStringList> values;
    QSet<QString> flags;
    bool has(const QString &f) const { return flags.contains(f); }
    QString value(const QString &k, const QString &def = {}) const {
        return values.contains(k) ? values.value(k).last() : def;
    }
};

const QSet<QString> kValueOptions{"mode", "prompt", "ref", "width", "height", "seconds", "seed", "steps", "shot",
                                  "out", "size", "preset"};

bool parse(const QStringList &args, Options *o, QString *error) {
    for (int i = 0; i < args.size(); ++i) {
        const QString a = args[i];
        if (a.startsWith("--")) {
            QString key = a.mid(2), value;
            const int eq = key.indexOf('=');
            if (eq >= 0) { value = key.mid(eq + 1); key = key.left(eq); }
            if (kValueOptions.contains(key)) {
                if (eq < 0) {
                    if (i + 1 >= args.size()) { *error = "--" + key + " needs a value"; return false; }
                    value = args[++i];
                }
                o->values[key] << value;
            } else {
                o->flags.insert(key);
            }
        } else {
            o->positional << a;
        }
    }
    return true;
}

bool g_json = false, g_progress = false;

void emitLine(const QJsonObject &o) {
    out << QJsonDocument(o).toJson(QJsonDocument::Compact) << "\n";
    out.flush();
}

int fail(const QString &message, QJsonObject extra = {}) {
    err << "omni-video-generator: " << message << "\n";
    err.flush();
    if (g_json || g_progress) {
        extra["ok"] = false;
        extra["error"] = message;
        if (g_progress) extra["event"] = "result";
        emitLine(extra);
    }
    return 1;
}

int succeed(QJsonObject result, const QString &human) {
    result["ok"] = true;
    if (g_progress) {
        result["event"] = "result";
        emitLine(result);
    } else if (g_json) {
        emitLine(result);
    } else if (!human.isEmpty()) {
        out << human << (human.endsWith('\n') ? "" : "\n");
        out.flush();
    }
    return 0;
}

QString fmtDuration(double s) {
    const int t = int(s + 0.5);
    return t >= 60 ? QString("%1 min %2 s").arg(t / 60).arg(t % 60) : QString("%1 s").arg(t);
}

// Human-readable progress on stderr, or JSON lines on stdout.
Runner::EventFn eventPrinter() {
    return [](const QJsonObject &e) {
        if (g_progress) {
            emitLine(e);
            return;
        }
        if (g_json) return;
        const QString ev = e.value("event").toString();
        if (ev == "waiting_gpu") err << e.value("message").toString() << " ...\n";
        else if (ev == "gpu_locked") err << "GPU lock held (" << e.value("lock").toString() << ")\n";
        else if (ev == "log") err << e.value("message").toString() << "\n";
        else if (ev == "queued")
            err << QString("queued %1: %2x%3, %4 frames, seed %5, %6 steps\n")
                       .arg(e.value("label").toString()).arg(e.value("width").toInt()).arg(e.value("height").toInt())
                       .arg(e.value("frames").toInt()).arg(e.value("seed").toInteger(0)).arg(e.value("steps").toInt());
        else if (ev == "progress") {
            QString line = QString("\r  step %1/%2").arg(e.value("value").toInt()).arg(e.value("max").toInt());
            if (e.contains("eta_seconds"))
                line += QString("  %1 s/step  ETA %2   ").arg(e.value("seconds_per_step").toDouble(), 0, 'f', 1)
                            .arg(fmtDuration(e.value("eta_seconds").toDouble()));
            err << line;
            if (e.value("value").toInt() == e.value("max").toInt()) err << "\n";
        } else if (ev == "done") {
            const QJsonObject r = e.value("result").toObject();
            err << QString("  done in %1, VRAM peak %2 MiB (baseline %3 MiB)\n")
                       .arg(fmtDuration(r.value("seconds").toDouble())).arg(r.value("vram_peak_mib").toInt())
                       .arg(r.value("vram_baseline_mib").toInt());
        } else if (ev == "project_progress")
            err << QString("shot %1 (%2 of %3)%4\n").arg(e.value("shot").toInt()).arg(e.value("index").toInt())
                       .arg(e.value("count").toInt())
                       .arg(e.contains("eta_seconds") ? "  project ETA " + fmtDuration(e.value("eta_seconds").toDouble()) : QString());
        else if (ev == "models_unloaded") err << "models unloaded" << (e.contains("comfy_vram_mib") ? QString(" (ComfyUI process still holds %1 MiB)").arg(e.value("comfy_vram_mib").toInt()) : QString()) << "\n";
        else if (ev == "comfy_stopped") err << "ComfyUI stopped\n";
        err.flush();
    };
}

// SIGINT / SIGTERM cancel the running job cleanly (interrupt ComfyUI, unload,
// release the GPU lock) through a self-pipe into the event loop.
int g_sigFd[2] = {-1, -1};
Runner *g_runner = nullptr;
void onSignal(int) {
    char c = 1;
    [[maybe_unused]] auto n = ::write(g_sigFd[0], &c, 1);
}
void installCancelHandler() {
    if (g_sigFd[0] >= 0) return;
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, g_sigFd);
    auto *notifier = new QSocketNotifier(g_sigFd[1], QSocketNotifier::Read, qApp);
    QObject::connect(notifier, &QSocketNotifier::activated, [] {
        char c;
        [[maybe_unused]] auto n = ::read(g_sigFd[1], &c, 1);
        if (g_runner) g_runner->cancel();
    });
    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

QJsonObject sidecarFor(const H3::GenParams &p, const Runner::Result &r, const QStringList &images) {
    return {{"kind", "clip"}, {"mode", H3::modeName(p.mode)}, {"prompt", p.prompt},
            {"width", p.width}, {"height", p.height}, {"requested_seconds", p.seconds},
            {"frames", H3::framesForSeconds(p.seconds)}, {"seed", p.seed}, {"steps", p.steps}, {"audio", p.audio},
            {"images", QJsonArray::fromStringList(images)},
            {"models", WorkflowBuilder::modelFiles(r.graph)}, {"workflow", r.graph}, {"template", r.templateSource},
            {"render", r.toJson()}, {"app", QJsonObject{{"version", appVersion()}, {"git_commit", appGitCommit()}}},
            {"created", QDateTime::currentDateTime().toString(Qt::ISODate)},
            {"notice", "AI-generated with MiniMax H3 (MiniMax H3 Community License)"}};
}

bool writeJson(const QString &path, const QJsonObject &o) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(QJsonDocument(o).toJson()) > 0;
}

// ---------------------------------------------------------------- comfy

int cmdComfy(const AppPaths &paths, const Options &o) {
    const QString sub = o.positional.value(1);
    if (sub == "status") {
        const ComfyStatus st = ComfyServer::detect(paths);
        QJsonObject j = st.toJson();
        j["gpu_lock"] = paths.gpuLock;
        j["gpu_lock_busy"] = GpuLock::isBusy(paths.gpuLock);
        j["comfy_dir"] = paths.comfyDir;
        j["python"] = paths.python;
        j["missing"] = QJsonArray::fromStringList(ComfyServer::checkInstall(paths));
        return succeed(j, st.describe() + (j["gpu_lock_busy"].toBool() ? "\nGPU lock is held by a running job" : ""));
    }
    if (sub == "start") {
        QString e;
        const ComfyStatus st = ComfyServer::start(paths, &e, [](const QString &m) { if (!g_json) err << m << "\n"; });
        if (!st.running) return fail(e);
        return succeed(st.toJson(), st.describe());
    }
    if (sub == "stop") {
        QString e;
        if (!ComfyServer::stop(paths, &e)) return fail(e);
        return succeed({}, "ComfyUI stopped");
    }
    return fail("usage: comfy start|stop|status");
}

// ---------------------------------------------------------------- gen

int cmdGen(const AppPaths &paths, const Options &o) {
    H3::GenParams p;
    if (!H3::parseMode(o.value("mode", "t2v"), &p.mode)) return fail("--mode must be t2v, i2v or r2v");
    p.prompt = o.value("prompt");
    if (p.prompt.trimmed().isEmpty()) return fail("--prompt is required");
    bool ok = true;
    p.width = H3::snapSize(o.value("width", "608").toDouble(&ok));
    p.height = H3::snapSize(o.value("height", "352").toDouble(&ok));
    p.seconds = o.value("seconds", "3").toDouble(&ok);
    if (!ok || p.seconds <= 0 || p.seconds > 15) return fail("--seconds must be a number between 0 and 15");
    p.steps = o.value("steps", "20").toInt(&ok);
    if (!ok || p.steps < 1 || p.steps > 100) return fail("--steps must be 1-100");
    p.seed = o.values.contains("seed") ? o.value("seed").toLongLong() : H3::randomSeed();
    p.audio = !o.has("no-audio");
    QStringList images;
    for (const QString &ref : o.values.value("ref")) {
        const QString abs = QFileInfo(ref).absoluteFilePath();
        if (!QFileInfo::exists(abs)) return fail("image not found: " + ref);
        images << abs;
    }
    if (p.mode == H3::Mode::I2V && images.size() != 1) return fail("i2v needs exactly one --ref (the first frame)");
    if (p.mode == H3::Mode::T2V && !images.isEmpty()) return fail("t2v takes no --ref; use --mode r2v or i2v");
    if (images.size() > 9) return fail("at most 9 reference images");
    QString e;
    if (!paths.ensureDirs(&e)) return fail(e);

    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    const QString base = QString("%1_%2_%3x%4_s%5").arg(stamp, H3::modeName(p.mode)).arg(p.width).arg(p.height).arg(p.seed);
    p.prefix = "gen/.render_" + base;
    const QString dest = o.value("out", paths.outputDir + "/gen/" + base + ".mp4");
    if (QFileInfo::exists(dest)) return fail(dest + " exists; not overwriting");

    installCancelHandler();
    Runner runner(paths, eventPrinter());
    g_runner = &runner;
    if (!runner.begin(&e)) return fail(e);
    Runner::Job job{p, images, dest, base};
    const Runner::Result r = runner.run(job);
    runner.end(o.has("keep-comfy"));
    g_runner = nullptr;
    if (!r.ok) return fail(r.error, {{"result", r.toJson()}});

    writeJson(QFileInfo(dest).absolutePath() + "/" + QFileInfo(dest).completeBaseName() + ".json", sidecarFor(p, r, images));
    const Media::ProbeResult probe = Media::probe(dest);
    QJsonObject j = r.toJson();
    j["probe"] = probe.toJson();
    j["frames_requested"] = H3::framesForSeconds(p.seconds);
    j["seed"] = p.seed;
    return succeed(j, QString("%1\n%2x%3, %4 frames, %5 s, audio %6, took %7")
                          .arg(dest).arg(probe.width).arg(probe.height).arg(probe.frames)
                          .arg(probe.duration, 0, 'f', 2).arg(probe.hasAudio ? "yes" : "no").arg(fmtDuration(r.seconds)));
}

// ---------------------------------------------------------------- project

QJsonObject shotStatus(const ProjectSpec &spec, const ShotSpec &s, const QJsonObject &state) {
    const QJsonObject st = state.value("shots").toObject().value(QString::number(s.id)).toObject();
    const QString file = spec.clipPath(s, state);
    QString status = "not rendered";
    if (spec.shotUpToDate(s, state)) status = "rendered";
    else if (!file.isEmpty() && QFileInfo::exists(file)) status = "stale";
    return {{"id", s.id}, {"mode", H3::modeName(spec.shotMode(s))}, {"prompt", spec.finalPrompt(s)},
            {"raw_prompt", s.prompt}, {"refs", QJsonArray::fromStringList(s.refs)},
            {"seconds", s.seconds}, {"frames", H3::framesForSeconds(s.seconds)},
            {"seed", spec.effectiveSeed(s, state)}, {"status", status}, {"file", file},
            {"render_seconds", st.value("render").toObject().value("seconds")}};
}

int cmdProjectStatus(const AppPaths &paths, const ProjectSpec &spec) {
    const QJsonObject state = spec.loadState();
    QJsonArray shots;
    QString human = QString("%1 (%2)  %3x%4, %5 steps, audio %6\n").arg(spec.title, spec.dir).arg(spec.width)
                        .arg(spec.height).arg(spec.steps).arg(spec.audio ? "on" : "off");
    double total = 0;
    for (const auto &s : spec.shots) {
        const QJsonObject st = shotStatus(spec, s, state);
        shots.append(st);
        total += H3::secondsForFrames(H3::framesForSeconds(s.seconds));
        human += QString("  shot %1  %2  %3 frames  seed %4  %5\n").arg(s.id, 2).arg(st["mode"].toString())
                     .arg(st["frames"].toInt(), 4).arg(st["seed"].toInteger()).arg(st["status"].toString());
    }
    QJsonArray videos;
    QStringList names = QDir(spec.dir).entryList({"video_*.mp4"}, QDir::Files, QDir::Name);
    for (const QString &n : names) videos.append(spec.dir + "/" + n);
    human += QString("  total %1 s; videos: %2").arg(total, 0, 'f', 2).arg(names.isEmpty() ? "none" : names.join(", "));
    Q_UNUSED(paths);
    return succeed({{"project", spec.name}, {"dir", spec.dir}, {"title", spec.title}, {"width", spec.width},
                    {"height", spec.height}, {"steps", spec.steps}, {"audio", spec.audio}, {"shots", shots},
                    {"videos", videos}, {"total_seconds", total},
                    {"next_video", Manifest::nextVideoNumber(spec.dir)}}, human);
}

int cmdProjectRender(const AppPaths &paths, const ProjectSpec &spec, const Options &o) {
    QJsonObject state = spec.loadState();
    QJsonObject shotsState = state.value("shots").toObject();
    QList<const ShotSpec *> todo;
    if (o.values.contains("shot")) {
        const ShotSpec *s = spec.shot(o.value("shot").toInt());
        if (!s) return fail("no shot " + o.value("shot"));
        if (o.has("new-seed")) {
            QJsonObject st = shotsState.value(QString::number(s->id)).toObject();
            st["seed_override"] = H3::randomSeed();
            shotsState[QString::number(s->id)] = st;
            state["shots"] = shotsState;
            spec.saveState(state);
        }
        todo << s;  // an explicit shot is always re-rendered
    } else {
        for (const auto &s : spec.shots)
            if (!spec.shotUpToDate(s, state) || o.has("force")) todo << &s;
    }
    if (todo.isEmpty()) return succeed({{"rendered", QJsonArray{}}}, "all shots are already rendered");

    installCancelHandler();
    Runner runner(paths, eventPrinter());
    g_runner = &runner;
    QString e;
    if (!runner.begin(&e)) return fail(e);
    QJsonArray rendered;
    // Project ETA: seconds per (frame x pixel x step) learned from shots done
    // in this run, applied to the shots still waiting.
    double unitsDone = 0, secondsDone = 0;
    auto units = [&](const ShotSpec *s) {
        return double(H3::framesForSeconds(s->seconds)) * spec.width * spec.height * spec.steps;
    };
    QString failure;
    for (int i = 0; i < todo.size(); ++i) {
        const ShotSpec *s = todo[i];
        QJsonObject pe{{"event", "project_progress"}, {"shot", s->id}, {"index", i + 1}, {"count", todo.size()}};
        if (secondsDone > 0) {
            double remaining = 0;
            for (int k = i; k < todo.size(); ++k) remaining += units(todo[k]);
            pe["eta_seconds"] = remaining * secondsDone / unitsDone;
        }
        eventPrinter()(pe);

        const qint64 seed = spec.effectiveSeed(*s, state);
        H3::GenParams p;
        p.mode = spec.shotMode(*s);
        p.prompt = spec.finalPrompt(*s);
        p.width = spec.width;
        p.height = spec.height;
        p.seconds = s->seconds;
        p.steps = spec.steps;
        p.seed = seed;
        p.audio = spec.audio;
        p.prefix = QString("projects/%1/.render_shot_%2").arg(spec.name).arg(s->id, 2, 10, QChar('0'));
        const QString rel = QString("clips/shot_%1_s%2.mp4").arg(s->id, 2, 10, QChar('0')).arg(seed);
        const QString dest = spec.dir + "/" + rel;
        const QString oldFile = spec.clipPath(*s, state);
        const Runner::Result r = runner.run({p, spec.imagePaths(*s), dest, QString("shot %1").arg(s->id)});
        if (!r.ok) {
            failure = QString("shot %1: %2").arg(s->id).arg(r.error);
            break;
        }
        unitsDone += units(s);
        secondsDone += r.seconds;
        if (!oldFile.isEmpty() && oldFile != dest) QFile::remove(oldFile);  // replaced take
        QJsonObject st = shotsState.value(QString::number(s->id)).toObject();
        st["file"] = rel;
        st["seed"] = seed;
        st["hash"] = spec.paramsHash(*s, seed);
        st["prompt"] = p.prompt;
        st["mode"] = H3::modeName(p.mode);
        st["frames"] = H3::framesForSeconds(s->seconds);
        st["models"] = WorkflowBuilder::modelFiles(r.graph);
        st["template"] = r.templateSource;
        st["render"] = r.toJson();
        st["rendered_at"] = QDateTime::currentDateTime().toString(Qt::ISODate);
        shotsState[QString::number(s->id)] = st;
        state["shots"] = shotsState;
        spec.saveState(state);
        rendered.append(QJsonObject{{"shot", s->id}, {"file", dest}, {"seed", seed}, {"seconds", r.seconds},
                                    {"vram_peak_mib", r.vramPeakMiB}});
    }
    runner.end(o.has("keep-comfy"));
    g_runner = nullptr;
    if (!failure.isEmpty()) return fail(failure, {{"rendered", rendered}});
    return succeed({{"rendered", rendered}}, QString("rendered %1 shot(s)").arg(rendered.size()));
}

int cmdProjectStitch(const AppPaths &paths, const ProjectSpec &spec, const Options &o) {
    Q_UNUSED(o);
    const QJsonObject state = spec.loadState();
    QStringList clips, problems;
    for (const auto &s : spec.shots) {
        if (!spec.shotUpToDate(s, state)) problems << QString::number(s.id);
        else clips << spec.clipPath(s, state);
    }
    if (!problems.isEmpty())
        return fail("shots not rendered or out of date: " + problems.join(", ") + " (run: project render " + spec.name + ")");
    QList<Media::ProbeResult> probes;
    double expected = 0;
    for (const QString &c : clips) {
        const Media::ProbeResult p = Media::probe(c);
        if (!p.ok) return fail(p.error);
        if (spec.audio && !p.hasAudio) return fail(c + " has no audio stream but the project has audio on");
        probes << p;
        expected += p.duration;
    }
    const int n = Manifest::nextVideoNumber(spec.dir);
    const QString final = QString("%1/video_%2.mp4").arg(spec.dir).arg(n);
    const QString partial = QString("%1/.video_%2.partial.mp4").arg(spec.dir).arg(n);
    QFile::remove(partial);
    QString e;
    const QString comment = "AI-generated with MiniMax H3 via omni-video-generator";
    eventPrinter()({{"event", "log"}, {"message", QString("stitching %1 clips into %2").arg(clips.size()).arg(final)}});
    if (!Media::stitch(clips, spec.width, spec.height, spec.audio, partial, comment, &e)) {
        QFile::remove(partial);
        return fail(e);
    }
    const Media::ProbeResult fp = Media::probe(partial);
    if (!fp.ok || std::abs(fp.duration - expected) > 0.5) {
        QFile::remove(partial);
        return fail(QString("stitched duration %1 s does not match the shots' %2 s").arg(fp.duration).arg(expected));
    }
    if (QFileInfo::exists(final) || !QFile::rename(partial, final)) {
        QFile::remove(partial);
        return fail(final + " appeared while stitching; not overwriting");
    }

    // ComfyUI is usually stopped by now; its version was recorded per shot.
    QString comfyVersion;
    for (const auto &v : state.value("shots").toObject())
        if (comfyVersion.isEmpty()) comfyVersion = v.toObject().value("render").toObject().value("comfyui_version").toString();
    QJsonArray clipEntries;
    double start = 0;
    for (int i = 0; i < spec.shots.size(); ++i) {
        const ShotSpec &s = spec.shots[i];
        const QJsonObject st = state.value("shots").toObject().value(QString::number(s.id)).toObject();
        QJsonArray refs;
        const QStringList imgs = spec.imagePaths(s);
        for (int k = 0; k < imgs.size(); ++k)
            refs.append(QJsonObject{{"tag", spec.shotMode(s) == H3::Mode::I2V ? "first_frame" : QString("<Picture %1>").arg(k + 1)},
                                    {"key", s.refs.value(k)}, {"path", QDir(spec.dir).relativeFilePath(imgs[k])}});
        clipEntries.append(QJsonObject{
            {"order", i + 1}, {"shot", s.id}, {"mode", H3::modeName(spec.shotMode(s))},
            {"prompt", spec.finalPrompt(s)}, {"raw_prompt", s.prompt}, {"refs", refs},
            {"seed", spec.effectiveSeed(s, state)}, {"requested_seconds", s.seconds},
            {"frames", probes[i].frames}, {"actual_duration_seconds", probes[i].duration},
            {"start_in_final_video_seconds", start}, {"clip_file", st.value("file")},
            {"models", st.value("models")}, {"template", st.value("template")},
            {"render_seconds", st.value("render").toObject().value("seconds")},
            {"vram_peak_mib", st.value("render").toObject().value("vram_peak_mib")}});
        start += probes[i].duration;
    }
    const QJsonObject manifest{
        {"title", spec.title}, {"project", spec.name}, {"date", QDateTime::currentDateTime().toString(Qt::ISODate)},
        {"file", QFileInfo(final).fileName()},
        {"output", fp.toJson()},
        {"settings", QJsonObject{{"width", spec.width}, {"height", spec.height}, {"fps", H3::Fps}, {"steps", spec.steps},
                                 {"audio", spec.audio}, {"style", spec.style}}},
        {"workflow", QJsonObject{{"engine", "ComfyUI (local)"}, {"comfyui_version", comfyVersion},
                                 {"sampler", "res_multistep"}, {"templates_dir", paths.templatesDir},
                                 {"launch_flags", QJsonArray::fromStringList(ComfyServer::launchArguments(paths, 8188))}}},
        {"stitch", QJsonObject{{"ffmpeg_args", QJsonArray::fromStringList(Media::stitchArguments(clips, spec.width, spec.height, spec.audio, final, comment))}}},
        {"app", QJsonObject{{"name", "omni-video-generator"}, {"version", appVersion()}, {"git_commit", appGitCommit()}}},
        {"licence", "MiniMax H3 Community License Agreement (https://huggingface.co/MiniMaxAI/MiniMax-H3/blob/main/LICENSE). AI-generated content."},
        {"clips", clipEntries}};
    const QString manifestPath = QString("%1/video_%2_manifest.json").arg(spec.dir).arg(n);
    if (!Manifest::write(manifestPath, manifest, &e)) return fail(e);
    return succeed({{"file", final}, {"manifest", manifestPath}, {"probe", fp.toJson()}},
                   QString("%1\n%2x%3, %4 frames, %5 s, audio %6\nmanifest: %7").arg(final).arg(fp.width).arg(fp.height)
                       .arg(fp.frames).arg(fp.duration, 0, 'f', 3).arg(fp.hasAudio ? "yes" : "no").arg(manifestPath));
}

int cmdProjectContact(const ProjectSpec &spec) {
    const QJsonObject state = spec.loadState();
    QDir().mkpath(spec.dir + "/contact");
    QStringList sheets;
    QString e;
    for (const auto &s : spec.shots) {
        const QString clip = spec.clipPath(s, state);
        if (clip.isEmpty() || !QFileInfo::exists(clip)) continue;
        const QString png = QString("%1/contact/shot_%2.png").arg(spec.dir).arg(s.id, 2, 10, QChar('0'));
        if (!Media::contactSheet(clip, png, &e)) return fail(e);
        sheets << png;
    }
    if (sheets.isEmpty()) return fail("no rendered shots yet");
    const QString all = spec.dir + "/contact/all.png";
    if (!Media::stackImages(sheets, all, 3 * std::min(spec.width, 480), &e)) return fail(e);
    return succeed({{"sheets", QJsonArray::fromStringList(sheets)}, {"grid", all}},
                   QString("%1 sheets\n%2").arg(sheets.size()).arg(all));
}

int cmdProject(const AppPaths &paths, const Options &o) {
    const QString sub = o.positional.value(1);
    if (sub == "list") {
        QJsonArray list;
        QString human;
        for (const QString &n : QDir(paths.projectsDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            list.append(n);
            human += n + "\n";
        }
        return succeed({{"projects", list}, {"dir", paths.projectsDir}}, human.isEmpty() ? "no projects" : human);
    }
    const QString name = o.positional.value(2);
    if (name.isEmpty() || !ProjectSpec::validName(name)) return fail("usage: project new|render|stitch|status|contact NAME");
    const QString dir = paths.projectsDir + "/" + name;
    QString e;
    if (sub == "new") {
        if (!paths.ensureDirs(&e) || !ProjectSpec::create(dir, name, &e)) return fail(e);
        return succeed({{"dir", dir}, {"shots", dir + "/shots.json"}},
                       QString("created %1\nedit %1/shots.json, put reference images in %1/refs/").arg(dir));
    }
    ProjectSpec spec;
    if (!ProjectSpec::load(dir, &spec, &e)) return fail(e);
    if (sub == "status") return cmdProjectStatus(paths, spec);
    if (sub == "render") return cmdProjectRender(paths, spec, o);
    if (sub == "stitch") return cmdProjectStitch(paths, spec, o);
    if (sub == "contact") return cmdProjectContact(spec);
    return fail("usage: project new|render|stitch|status|contact NAME");
}

// ---------------------------------------------------------------- misc

int cmdContact(const Options &o) {
    const QString file = o.positional.value(1);
    if (file.isEmpty()) return fail("usage: contact FILE.mp4 [--out FILE.png]");
    const QString png = o.value("out", QFileInfo(file).absolutePath() + "/" + QFileInfo(file).completeBaseName() + "_contact.png");
    QString e;
    if (!Media::contactSheet(file, png, &e)) return fail(e);
    return succeed({{"file", png}}, png);
}

int cmdProbe(const Options &o) {
    const Media::ProbeResult p = Media::probe(o.positional.value(1));
    if (!p.ok) return fail(p.error);
    return succeed(p.toJson(), QString("%1x%2, %3 fps, %4 frames, %5 s, audio %6")
                                   .arg(p.width).arg(p.height).arg(p.fps).arg(p.frames).arg(p.duration, 0, 'f', 3)
                                   .arg(p.hasAudio ? QString("%1 %2 Hz %3 ch").arg(p.audioCodec).arg(p.audioRate).arg(p.audioChannels) : "none"));
}

int cmdGallery(const AppPaths &paths) {
    QJsonArray items;
    QString human;
    auto add = [&](const QString &file, const QString &kind, const QString &project) {
        QFileInfo fi(file);
        items.append(QJsonObject{{"file", fi.absoluteFilePath()}, {"kind", kind}, {"project", project},
                                 {"modified", fi.lastModified().toString(Qt::ISODate)}, {"bytes", fi.size()}});
        human += QString("%1  %2\n").arg(kind, -6).arg(fi.absoluteFilePath());
    };
    for (const QString &f : QDir(paths.outputDir + "/gen").entryList({"*.mp4"}, QDir::Files, QDir::Time))
        add(paths.outputDir + "/gen/" + f, "clip", {});
    for (const QString &p : QDir(paths.projectsDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString d = paths.projectsDir + "/" + p;
        for (const QString &f : QDir(d).entryList({"video_*.mp4"}, QDir::Files, QDir::Name)) add(d + "/" + f, "video", p);
        for (const QString &f : QDir(d + "/clips").entryList({"*.mp4"}, QDir::Files, QDir::Name)) add(d + "/clips/" + f, "shot", p);
    }
    return succeed({{"items", items}}, human.isEmpty() ? "nothing generated yet" : human);
}

}  // namespace

namespace Cli {

QString usage() {
    return QStringLiteral(
        "omni-video-generator - local MiniMax H3 video generation through ComfyUI\n\n"
        "  open                                  the window (default)\n"
        "  comfy start|stop|status               manage the local ComfyUI server\n"
        "  gen --mode t2v|i2v|r2v --prompt TEXT [--ref IMG]... [--width W --height H]\n"
        "      [--seconds S] [--seed N] [--steps N] [--no-audio] [--out FILE] [--keep-comfy]\n"
        "  project new|render|stitch|status|contact NAME\n"
        "      render: [--shot N [--new-seed]] [--force] [--keep-comfy]\n"
        "  project list                          list projects\n"
        "  gallery                               list generated clips and videos\n"
        "  contact FILE.mp4 [--out PNG]          start/middle/end frame strip\n"
        "  probe FILE.mp4                        size, fps, frames, duration, audio\n\n"
        "  --json            one JSON object on stdout\n"
        "  --progress-json   JSON event lines on stdout\n\n"
        "Every GPU job holds $XDG_RUNTIME_DIR/omni-gpu.lock (shared with other apps).\n"
        "MiniMax H3 weights: MiniMax H3 Community License (not valid in the US, EU, UK, South Korea).\n");
}

bool isCommand(const QStringList &args) {
    static const QSet<QString> commands{"comfy", "gen", "project", "gallery", "contact", "probe", "help", "--help", "-h", "--version"};
    return args.size() > 1 && commands.contains(args[1]);
}

int run(const QStringList &argv) {
    Options o;
    QString e;
    if (!parse(argv.mid(1), &o, &e)) return fail(e);
    g_json = o.has("json");
    g_progress = o.has("progress-json");
    const QString cmd = o.positional.value(0);
    const AppPaths paths = AppPaths::fromEnvironment();
    if (cmd == "help" || o.has("help")) { out << usage(); return 0; }
    if (o.has("version")) { out << "omni-video-generator " << appVersion() << " (" << appGitCommit() << ")\n"; return 0; }
    QString missing;
    if (!Media::toolsAvailable(&missing) && cmd != "comfy") return fail("missing tools: " + missing);
    if (cmd == "comfy") return cmdComfy(paths, o);
    if (cmd == "gen") return cmdGen(paths, o);
    if (cmd == "project") return cmdProject(paths, o);
    if (cmd == "gallery") return cmdGallery(paths);
    if (cmd == "contact") return cmdContact(o);
    if (cmd == "probe") return cmdProbe(o);
    err << usage();
    return 1;
}

}  // namespace Cli
