// omni-video-generator — local MiniMax H3 video generation through ComfyUI.

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>

#include "backend.h"
#include "cli.h"
#include "paths.h"
#include "portalfilepicker.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    QStringList args;
    for (int i = 0; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);

    // Commands run without a display, so agents and scripts can drive the app.
    if (Cli::isCommand(args)) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QGuiApplication app(argc, argv);
        app.setApplicationName("omni-video-generator");
        return Cli::run(args);
    }
    if (args.size() > 1 && args[1] != "open") {
        fprintf(stderr, "%s", qPrintable(Cli::usage()));
        return 1;
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("omni-video-generator");
    app.setApplicationVersion(appVersion());
    // Matches the window to omni-video-generator.desktop (Wayland app id).
    app.setDesktopFileName("omni-video-generator");
    app.setWindowIcon(QIcon::fromTheme("omni-video-generator"));
    QQuickStyle::setStyle("Material");

    Translator translator;
    app.installTranslator(&translator);
    Theme theme;
    PortalFilePicker picker;
    Backend backend(AppPaths::fromEnvironment(), &picker);

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    return app.exec();
}
