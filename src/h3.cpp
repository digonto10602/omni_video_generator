#include "h3.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <cmath>

namespace H3 {

int snapSize(double value) {
    const int snapped = int(std::lround(value / SizeMultiple)) * SizeMultiple;
    return std::max(SizeMultiple, snapped);
}

int alignFrames(int frames) {
    int n = std::max(5, frames);
    while (n % 17 != 5)
        ++n;
    return n;
}

int framesForSeconds(double seconds) {
    // std::nearbyint uses the current rounding mode, round-half-to-even by
    // default, which is what Python's round() does in h3_api.py.
    const int n = std::max(5, int(std::nearbyint(seconds * Fps)));
    return alignFrames(n);
}

double secondsForFrames(int frames) { return double(frames) / Fps; }

QString modeName(Mode mode) {
    switch (mode) {
    case Mode::T2V: return "t2v";
    case Mode::I2V: return "i2v";
    case Mode::R2V: return "r2v";
    }
    return "t2v";
}

bool parseMode(const QString &name, Mode *mode) {
    const QString n = name.trimmed().toLower();
    if (n == "t2v") { *mode = Mode::T2V; return true; }
    if (n == "i2v") { *mode = Mode::I2V; return true; }
    if (n == "r2v") { *mode = Mode::R2V; return true; }
    return false;
}

QList<Preset> presets() {
    // 608x352 / 352x608 are the fast drafts proven by the hellfire project;
    // 1344x768 is H3's native canvas cap and was run on this 8 GB card.
    return {
        {"608 x 352  draft 16:9", 608, 352},
        {"352 x 608  draft 9:16", 352, 608},
        {"864 x 480  0.4 MP 16:9", 864, 480},
        {"480 x 864  0.4 MP 9:16", 480, 864},
        {"768 x 768  square", 768, 768},
        {"1344 x 768  native max 16:9", 1344, 768},
        {"768 x 1344  native max 9:16", 768, 1344},
    };
}

qint64 randomSeed() { return qint64(QRandomGenerator::global()->bounded(1, 2147483647)); }

}  // namespace H3

WorkflowBuilder::WorkflowBuilder(const QString &templatesDir) : m_dir(templatesDir) {}

QJsonObject WorkflowBuilder::loadTemplate(H3::Mode mode, QString *source, QString *error) const {
    const QString name = H3::modeName(mode) + ".json";
    QStringList candidates;
    if (!m_dir.isEmpty())
        candidates << m_dir + "/" + name;
    candidates << ":/templates/" + name;
    for (const QString &path : candidates) {
        QFile f(path);
        if (!f.exists() || !f.open(QIODevice::ReadOnly))
            continue;
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
        if (!doc.isObject()) {
            if (error) *error = QString("template %1 is not valid JSON: %2").arg(path, pe.errorString());
            return {};
        }
        if (source) *source = path;
        return doc.object();
    }
    if (error) *error = QString("no workflow template %1").arg(name);
    return {};
}

QJsonObject WorkflowBuilder::meta(H3::Mode mode) const {
    return loadTemplate(mode).value("_meta").toObject();
}

namespace {
// A string that is exactly "{{key}}" becomes the typed value; a string that
// merely contains placeholders gets text substitution.
QJsonValue fill(const QJsonValue &value, const QJsonObject &vars) {
    if (value.isString()) {
        const QString s = value.toString();
        static const QRegularExpression whole(R"(^\{\{(\w+)\}\}$)");
        const auto m = whole.match(s);
        if (m.hasMatch() && vars.contains(m.captured(1)))
            return vars.value(m.captured(1));
        if (!s.contains("{{"))
            return value;
        QString out = s;
        for (auto it = vars.begin(); it != vars.end(); ++it)
            out.replace("{{" + it.key() + "}}", it.value().isString() ? it.value().toString()
                                                                    : it.value().toVariant().toString());
        return out;
    }
    if (value.isObject()) {
        QJsonObject o = value.toObject();
        for (auto it = o.begin(); it != o.end(); ++it)
            it.value() = fill(it.value(), vars);
        return o;
    }
    if (value.isArray()) {
        QJsonArray a = value.toArray();
        for (int i = 0; i < a.size(); ++i)
            a[i] = fill(a[i], vars);
        return a;
    }
    return value;
}
}  // namespace

