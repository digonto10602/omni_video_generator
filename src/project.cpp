#include "project.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>

bool ProjectSpec::validName(const QString &name) {
    static const QRegularExpression re("^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$");
    return re.match(name).hasMatch();
}

bool ProjectSpec::load(const QString &dir, ProjectSpec *out, QString *error) {
    QFile f(dir + "/shots.json");
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "no shots.json in " + dir;
        return false;
    }
    QJsonParseError pe;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll(), &pe).object();
    if (pe.error != QJsonParseError::NoError) {
        if (error) *error = QString("shots.json: %1 at offset %2").arg(pe.errorString()).arg(pe.offset);
        return false;
    }
    ProjectSpec p;
    p.dir = QDir(dir).absolutePath();
    p.name = QFileInfo(p.dir).fileName();
    p.title = o.value("title").toString(p.name);
    p.mode = o.value("mode").toString("t2v");
    p.style = o.value("style").toString();
    p.width = H3::snapSize(o.value("width").toDouble(608));
    p.height = H3::snapSize(o.value("height").toDouble(352));
    p.steps = o.value("steps").toInt(20);
    p.audio = o.value("audio").toBool(true);
    const QJsonObject refs = o.value("refs").toObject();
    for (auto it = refs.begin(); it != refs.end(); ++it)
        p.refs.insert(it.key(), it.value().toString());
    QSet<int> ids;
    for (const auto &v : o.value("shots").toArray()) {
        const QJsonObject s = v.toObject();
        ShotSpec shot;
        shot.id = s.value("id").toInt();
        shot.mode = s.value("mode").toString(p.mode);
        shot.prompt = s.value("prompt").toString();
        for (const auto &r : s.value("refs").toArray()) shot.refs << r.toString();
        shot.image = s.value("image").toString();
        shot.seconds = s.value("seconds").toDouble(3.0);
        shot.seed = s.contains("seed") ? s.value("seed").toInteger() : 1000 + shot.id;
        H3::Mode m;
        QString problem;
        if (shot.id <= 0 || ids.contains(shot.id)) problem = "needs a unique positive id";
        else if (shot.prompt.trimmed().isEmpty()) problem = "has an empty prompt";
        else if (!H3::parseMode(shot.mode, &m)) problem = "has unknown mode " + shot.mode;
        else if (shot.seconds <= 0 || shot.seconds > 15) problem = "seconds must be between 0 and 15";
        for (const QString &key : shot.refs)
            if (!p.refs.contains(key)) problem = "uses unknown ref " + key;
        if (problem.isEmpty()) {
            const QStringList imgs = p.imagePaths(shot);
            if (m == H3::Mode::I2V && imgs.size() != 1) problem = "is i2v and needs exactly one image";
            if (m == H3::Mode::R2V && imgs.size() > 9) problem = "has more than 9 references";
            for (const QString &img : imgs)
                if (!QFileInfo::exists(img)) problem = "image not found: " + img;
        }
        if (!problem.isEmpty()) {
            if (error) *error = QString("shot %1 %2").arg(shot.id).arg(problem);
            return false;
        }
        ids.insert(shot.id);
        p.shots << shot;
    }
    if (p.shots.isEmpty()) {
        if (error) *error = "shots.json has no shots";
        return false;
    }
    *out = p;
    return true;
}

bool ProjectSpec::create(const QString &dir, const QString &name, QString *error) {
    if (!validName(name)) {
        if (error) *error = "project names use letters, digits, '.', '_' and '-'";
        return false;
    }
    if (QFileInfo::exists(dir)) {
        if (error) *error = dir + " already exists";
        return false;
    }
    if (!QDir().mkpath(dir + "/refs") || !QDir().mkpath(dir + "/clips")) {
        if (error) *error = "cannot create " + dir;
        return false;
    }
    const QJsonObject starter{
        {"title", name},
        {"mode", "t2v"},
        {"width", 608}, {"height", 352}, {"steps", 20}, {"audio", true},
        {"style", "Cinematic, natural light, coherent anatomy, no text, no subtitles, no watermark."},
        {"refs", QJsonObject{}},
        {"shots", QJsonArray{
            QJsonObject{{"id", 1}, {"seconds", 2}, {"seed", 1001},
                        {"prompt", "A paper boat drifts down a rain-filled gutter, camera follows at water level. Sound: steady rain, trickling water."}},
            QJsonObject{{"id", 2}, {"seconds", 2}, {"seed", 1002},
                        {"prompt", "The paper boat reaches a storm drain and spins slowly in a small whirlpool. Sound: rain, a hollow gurgle."}}}},
        {"_help", "mode: t2v | i2v | r2v per project or per shot. refs: {\"KEY\": \"refs/file.png\"}; a shot lists \"refs\": [\"KEY\"] and writes {KEY} in its prompt, which becomes <Picture 1>, <Picture 2>... i2v shots set \"image\": \"refs/first_frame.png\". Seconds snap up to H3's 17k+5 frame grid at 24 fps."}};
    QSaveFile f(dir + "/shots.json");
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(starter).toJson()) < 0 || !f.commit()) {
        if (error) *error = "cannot write shots.json";
        return false;
    }
    return true;
}

