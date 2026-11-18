QT += core gui qml quick quickcontrols2 dbus network multimedia

CONFIG += c++17 release
TARGET = omni-video-generator
TEMPLATE = app

# The git commit is compiled in so every manifest can name the exact app code
# that produced a video.
GIT_COMMIT = $$system(git -C $$PWD rev-parse --short HEAD 2>/dev/null)
isEmpty(GIT_COMMIT): GIT_COMMIT = unknown
GIT_DIRTY = $$system(git -C $$PWD status --porcelain --untracked-files=no 2>/dev/null | wc -l)
!equals(GIT_DIRTY, 0): GIT_COMMIT = $${GIT_COMMIT}-dirty
DEFINES += APP_GIT_COMMIT=\\\"$$GIT_COMMIT\\\" APP_VERSION=\\\"0.1.0\\\"

include(src/core.pri)

HEADERS += \
    src/backend.h \
    src/cli.h \
    src/filepicker.h \
    src/portalfilepicker.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/cli.cpp \
    src/portalfilepicker.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
