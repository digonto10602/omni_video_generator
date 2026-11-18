# Logic shared by the app and the Qt Test binary. Everything here works
# without a display.
INCLUDEPATH += $$PWD
HEADERS += \
    $$PWD/paths.h \
    $$PWD/h3.h \
    $$PWD/gpulock.h \
    $$PWD/wsreader.h \
    $$PWD/comfy.h \
    $$PWD/media.h \
    $$PWD/project.h \
    $$PWD/runner.h
SOURCES += \
    $$PWD/paths.cpp \
    $$PWD/h3.cpp \
    $$PWD/gpulock.cpp \
    $$PWD/wsreader.cpp \
    $$PWD/comfy.cpp \
    $$PWD/media.cpp \
    $$PWD/project.cpp \
    $$PWD/runner.cpp
