#pragma once

#include <QByteArray>
#include <QJsonArray>
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
        Draft,      // создана по POST, но offer ещё не пришёл: живёт 60 секунд
        Active,     // отправитель на связи
        Delegated,  // отправитель ушёл, раздают те, кто скачал целиком (§5)
        Closed,
    };

    // Чем кончилось hello.
    enum class Admission {
        Rejected,   // отказ, код в errorCode
        Admitted,   // принят: hello_ok уже ушёл
        Pending,    // ждёт решения владельца: ушёл approval_pending
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
    bool requiresApproval() const { return m_requireApproval; }

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
    // streamClient — клиент прислал заголовок X-Ferry-Features со строкой
    // stream_hashes. Пока хеши не досчитаны, ему отдаётся промежуточный
    // манифест без списка, а всем остальным отдать нечего (см. preparing()).
    QJsonObject metaJson(qint64 nowMs, bool streamClient = false) const;

    // Хеши на лету: отправитель ещё считает, и клиенту, который не умеет
    // принимать их сегментами, показать пока нечего.
    bool streamingHashes() const { return m_streamHashes && !m_hashesComplete; }

    // Сегмент списка хешей и конец списка — от отправителя.
    bool onHashes(const QJsonObject &msg, QString *errorCode);
    bool onHashesDone(const QJsonObject &msg, QString *errorCode);

    // Всё, что накоплено, — только что подключившемуся получателю,
    // ДО первого фрейма. Порядок в сокете тот же, что порядок отправки,
    // так что хеш чанка всегда приезжает раньше самого чанка.
    void sendHashesSoFar(ClientSession *receiver);

    // ---- участники ----
    void attachSender(ClientSession *sender);
    // Проверка политики (использования, TTL, потолок одновременных) и
    // подключение. errorCode получает код из §8. Если раздача спрашивает
    // владельца, получатель встаёт в очередь (Pending) и ничего не
    // списывается, пока владелец не ответит.
    //
    // hello разбирается здесь целиком, а не в TransferServer: из него
    // берутся и карта уже принятого (`have` диапазонами или `have_upto`
    // префиксом у клиента M1), и объявленные клиентом возможности.
    // Курсор ставится на конец непрерывного начала, и повторно эти чанки
    // не уезжают.
    Admission attachReceiver(ClientSession *receiver, const QJsonObject &hello, qint64 nowMs,
                             QString *errorCode);
    void detach(ClientSession *session);

    // ---- владелец (§5) ----
    //
    // Владелец и источник — разные вещи. Пока отправитель на связи, это
    // одно соединение. Ушёл и вернулся по owner_token из другой вкладки —
    // он снова распоряжается раздачей, но файлов у него может и не быть,
    // и тогда источником он не становится.
    ClientSession *owner() const { return m_owner; }

    // Владелец вернулся. serve — у него есть том и он готов отдавать:
    // тогда раздача снова Active и живая волна продолжается с того места,
    // где остановилась. Прежнее соединение владельца, если оно ещё
    // числится, вытесняется: полуживой сокет не должен держать управление.
    //
    // Вернувшийся с томом вытесняет и прежний сокет ОТПРАВИТЕЛЯ: после
    // смены сети старое соединение мертво, но TCP узнает об этом через
    // минуты, а токен владельца у нового тот же. resumes — обещает ли он
    // вернуться снова, если оборвётся опять (kFeatureResume).
    bool resumeOwner(ClientSession *session, bool serve, bool resumes, QString *errorCode);

    // Вернувшемуся владельцу — те, кто ждёт его решения у двери. Зовётся
    // после resume_ok: вопросы должны прийти уже в пульт, а не раньше него.
    void sendPendingToOwner();

    // Отправитель сказал, что уходит сам: bye по сокету или leave по HTTP
    // из закрывающейся вкладки. Ждать его возвращения незачем; если связь
    // уже пропала и раздача ждёт, ожидание кончается сейчас.
    void senderLeaving(ClientSession *session = nullptr);

    // Связь с отправителем пропала без bye, и раздача ждёт его возвращения
    // (kSenderGraceMs). Состояние при этом Active: получатели остаются на
    // местах, окно и пиры отдают то, что могут.
    bool senderAway() const { return m_senderLostAtMs != 0; }
    void checkSenderGrace(qint64 nowMs);

    // Сколько хешей релей уже принял — с этого места вернувшийся
    // отправитель продолжает сегменты.
    quint64 hashedUpTo() const { return m_streamHashes ? m_hashedUpTo : m_chunkCount; }
    bool hashesComplete() const { return m_hashesComplete; }

    // Сообщения владельца. false — bad_message.
    bool onApprove(const QJsonObject &msg);
    bool onPolicy(const QJsonObject &msg, qint64 nowMs);
    void revoke();

    // Сводка для владельца и получателей: состояние и политика сейчас.
    QJsonObject stateJson(qint64 nowMs) const;

    // Что сервер умеет. Здесь, а не в TransferServer, потому что hello_ok
    // теперь уходит и отсюда — когда владелец пустил того, кто ждал.
    static QJsonArray serverFeatures();
    static QString stateName(State state);

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

    // Разбивка источников для конкретного получателя (§8, stats).
    QJsonObject statsJson(const ClientSession *receiver) const;

    void closeWith(const QString &reasonCode);

    // Живёт ли раздача без отправителя (§5): сид есть, использования не
    // кончились у тех, кому ещё надо. Зовётся на каждый уход и сторожем
    // реестра — раздачу без участников иначе не тронул бы никто.
    void checkDelegated(qint64 nowMs);

signals:
    void needsClosing(TransferSession *self);

private:
    // Пустить получателя: последние проверки политики, курсор, списание
    // использования, hello_ok и всё, что идёт сразу за ним.
    //
    // rejoin — получатель вернулся по токену возврата: использование не
    // списывается, лимит использований его не останавливает.
    bool admit(ClientSession *receiver, QString *errorCode, const QByteArray &rejoin = {});
    void rejectPending(ClientSession *receiver, const char *reason);
    void admitAllPending();

    // Отправитель ушёл: делегировать или закрыть.
    void onSenderLeft();
    // Можно ли жить без отправителя прямо сейчас (§5).
    bool canDelegate() const;
    // Связь пропала, ждём отправителя назад.
    void senderLost(qint64 nowMs);
    // Просьбы к отправителю, которые с его сокетом пропали: живую волну
    // попросим заново от края окна, вторую — у того, у кого будет.
    void forgetSenderRequests();
    QJsonObject approvalRequestJson(const ClientSession *receiver) const;

    // Есть ли кому раздавать без отправителя: получатель со всем томом,
    // который умеет отдавать и на которого не жаловались.
    bool hasSeed() const;
    bool complete(const ClientSession *receiver) const;


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
    void deliverBackfillFrame(quint64 index, const QByteArray &frame, bool fromPeer);

    // Сколько байт разрешаем держать в буфере сокета получателя, прежде чем
    // перестаём ему слать. Больше — память сервера уходит в буферы медленных
    // клиентов; меньше — на быстром канале появляются паузы между чанками.
    static constexpr qint64 kSocketHighWater = 4 * 1024 * 1024;

    // Сколько сторож ждёт, прежде чем повторить просьбу. Пять секунд —
    // заведомо больше любой сетевой заминки и заведомо меньше того, за
    // что человек успевает решить, что всё сломалось.
    static constexpr qint64 kStallMs = 5000;

    // Самый медленный аплоад сида, под который рассчитан сторож второй
    // волны: 64 КиБ/с, около полумегабита. Сид отдаёт своим каналом, и
    // ждать чанк меньше, чем он идёт по такому каналу, — значит забывать
    // просьбы, которые сид честно везёт (forgetStalledBackfill).
    static constexpr qint64 kSlowUplinkBytesPerSec = 64 * 1024;
    // Дальше сторож второй волны не растягивается: две минуты на чанк —
    // это уже не медленный сид, а молчащий.
    static constexpr qint64 kMaxBackfillStallMs = 120000;

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

    // Сколько раздача без отправителя ждёт, пока вернётся сид. Веб-
    // получатель после обрыва переподключается сам, и мгновенное закрытие
    // наказывало бы всех за секундную заминку у одного.
    static constexpr qint64 kSeedGraceMs = 30000;

    // Сколько раздача ждёт отправителя, у которого пропала связь. Хватает
    // на смену сети и на перезапуск вкладки; больше — получатели слишком
    // долго смотрели бы на стоящую полосу, не зная, вернётся ли он.
    static constexpr qint64 kSenderGraceMs = 30000;

    // Сколько после обрыва ждём получателя, который не докачал и может
    // вернуться по токену возврата, прежде чем закрыть раздачу без
    // использований (checkDelegated). Вкладка переподключается через 1, 2,
    // 4, 8, 15 и дальше каждые 30 секунд — минуты хватает на смену сети.
    static constexpr qint64 kRejoinGraceMs = 60000;
    // Сколько токенов возврата помним. Больше — забываем самые давние из
    // ушедших: раздачу без лимита могут брать тысячи человек.
    static constexpr int kMaxRejoinTokens = 1024;

    // Сколько человек может одновременно ждать решения владельца. Больше —
    // это уже не «друзья по ссылке», а кто-то, кто стучится без остановки.
    static constexpr int kMaxPending = 16;

    // Как часто говорить получателю, откуда к нему едет. Четверть
    // секунды: чаще глазу незачем, реже — быстрая передача успевает
    // кончиться, не услышав ни одного сообщения.
    static constexpr qint64 kStatsIntervalMs = 250;

    QByteArray m_id;
    QByteArray m_ownerToken;
    QByteArray m_verifier;
    QByteArray m_noncePrefix;
    QByteArray m_encryptedManifest;
    QByteArray m_encryptedHashList;

    // ---- хеши на лету ----
    // Промежуточный манифест (без корня) и сегменты списка встык. Когда
    // отправитель досчитает, m_encryptedManifest и m_encryptedHashList
    // получают итоговые — ровно в том виде, в каком их ждут все.
    bool m_streamHashes = false;
    bool m_hashesComplete = true;
    QByteArray m_encryptedStreamManifest;
    quint64 m_hashedUpTo = 0;
    QList<QJsonObject> m_hashSegments;   // уже готовые сообщения hashes
    QJsonObject m_hashesDoneMsg;
    QString m_mode = QStringLiteral("key");
    // Соль Argon2id для режима «пароль вместо ключа». Не секрет: отдаётся
    // всем по GET, чтобы получатель вывел ключ из пароля сам.
    QByteArray m_passwordSalt;

    quint64 m_totalBytes = 0;
    quint32 m_chunkSize = 0;
    quint64 m_chunkCount = 0;

    int m_usesLeft = -1;          // -1 — без лимита
    int m_maxConcurrent = 8;
    bool m_requireApproval = false;
    bool m_leaveOpen = false;
    // Что делать с подтверждением, когда владелец ушёл: true — снять
    // и пускать всех с ключом, false — закрыть раздачу.
    bool m_freezeOnLeave = false;
    // С какого момента без отправителя нет ни одного сида. 0 — сид есть.
    qint64 m_noSeedSinceMs = 0;
    bool m_senderResumes = false;     // отправитель объявил resume
    bool m_senderLeaving = false;     // и сказал, что уходит сам
    qint64 m_senderLostAtMs = 0;      // связь пропала; 0 — на связи
    qint64 m_createdAtMs = 0;
    qint64 m_expiresAtMs = 0;

    State m_state = State::Draft;
    bool m_hasOffer = false;

    ClientSession *m_sender = nullptr;       // источник; не владеет
    ClientSession *m_owner = nullptr;        // тот, кто распоряжается; не владеет
    QList<ClientSession *> m_receivers;      // не владеет
    QList<ClientSession *> m_pending;        // ждут решения владельца
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
    qint64 m_backfillArrivedMs = 0;   // когда пришёл последний чанк второй волны
    qint64 m_backfillStallMs = kStallMs;   // срок сторожа второй волны, растёт сам
    QHash<quint64, ClientSession *> m_forgotten;   // что сторож сбросил последним

    // Токены возврата получателей (rejoin в hello_ok). Получатель, однажды
    // пущенный, после обрыва возвращается с токеном и не тратит
    // использование: одноразовая ссылка защищает от второго получателя, а
    // не от плохой сети у первого. Токен одноразовый — при возврате
    // выдаётся новый, старый гаснет.
    struct Rejoin {
        ClientSession *current = nullptr;   // кто сейчас на связи с ним
        qint64 leftAtMs = 0;                // когда ушёл, если ушёл
        bool complete = false;              // ушёл с томом целиком
    };
    QHash<QByteArray, Rejoin> m_rejoin;
    QByteArray issueRejoin(ClientSession *receiver);
    bool rejoinExpected(qint64 nowMs) const;

    // Кто прислал какой чанк — чтобы было кому записать жалобу из
    // bad_chunk. Живёт недолго: получатель проверяет чанк сразу, а держать
    // эту таблицу вечно значило бы хранить запись на каждый чанк тома.
    QHash<quint64, QPair<ClientSession *, qint64>> m_servedBy;
    // Когда окно последний раз выросло. По этому времени сторож понимает,
    // что попрошенное не едет.
    qint64 m_lastGrowthMs = 0;
};
