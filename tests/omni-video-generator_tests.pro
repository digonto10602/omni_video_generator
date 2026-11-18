QT += core gui quick quickcontrols2 testlib dbus network
CONFIG += c++17 testcase
TARGET = omni-video-generator_tests
TEMPLATE = app
DEFINES += FIXTURES_DIR=\\\"$$PWD/fixtures\\\" TEMPLATES_DIR=\\\"$$PWD/../templates\\\"

include(../src/core.pri)
HEADERS += ../src/theme.h ../src/translator.h
SOURCES += omni-video-generator_tests.cpp ../src/theme.cpp ../src/translator.cpp
RESOURCES += tests.qrc
