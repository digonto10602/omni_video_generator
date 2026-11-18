#pragma once

#include <QByteArray>
#include <QObject>
#include <QTcpSocket>
#include <QTimer>

// A receive-only WebSocket client (RFC 6455), just enough for ComfyUI's
// /ws progress stream. Written here because the Qt WebSockets module is not
// part of Omarchy's Qt install; this avoids a new system package.
class WsReader : public QObject {
    Q_OBJECT
public:
    explicit WsReader(QObject *parent = nullptr);
    void open(const QString &host, quint16 port, const QString &path);
    void close();
    bool isOpen() const { return m_open; }

    // Parses as many complete frames as `buffer` holds; consumed bytes are
    // removed. Exposed for tests.
    struct Frame { int opcode; QByteArray payload; };
    static QList<Frame> parseFrames(QByteArray &buffer);

signals:
    void textMessage(const QString &message);
    void opened();
    void closed();

private:
    void onReadyRead();
    void sendControl(int opcode, const QByteArray &payload);

    QTcpSocket m_socket;
    QByteArray m_buffer;
    QByteArray m_fragments;
    int m_messageOpcode = 0;
    bool m_handshakeDone = false;
    bool m_open = false;
    QByteArray m_key;
    QString m_path;
    QString m_host;
};
