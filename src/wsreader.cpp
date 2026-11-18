#include "wsreader.h"

#include <QRandomGenerator>

WsReader::WsReader(QObject *parent) : QObject(parent) {
    connect(&m_socket, &QTcpSocket::connected, this, [this] {
        QByteArray key(16, 0);
        for (auto &c : key)
            c = char(QRandomGenerator::global()->bounded(256));
        m_key = key.toBase64();
        const QByteArray req = "GET " + m_path.toUtf8() + " HTTP/1.1\r\nHost: " + m_host.toUtf8() +
                               "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + m_key +
                               "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        m_socket.write(req);
    });
    connect(&m_socket, &QTcpSocket::readyRead, this, &WsReader::onReadyRead);
    connect(&m_socket, &QTcpSocket::disconnected, this, [this] {
        const bool was = m_open;
        m_open = false;
        m_handshakeDone = false;
        if (was) emit closed();
    });
}

void WsReader::open(const QString &host, quint16 port, const QString &path) {
    m_host = QString("%1:%2").arg(host).arg(port);
    m_path = path;
    m_buffer.clear();
    m_fragments.clear();
    m_handshakeDone = false;
    m_socket.connectToHost(host, port);
}

void WsReader::close() {
    if (m_open)
        sendControl(0x8, {});
    m_socket.disconnectFromHost();
    m_open = false;
}

QList<WsReader::Frame> WsReader::parseFrames(QByteArray &buf) {
    QList<Frame> frames;
    while (buf.size() >= 2) {
        const quint8 b0 = quint8(buf[0]), b1 = quint8(buf[1]);
        const bool fin = b0 & 0x80;
        const int opcode = b0 & 0x0F;
        const bool masked = b1 & 0x80;
        quint64 len = b1 & 0x7F;
        int pos = 2;
        if (len == 126) {
            if (buf.size() < 4) break;
            len = (quint64(quint8(buf[2])) << 8) | quint8(buf[3]);
            pos = 4;
        } else if (len == 127) {
            if (buf.size() < 10) break;
            len = 0;
            for (int i = 0; i < 8; ++i)
                len = (len << 8) | quint8(buf[2 + i]);
            pos = 10;
        }
        QByteArray mask;
        if (masked) {
            if (buf.size() < pos + 4) break;
            mask = buf.mid(pos, 4);
            pos += 4;
        }
        if (quint64(buf.size()) < pos + len) break;
        QByteArray payload = buf.mid(pos, int(len));
        if (masked)
            for (int i = 0; i < payload.size(); ++i)
                payload[i] = char(payload[i] ^ mask[i % 4]);
        buf.remove(0, pos + int(len));
        // Fragmented messages carry fin=0; mark them with opcode | 0x100 so
        // the caller can reassemble.
        frames.append({fin ? opcode : (opcode | 0x100), payload});
    }
    return frames;
}

void WsReader::sendControl(int opcode, const QByteArray &payload) {
    // Client frames must be masked.
    QByteArray frame;
    frame.append(char(0x80 | opcode));
    frame.append(char(0x80 | payload.size()));
    QByteArray mask(4, 0);
    for (auto &c : mask) c = char(QRandomGenerator::global()->bounded(256));
    frame.append(mask);
    for (int i = 0; i < payload.size(); ++i)
        frame.append(char(payload[i] ^ mask[i % 4]));
    m_socket.write(frame);
}

void WsReader::onReadyRead() {
    m_buffer += m_socket.readAll();
    if (!m_handshakeDone) {
        const int end = m_buffer.indexOf("\r\n\r\n");
        if (end < 0) return;
        const QByteArray head = m_buffer.left(end);
        m_buffer.remove(0, end + 4);
        if (!head.startsWith("HTTP/1.1 101")) {
            m_socket.disconnectFromHost();
            return;
        }
        m_handshakeDone = true;
        m_open = true;
        emit opened();
    }
    for (const Frame &f : parseFrames(m_buffer)) {
        const int op = f.opcode & 0xFF;
        const bool fin = !(f.opcode & 0x100);
        if (op == 0x9) { sendControl(0xA, f.payload); continue; }
        if (op == 0x8) { m_socket.disconnectFromHost(); continue; }
        if (op == 0x1 || op == 0x2)
            m_messageOpcode = op;
        if (op > 0x2)
            continue;
        // Binary messages (preview images) are skipped; text is ComfyUI's JSON.
        if (m_messageOpcode == 0x1)
            m_fragments += f.payload;
        if (fin) {
            if (m_messageOpcode == 0x1)
                emit textMessage(QString::fromUtf8(m_fragments));
            m_fragments.clear();
        }
    }
}