const ShotSpec *ProjectSpec::shot(int id) const {
    for (const auto &s : shots)
        if (s.id == id) return &s;
    return nullptr;
}

H3::Mode ProjectSpec::shotMode(const ShotSpec &s) const {
    H3::Mode m = H3::Mode::T2V;
    H3::parseMode(s.mode, &m);
    return m;
}

QString ProjectSpec::finalPrompt(const ShotSpec &s) const {
    QString prompt = s.prompt;
    for (int i = 0; i < s.refs.size(); ++i)
        prompt.replace("{" + s.refs[i] + "}", QString("<Picture %1>").arg(i + 1));
    if (!style.trimmed().isEmpty())
        prompt += " " + style.trimmed();
    return prompt;
}

QStringList ProjectSpec::imagePaths(const ShotSpec &s) const {
    QStringList out;
    auto abs = [this](const QString &rel) { return QDir(dir).absoluteFilePath(rel); };
    if (shotMode(s) == H3::Mode::I2V) {
        if (!s.image.isEmpty()) out << abs(s.image);
        else if (!s.refs.isEmpty()) out << abs(refs.value(s.refs.first()));
    } else if (shotMode(s) == H3::Mode::R2V) {
        for (const QString &key : s.refs) out << abs(refs.value(key));
    }
    return out;
}

QString ProjectSpec::paramsHash(const ShotSpec &s, qint64 seed) const {
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(QStringList{H3::modeName(shotMode(s)), finalPrompt(s), QString::number(H3::framesForSeconds(s.seconds)),
                          QString::number(width), QString::number(height), QString::number(steps),
                          audio ? "audio" : "silent", QString::number(seed)}.join('\x1f').toUtf8());
    for (const QString &img : imagePaths(s)) {
        QFile f(img);
        if (f.open(QIODevice::ReadOnly)) h.addData(&f);
    }
    return QString::fromLatin1(h.result().toHex().left(16));
}

QJsonObject ProjectSpec::loadState() const {
    QFile f(dir + "/state.json");
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

bool ProjectSpec::saveState(const QJsonObject &state, QString *error) const {
    QSaveFile f(dir + "/state.json");
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(state).toJson()) < 0 || !f.commit()) {
        if (error) *error = "cannot write state.json";
        return false;
    }
    return true;
}

qint64 ProjectSpec::effectiveSeed(const ShotSpec &s, const QJsonObject &state) const {
    const QJsonObject st = state.value("shots").toObject().value(QString::number(s.id)).toObject();
    return st.contains("seed_override") ? st.value("seed_override").toInteger() : s.seed;
}

QString ProjectSpec::clipPath(const ShotSpec &s, const QJsonObject &state) const {
    const QString rel = state.value("shots").toObject().value(QString::number(s.id)).toObject().value("file").toString();
    return rel.isEmpty() ? QString() : QDir(dir).absoluteFilePath(rel);
}

bool ProjectSpec::shotUpToDate(const ShotSpec &s, const QJsonObject &state) const {
    const QJsonObject st = state.value("shots").toObject().value(QString::number(s.id)).toObject();
    const QString file = clipPath(s, state);
    return !file.isEmpty() && QFileInfo::exists(file) && st.value("hash").toString() == paramsHash(s, effectiveSeed(s, state));
}

namespace Manifest {

int nextVideoNumber(const QString &dir) {
    static const QRegularExpression re(R"(^video_(\d+)(?:\.mp4|_manifest\.json)$)");
    int highest = 0;
    for (const QString &name : QDir(dir).entryList(QDir::Files)) {
        const auto m = re.match(name);
        if (m.hasMatch()) highest = std::max(highest, m.captured(1).toInt());
    }
    return highest + 1;
}

bool write(const QString &path, const QJsonObject &manifest, QString *error) {
    if (QFileInfo::exists(path)) {
        if (error) *error = path + " already exists; manifests are never overwritten";
        return false;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(manifest).toJson()) < 0 || !f.commit()) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    return true;
}

}  // namespace Manifest
