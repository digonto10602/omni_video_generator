#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

// MiniMax H3 rules, taken from ComfyUI's comfy_extras/nodes_minimax_h3.py and
// the h3_api.py graph proven in an earlier local project.
namespace H3 {
constexpr int Fps = 24;
constexpr int SizeMultiple = 32;
constexpr int NativeShortEdge = 768;
constexpr int MaxPixels = 768 * 1344;  // the node's native canvas cap

// Nearest multiple of 32, never below 32.
int snapSize(double value);
// Frame count for a duration: round(seconds*24), at least 5, then up to the
// model's 17k+5 grid (5, 22, 39, 56, 73, ...). Same arithmetic as h3_api.py,
// including Python's round-half-to-even.
int framesForSeconds(double seconds);
// Up to the 17k+5 grid, like the node's align_frame_count().
int alignFrames(int frames);
double secondsForFrames(int frames);

enum class Mode { T2V, I2V, R2V };
QString modeName(Mode mode);
bool parseMode(const QString &name, Mode *mode);

struct Preset {
    QString label;
    int width;
    int height;
};
QList<Preset> presets();

struct GenParams {
    Mode mode = Mode::T2V;
    QString prompt;
    int width = 608;
    int height = 352;
    double seconds = 3.0;
    int steps = 20;
    qint64 seed = 0;
    bool audio = true;
    QString prefix = "gen/omni";
    // Image names as ComfyUI's LoadImage sees them (already uploaded). For
    // i2v the first one is the first frame; for r2v they are <Picture 1..9>.
    QStringList images;
};

qint64 randomSeed();
}  // namespace H3

// Builds API-format workflow JSON from the template files. Templates are
// plain ComfyUI API graphs with {{placeholders}} plus a "_meta" object that
// says where reference images plug in and which nodes only exist for audio.
class WorkflowBuilder {
public:
    // Looks in templatesDir first (editable without recompiling), then in the
    // copies compiled into the binary.
    explicit WorkflowBuilder(const QString &templatesDir = {});

    QJsonObject loadTemplate(H3::Mode mode, QString *source = nullptr, QString *error = nullptr) const;
    // Returns the graph ready for POST /prompt, or an empty object with error set.
    QJsonObject build(const H3::GenParams &params, QString *error = nullptr) const;
    // Template meta (description, max_refs, sampler...) for the UI.
    QJsonObject meta(H3::Mode mode) const;

    // Model files named by loader nodes: {"unet": ..., "clip": ..., "vae": [...]}.
    static QJsonObject modelFiles(const QJsonObject &graph);
    static QString templateSha256(const QJsonObject &tmpl);

private:
    QString m_dir;
};