QJsonObject WorkflowBuilder::build(const H3::GenParams &p, QString *error) const {
    QString err;
    QJsonObject tmpl = loadTemplate(p.mode, nullptr, &err);
    if (tmpl.isEmpty()) {
        if (error) *error = err;
        return {};
    }
    const QJsonObject meta = tmpl.take("_meta").toObject();
    const int maxRefs = meta.value("max_refs").toInt(0);
    const int minRefs = meta.value("min_refs").toInt(0);
    if (p.images.size() > maxRefs || p.images.size() < minRefs) {
        if (error)
            *error = QString("%1 takes %2 to %3 images, got %4")
                         .arg(H3::modeName(p.mode)).arg(minRefs).arg(maxRefs).arg(p.images.size());
        return {};
    }
    if (p.width % H3::SizeMultiple || p.height % H3::SizeMultiple || p.width <= 0 || p.height <= 0) {
        if (error) *error = "width and height must be positive multiples of 32";
        return {};
    }
    if (p.prompt.trimmed().isEmpty()) {
        if (error) *error = "the prompt is empty";
        return {};
    }

    QJsonObject vars;
    vars["prompt"] = p.prompt;
    vars["width"] = p.width;
    vars["height"] = p.height;
    vars["length"] = H3::framesForSeconds(p.seconds);
    vars["steps"] = p.steps;
    vars["seed"] = p.seed;
    vars["prefix"] = p.prefix;
    QJsonObject graph = fill(tmpl, vars).toObject();

    // Reference images: a LoadImage node r<i> per image, wired into the
    // conditioning node under the template's input name.
    const QString condId = meta.value("conditioning_node").toString();
    const QString pattern = meta.value("ref_input").toString();
    if (!p.images.isEmpty()) {
        QJsonObject cond = graph.value(condId).toObject();
        QJsonObject inputs = cond.value("inputs").toObject();
        for (int i = 0; i < p.images.size(); ++i) {
            const QString id = QString("r%1").arg(i);
            graph[id] = QJsonObject{{"class_type", "LoadImage"},
                                    {"inputs", QJsonObject{{"image", p.images[i]}}}};
            QString input = pattern;
            input.replace("{i}", QString::number(i));
            inputs[input] = QJsonArray{id, 0};
        }
        cond["inputs"] = inputs;
        graph[condId] = cond;
    }

    // Audio off: drop the audio decode branch and the video's audio input.
    // The model still generates the audio latent jointly; it is just not
    // decoded or saved.
    if (!p.audio) {
        for (const auto &id : meta.value("audio_nodes").toArray())
            graph.remove(id.toString());
        const QString videoId = meta.value("video_node").toString();
        QJsonObject video = graph.value(videoId).toObject();
        QJsonObject inputs = video.value("inputs").toObject();
        inputs.remove("audio");
        video["inputs"] = inputs;
        graph[videoId] = video;
    }
    return graph;
}

QJsonObject WorkflowBuilder::modelFiles(const QJsonObject &graph) {
    QJsonObject out;
    QJsonArray vaes;
    for (auto it = graph.begin(); it != graph.end(); ++it) {
        const QJsonObject node = it.value().toObject();
        const QString cls = node.value("class_type").toString();
        const QJsonObject in = node.value("inputs").toObject();
        if (cls == "UNETLoader") out["diffusion_model"] = in.value("unet_name");
        else if (cls == "CLIPLoader") out["text_encoder"] = in.value("clip_name");
        else if (cls == "VAELoader") vaes.append(in.value("vae_name"));
    }
    out["vae"] = vaes;
    return out;
}

QString WorkflowBuilder::templateSha256(const QJsonObject &tmpl) {
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(tmpl).toJson(QJsonDocument::Compact),
                                                        QCryptographicHash::Sha256).toHex());
}
