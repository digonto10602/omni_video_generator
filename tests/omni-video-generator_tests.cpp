#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include "gpulock.h"
#include "h3.h"
#include "media.h"
#include "project.h"
#include "theme.h"
#include "translator.h"
#include "wsreader.h"

class OmniVideoGeneratorTests : public QObject {
    Q_OBJECT

private slots:
    // ---- size and length snapping (h3_api.py / nodes_minimax_h3.py rules)

    void snapsSizesToMultiplesOf32() {
        QCOMPARE(H3::snapSize(608), 608);
        QCOMPARE(H3::snapSize(600), 608);
        QCOMPARE(H3::snapSize(591), 576);
        QCOMPARE(H3::snapSize(1344), 1344);
        QCOMPARE(H3::snapSize(1), 32);
        QCOMPARE(H3::snapSize(0), 32);
        QCOMPARE(H3::snapSize(1080), 1088);
    }

    void snapsSecondsToThe17kPlus5Grid_data() {
        QTest::addColumn<double>("seconds");
        QTest::addColumn<int>("frames");
        // Expected values computed with Python's round(), as h3_api.py does.
        const QList<QPair<double, int>> table{{0.1, 5}, {0.5, 22}, {0.9375, 22}, {1, 39}, {1.5, 39}, {2, 56},
                                              {2.5, 73}, {3, 73}, {4, 107}, {5, 124}, {6, 158}, {7, 175},
                                              {8, 192}, {9, 226}, {10, 243}, {12, 294}, {15, 362}};
        for (const auto &[s, f] : table)
            QTest::newRow(qPrintable(QString::number(s))) << s << f;
    }
    void snapsSecondsToThe17kPlus5Grid() {
        QFETCH(double, seconds);
        QFETCH(int, frames);
        const int got = H3::framesForSeconds(seconds);
        QCOMPARE(got, frames);
        QCOMPARE(got % 17, 5);
    }

    void alignFramesMatchesTheNode() {
        QCOMPARE(H3::alignFrames(1), 5);
        QCOMPARE(H3::alignFrames(73), 73);
        QCOMPARE(H3::alignFrames(74), 90);
        QCOMPARE(H3::secondsForFrames(124), 124.0 / 24.0);
    }

    // ---- workflow JSON

