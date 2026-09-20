#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include "core/ChunkSet.h"

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
    // что мы отдали сокету, ack — что клиент записал на диск.
    quint64 acked() const { return m_acked; }
    void setAcked(quint64 a) { m_acked = a; }

    // ---- то, что появилось в M2 ----

    // Чем клиент владеет сверх обязательного минимума. Объявляется им
    // самим в hello, и спрашивать об этом приходится потому, что клиент
    // может оказаться старее сервера: релей раздаёт свою версию клиента,
    // но у человека на диске вполне может лежать прошлая.
    //
    // Молчание клиента о возможности — это всегда «не умеет». Обратное
    // (считать, что умеет, раз не сказал обратного) означало бы ждать от
    // него сообщений, которых он не пошлёт, и вешать на этом чужую
    // раздачу.
    enum Feature : quint32 {
        FeatureRanges = 1u << 0,     // говорит have/request диапазонами
        FeatureBackfill = 1u << 1,   // умеет отвечать на serve
    };
    quint32 features() const { return m_features; }
    void setFeatures(quint32 f) { m_features = f; }
    bool speaks(Feature f) const { return (m_features & quint32(f)) != 0; }

    // Карта того, что у получателя есть. В M1 хватало префикса (чанки
    // приходили по порядку), с двумя волнами — уже нет: у получателя
    // появляются дырки, и прогресс перестаёт быть одним числом.
    ferry::ChunkSet &have() { return m_have; }
    const ferry::ChunkSet &have() const { return m_have; }

    // Что мы ему уже отправили — не путать с have.
    //
    // Разница между ними стоила мне вечера. have приходит от
    // получателя не чаще раза в 300 мс, а отдаём мы всё это время. Всё,
    // что ушло, но ещё не подтверждено, по одному только have выглядит
    // дыркой — и вторая волна начинает тянуть заново то, что прямо сейчас
    // летит по проводу. А если чанк к тому же успел вытесниться из окна,
    // тянется он у отправителя — то есть ровно ценой, которую весь M2
    // и затевался избежать.
    //
    // Очищается только по bad_chunk: если получатель сказал, что чанк не
    // сошёлся, значит отправленного у него нет.
    ferry::ChunkSet &sent() { return m_sent; }
    const ferry::ChunkSet &sent() const { return m_sent; }

    // Что получатель просит. Заполняется сообщением request.
    ferry::ChunkSet &wanted() { return m_wanted; }
    const ferry::ChunkSet &wanted() const { return m_wanted; }

    // Откуда искать следующий недостающий чанк для второй волны.
    // Чисто ускорение: без неё планировщик просматривал бы карту
    // каждого получателя с нуля на каждом обороте, а оборотов
    // столько же, сколько чанков.
    // Две подсказки, а не одна, потому что источника у второй волны
    // два и идут они независимо: то, что ещё лежит в окне, уезжает
    // немедленно, а за остальным надо ходить к отправителю. Общая
    // подсказка на двоих значила бы, что быстрый источник перепрыгивает
    // через то, что должен был привезти медленный.
    // Сколько раз этот участник отдавал чанк, который потом не сошёлся.
    //
    // Сервер не знает ключа и проверить, кто из двоих врёт, не может.
    // Зато может считать: источник, на который жалуются раз за разом,
    // почти наверняка виноват — по злому умыслу или из-за битого диска,
    // разницы для нас нет. После нескольких жалоб мы просто перестаём
    // у него спрашивать. Отключать его нельзя: жалоба могла быть и ложной,
    // а его собственная загрузка ни в чём не провинилась.
    int backfillStrikes() const { return m_backfillStrikes; }
    void addBackfillStrike() { ++m_backfillStrikes; }

    quint64 windowHint() const { return m_windowHint; }
    void setWindowHint(quint64 h) { m_windowHint = h; }
    quint64 backfillHint() const { return m_backfillHint; }
    void setBackfillHint(quint64 h) { m_backfillHint = h; }

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
    quint32 m_features = 0;
    int m_backfillStrikes = 0;
    quint64 m_windowHint = 0;
    quint64 m_backfillHint = 0;
    ferry::ChunkSet m_have;
    ferry::ChunkSet m_sent;
    ferry::ChunkSet m_wanted;
};
