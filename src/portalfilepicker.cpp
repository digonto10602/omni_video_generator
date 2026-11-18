#include "portalfilepicker.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QRandomGenerator>
#include <QUrl>

namespace {
// The portal's filter type a(sa(us)): a name and (kind, pattern) pairs.
struct FilterRule { uint kind; QString pattern; };
struct Filter { QString name; QList<FilterRule> rules; };

QDBusArgument &operator<<(QDBusArgument &a, const FilterRule &r) {
    a.beginStructure(); a << r.kind << r.pattern; a.endStructure(); return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, FilterRule &r) {
    a.beginStructure(); a >> r.kind >> r.pattern; a.endStructure(); return a;
}
QDBusArgument &operator<<(QDBusArgument &a, const Filter &f) {
    a.beginStructure(); a << f.name << f.rules; a.endStructure(); return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, Filter &f) {
    a.beginStructure(); a >> f.name >> f.rules; a.endStructure(); return a;
}
}  // namespace
Q_DECLARE_METATYPE(FilterRule)
Q_DECLARE_METATYPE(Filter)

void PortalFilePicker::pickImages(const QString &title, bool multiple) {
    qDBusRegisterMetaType<FilterRule>();
    qDBusRegisterMetaType<QList<FilterRule>>();
    qDBusRegisterMetaType<Filter>();
    qDBusRegisterMetaType<QList<Filter>>();

    QDBusConnection bus = QDBusConnection::sessionBus();
    const QString token = QString("ovg%1").arg(QRandomGenerator::global()->generate());
    QString sender = bus.baseService().mid(1);
    sender.replace('.', '_');
    m_requestPath = QString("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
    // Subscribe before calling, so a fast response is not missed.
    bus.connect("org.freedesktop.portal.Desktop", m_requestPath, "org.freedesktop.portal.Request", "Response",
                this, SLOT(onResponse(uint, QVariantMap)));

    const QList<Filter> filters{{"Images", {{1, "image/png"}, {1, "image/jpeg"}, {1, "image/webp"}}}};
    QVariantMap options{{"handle_token", token}, {"multiple", multiple},
                        {"filters", QVariant::fromValue(filters)}};
    QDBusMessage call = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                                       "org.freedesktop.portal.FileChooser", "OpenFile");
    call << QString() << title << options;
    QDBusReply<QDBusObjectPath> reply = bus.call(call);
    if (!reply.isValid()) {
        emit picked({});
        return;
    }
    if (reply.value().path() != m_requestPath) {
        bus.disconnect("org.freedesktop.portal.Desktop", m_requestPath, "org.freedesktop.portal.Request", "Response",
                       this, SLOT(onResponse(uint, QVariantMap)));
        m_requestPath = reply.value().path();
        bus.connect("org.freedesktop.portal.Desktop", m_requestPath, "org.freedesktop.portal.Request", "Response",
                    this, SLOT(onResponse(uint, QVariantMap)));
    }
}

void PortalFilePicker::onResponse(uint response, const QVariantMap &results) {
    QDBusConnection::sessionBus().disconnect("org.freedesktop.portal.Desktop", m_requestPath,
                                             "org.freedesktop.portal.Request", "Response", this,
                                             SLOT(onResponse(uint, QVariantMap)));
    QStringList paths;
    if (response == 0)
        for (const QString &uri : results.value("uris").toStringList())
            paths << QUrl(uri).toLocalFile();
    emit picked(paths);
}
