#pragma once

#include "filepicker.h"

#include <QDBusObjectPath>
#include <QVariantMap>

// Opens images through xdg-desktop-portal's FileChooser over D-Bus, so the
// dialog is the desktop's own and sandboxes work.
class PortalFilePicker : public FilePicker {
    Q_OBJECT
public:
    using FilePicker::FilePicker;
    void pickImages(const QString &title, bool multiple) override;

private slots:
    void onResponse(uint response, const QVariantMap &results);

private:
    QString m_requestPath;
};
