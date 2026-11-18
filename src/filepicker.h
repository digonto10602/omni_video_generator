#pragma once

#include <QObject>
#include <QStringList>

// Abstract file picker, so tests (and the offscreen harness) can inject a
// fake instead of talking to the desktop portal. Pattern from Omacut.
class FilePicker : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void pickImages(const QString &title, bool multiple) = 0;
signals:
    void picked(const QStringList &paths);
};
