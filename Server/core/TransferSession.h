#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include "core/RingWindow.h"

class ClientSession;

// Одна раздача. Живёт целиком в оперативке и умирает вместе с процессом —
// это не недоработка, а обещание продукта (§1): перезапуск сервера = все
// раздачи умерли, и на диске не осталось ничего.
//
// Что сервер про раздачу знает: размер, размер чанка, количество чанков,
// одностороннюю производную ключа и два непрозрачных шифротекста (манифест
// и список хешей). Чего не знает: имени файла, содержимого, структуры и
// самого ключа. Имя файла лежит внутри зашифрованного манифеста, и это не
// случайность — иначе сервер стал бы знать, что именно через него возят.
class TransferSession : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Draft,    // создана по POST, но offer ещё не пришёл: живёт 60 секунд
        Active,   // отправитель на связи
        Closed,
    };

    TransferSession(const QByteArray &id, const QByteArray &ownerToken, QObject *parent = nullptr);

    // ---- то, что знает сервер ----
    QByteArray id() const { return m_id; }
    QByteArray ownerToken() const { return m_ownerToken; }
    QByteArray verifier() const { return m_verifier; }
    State state() const { return m_state; }
    bool hasOffer() const { return m_hasOffer; }

    quint64 totalBytes() const { return m_totalBytes; }
    quint32 chunkSize() const { return m_chunkSize; }
    quint64 chunkCount() const { return m_chunkCount; }

    qint64 createdAtMs() const { return m_createdAtMs; }
    qint64 expiresAtMs() const { return m_expiresAtMs; }
    int usesLeft() const { return m_usesLeft; }

    // ---- настройка ----
    // Окно задаётся снаружи (из конфига) и может ужиматься, когда раздач
    // стало много: §6, защита памяти сервера.
    void setWindowCapacity(qint64 bytes);
    qint64 windowBytes() const { return m_window.bytes(); }

    // Разбирает offer от отправителя. Вызывается один раз; повторный offer
    // по той же раздаче — ошибка, а не переустановка: клиент, который её
    // шлёт, сломан, и молча принять его значит выдать получателям половину
    // одного тома и половину другого.
    bool applyOffer(const QJsonObject &msg, QString *errorCode);

    // Метаданные для GET /api/transfers/<id>. Ничего не сжигает.
    QJsonObject metaJson(qint64 nowMs) const;

    // ---- участники ----
    void attachSender(ClientSession *sender);
    // Проверка политики (использования, TTL, потолок одновременных) и
    // подключение. errorCode получает код из §8.
    //
    // haveUpto — сколько чанков подряд с начала у получателя уже есть
    // (докачка). Курсор ставится сюда, и повторно эти чанки не уезжают.
    // Полноценные диапазоны have придут в M2 вместе с backfill; префикса
    // хватает, пока чанки приходят по порядку.
    bool attachReceiver(ClientSession *receiver, quint64 haveUpto, qint64 nowMs,
                        QString *errorCode);
    void detach(ClientSession *session);

    ClientSession *sender() const { return m_sender; }
    const QList<ClientSession *> &receivers() const { return m_receivers; }

    // ---- поток ----
    // Целый бинарный фрейм от отправителя: [op:1][index:8 BE][шифротекст].
    // Фрейм кладётся в окно и уезжает получателям КАК ЕСТЬ — заголовок
    // внутри тех же байт именно для того, чтобы fan-out не копировал
    // полезную нагрузку.
    bool onSenderFrame(const QByteArray &frame, QString *errorCode);
    void onReceiverAck(ClientSession *receiver, quint64 upto);

    // Сторожевой такт раз в секунду.
    //
    // Нужен из-за одного неприятного свойства схемы «pull с кредитом»:
    // весь поток держится на одном сообщении need. Потерялось оно — и
    // раздача встаёт навсегда, причём тихо, потому что формально все живы
    // и ждут друг друга. Сторож замечает, что попрошенное не едет, и
    // просит заново.
    void tick(qint64 nowMs);

    // Отдать получателям то, что есть, и попросить у отправителя следующее.
    // Зовётся на каждое событие: пришёл чанк, разгрузился сокет, подключился
    // получатель, тикнул сторожевой таймер.
    void pump();

    // Сводка для отправителя: кто качает и насколько.
    QJsonObject peersJson() const;

    void closeWith(const QString &reasonCode);

signals:
    void needsClosing(TransferSession *self);

private:
    void requestFromSender();
    quint64 slowestCursor() const;
    void broadcastToReceivers(const QJsonObject &obj);

    // Сколько байт разрешаем держать в буфере сокета получателя, прежде чем
    // перестаём ему слать. Больше — память сервера уходит в буферы медленных
    // клиентов; меньше — на быстром канале появляются паузы между чанками.
    static constexpr qint64 kSocketHighWater = 4 * 1024 * 1024;

    // Сколько сторож ждёт, прежде чем повторить просьбу. Пять секунд —
    // заведомо больше любой сетевой заминки и заведомо меньше того, за
    // что человек успевает решить, что всё сломалось.
    static constexpr qint64 kStallMs = 5000;

    QByteArray m_id;
    QByteArray m_ownerToken;
    QByteArray m_verifier;
    QByteArray m_noncePrefix;
    QByteArray m_encryptedManifest;
    QByteArray m_encryptedHashList;
    QString m_mode = QStringLiteral("key");

    quint64 m_totalBytes = 0;
    quint32 m_chunkSize = 0;
    quint64 m_chunkCount = 0;

    int m_usesLeft = -1;          // -1 — без лимита
    int m_maxConcurrent = 8;
    qint64 m_createdAtMs = 0;
    qint64 m_expiresAtMs = 0;

    State m_state = State::Draft;
    bool m_hasOffer = false;

    ClientSession *m_sender = nullptr;       // не владеет
    QList<ClientSession *> m_receivers;      // не владеет
    quint32 m_nextReceiverId = 1;

    RingWindow m_window;
    qint64 m_windowCapacity = 0;
    // Докуда включительно уже попрошено у отправителя. -1 — ещё ничего.
    qint64 m_requestedUpTo = -1;
    // Когда окно последний раз выросло. По этому времени сторож понимает,
    // что попрошенное не едет.
    qint64 m_lastGrowthMs = 0;
};
