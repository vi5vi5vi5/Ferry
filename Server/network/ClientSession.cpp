#include "network/ClientSession.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QWebSocket>

ClientSession::ClientSession(QWebSocket *socket, QObject *parent)
    : QObject(parent), m_socket(socket)
{
    m_socket->setParent(this);
    m_peerAddress = m_socket->peerAddress().toString();

    connect(m_socket, &QWebSocket::textMessageReceived,
            this, &ClientSession::onTextMessageReceived);
    connect(m_socket, &QWebSocket::binaryMessageReceived,
            this, &ClientSession::onBinaryMessageReceived);
    connect(m_socket, &QWebSocket::disconnected,
            this, &ClientSession::onSocketDisconnected);
    connect(m_socket, &QWebSocket::bytesWritten,
            this, &ClientSession::onSocketBytesWritten);
}

ClientSession::~ClientSession() = default;

void ClientSession::sendJson(const QJsonObject &obj)
{
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
}

void ClientSession::sendBinary(const QByteArray &data)
{
    // data приходит прямо из окна и уезжает во все сокеты как есть.
    // QByteArray разделяет буфер неявно, поэтому N получателей не стоят
    // N копий чанка — это и есть причина, по которой заголовок фрейма
    // лежит внутри той же самой последовательности байт, а не
    // приклеивается отдельно на каждого.
    m_socket->sendBinaryMessage(data);
}

qint64 ClientSession::pendingBytes() const
{
    return m_socket->bytesToWrite();
}

void ClientSession::close()
{
    m_socket->close();
}

void ClientSession::onTextMessageReceived(const QString &text)
{
    emit textReceived(this, text);
}

void ClientSession::onBinaryMessageReceived(const QByteArray &data)
{
    emit binaryReceived(this, data);
}

void ClientSession::onSocketDisconnected()
{
    emit disconnected(this);
}

void ClientSession::onSocketBytesWritten(qint64 bytes)
{
    Q_UNUSED(bytes)
    emit bytesWritten(this);
}