    void r2vMatchesTheProvenH3ApiGraph() {
        // The fixture is the graph h3_api.py posted for these exact settings.
        QFile f(QStringLiteral(FIXTURES_DIR) + "/h3_api_reference.json");
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject reference = QJsonDocument::fromJson(f.readAll()).object();
        QVERIFY(!reference.isEmpty());

        H3::GenParams p;
        p.mode = H3::Mode::R2V;
        p.prompt = "A red ball rolls slowly across a wooden table. <Picture 1> is the ball.";
        p.width = 608;
        p.height = 352;
        p.seconds = 2;
        p.steps = 8;
        p.seed = 42;
        p.prefix = "test/fixture";
        p.images = {"example_ref_0.png", "example_ref_1.png"};
        QString error;
        const QJsonObject built = WorkflowBuilder(QStringLiteral(TEMPLATES_DIR)).build(p, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        if (built != reference) {
            qWarning().noquote() << QJsonDocument(built).toJson();
        }
        QCOMPARE(built, reference);
    }

    void compiledInTemplatesMatchTheFiles() {
        for (H3::Mode m : {H3::Mode::T2V, H3::Mode::I2V, H3::Mode::R2V}) {
            QString src;
            const QJsonObject fromFile = WorkflowBuilder(QStringLiteral(TEMPLATES_DIR)).loadTemplate(m, &src);
            QVERIFY(src.startsWith(QStringLiteral(TEMPLATES_DIR)));
            const QJsonObject fromQrc = WorkflowBuilder().loadTemplate(m, &src);
            QCOMPARE(src, ":/templates/" + H3::modeName(m) + ".json");
            QCOMPARE(fromFile, fromQrc);
        }
    }

    void t2vUsesTheFl2vaGraphWithoutImages() {
        H3::GenParams p;
        p.mode = H3::Mode::T2V;
        p.prompt = "A lighthouse at dusk.";
        p.seed = 7;
        QString error;
        const QJsonObject g = WorkflowBuilder(QStringLiteral(TEMPLATES_DIR)).build(p, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!g.contains("_meta"));
        QCOMPARE(g["5"].toObject()["class_type"].toString(), QString("MiniMaxH3ImageToVideo"));
        QCOMPARE(g["1"].toObject()["inputs"].toObject()["unet_name"].toString(),
                 QString("minimax_h3_fl2va_pruned_int8_convrot.safetensors"));
        QCOMPARE(g["8"].toObject()["inputs"].toObject()["scheduler"].toString(), QString("simple"));
        QCOMPARE(g["5"].toObject()["inputs"].toObject()["length"].toInt(), 73);
        QCOMPARE(g["6"].toObject()["inputs"].toObject()["noise_seed"].toInteger(), 7);
        QVERIFY(!g.contains("r0"));
        // t2v refuses images
        p.images = {"x.png"};
        QVERIFY(WorkflowBuilder(QStringLiteral(TEMPLATES_DIR)).build(p, &error).isEmpty());
        QVERIFY(error.contains("0 to 0"));
    }

    void i2vWiresTheImageAsFirstFrame() {
        H3::GenParams p;
        p.mode = H3::Mode::I2V;
        p.prompt = "<Picture 1> comes to life.";
        p.images = {"start.png"};
        QString error;
        const QJsonObject g = WorkflowBuilder(QStringLiteral(TEMPLATES_DIR)).build(p, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(g["5"].toObject()["inputs"].toObject()["first_frame"].toArray(), (QJsonArray{"r0", 0}));
        QCOMPARE(g["r0"].toObject()["inputs"].toObject()["image"].toString(), QString("start.png"));
        p.images.clear();
        QVERIFY(WorkflowBuilder(QStringLiteral(TEMPLATES_DIR)).build(p, &error).isEmpty());
    }

    void audioOffDropsTheAudioDecode() {
        for (H3::Mode m : {H3::Mode::T2V, H3::Mode::R2V}) {
            H3::GenParams p;
            p.mode = m;
            p.prompt = "Silent film.";
            p.audio = false;
            QString error;
            const QJsonObject g = WorkflowBuilder(QStringLiteral(TEMPLATES_DIR)).build(p, &error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QVERIFY(!g.contains("12"));
            QVERIFY(!g["13"].toObject()["inputs"].toObject().contains("audio"));
            // r2v's conditioning node still needs the audio VAE.
            QCOMPARE(g.contains("4"), m == H3::Mode::R2V);
        }
    }

    void rejectsBadSizesAndEmptyPrompts() {
        H3::GenParams p;
        p.prompt = "x";
        p.width = 600;
        QString error;
        QVERIFY(WorkflowBuilder(QStringLiteral(TEMPLATES_DIR)).build(p, &error).isEmpty());
        p.width = 608;
        p.prompt = "  ";
        QVERIFY(WorkflowBuilder(QStringLiteral(TEMPLATES_DIR)).build(p, &error).isEmpty());
    }

    void reportsModelFiles() {
        H3::GenParams p;
        p.mode = H3::Mode::R2V;
        p.prompt = "x";
        const QJsonObject models = WorkflowBuilder::modelFiles(WorkflowBuilder(QStringLiteral(TEMPLATES_DIR)).build(p));
        QCOMPARE(models["diffusion_model"].toString(), QString("minimax_h3_ref2va_pruned_int8_convrot.safetensors"));
        QCOMPARE(models["text_encoder"].toString(), QString("qwen3vl_32b_minimax_h3_nvfp4_awq.safetensors"));
        QCOMPARE(models["vae"].toArray().size(), 2);
    }

    // ---- projects, manifests, next unused N

    void nextVideoNumberNeverReuses() {
        QTemporaryDir dir;
        QCOMPARE(Manifest::nextVideoNumber(dir.path()), 1);
        touch(dir.filePath("video_1.mp4"));
        QCOMPARE(Manifest::nextVideoNumber(dir.path()), 2);
        touch(dir.filePath("video_3_manifest.json"));  // manifest alone still claims 3
        touch(dir.filePath("video_x.mp4"));
        touch(dir.filePath("video_9.mov"));
        touch(dir.filePath("my_video_7.mp4"));
        QCOMPARE(Manifest::nextVideoNumber(dir.path()), 4);
        touch(dir.filePath("video_12.mp4"));
        QCOMPARE(Manifest::nextVideoNumber(dir.path()), 13);
    }

    void manifestWritesValidJsonAndNeverOverwrites() {
        QTemporaryDir dir;
        const QString path = dir.filePath("video_1_manifest.json");
        const QJsonObject m{{"title", "Test"}, {"clips", QJsonArray{QJsonObject{{"seed", 1001}, {"prompt", "x"}}}},
                            {"app", QJsonObject{{"git_commit", "abc123"}}}};
        QString error;
        QVERIFY(Manifest::write(path, m, &error));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(f.readAll()).object(), m);
        f.close();
        QVERIFY(!Manifest::write(path, QJsonObject{{"title", "Other"}}, &error));
        QVERIFY(error.contains("never overwritten"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(f.readAll()).object()["title"].toString(), QString("Test"));
    }

    void projectStarterLoads() {
        QTemporaryDir dir;
        const QString pdir = dir.filePath("demo");
        QString error;
        QVERIFY2(ProjectSpec::create(pdir, "demo", &error), qPrintable(error));
        QVERIFY(!ProjectSpec::create(pdir, "demo", &error));  // never clobbers
        ProjectSpec spec;
        QVERIFY2(ProjectSpec::load(pdir, &spec, &error), qPrintable(error));
        QCOMPARE(spec.shots.size(), 2);
        QCOMPARE(spec.shots[0].seed, qint64(1001));
        QCOMPARE(H3::framesForSeconds(spec.shots[0].seconds), 56);
        QVERIFY(!ProjectSpec::validName("../evil"));
        QVERIFY(!ProjectSpec::validName("a b"));
    }

    void projectSubstitutesRefsLikeRenderShots() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("refs");
        touch(dir.filePath("refs/a.png"));
        touch(dir.filePath("refs/b.png"));
        write(dir.filePath("shots.json"), R"({"mode":"r2v","style":"Grainy film.","refs":{"HERO":"refs/a.png","CAR":"refs/b.png"},
            "shots":[{"id":3,"refs":["CAR","HERO"],"seconds":5,"prompt":"{HERO} drives {CAR}."}]})");
        ProjectSpec spec;
        QString error;
        QVERIFY2(ProjectSpec::load(dir.path(), &spec, &error), qPrintable(error));
        const ShotSpec &s = spec.shots[0];
        QCOMPARE(s.seed, qint64(1003));  // default seed 1000 + id, as render_shots.py
        QCOMPARE(spec.finalPrompt(s), QString("<Picture 2> drives <Picture 1>. Grainy film."));
        QCOMPARE(spec.imagePaths(s), (QStringList{dir.filePath("refs/b.png"), dir.filePath("refs/a.png")}));
        QVERIFY(spec.paramsHash(s, 1) != spec.paramsHash(s, 2));

        // State: not rendered, then rendered, then stale after a seed change.
        QJsonObject state;
        QVERIFY(!spec.shotUpToDate(s, state));
        touch(dir.filePath("clips_shot.mp4"));
        state["shots"] = QJsonObject{{"3", QJsonObject{{"file", "clips_shot.mp4"}, {"hash", spec.paramsHash(s, 1003)}}}};
        QVERIFY(spec.shotUpToDate(s, state));
        QJsonObject st = state["shots"].toObject()["3"].toObject();
        st["seed_override"] = 77;
        state["shots"] = QJsonObject{{"3", st}};
        QCOMPARE(spec.effectiveSeed(s, state), qint64(77));
        QVERIFY(!spec.shotUpToDate(s, state));
    }

    void projectRejectsBadShots() {
        QTemporaryDir dir;
        QString error;
        ProjectSpec spec;
        write(dir.filePath("shots.json"), R"({"shots":[{"id":1,"prompt":"x","refs":["NOPE"]}]})");
        QVERIFY(!ProjectSpec::load(dir.path(), &spec, &error));
        QVERIFY(error.contains("unknown ref"));
        write(dir.filePath("shots.json"), R"({"shots":[{"id":1,"prompt":"x","mode":"i2v"}]})");
        QVERIFY(!ProjectSpec::load(dir.path(), &spec, &error));
        QVERIFY(error.contains("exactly one image"));
        write(dir.filePath("shots.json"), R"({"shots":[{"id":1,"prompt":"x"},{"id":1,"prompt":"y"}]})");
        QVERIFY(!ProjectSpec::load(dir.path(), &spec, &error));
    }

    void stitchArgumentsNormaliseAndConcat() {
        const QStringList a = Media::stitchArguments({"a.mp4", "b.mp4"}, 608, 352, true, "out.mp4", "AI");
        const QString graph = a[a.indexOf("-filter_complex") + 1];
        QVERIFY(graph.contains("concat=n=2:v=1:a=1[v][a]"));
        QVERIFY(graph.contains("scale=608:352:force_original_aspect_ratio=increase,crop=608:352,fps=24"));
        QVERIFY(graph.contains("aresample=48000"));
        QVERIFY(a.contains("-n"));  // ffmpeg never overwrites
        const QStringList silent = Media::stitchArguments({"a.mp4"}, 352, 608, false, "o.mp4", "AI");
        QVERIFY(!silent.contains("[a]"));
        QVERIFY(silent[silent.indexOf("-filter_complex") + 1].contains("concat=n=1:v=1:a=0[v]"));
    }

    // ---- GPU lock interoperates with flock(1)

    void gpuLockExcludesOthers() {
        QTemporaryDir dir;
        const QString path = dir.filePath("omni-gpu.lock");
        GpuLock a(path), b(path);
        QVERIFY(a.tryLock());
        QVERIFY(!b.tryLock());
        QVERIFY(GpuLock::isBusy(path));
        // The shell tool the other app uses sees our lock too.
        QCOMPARE(QProcess::execute("flock", {"-n", path, "true"}), 1);
        a.unlock();
        QVERIFY(!GpuLock::isBusy(path));
        QVERIFY(b.tryLock());
        b.unlock();
        QCOMPARE(QProcess::execute("flock", {"-n", path, "true"}), 0);
    }

    // ---- WebSocket frame parsing

    void parsesWebSocketFrames() {
        QByteArray buf;
        buf += frame(0x1, "{\"type\":\"status\"}", true);
        const QByteArray big(300, 'x');
        buf += frame(0x1, big, true);
        buf += frame(0x2, "bin", true);
        QByteArray partial = frame(0x1, "tail", true);
        buf += partial.left(3);
        auto frames = WsReader::parseFrames(buf);
        QCOMPARE(frames.size(), 3);
        QCOMPARE(frames[0].payload, QByteArray("{\"type\":\"status\"}"));
        QCOMPARE(frames[1].payload, big);
        QCOMPARE(frames[2].opcode, 0x2);
        QCOMPARE(buf.size(), 3);  // incomplete frame stays buffered
        buf += partial.mid(3);
        frames = WsReader::parseFrames(buf);
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames[0].payload, QByteArray("tail"));
        // Fragment marker
        buf = frame(0x1, "ab", false);
        frames = WsReader::parseFrames(buf);
        QCOMPARE(frames[0].opcode, 0x101);
    }

    // ---- theme and translator (Omarchy template)

    void themeFallsBack() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n");
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void translatorFallsBackToEnglish() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

private:
    static void touch(const QString &path) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
    }
    static void write(const QString &path, const QByteArray &contents) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(contents);
    }
    static QByteArray frame(int opcode, const QByteArray &payload, bool fin) {
        QByteArray f;
        f.append(char((fin ? 0x80 : 0) | opcode));
        if (payload.size() < 126) {
            f.append(char(payload.size()));
        } else {
            f.append(char(126));
            f.append(char(payload.size() >> 8));
            f.append(char(payload.size() & 0xff));
        }
        return f + payload;
    }
};

QTEST_MAIN(OmniVideoGeneratorTests)
#include "omni-video-generator_tests.moc"
