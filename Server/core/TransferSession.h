#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include "core/ChunkSet.h"
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
    // hello разбирается здесь целиком, а не в TransferServer: из него
    // берутся и карта уже принятого (`have` диапазонами или `have_upto`
    // префиксом у клиента M1), и объявленные клиентом возможности.
    // Курсор ставится на конец непрерывного начала, и повторно эти чанки
    // не уезжают.
    bool attachReceiver(ClientSession *receiver, const QJsonObject &hello, qint64 nowMs,
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

    // Получатель уточняет, что у него есть и что ему нужно. Оба сообщения
    // — диапазонами, с включительными границами, как в §8.
    //
    // false означает негодный список (перевёрнутый диапазон, выход за
    // пределы тома, слишком длинный перечень), и это bad_message: такое
    // не приходит от исправного клиента, а молча проглоченный мусор
    // разошёлся бы с картиной мира у получателя.
    bool onReceiverHave(ClientSession *receiver, const QJsonObject &msg);
    bool onReceiverRequest(ClientSession *receiver, const QJsonObject &msg);

    // Чанк не сошёлся у получателя. Не ошибка протокола и не повод
    // кого-то отключать: сервер ключа не знает и проверить, кто прав,
    // не может. Всё, что он делает сейчас, — забывает, что этот индекс
    // уже ехал, и просит его заново. Чёрный список источников — M2.2,
    // вместе с теми самыми источниками.
    void onBadChunk(ClientSession *receiver, quint64 index, const QString &reason);

    // Бинарный фрейм от ПОЛУЧАТЕЛЯ — ответ на serve.
    bool onPeerFrame(ClientSession *peer, const QByteArray &frame, QString *errorCode);

    // Умеет ли сервер звать этого получателя в источники.
    static bool canSeed(const ClientSession *receiver);

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

    // ---- вторая волна (M2.1) ----

    // Чего получателю не хватает, начиная с from. m_chunkCount — всё есть.
    //
    // Не просто дополнение have: если получатель сказал request, мы
    // уважаем его список и не навязываем то, чего он не просил.
    quint64 nextMissing(const ClientSession *receiver, quint64 from) const;

    // Отдать получателю то из недостающего, что ещё лежит в окне.
    // Это источник № 1 из §6 и самый дешёвый: никуда ходить не надо.
    void serveBackfillFromWindow(ClientSession *receiver);

    // Попросить то, чего в окне уже нет, — у пира (источник № 2) или, если
    // не у кого, у отправителя (№ 3). Порядок важен: аплоад пира и
    // так простаивает, а отправителю каждый чанк стоит чтения с диска
    // и его собственного канала, который мы и обещали не тратить дважды.
    void requestBackfill();

    // Кто из получателей может отдать этот чанк. nullptr — никто.
    ClientSession *pickPeerFor(quint64 index, const ClientSession *forWhom) const;

    // Забыть просьбы второй волны, на которые не ответили.
    void forgetStalledBackfill(qint64 nowMs);

    // Раздать пришедший вне окна чанк тем, кто его ждёт.
    //
    // Здесь же живёт коалесцирование, и оно досталось даром: чанк
    // просится один раз, а уезжает всем, кому нужен.
    void deliverBackfillFrame(quint64 index, const QByteArray &frame);

    // Сколько байт разрешаем держать в буфере сокета получателя, прежде чем
    // перестаём ему слать. Больше — память сервера уходит в буферы медленных
    // клиентов; меньше — на быстром канале появляются паузы между чанками.
    static constexpr qint64 kSocketHighWater = 4 * 1024 * 1024;

    // Сколько сторож ждёт, прежде чем повторить просьбу. Пять секунд —
    // заведомо больше любой сетевой заминки и заведомо меньше того, за
    // что человек успевает решить, что всё сломалось.
    static constexpr qint64 kStallMs = 5000;

    // Сколько чанков второй волны держим в пути одновременно.
    //
    // Потолок маленький нарочно. Чанк, пришедший по второй волне, не
    // ложится в окно — он уходит сразу тем, кто его ждёт, и забывается.
    // Значит всё, что мы попросили, обязано влезть в буферы сокетов
    // прямо сейчас, и просить впрок нельзя.
    static constexpr int kBackfillInFlight = 4;

    // Сколько жалоб терпим, прежде чем перестать спрашивать у этого
    // источника. Три — потому что одна жалоба бывает от случайности,
    // три подряд — уже закономерность.
    static constexpr int kMaxStrikes = 3;

    // Сколько помним, кто что прислал.
    static constexpr qint64 kServedByTtlMs = 30000;

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

    // Что попрошено второй волной и ещё не пришло, и у кого попрошено.
    //
    // Второе нужно не для учёта, а ради безопасности: бинарный фрейм от
    // получателя принимается только тогда, когда мы сами попросили
    // ИМЕННО ЕГО и ИМЕННО этот индекс. Иначе любой подключившийся
    // смог бы подмешивать байты в чужую раздачу.
    ferry::ChunkSet m_backfillInFlight;
    QHash<quint64, ClientSession *> m_askedOf;   // nullptr — спросили отправителя
    qint64 m_backfillAskedMs = 0;

    // Кто прислал какой чанк — чтобы было кому записать жалобу из
    // bad_chunk. Живёт недолго: получатель проверяет чанк сразу, а держать
    // эту таблицу вечно значило бы хранить запись на каждый чанк тома.
    QHash<quint64, QPair<ClientSession *, qint64>> m_servedBy;
    // Когда окно последний раз выросло. По этому времени сторож понимает,
    // что попрошенное не едет.
    qint64 m_lastGrowthMs = 0;
};
