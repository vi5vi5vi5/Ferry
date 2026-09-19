#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QWebSocket;
class QJsonObject;
class TransferSession;

// Одно WebSocket-соединение. Обёртка по мотивам MeetUp, но с двумя
// отличиями, которые идут от природы задачи.
//
// Первое: у соединения есть РОЛЬ. В конференции все участники равны, здесь
// отправитель ровно один, и путать его с получателем нельзя ни на шаг.
//
// Второе: pendingBytes() здесь — центральная вещь, а не диагностика. Мы
// релеим одни и те же чанки нескольким получателям с разными каналами, и
// единственная честная мера того, поспевает ли конкретный получатель, —
// сколько байт мы уже отдали его сокету, а он ещё не сумел отправить.
// Без этого быстрый клиент набил бы буфер сокета медленного на гигабайт.
class ClientSession : public QObject
{
    Q_OBJECT
public:
    enum class Role {
        Unknown,    // подключился, но ещё не назвался
        Sender,
        Receiver,
    };

    explicit ClientSession(QWebSocket *socket, QObject *parent = nullptr);
    ~ClientSession() override;

    Role role() const { return m_role; }
    void setRole(Role role) { m_role = role; }

    // Номер получателя внутри раздачи; отправителю не присваивается.
    quint32 receiverId() const { return m_receiverId; }
    void setReceiverId(quint32 id) { m_receiverId = id; }

    QString name() const { return m_name; }
    void setName(const QString &name) { m_name = name; }

    TransferSession *transfer() const { return m_transfer; }
    void setTransfer(TransferSession *t) { m_transfer = t; }

    // Курсор живого потока: индекс следующего чанка, который этому
    // получателю ещё не отправлен.
    quint64 cursor() const { return m_cursor; }
    void setCursor(quint64 c) { m_cursor = c; }

    // Сколько чанков получатель подтвердил. Отличается от курсора: курсор —
    // что мы отдали сокету, ack — что клиент записал на диск. По ack
    // считается прогресс, который видит отправитель.
    quint64 acked() const { return m_acked; }
    void setAcked(quint64 a) { m_acked = a; }

    void sendJson(const QJsonObject &obj);
    void sendBinary(const QByteArray &data);

    qint64 pendingBytes() const;

    // Адрес пира — только для лимитов по IP в оперативке (§10). Никуда не
    // пишется и никому не показывается.
    QString peerAddress() const { return m_peerAddress; }

    void close();

signals:
    void textReceived(ClientSession *self, const QString &text);
    void binaryReceived(ClientSession *self, const QByteArray &data);
    void disconnected(ClientSession *self);
    // Сокет разгрузился — самое время дослать следующие чанки.
    void bytesWritten(ClientSession *self);

private slots:
    void onTextMessageReceived(const QString &text);
    void onBinaryMessageReceived(const QByteArray &data);
    void onSocketDisconnected();
    void onSocketBytesWritten(qint64 bytes);

private:
    QWebSocket *m_socket;
    Role m_role = Role::Unknown;
    quint32 m_receiverId = 0;
    QString m_name;
    QString m_peerAddress;
    TransferSession *m_transfer = nullptr;
    quint64 m_cursor = 0;
    quint64 m_acked = 0;
};
