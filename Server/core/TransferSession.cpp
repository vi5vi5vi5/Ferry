#include "core/TransferSession.h"

#include <QDateTime>
#include <QJsonArray>
#include <QRandomGenerator>
#include <algorithm>
#include <cmath>
#include <vector>

#include "config/Log.h"
#include "core/Chunker.h"
#include "core/Protocol.h"
#include "network/ClientSession.h"

namespace {

quint64 readBe64(const char *p)
{
    quint64 v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | quint8(p[i]);
    return v;
}

// Разумный потолок TTL: сутки по умолчанию (§5), неделя максимум. Дольше
// держать раздачу в оперативке незачем — это уже не паром, а хранилище.
constexpr qint64 kDefaultTtlMs = 24 * 60 * 60 * 1000LL;
constexpr qint64 kMaxTtlMs = 7 * 24 * 60 * 60 * 1000LL;

// Сколько диапазонов готовы принять в одном сообщении.
//
// Потолок нужен, потому что список приходит снаружи: клиент, приславший
// миллион диапазонов по одному чанку, заставил бы сервер держать их все.
// Четыре тысячи — это заведомо больше, чем бывает у честного получателя
// (у него их единицы: живая волна плюс то, что тянется второй), и
// заведомо меньше, чем нужно, чтобы кому-то навредить.
constexpr int kMaxRangesPerMessage = 4096;

// Диапазоны из JSON: [[a,b],[c,d]]. Границы ВКЛЮЧИТЕЛЬНЫЕ с обеих сторон
// (§8), и это единственное место на сервере, где они превращаются в
// ferry::ChunkRange. Клиент делает то же самое своим json::Value —
// расходиться им нельзя, поэтому смысл границ живёт в ядре, в ChunkSet, а
// здесь остаётся только разбор.
bool parseRanges(const QJsonValue &value, quint64 chunkCount,
                 std::vector<ferry::ChunkRange> &out)
{
    out.clear();
    if (value.isUndefined() || value.isNull())
        return true;   // поля нет — это пустой список, а не ошибка
    if (!value.isArray())
        return false;

    const QJsonArray arr = value.toArray();
    if (arr.size() > kMaxRangesPerMessage)
        return false;

    out.reserve(size_t(arr.size()));
    for (const QJsonValue &item : arr) {
        if (!item.isArray())
            return false;
        const QJsonArray pair = item.toArray();
        if (pair.size() != 2)
            return false;
        const double from = pair.at(0).toDouble(-1);
        const double to = pair.at(1).toDouble(-1);
        // Дробное или отрицательное — это не «почти индекс», это мусор.
        if (from < 0 || to < 0 || from != std::floor(from) || to != std::floor(to))
            return false;
        const ferry::ChunkRange r{uint64_t(from), uint64_t(to)};
        if (!r.valid() || r.to >= chunkCount)
            return false;
        out.push_back(r);
    }
    return true;
}

// Возможности, объявленные клиентом. Неизвестные имена пропускаем молча:
// так клиент из будущего сможет объявить что-то новое, не поссорившись с
// сегодняшним сервером.
quint32 parseFeatures(const QJsonValue &value)
{
    quint32 flags = 0;
    if (!value.isArray())
        return flags;
    for (const QJsonValue &item : value.toArray()) {
        const QString name = item.toString();
        if (name == QLatin1String("ranges"))
            flags |= ClientSession::FeatureRanges;
        else if (name == QLatin1String("backfill"))
            flags |= ClientSession::FeatureBackfill;
        else if (name == QLatin1String("stream_hashes"))
            flags |= ClientSession::FeatureStreamHashes;
    }
    return flags;
}

} // namespace

QString TransferSession::stateName(State state)
{
    switch (state) {
    case State::Draft:
        return QStringLiteral("draft");
    case State::Active:
        return QStringLiteral("active");
    case State::Delegated:
        return QStringLiteral("delegated");
    case State::Closed:
        break;
    }
    return QStringLiteral("closed");
}

TransferSession::TransferSession(const QByteArray &id, const QByteArray &ownerToken, QObject *parent)
    : QObject(parent), m_id(id), m_ownerToken(ownerToken)
{
    m_createdAtMs = QDateTime::currentMSecsSinceEpoch();
    // Черновик живёт минуту: столько есть у отправителя, чтобы после
    // POST /api/transfers дойти до WebSocket и прислать offer.
    m_expiresAtMs = m_createdAtMs + 60 * 1000;
}

void TransferSession::setWindowCapacity(qint64 bytes)
{
    m_windowCapacity = bytes;
    if (m_hasOffer)
        m_window.setCapacity(bytes);
}

bool TransferSession::applyOffer(const QJsonObject &msg, QString *errorCode)
{
    const auto fail = [&](const char *code) {
        if (errorCode)
            *errorCode = QString::fromLatin1(code);
        return false;
    };

    if (m_hasOffer)
        return fail(ferry::err::kBadMessage);

    const qint64 total = qint64(msg.value(QStringLiteral("total")).toDouble(-1));
    const qint64 chunkSize = qint64(msg.value(QStringLiteral("chunk_size")).toDouble(-1));
    const qint64 chunks = qint64(msg.value(QStringLiteral("chunks")).toDouble(-1));
    if (total < 0 || chunkSize <= 0 || chunks < 0)
        return fail(ferry::err::kBadMessage);

    // Сервер не обязан доверять клиенту в арифметике: количество чанков
    // однозначно выводится из размера тома и размера чанка, и расхождение
    // означает либо сломанный клиент, либо попытку заставить нас держать
    // окно не того размера.
    const ferry::ChunkPlan plan = ferry::planWith(quint64(total), quint32(chunkSize));
    if (!plan.valid() || plan.chunkCount != quint64(chunks))
        return fail(ferry::err::kBadMessage);

    m_verifier = QByteArray::fromBase64(
        msg.value(QStringLiteral("verifier")).toString().toLatin1(),
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    if (m_verifier.size() != int(ferry::kVerifierSize))
        return fail(ferry::err::kBadMessage);

    m_noncePrefix = QByteArray::fromBase64(
        msg.value(QStringLiteral("nonce_prefix")).toString().toLatin1(),
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    if (m_noncePrefix.size() != int(ferry::kNoncePrefixSize))
        return fail(ferry::err::kBadMessage);

    // Манифест и список хешей — непрозрачные байты. Сервер проверяет только
    // размер: список хешей обязан быть ровно 32 байта на чанк плюс тег GCM,
    // иначе получатель не сможет проверить ни одного чанка, а узнает он об
    // этом уже после того, как выкачает половину тома.
    const QByteArray manifest = QByteArray::fromBase64(
        msg.value(QStringLiteral("manifest")).toString().toLatin1(),
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    if (manifest.isEmpty() || manifest.size() > 16 * 1024 * 1024)
        return fail(ferry::err::kBadMessage);

    // Хеши на лету: списка в offer нет, он приедет сегментами, а манифест
    // пока промежуточный. Итоговые манифест и список займут обычные
    // места, когда отправитель досчитает (onHashesDone).
    const QString hashMode = msg.value(QStringLiteral("hash_mode")).toString();
    if (!hashMode.isEmpty() && hashMode != QLatin1String("stream"))
        return fail(ferry::err::kBadMessage);
    m_streamHashes = hashMode == QLatin1String("stream");
    m_hashesComplete = !m_streamHashes;
    m_hashedUpTo = 0;
    m_hashSegments.clear();
    m_hashesDoneMsg = QJsonObject();
    if (m_streamHashes) {
        if (msg.contains(QStringLiteral("hash_list")))
            return fail(ferry::err::kBadMessage);
        m_encryptedStreamManifest = manifest;
        m_encryptedManifest.clear();
        m_encryptedHashList.clear();
    } else {
        m_encryptedManifest = manifest;
        m_encryptedHashList = QByteArray::fromBase64(
            msg.value(QStringLiteral("hash_list")).toString().toLatin1(),
            QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        const qint64 expectHashBytes =
            qint64(plan.chunkCount) * 32 + qint64(ferry::kGcmTagSize);
        if (m_encryptedHashList.size() != expectHashBytes)
            return fail(ferry::err::kBadMessage);
    }

    m_totalBytes = quint64(total);
    m_chunkSize = quint32(chunkSize);
    m_chunkCount = plan.chunkCount;
    // Пароль вместо ключа (§4): ключ выводится из пароля, соль лежит
    // здесь и отдаётся всем по GET. Сервер пароля не видит, как не видел
    // ключа, — он только хранит соль, без которой ключ не вывести.
    m_mode = msg.value(QStringLiteral("mode")).toString(QStringLiteral("key"));
    m_passwordSalt.clear();
    if (m_mode == QLatin1String("password")) {
        m_passwordSalt = QByteArray::fromBase64(
            msg.value(QStringLiteral("salt")).toString().toLatin1(),
            QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        if (m_passwordSalt.size() != int(ferry::kPasswordSaltSize))
            return fail(ferry::err::kBadMessage);
    } else if (m_mode != QLatin1String("key")) {
        return fail(ferry::err::kBadMessage);
    }

    // ---- политика (§5) ----
    const QJsonObject policy = msg.value(QStringLiteral("policy")).toObject();
    const QJsonValue uses = policy.value(QStringLiteral("uses"));
    m_usesLeft = uses.isNull() || uses.isUndefined() ? -1 : std::max(1, uses.toInt(-1));

    qint64 ttl = qint64(policy.value(QStringLiteral("ttl_ms")).toDouble(kDefaultTtlMs));
    ttl = std::clamp<qint64>(ttl, 60 * 1000, kMaxTtlMs);
    m_expiresAtMs = QDateTime::currentMSecsSinceEpoch() + ttl;

    m_maxConcurrent = std::clamp(policy.value(QStringLiteral("max_concurrent")).toInt(8), 1, 64);

    // Подтверждение и уход владельца (§5). Без подтверждения снимать
    // при уходе нечего, и on_owner_leave ни на что не влияет; с ним по
    // умолчанию раздача закрывается — «сид подтвердил за отправителя»
    // в системе не бывает.
    m_requireApproval = policy.value(QStringLiteral("require_approval")).toBool(false);
    m_leaveOpen = policy.value(QStringLiteral("leave_open")).toBool(false);
    const QString onLeave = policy.value(QStringLiteral("on_owner_leave")).toString();
    if (!onLeave.isEmpty() && onLeave != QLatin1String("close")
        && onLeave != QLatin1String("freeze"))
        return fail(ferry::err::kBadMessage);
    m_freezeOnLeave = onLeave == QLatin1String("freeze");

    // Что умеет отправитель. Молчание — «не умеет»: старый отправитель
    // после обрыва не вернётся, и ждать его значило бы держать получателей
    // у стоящей полосы ни за что.
    m_senderResumes = msg.value(QStringLiteral("features")).toArray().contains(
        QString::fromLatin1(ferry::kFeatureResume));

    // Окно меряется ФРЕЙМАМИ, а не открытыми чанками: хранится в нём
    // ровно то, что приехало от отправителя и уедет получателям —
    // заголовок, шифротекст и тег GCM. Считать по размеру чанка значило бы
    // просить у отправителя ровно столько, сколько окно НЕ вмещает, и
    // первый же лишний байт вытеснял бы из окна начало тома.
    m_window.configure(m_chunkSize + quint32(ferry::kBinaryHeaderSize + ferry::kGcmTagSize),
                       m_windowCapacity);
    m_backfillInFlight.reset(m_chunkCount);
    m_askedOf.clear();
    m_servedBy.clear();
    m_forgotten.clear();
    m_backfillStallMs = std::max(kStallMs, qint64(m_chunkSize) * 1000 / kSlowUplinkBytesPerSec);
    m_lastGrowthMs = QDateTime::currentMSecsSinceEpoch();
    m_hasOffer = true;
    m_state = State::Active;
    return true;
}

QJsonObject TransferSession::metaJson(qint64 nowMs, bool streamClient) const
{
    QJsonObject o;
    o[QStringLiteral("id")] = QString::fromLatin1(m_id);
    o[QStringLiteral("total")] = double(m_totalBytes);
    o[QStringLiteral("chunk_size")] = double(m_chunkSize);
    o[QStringLiteral("chunks")] = double(m_chunkCount);
    if (streamingHashes()) {
        // Сюда попадает только тот, кто умеет хеши на лету: остальным
        // HttpApi отвечает preparing, не доходя до этой функции.
        Q_UNUSED(streamClient)
        o[QStringLiteral("hash_mode")] = QStringLiteral("stream");
        o[QStringLiteral("hashed")] = double(m_hashedUpTo);
        o[QStringLiteral("manifest")] = QString::fromLatin1(m_encryptedStreamManifest.toBase64(
            QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    } else {
        o[QStringLiteral("manifest")] = QString::fromLatin1(m_encryptedManifest.toBase64(
            QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        o[QStringLiteral("hash_list")] = QString::fromLatin1(m_encryptedHashList.toBase64(
            QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    }
    o[QStringLiteral("nonce_prefix")] = QString::fromLatin1(m_noncePrefix.toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    o[QStringLiteral("mode")] = m_mode;
    if (!m_passwordSalt.isEmpty())
        o[QStringLiteral("salt")] = QString::fromLatin1(m_passwordSalt.toBase64(
            QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    o[QStringLiteral("state")] = stateName(m_state);
    o[QStringLiteral("receivers")] = m_receivers.size();
    o[QStringLiteral("uses_left")] = m_usesLeft;
    o[QStringLiteral("expires_in_ms")] = double(std::max<qint64>(0, m_expiresAtMs - nowMs));
    o[QStringLiteral("require_approval")] = m_requireApproval;
    return o;
}

QJsonObject TransferSession::stateJson(qint64 nowMs) const
{
    QJsonObject o;
    o[QStringLiteral("type")] = QStringLiteral("state");
    o[QStringLiteral("state")] = stateName(m_state);
    o[QStringLiteral("uses_left")] = m_usesLeft;
    o[QStringLiteral("expires_in_ms")] = double(std::max<qint64>(0, m_expiresAtMs - nowMs));
    o[QStringLiteral("require_approval")] = m_requireApproval;
    o[QStringLiteral("leave_open")] = m_leaveOpen;
    o[QStringLiteral("on_owner_leave")] =
        m_freezeOnLeave ? QStringLiteral("freeze") : QStringLiteral("close");
    o[QStringLiteral("max_concurrent")] = m_maxConcurrent;
    o[QStringLiteral("receivers")] = m_receivers.size();
    // Связь с отправителем пропала, и раздача ждёт его: получателю стоит
    // знать, почему полоса стоит, и сколько ещё ждать.
    o[QStringLiteral("sender_away")] = m_senderLostAtMs != 0;
    if (m_senderLostAtMs != 0)
        o[QStringLiteral("sender_wait_ms")] =
            double(std::max<qint64>(0, kSenderGraceMs - (nowMs - m_senderLostAtMs)));
    return o;
}

QJsonArray TransferSession::serverFeatures()
{
    QJsonArray f;
    f.append(QStringLiteral("ranges"));
    f.append(QStringLiteral("backfill"));
    // Хеши на лету: отправитель может начать раздачу, не посчитав том
    // целиком, а получатель — принимать список сегментами.
    f.append(QString::fromLatin1(ferry::kFeatureStreamHashes));
    // M4: подтверждение, смена условий, отзыв, уход владельца с
    // делегированием и возврат по owner_token.
    f.append(QString::fromLatin1(ferry::kFeaturePolicy));
    // Пароль вместо ключа: соль в метаданных, ключ из Argon2id.
    f.append(QString::fromLatin1(ferry::kFeaturePassword));
    // Отправителя после обрыва ждут kSenderGraceMs, а не закрывают сразу.
    f.append(QString::fromLatin1(ferry::kFeatureResume));
    return f;
}

void TransferSession::attachSender(ClientSession *sender)
{
    m_sender = sender;
    m_owner = sender;
    sender->setRole(ClientSession::Role::Sender);
    sender->setTransfer(this);
}

TransferSession::Admission TransferSession::attachReceiver(ClientSession *receiver,
                                                          const QJsonObject &hello,
                                                          qint64 nowMs, QString *errorCode)
{
    const auto fail = [&](const char *code) {
        if (errorCode)
            *errorCode = QString::fromLatin1(code);
        return Admission::Rejected;
    };

    if (!m_hasOffer || (m_state != State::Active && m_state != State::Delegated))
        return fail(ferry::err::kSenderGone);
    if (nowMs >= m_expiresAtMs)
        return fail(ferry::err::kExpired);

    // Вернулся тот, кого уже пускали (обрыв, перезагрузка вкладки): токен
    // возврата из его hello_ok. Использование он уже потратил, владелец его
    // уже пустил — второй раз ни того, ни другого не нужно. Неизвестный или
    // погасший токен — не ошибка: такой hello просто идёт обычным путём.
    QByteArray rejoin = QByteArray::fromBase64(
        hello.value(QStringLiteral("rejoin")).toString().toLatin1(),
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    if (rejoin.size() != int(ferry::kRejoinTokenSize) || !m_rejoin.contains(rejoin))
        rejoin.clear();
    // Старый сокет того же получателя ещё числится живым — сеть «провалилась»
    // без закрытия. Вытесняем его, как вытесняет вернувшийся отправитель.
    if (!rejoin.isEmpty()) {
        if (ClientSession *stale = m_rejoin.value(rejoin).current) {
            detach(stale);
            stale->close();
        }
    }

    if (rejoin.isEmpty() && m_usesLeft == 0)
        return fail(ferry::err::kUsesExhausted);
    // Ждущие решения считаются вместе с теми, кто качает: иначе потолок
    // обходился бы очередью у двери.
    if (m_receivers.size() + m_pending.size() >= m_maxConcurrent)
        return fail(ferry::err::kTooManyReceivers);

    // ---- что клиент умеет и что у него уже есть ----
    receiver->setFeatures(parseFeatures(hello.value(QStringLiteral("features"))));

    // Хеши ещё считаются, а клиент не умеет принимать их сегментами.
    // Пустить его дальше нельзя — проверять чанки ему не против чего, —
    // но и отказ окончательным быть не должен: как только отправитель
    // досчитает, раздача для него станет обычной.
    const bool wantsStream =
        hello.value(QStringLiteral("hashes")).toString() == QLatin1String("stream");
    if (streamingHashes()
        && !(wantsStream && receiver->speaks(ClientSession::FeatureStreamHashes)))
        return fail(ferry::err::kPreparing);
    // Просить сегменты можно только у раздачи с хешами на лету — у
    // обычной их не было и не будет.
    if (wantsStream && !m_streamHashes)
        return fail(ferry::err::kBadMessage);
    receiver->setWantsHashStream(wantsStream);
    receiver->have().reset(m_chunkCount);
    receiver->sent().reset(m_chunkCount);
    receiver->wanted().reset(m_chunkCount);
    receiver->askedToServe().reset(m_chunkCount);

    std::vector<ferry::ChunkRange> haveRanges;
    if (!parseRanges(hello.value(QStringLiteral("have")), m_chunkCount, haveRanges))
        return fail(ferry::err::kBadMessage);
    receiver->have().applyRanges(haveRanges);

    // have_upto — форма из M1. Принимаем её и от нового клиента тоже:
    // диапазоны и префикс не противоречат друг другу, а объединяются.
    // Так недокачка, начатая старым клиентом, продолжается новым без
    // единого лишнего чанка.
    const double haveUptoRaw = hello.value(QStringLiteral("have_upto")).toDouble(0);
    if (haveUptoRaw > 0) {
        const quint64 upto = std::min(quint64(haveUptoRaw), m_chunkCount);
        if (upto > 0)
            receiver->have().setRange({0, uint64_t(upto) - 1});
    }

    receiver->setTransfer(this);
    receiver->setReceiverId(m_nextReceiverId++);

    // Раздача спрашивает владельца — и владелец на связи. Получатель
    // встаёт в очередь: ключ он доказал, но использование не списано,
    // пока ему не сказали «да». Без владельца подтверждать некому, и
    // такого состояния не бывает: при уходе подтверждение либо снято,
    // либо раздача закрыта (onSenderLeft).
    //
    // Одно исключение: связь с отправителем пропала, и он вот-вот
    // вернётся. Пустить без спроса значило бы, что минута без сети
    // отменяет подтверждение. Встаёт в очередь молча — вопрос уйдёт
    // вернувшемуся (sendPendingToOwner), а не вернётся — решит
    // onSenderLeft: снимет подтверждение по freeze или закроет раздачу.
    if (m_requireApproval && (m_owner || senderAway()) && rejoin.isEmpty()) {
        if (m_pending.size() >= kMaxPending) {
            receiver->setTransfer(nullptr);
            return fail(ferry::err::kTooManyReceivers);
        }
        receiver->setRole(ClientSession::Role::Pending);
        m_pending.append(receiver);

        QJsonObject wait;
        wait[QStringLiteral("type")] = QStringLiteral("approval_pending");
        receiver->sendJson(wait);

        if (m_owner)
            m_owner->sendJson(approvalRequestJson(receiver));
        return Admission::Pending;
    }

    return admit(receiver, errorCode, rejoin) ? Admission::Admitted : Admission::Rejected;
}

bool TransferSession::admit(ClientSession *receiver, QString *errorCode, const QByteArray &rejoin)
{
    const auto fail = [&](const char *code) {
        if (errorCode)
            *errorCode = QString::fromLatin1(code);
        receiver->setRole(ClientSession::Role::Unknown);
        receiver->setTransfer(nullptr);
        return false;
    };

    // Проверки политики ещё раз: между hello и решением владельца могло
    // пройти сколько угодно времени, и последнее использование успел
    // забрать кто-то другой.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs >= m_expiresAtMs)
        return fail(ferry::err::kExpired);
    if (rejoin.isEmpty() && m_usesLeft == 0)
        return fail(ferry::err::kUsesExhausted);
    if (m_receivers.size() >= m_maxConcurrent)
        return fail(ferry::err::kTooManyReceivers);

    // Курсор живого потока ставится на конец непрерывного НАЧАЛА, а не на
    // количество принятого: чанк, лежащий за дыркой, живому потоку не
    // помогает — он поедет второй волной.
    const quint64 haveUpto = std::min<quint64>(receiver->have().prefix(), m_chunkCount);

    // Опоздавший больше НЕ получает отказ.
    //
    // Здесь стоял `no_source`, и он был честным ровно до тех пор, пока
    // достать начало тома было неоткуда. Теперь есть откуда:
    // получатель СРАЗУ подписывается на живой поток — хвост и так
    // летит всем, серверу это не стоит ни одного обращения к источнику, —
    // а пропущенное начало тянется второй волной (§6).
    //
    // И одна оговорка, без которой всё это стало бы хуже честного
    // отказа. Вторая волна держится на том, что получатель говорит,
    // что у него есть. Клиент M1 этого не умеет — значит достать ему
    // пропущенное неоткуда, и пустить его дальше значило бы отдать том
    // с дырой молча. Отказ здесь остаётся — но только для него.
    if (!receiver->speaks(ClientSession::FeatureRanges)) {
        if (m_window.firstIndex() > haveUpto)
            return fail(ferry::err::kNoSource);
        // Без отправителя живой волны нет, а ничего, кроме неё, клиент M1
        // не понимает: он простоял бы на краю окна до конца срока.
        // Пока его ждут после обрыва — пускаем: живая волна продолжится,
        // как только он вернётся, а не вернётся — отказ придёт тогда.
        if (!m_sender && !senderAway())
            return fail(ferry::err::kSenderGone);
    }

    // Курсор живой волны ставится туда, где для этого получателя
    // начинается бесплатное: либо сразу за его непрерывным началом,
    // либо с начала окна, если он опоздал сильнее.
    receiver->setRole(ClientSession::Role::Receiver);
    receiver->setTransfer(this);
    receiver->setCursor(std::max<quint64>(haveUpto, m_window.firstIndex()));
    receiver->setAcked(haveUpto);
    receiver->setWindowHint(0);
    receiver->setBackfillHint(0);
    m_receivers.append(receiver);

    // Использование списывается ЗДЕСЬ — то есть после того, как получатель
    // доказал владение ключом (§5) и, если раздача спрашивает, владелец
    // сказал «да». Бот, дёрнувший ссылку без фрагмента, до этой строки не
    // доходит и ничего не сжигает. Вернувшийся по токену его уже потратил.
    if (rejoin.isEmpty() && m_usesLeft > 0)
        --m_usesLeft;
    m_rejoin.remove(rejoin);

    QJsonObject ok;
    ok[QStringLiteral("type")] = QStringLiteral("hello_ok");
    ok[QStringLiteral("receiver_id")] = int(receiver->receiverId());
    ok[QStringLiteral("state")] = stateName(m_state);
    // Честный ответ на вопрос «будут ли меня просить отдавать»: да, если
    // клиент сам объявил, что умеет.
    ok[QStringLiteral("can_seed")] = canSeed(receiver);
    ok[QStringLiteral("chunks")] = double(m_chunkCount);
    ok[QStringLiteral("uses_left")] = m_usesLeft;
    ok[QStringLiteral("from_chunk")] = double(receiver->cursor());
    ok[QStringLiteral("features")] = serverFeatures();
    ok[QStringLiteral("rejoin")] = QString::fromLatin1(issueRejoin(receiver).toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    if (senderAway())
        ok[QStringLiteral("sender_away")] = true;
    receiver->sendJson(ok);

    // Сегменты хешей — сразу за hello_ok и раньше первого фрейма (он
    // уйдёт только из pump ниже). Порядок в сокете и есть порядок
    // прихода, так что хеш каждого чанка окажется у получателя раньше
    // самого чанка.
    sendHashesSoFar(receiver);

    if (m_owner)
        m_owner->sendJson(peersJson());
    m_noSeedSinceMs = hasSeed() ? 0 : m_noSeedSinceMs;
    pump();
    return true;
}

void TransferSession::rejectPending(ClientSession *receiver, const char *reason)
{
    m_pending.removeAll(receiver);
    QJsonObject err;
    err[QStringLiteral("type")] = QStringLiteral("error");
    err[QStringLiteral("reason")] = QString::fromLatin1(reason);
    receiver->sendJson(err);
    receiver->setRole(ClientSession::Role::Unknown);
    receiver->setTransfer(nullptr);
    receiver->close();
}

void TransferSession::detach(ClientSession *session)
{
    // Ждал решения и не дождался. Владельцу — снять вопрос с экрана.
    if (m_pending.contains(session)) {
        m_pending.removeAll(session);
        session->setTransfer(nullptr);
        if (m_owner) {
            QJsonObject gone;
            gone[QStringLiteral("type")] = QStringLiteral("approval_cancel");
            gone[QStringLiteral("receiver")] = int(session->receiverId());
            m_owner->sendJson(gone);
        }
        return;
    }

    // Владелец, вернувшийся без тома, ушёл снова. Раздача от этого не
    // меняется: отдавали и без него.
    if (session == m_owner && session != m_sender) {
        m_owner = nullptr;
        session->setTransfer(nullptr);
        // Вернувшись, он мог снова включить подтверждение. Теперь
        // спрашивать некого — то же правило, что при первом уходе:
        // пускают всех с ключом, ждавшие у двери входят.
        if (m_requireApproval) {
            m_requireApproval = false;
            admitAllPending();
            broadcastToReceivers(stateJson(QDateTime::currentMSecsSinceEpoch()));
        }
        return;
    }

    if (session == m_sender) {
        m_sender = nullptr;
        if (m_owner == session)
            m_owner = nullptr;
        session->setTransfer(nullptr);
        // Сокет пропал, а «ухожу» отправитель не говорил — значит это
        // обрыв, и он обещал вернуться. Ждём его, если без него раздаче
        // жить не на чем; если есть на чём — делегируем сразу, как и при
        // уходе: вернётся — получит раздачу обратно (resumeOwner).
        //
        // Кроме раздачи с подтверждением: делегирование снимает его, и
        // минута без сети пустила бы любого с ключом. Ждём и тогда — сиды
        // отдают уже вошедшим, новые стоят у двери (attachReceiver), а не
        // вернётся — решит onSenderLeft, как при уходе.
        if (m_senderResumes && !m_senderLeaving && (!canDelegate() || m_requireApproval)) {
            senderLost(QDateTime::currentMSecsSinceEpoch());
            return;
        }
        onSenderLeft();
        return;
    }

    // Помним, что ушёл и с чем: не докачавший может вернуться по токену,
    // и раздачу без использований до тех пор не закрываем (checkDelegated).
    const auto rejoin = m_rejoin.find(session->rejoinToken());
    if (rejoin != m_rejoin.end() && rejoin->current == session) {
        rejoin->current = nullptr;
        rejoin->leftAtMs = QDateTime::currentMSecsSinceEpoch();
        rejoin->complete = complete(session);
    }

    m_receivers.removeAll(session);
    session->setTransfer(nullptr);

    // Его могли попросить отдать чанки — теперь их не дождаться.
    // Забываем сразу, а не по сторожевому таймеру: потолок чанков
    // в пути маленький, и четыре повисших просьбы остановили бы
    // вторую волну целиком на пять секунд.
    for (auto it = m_askedOf.begin(); it != m_askedOf.end();) {
        if (it.value() == session) {
            m_backfillInFlight.clear(it.key());
            it = m_askedOf.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_forgotten.begin(); it != m_forgotten.end();)
        it = it.value() == session ? m_forgotten.erase(it) : std::next(it);
    for (ClientSession *r : std::as_const(m_receivers))
        r->setBackfillHint(0);

    // Ушёл самый медленный — остальным можно ехать дальше.
    pump();
    checkDelegated(QDateTime::currentMSecsSinceEpoch());
}

void TransferSession::onSenderLeft()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Ушёл совсем (сказал bye или не вернулся вовремя): ждать больше
    // некого. Дальше — делегировать или закрыть (canDelegate).
    m_senderLostAtMs = 0;
    if (!canDelegate()) {
        // Отправитель ушёл — раздача кончилась. Тем, кто уже выкачал
        // всё, говорить нечего — они и так закончили.
        for (ClientSession *r : std::as_const(m_receivers)) {
            if (!complete(r)) {
                QJsonObject err;
                err[QStringLiteral("type")] = QStringLiteral("error");
                err[QStringLiteral("reason")] = QString::fromLatin1(ferry::err::kSenderGone);
                r->sendJson(err);
            }
        }
        emit needsClosing(this);
        return;
    }

    m_state = State::Delegated;
    m_noSeedSinceMs = 0;

    // Просьбы к отправителю больше не исполнятся. Живую волну запомним
    // ровно по краю окна: если владелец вернётся с томом, она продолжится
    // оттуда, а не с того, что просили и не получили.
    forgetSenderRequests();

    // Клиент без диапазонов (M1, потоковый приём в браузере) берёт только
    // живую волну и только по порядку. Без отправителя она кончается на
    // краю окна, а взять у сидов он не умеет: если окно не доходит до конца
    // тома, ему честнее сказать сразу, чем держать у стоящей полосы.
    if (m_window.endIndex() < m_chunkCount) {
        QList<ClientSession *> stuck;
        for (ClientSession *r : std::as_const(m_receivers)) {
            if (!r->speaks(ClientSession::FeatureRanges) && !complete(r))
                stuck.append(r);
        }
        for (ClientSession *r : std::as_const(stuck)) {
            QJsonObject err;
            err[QStringLiteral("type")] = QStringLiteral("error");
            err[QStringLiteral("reason")] = QString::fromLatin1(ferry::err::kSenderGone);
            r->sendJson(err);
            m_receivers.removeAll(r);
            r->setTransfer(nullptr);
            r->close();
        }
    }

    // Подтверждение снимается: дальше пускают всех, кто знает ключ, в
    // пределах оставшихся использований. Те, кто ждал у двери, входят.
    if (m_requireApproval) {
        m_requireApproval = false;
        admitAllPending();
    }

    qInfo().noquote() << QStringLiteral("отправитель ушёл, раздача живёт на сидах");
    broadcastToReceivers(stateJson(now));
    pump();
    checkDelegated(now);
}

bool TransferSession::canDelegate() const
{
    // Делегировать можно, только если (§5):
    //  — отправитель сам это разрешил;
    //  — список хешей досчитан: без него новому получателю нечем
    //    проверять чанки, а досчитать его некому;
    //  — есть сид, то есть кто-то со всем томом: окно — буфер джиттера,
    //    а не хранилище, и держать в нём файл за ушедшего мы не обещали;
    //  — подтверждение либо выключено, либо снимается при уходе.
    return m_leaveOpen && m_hashesComplete && hasSeed()
           && (!m_requireApproval || m_freezeOnLeave);
}

void TransferSession::senderLost(qint64 nowMs)
{
    m_senderLostAtMs = nowMs;
    forgetSenderRequests();
    qInfo().noquote() << QStringLiteral("связь с отправителем пропала, ждём его %1 с")
                             .arg(kSenderGraceMs / 1000);
    broadcastToReceivers(stateJson(nowMs));
    pump();
}

void TransferSession::checkSenderGrace(qint64 nowMs)
{
    if (m_senderLostAtMs == 0 || m_state != State::Active)
        return;
    if (nowMs - m_senderLostAtMs < kSenderGraceMs)
        return;
    qInfo().noquote() << QStringLiteral("отправитель не вернулся");
    onSenderLeft();
}

void TransferSession::senderLeaving(ClientSession *session)
{
    if (session && session != m_sender)
        return;
    m_senderLeaving = true;
    // Связь уже пропала, и раздача ждёт — дальше ждать нечего.
    if (m_senderLostAtMs != 0 && m_state == State::Active)
        onSenderLeft();
}

void TransferSession::forgetSenderRequests()
{
    for (auto it = m_askedOf.begin(); it != m_askedOf.end();) {
        if (it.value() == nullptr) {
            m_backfillInFlight.clear(it.key());
            it = m_askedOf.erase(it);
        } else {
            ++it;
        }
    }
    m_requestedUpTo = qint64(m_window.endIndex()) - 1;
    for (ClientSession *r : std::as_const(m_receivers))
        r->setBackfillHint(0);
}

QJsonObject TransferSession::approvalRequestJson(const ClientSession *receiver) const
{
    QJsonObject ask;
    ask[QStringLiteral("type")] = QStringLiteral("approval_request");
    ask[QStringLiteral("receiver")] = int(receiver->receiverId());
    ask[QStringLiteral("name")] = receiver->name();
    return ask;
}

void TransferSession::sendPendingToOwner()
{
    if (!m_owner)
        return;
    for (const ClientSession *r : std::as_const(m_pending))
        m_owner->sendJson(approvalRequestJson(r));
}

bool TransferSession::complete(const ClientSession *receiver) const
{
    const quint64 done = receiver->speaks(ClientSession::FeatureRanges)
                             ? receiver->have().cardinality()
                             : receiver->acked();
    return done >= m_chunkCount;
}

bool TransferSession::hasSeed() const
{
    for (const ClientSession *r : m_receivers) {
        if (canSeed(r) && r->backfillStrikes() < kMaxStrikes && complete(r))
            return true;
    }
    return false;
}

QByteArray TransferSession::issueRejoin(ClientSession *receiver)
{
    // Помним не всех: раздачу без лимита могут брать тысячи. Сначала
    // забываем ушедших с томом, потом самых давних из ушедших.
    while (m_rejoin.size() >= kMaxRejoinTokens) {
        auto victim = m_rejoin.end();
        for (auto it = m_rejoin.begin(); it != m_rejoin.end(); ++it) {
            if (it->current)
                continue;
            if (victim == m_rejoin.end() || (it->complete && !victim->complete)
                || (it->complete == victim->complete && it->leftAtMs < victim->leftAtMs))
                victim = it;
        }
        if (victim == m_rejoin.end())
            break;   // все на связи — их не больше потолка одновременных
        m_rejoin.erase(victim);
    }

    QByteArray token(int(ferry::kRejoinTokenSize), 0);
    QRandomGenerator::system()->generate(token.begin(), token.end());
    m_rejoin.insert(token, Rejoin{receiver, 0, false});
    receiver->setRejoinToken(token);
    return token;
}

bool TransferSession::rejoinExpected(qint64 nowMs) const
{
    for (const Rejoin &r : m_rejoin) {
        if (!r.current && !r.complete && nowMs - r.leftAtMs < kRejoinGraceMs)
            return true;
    }
    return false;
}

void TransferSession::checkDelegated(qint64 nowMs)
{
    if (m_state != State::Delegated)
        return;

    // Ни одного сида — отдать недостающее некому. Не закрываем сразу:
    // веб-получатель после обрыва возвращается сам, и секундная заминка
    // у одного не должна стоить раздачи всем.
    if (!hasSeed()) {
        if (m_noSeedSinceMs == 0) {
            m_noSeedSinceMs = nowMs;
        } else if (nowMs - m_noSeedSinceMs >= kSeedGraceMs) {
            qInfo().noquote() << QStringLiteral("раздача без отправителя закрыта: не осталось сидов");
            closeWith(QString::fromLatin1(ferry::err::kSenderGone));
            emit needsClosing(this);
            return;
        }
    } else {
        m_noSeedSinceMs = 0;
    }

    bool allDone = true;
    for (const ClientSession *r : std::as_const(m_receivers)) {
        if (!complete(r)) {
            allDone = false;
            break;
        }
    }

    // Использования кончились, и всем, кто внутри, уже хватает. Новых не
    // пустят, старым ничего не нужно — ссылка гаснет. Если только не ждём
    // недокачавшего, у которого оборвалась связь: он вернётся по токену.
    if (allDone && m_usesLeft == 0 && !rejoinExpected(nowMs)) {
        qInfo().noquote() << QStringLiteral("раздача без отправителя закрыта: использования кончились");
        closeWith(QString::fromLatin1(ferry::err::kUsesExhausted));
        emit needsClosing(this);
        return;
    }

    // Окно без отправителя больше не пополняется, и если сейчас оно никому
    // не нужно — отпускаем память. Следующему придущему всё отдадут сиды.
    if (allDone && m_window.count() > 0)
        m_window.dropBefore(m_window.endIndex());
}

bool TransferSession::resumeOwner(ClientSession *session, bool serve, bool resumes,
                                  QString *errorCode)
{
    const auto fail = [&](const char *code) {
        if (errorCode)
            *errorCode = QString::fromLatin1(code);
        return false;
    };
    if (!m_hasOffer || (m_state != State::Active && m_state != State::Delegated))
        return fail(ferry::err::kNotFound);
    // Отправитель на связи — он и владелец, второго пульта не бывает.
    if (m_sender && !serve)
        return fail(ferry::err::kOwnerConflict);

    // Вернулся отправитель с томом, а старый его сокет ещё числится. Это
    // он же: токен тот же, а прежнее соединение после смены сети мертво,
    // и TCP узнает об этом через минуты. Старое отцепляем сразу — иначе
    // его disconnected пришёл бы позже и закрыл раздачу уже вернувшемуся.
    if (m_sender) {
        ClientSession *old = m_sender;
        m_sender = nullptr;
        if (m_owner == old)
            m_owner = nullptr;
        QJsonObject err;
        err[QStringLiteral("type")] = QStringLiteral("error");
        err[QStringLiteral("reason")] = QString::fromLatin1(ferry::err::kOwnerConflict);
        old->sendJson(err);
        old->setTransfer(nullptr);
        old->setRole(ClientSession::Role::Unknown);
        old->close();
    }

    if (m_owner) {
        QJsonObject err;
        err[QStringLiteral("type")] = QStringLiteral("error");
        err[QStringLiteral("reason")] = QString::fromLatin1(ferry::err::kOwnerConflict);
        m_owner->sendJson(err);
        m_owner->setTransfer(nullptr);
        m_owner->setRole(ClientSession::Role::Unknown);
        m_owner->close();
    }

    m_owner = session;
    session->setTransfer(this);
    if (serve) {
        m_sender = session;
        session->setRole(ClientSession::Role::Sender);
        m_state = State::Active;
        m_noSeedSinceMs = 0;
        m_senderLostAtMs = 0;
        m_senderLeaving = false;
        m_senderResumes = resumes;
        // Что просили у прежнего сокета, с ним и пропало: живую волну
        // попросим заново от края окна.
        forgetSenderRequests();
        m_lastGrowthMs = QDateTime::currentMSecsSinceEpoch();
        broadcastToReceivers(stateJson(m_lastGrowthMs));
    } else {
        session->setRole(ClientSession::Role::Owner);
    }
    return true;
}

void TransferSession::admitAllPending()
{
    const QList<ClientSession *> waiting = m_pending;
    m_pending.clear();
    for (ClientSession *r : waiting) {
        QString code;
        if (!admit(r, &code)) {
            QJsonObject err;
            err[QStringLiteral("type")] = QStringLiteral("error");
            err[QStringLiteral("reason")] = code;
            r->sendJson(err);
            r->close();
        }
    }
}

bool TransferSession::onApprove(const QJsonObject &msg)
{
    const bool ok = msg.value(QStringLiteral("ok")).toBool(false);
    const bool all = msg.value(QStringLiteral("all")).toBool(false);
    const QJsonValue idValue = msg.value(QStringLiteral("receiver"));
    if (!all && !idValue.isDouble())
        return false;

    // «Пускать всех дальше»: подтверждение выключается, и очередь
    // входит целиком.
    if (all && ok) {
        m_requireApproval = false;
        admitAllPending();
        if (m_owner)
            m_owner->sendJson(stateJson(QDateTime::currentMSecsSinceEpoch()));
        return true;
    }

    // Ответ мог разминуться с уходом того, о ком он: это не ошибка.
    ClientSession *target = nullptr;
    for (ClientSession *r : std::as_const(m_pending)) {
        if (int(r->receiverId()) == idValue.toInt(-1)) {
            target = r;
            break;
        }
    }
    if (!target)
        return true;

    if (!ok) {
        rejectPending(target, ferry::err::kApprovalDenied);
        return true;
    }
    m_pending.removeAll(target);
    QString code;
    if (!admit(target, &code)) {
        QJsonObject err;
        err[QStringLiteral("type")] = QStringLiteral("error");
        err[QStringLiteral("reason")] = code;
        target->sendJson(err);
        target->close();
    }
    return true;
}

bool TransferSession::onPolicy(const QJsonObject &msg, qint64 nowMs)
{
    if (m_state != State::Active && m_state != State::Delegated)
        return false;

    // Неверное значение отвергает сообщение целиком, пока ничего не
    // поменяно.
    const QString onLeave = msg.value(QStringLiteral("on_owner_leave")).toString();
    if (msg.contains(QStringLiteral("on_owner_leave"))
        && onLeave != QLatin1String("close") && onLeave != QLatin1String("freeze"))
        return false;

    // Использования — новое значение остатка, а не прибавка: владелец
    // видит «осталось 2» и ставит «осталось 5». null — без лимита.
    if (msg.contains(QStringLiteral("uses"))) {
        const QJsonValue uses = msg.value(QStringLiteral("uses"));
        if (uses.isNull()) {
            m_usesLeft = -1;
        } else {
            const double n = uses.toDouble(-1);
            if (n < 0 || n != std::floor(n) || n > 1e6)
                return false;
            m_usesLeft = int(n);
        }
    }
    // Срок — сколько жить начиная с этой минуты. Потолок тот же, что у
    // offer: неделя, считая от сейчас.
    if (msg.contains(QStringLiteral("ttl_ms"))) {
        const double ttl = msg.value(QStringLiteral("ttl_ms")).toDouble(-1);
        if (ttl < 0)
            return false;
        m_expiresAtMs = nowMs + std::clamp<qint64>(qint64(ttl), 60 * 1000, kMaxTtlMs);
    }
    if (msg.contains(QStringLiteral("require_approval")))
        m_requireApproval = msg.value(QStringLiteral("require_approval")).toBool(false);
    if (msg.contains(QStringLiteral("leave_open")))
        m_leaveOpen = msg.value(QStringLiteral("leave_open")).toBool(false);
    if (msg.contains(QStringLiteral("on_owner_leave")))
        m_freezeOnLeave = onLeave == QLatin1String("freeze");

    // Подтверждение выключили — ждущим больше нечего ждать.
    if (!m_requireApproval && !m_pending.isEmpty()) {
        QJsonObject all;
        all[QStringLiteral("ok")] = true;
        all[QStringLiteral("all")] = true;
        onApprove(all);
    }

    const QJsonObject state = stateJson(nowMs);
    if (m_owner)
        m_owner->sendJson(state);
    broadcastToReceivers(state);
    return true;
}

void TransferSession::revoke()
{
    qInfo().noquote() << QStringLiteral("владелец закрыл доступ");
    closeWith(QString::fromLatin1(ferry::err::kRevoked));
    emit needsClosing(this);
}

bool TransferSession::onHashes(const QJsonObject &msg, QString *errorCode)
{
    const auto fail = [&](const char *code) {
        if (errorCode)
            *errorCode = QString::fromLatin1(code);
        return false;
    };
    if (!m_hasOffer || m_state != State::Active || !m_streamHashes || m_hashesComplete)
        return fail(ferry::err::kBadMessage);

    // Сегменты идут строго встык: следующий начинается там, где кончился
    // предыдущий. Иначе «сколько посчитано» перестало бы быть одним
    // числом, а правило «чанк не раньше хеша» — проверяемым.
    const double fromRaw = msg.value(QStringLiteral("from")).toDouble(-1);
    const double countRaw = msg.value(QStringLiteral("count")).toDouble(-1);
    if (fromRaw < 0 || countRaw < 1 || countRaw > double(ferry::kHashSegmentMax))
        return fail(ferry::err::kBadMessage);
    const quint64 from = quint64(fromRaw);
    const quint64 count = quint64(countRaw);
    if (from != m_hashedUpTo || from + count > m_chunkCount)
        return fail(ferry::err::kBadMessage);

    // Сам сегмент — непрозрачные байты под K_meta. Сервер проверяет только
    // длину: 32 байта на хеш плюс тег GCM.
    const QString data = msg.value(QStringLiteral("data")).toString();
    const QByteArray raw = QByteArray::fromBase64(
        data.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    if (raw.size() != qint64(count) * 32 + qint64(ferry::kGcmTagSize))
        return fail(ferry::err::kBadMessage);

    QJsonObject out;
    out[QStringLiteral("type")] = QStringLiteral("hashes");
    out[QStringLiteral("from")] = double(from);
    out[QStringLiteral("count")] = double(count);
    out[QStringLiteral("data")] = data;
    m_hashSegments.append(out);
    m_hashedUpTo = from + count;

    // Сразу всем, кто ждёт сегменты. В очереди сокета это встанет раньше
    // любого чанка из этого сегмента: чанки сюда ещё не приходили.
    for (ClientSession *r : std::as_const(m_receivers)) {
        if (r->wantsHashStream())
            r->sendJson(out);
    }
    return true;
}

bool TransferSession::onHashesDone(const QJsonObject &msg, QString *errorCode)
{
    const auto fail = [&](const char *code) {
        if (errorCode)
            *errorCode = QString::fromLatin1(code);
        return false;
    };
    if (!m_hasOffer || m_state != State::Active || !m_streamHashes || m_hashesComplete)
        return fail(ferry::err::kBadMessage);
    if (m_hashedUpTo != m_chunkCount)
        return fail(ferry::err::kBadMessage);

    // Итоговые манифест и список — в том самом виде, в каком их ждёт
    // любой клиент, включая старый. С этой минуты раздача снаружи
    // ничем не отличается от обычной.
    const QByteArray manifest = QByteArray::fromBase64(
        msg.value(QStringLiteral("manifest")).toString().toLatin1(),
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    const QByteArray hashList = QByteArray::fromBase64(
        msg.value(QStringLiteral("hash_list")).toString().toLatin1(),
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    if (manifest.isEmpty() || manifest.size() > 16 * 1024 * 1024)
        return fail(ferry::err::kBadMessage);
    if (hashList.size() != qint64(m_chunkCount) * 32 + qint64(ferry::kGcmTagSize))
        return fail(ferry::err::kBadMessage);

    m_encryptedManifest = manifest;
    m_encryptedHashList = hashList;
    m_hashesComplete = true;

    // Тем, кто ехал с сегментами, — итоговый манифест: по нему они
    // сверяют корень и дописывают его в карту принятого.
    QJsonObject out;
    out[QStringLiteral("type")] = QStringLiteral("hashes_done");
    out[QStringLiteral("manifest")] = msg.value(QStringLiteral("manifest"));
    m_hashesDoneMsg = out;
    for (ClientSession *r : std::as_const(m_receivers)) {
        if (r->wantsHashStream())
            r->sendJson(out);
    }
    return true;
}

void TransferSession::sendHashesSoFar(ClientSession *receiver)
{
    if (!receiver->wantsHashStream())
        return;
    for (const QJsonObject &seg : std::as_const(m_hashSegments))
        receiver->sendJson(seg);
    if (m_hashesComplete && !m_hashesDoneMsg.isEmpty())
        receiver->sendJson(m_hashesDoneMsg);
}

bool TransferSession::onSenderFrame(const QByteArray &frame, QString *errorCode)
{
    const auto fail = [&](const char *code) {
        if (errorCode)
            *errorCode = QString::fromLatin1(code);
        return false;
    };

    if (!m_hasOffer || m_state != State::Active)
        return fail(ferry::err::kBadMessage);
    if (frame.size() < int(ferry::kBinaryHeaderSize))
        return fail(ferry::err::kBadMessage);
    if (quint8(frame.at(0)) != ferry::OpChunk)
        return fail(ferry::err::kBadMessage);

    const quint64 index = readBe64(frame.constData() + 1);
    if (index >= m_chunkCount)
        return fail(ferry::err::kBadMessage);

    // Хеши на лету держатся на одном правиле: чанк не приходит раньше
    // своего хеша. Получатель видит в сокете ровно тот порядок, в каком
    // мы отдаём, и чанк без хеша ему проверить не против чего. Отправитель,
    // нарушивший это, сломан, и разговаривать с ним дальше не о чем.
    if (m_streamHashes && index >= m_hashedUpTo)
        return fail(ferry::err::kBadMessage);

    // Длина обязана сойтись ровно: чанк плюс тег GCM. Проверка нужна не
    // ради аккуратности, а ради памяти — иначе отправитель мог бы прислать
    // «чанк» на гигабайт и занять им окно.
    const ferry::ChunkPlan plan = ferry::planWith(m_totalBytes, m_chunkSize);
    const qint64 expect = qint64(ferry::kBinaryHeaderSize) + plan.sizeOf(index)
                          + qint64(ferry::kGcmTagSize);
    if (frame.size() != expect)
        return fail(ferry::err::kBadMessage);

    // Чанк второй волны: мы его просили полосой backfill. Уезжает
    // сразу тем, кто ждёт, и нигде не оседает.
    if (m_backfillInFlight.has(index)) {
        m_backfillInFlight.clear(index);
        m_lastGrowthMs = QDateTime::currentMSecsSinceEpoch();
        m_backfillArrivedMs = m_lastGrowthMs;
        deliverBackfillFrame(index, frame, false);
        pump();
        return true;
    }

    // Повтор того, что уже уехало получателям, — не ошибка: отправитель мог
    // не успеть увидеть наш need и прислать диапазон дважды.
    if (index < m_window.endIndex())
        return true;

    m_lastGrowthMs = QDateTime::currentMSecsSinceEpoch();
    if (!m_window.append(index, frame)) {
        // Дырка в потоке. В M1 отправитель шлёт строго по порядку, и всё
        // остальное означает сломанного клиента.
        return fail(ferry::err::kBadMessage);
    }

    pump();
    return true;
}

void TransferSession::onReceiverAck(ClientSession *receiver, quint64 upto)
{
    // ack — это «записал на диск», а не «получил в сокет». Курсором он не
    // управляет (курсором управляем мы), но именно по нему отправитель
    // видит честный прогресс у клиента, не умеющего диапазонов.
    receiver->setAcked(std::min(upto, m_chunkCount));
}

bool TransferSession::onReceiverHave(ClientSession *receiver, const QJsonObject &msg)
{
    std::vector<ferry::ChunkRange> ranges;
    if (!parseRanges(msg.value(QStringLiteral("ranges")), m_chunkCount, ranges))
        return false;

    // Только добавляем. Получатель не может «разыметь» чанк: снятие битов
    // по его слову означало бы, что чужое сообщение способно заставить
    // сервер переслать уже доставленное, сколько угодно раз.
    receiver->have().applyRanges(ranges);
    return true;
}

bool TransferSession::onPeerFrame(ClientSession *peer, const QByteArray &frame,
                                  QString *errorCode)
{
    const auto fail = [&](const char *code) {
        if (errorCode)
            *errorCode = QString::fromLatin1(code);
        return false;
    };

    if (!m_hasOffer || (m_state != State::Active && m_state != State::Delegated))
        return fail(ferry::err::kBadMessage);
    if (frame.size() < int(ferry::kBinaryHeaderSize))
        return fail(ferry::err::kBadMessage);
    if (quint8(frame.at(0)) != ferry::OpChunk)
        return fail(ferry::err::kBadMessage);

    const quint64 index = readBe64(frame.constData() + 1);
    if (index >= m_chunkCount)
        return fail(ferry::err::kBadMessage);

    // Принимаем только то, что сами попросили ИМЕННО У НЕГО.
    //
    // Без этой проверки любой подключившийся мог бы подмешивать
    // байты в чужую раздачу — и хотя получатель всё равно проверит хеш
    // и выбросит мусор, трафик и время были бы потрачены чужими.
    //
    // «Попросили» — хоть когда-то, а не обязательно сейчас. Сторож второй
    // волны (forgetStalledBackfill) забывает просьбы к сиду, который не
    // уложился в срок, и просит заново — а сид с медленным аплоадом их уже
    // везёт и привезёт, иногда и дважды. Опоздавший чанк — наш же заказ:
    // рвать за него связь значило бы потерять сида, который просто
    // медленный, а с ним, без отправителя, и раздачу.
    if (!peer->askedToServe().has(index))
        return fail(ferry::err::kBadMessage);

    const ferry::ChunkPlan plan = ferry::planWith(m_totalBytes, m_chunkSize);
    const qint64 expect = qint64(ferry::kBinaryHeaderSize) + plan.sizeOf(index)
                          + qint64(ferry::kGcmTagSize);
    if (frame.size() != expect)
        return fail(ferry::err::kBadMessage);

    // Пришёл чанк, просьбу о котором сторож уже сбросил, — от того же, у
    // кого просили. Значит сид не молчал, а вёз, и теперь повезёт ещё и
    // повтор. Срок сторожа мал для этого канала: удваиваем, иначе каждый
    // сброс добавлял бы сиду очередь повторов, и полезная скорость падала
    // бы всё ниже.
    const auto forgot = m_forgotten.constFind(index);
    if (forgot != m_forgotten.cend() && forgot.value() == peer) {
        m_backfillStallMs = std::min(kMaxBackfillStallMs, m_backfillStallMs * 2);
        m_forgotten.clear();
    }

    // Просьба к нему ещё числится — закрываем её. Если чанк тем временем
    // попросили у другого, его просьбу не трогаем: чей придёт первым, тот и
    // уедет, второй уже никому не нужен (deliverBackfillFrame смотрит sent).
    if (m_backfillInFlight.has(index) && m_askedOf.value(index, nullptr) == peer) {
        m_backfillInFlight.clear(index);
        m_askedOf.remove(index);
    }
    m_servedBy.insert(index, {peer, QDateTime::currentMSecsSinceEpoch()});
    m_lastGrowthMs = QDateTime::currentMSecsSinceEpoch();
    m_backfillArrivedMs = m_lastGrowthMs;

    deliverBackfillFrame(index, frame, true);
    pump();
    return true;
}

void TransferSession::onBadChunk(ClientSession *receiver, quint64 index, const QString &reason)
{
    if (index >= m_chunkCount)
        return;

    // Забываем, что этот чанк уже ехал, и откатываем подсказку
    // получателя туда же: иначе планировщик прошёл бы мимо дырки
    // вперёд и больше к ней не вернулся.
    m_backfillInFlight.clear(index);
    m_askedOf.remove(index);
    receiver->sent().clear(index);
    if (index < receiver->backfillHint())
        receiver->setBackfillHint(index);
    if (index < receiver->windowHint())
        receiver->setWindowHint(index);

    // Жалоба записывается тому, кто этот чанк прислал. Отправитель
    // счётчика не имеет сознательно: отказаться от него значило бы
    // остаться вовсе без источника, а если врёт он, то том и так не соберётся.
    const auto served = m_servedBy.value(index, {nullptr, 0});
    if (served.first && m_receivers.contains(served.first)) {
        served.first->addBackfillStrike();
        if (served.first->backfillStrikes() == kMaxStrikes) {
            qInfo().noquote()
                << QStringLiteral("получатель %1 больше не спрашивается как источник")
                       .arg(served.first->receiverId());
        }
    }
    m_servedBy.remove(index);

    // В журнал это стоит писать: одиночный отказ ничего не значит, а
    // поток отказов — единственный след источника, который врёт. Имени
    // раздачи здесь нет — только номер получателя внутри неё.
    qInfo().noquote() << QStringLiteral("чанк %1 отвергнут получателем %2 (%3)")
                             .arg(index)
                             .arg(receiver->receiverId())
                             .arg(reason.isEmpty() ? QStringLiteral("без причины")
                                                   : reason);
}

bool TransferSession::onReceiverRequest(ClientSession *receiver, const QJsonObject &msg)
{
    std::vector<ferry::ChunkRange> ranges;
    if (!parseRanges(msg.value(QStringLiteral("ranges")), m_chunkCount, ranges))
        return false;

    receiver->wanted().applyRanges(ranges);
    return true;
}

quint64 TransferSession::slowestCursor() const
{
    if (m_receivers.isEmpty()) {
        // Получателей ещё нет — значит НИЧЕГО не прочитано, и точка отсчёта
        // это начало тома, а не начало окна.
        //
        // Здесь была ошибка, которую стоит описать, чтобы не вернуться к
        // ней: если возвращать firstIndex(), окно начинает гнаться за
        // собственным хвостом. Отправитель заливает первые 256 МиБ, окно
        // вытесняет голову, firstIndex растёт, планировщик видит новое
        // место и просит ещё — и так весь том, пока никто не подключён.
        // Первый же получатель после этого получает no_source на пустом
        // месте, а тот, кто успел подключиться, молча выпадает из окна.
        return 0;
    }
    quint64 slowest = m_chunkCount;
    for (const ClientSession *r : m_receivers)
        slowest = std::min(slowest, r->cursor());
    return slowest;
}

void TransferSession::tick(qint64 nowMs)
{
    if (!m_hasOffer || (m_state != State::Active && m_state != State::Delegated))
        return;

    forgetStalledBackfill(nowMs);

    // Кто что прислал — помним недолго: жалоба приходит сразу за
    // чанком, а держать запись на каждый чанк тома — это уже хранилище.
    for (auto it = m_servedBy.begin(); it != m_servedBy.end();) {
        if (nowMs - it.value().second > kServedByTtlMs)
            it = m_servedBy.erase(it);
        else
            ++it;
    }

    if (m_state == State::Delegated) {
        checkDelegated(nowMs);
        return;
    }
    if (m_senderLostAtMs != 0) {
        checkSenderGrace(nowMs);
        return;
    }
    if (!m_sender)
        return;

    // Ждём ли мы чего-то от отправителя прямо сейчас.
    const bool outstanding = m_requestedUpTo >= qint64(m_window.endIndex());
    if (!outstanding)
        return;
    if (nowMs - m_lastGrowthMs < kStallMs)
        return;

    // Просим заново ровно то, что не доехало. Отправитель, который просто
    // медленный, от повторной просьбы не пострадает: он пришлёт то же
    // самое, а повтор уже приехавшего сервер отбрасывает молча.
    m_requestedUpTo = qint64(m_window.endIndex()) - 1;
    m_lastGrowthMs = nowMs;
    requestFromSender();
}

void TransferSession::forgetStalledBackfill(qint64 nowMs)
{
    // Тот же сторож, но для второй волны, и без него здесь хуже, чем
    // с живой: потолок чанков в пути маленький, и одна потерянная
    // просьба заняла бы четверть полосы навсегда.
    //
    // Срок считается от последнего признака жизни полосы — просьбы или
    // пришедшего чанка — и не короче, чем чанк идёт по медленному аплоаду:
    // сид отдаёт своим каналом, а он бывает и 150 КБ/с (мобильная сеть,
    // ADSL). Мегабайтный чанк едет так семь секунд, и сторож на пяти
    // забывал бы всё, что сид честно везёт.
    //
    // Если и такой срок мал — сброшенное всё же приходит, — он удваивается
    // (onPeerFrame), до kMaxBackfillStallMs.
    if (m_backfillInFlight.empty() || m_backfillAskedMs == 0)
        return;
    if (nowMs - std::max(m_backfillAskedMs, m_backfillArrivedMs) < m_backfillStallMs)
        return;
    m_forgotten = m_askedOf;
    m_backfillInFlight.reset(m_chunkCount);
    m_askedOf.clear();
    m_backfillAskedMs = 0;
    // Подсказку откатываем только сендерную: то, что уже ушло из
    // окна, ушло честно и пересылать его незачем.
    for (ClientSession *r : std::as_const(m_receivers))
        r->setBackfillHint(0);
}

void TransferSession::pump()
{
    if (!m_hasOffer || (m_state != State::Active && m_state != State::Delegated))
        return;

    // 1. Отдать каждому получателю то, что для него уже лежит в окне.
    //    У каждого свой курсор и свой канал; быстрый не ждёт медленного.
    QList<ClientSession *> lost;
    for (ClientSession *r : std::as_const(m_receivers)) {
        // Выпал из окна — подхватываем живую волну с начала окна,
        // а всё, что между ним и курсором, уходит во вторую волну.
        //
        // Здесь стоял отказ no_source. Дроп-бихайнд теперь работает так,
        // как задумано в §6: отставший не выбывает, а переходит в догон,
        // и один медленный клиент не придерживает группу.
        if (r->cursor() < m_window.firstIndex()) {
            if (!r->speaks(ClientSession::FeatureRanges)) {
                lost.append(r);
                continue;
            }
            r->setCursor(m_window.firstIndex());
        }

        while (r->cursor() < m_window.endIndex() && r->pendingBytes() < kSocketHighWater) {
            const QByteArray frame = m_window.at(r->cursor());
            if (frame.isEmpty())
                break;
            // Живая волна не пересылает то, что у получателя уже есть:
            // при докачке с дырками это целые мегабайты впустую. И то, что
            // уже уехало второй волной: без отправителя вторая волна идёт
            // и впереди курсора, и вернувшийся источник иначе повторил бы её.
            if (!r->have().has(r->cursor()) && !r->sent().has(r->cursor())) {
                r->sendBinary(frame);
                r->sent().set(r->cursor());
                r->countFromWindow();
            }
            r->setCursor(r->cursor() + 1);
        }

        // 1б. Вторая волна, источник № 1: недостающее, которое ещё в окне.
        serveBackfillFromWindow(r);
    }
    for (ClientSession *r : std::as_const(lost)) {
        QJsonObject err;
        err[QStringLiteral("type")] = QStringLiteral("error");
        err[QStringLiteral("reason")] = QString::fromLatin1(ferry::err::kNoSource);
        r->sendJson(err);
        m_receivers.removeAll(r);
        r->setTransfer(nullptr);
        r->close();
    }

    // 2. Попросить у отправителя следующее.
    //
    //    Заметьте, чего здесь НЕТ: выбрасывания прочитанного. Раньше тут
    //    стоял dropBefore(slowestCursor()) — «все прочли, можно
    //    отпускать», — и это была ошибка. Пока в окне есть место, начало
    //    тома стоит держать: подключившийся через секунду получатель
    //    иначе упирался в no_source на пустом месте, хотя весь том
    //    помещался в окно целиком.
    //
    //    Голову вытесняет только нехватка места, внутри RingWindow::evict,
    //    и ровно настолько, насколько её не хватает. А зайти дальше, чем
    //    окно способно удержать, планировщику не даёт кредит: он не
    //    просит у отправителя больше, чем slowest + вместимость окна.
    requestFromSender();

    // 3. И то, чего в окне уже нет, — отдельной полосой и с потолком.
    requestBackfill();

    // 4. Разбивка по источникам — каждому своя. Здесь, а не на секундном
    //    такте: том в двести мегабайт по локальной сети уезжает быстрее,
    //    чем такт успевает ткнуть, и получатель так и не узнавал бы, кто
    //    ему всё это привёз.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (ClientSession *r : std::as_const(m_receivers)) {
        if (!r->speaks(ClientSession::FeatureRanges))
            continue;
        if (now - r->lastStatsMs() < kStatsIntervalMs)
            continue;
        r->setLastStatsMs(now);
        r->sendJson(statsJson(r));
    }
}

quint64 TransferSession::nextMissing(const ClientSession *receiver, quint64 from) const
{
    if (from >= m_chunkCount)
        return m_chunkCount;

    // Если получатель прислал request, мы отдаём только то, что он
    // просил. Не прислал — считаем, что нужен весь том: именно так
    // ведёт себя клиент M1, и оставить его без второй волны значило бы
    // сломать ему ровно тот случай, ради которого всё делалось.
    // Не умеет диапазонов — значит его have у нас всегда пусто, и любой
    // чанк выглядит недостающим. Засыпать его повторами нельзя.
    if (!receiver->speaks(ClientSession::FeatureRanges))
        return m_chunkCount;

    const bool asked = !receiver->wanted().empty();
    quint64 i = from;
    while (i < m_chunkCount) {
        i = receiver->have().firstMissing(i);
        if (i >= m_chunkCount)
            return m_chunkCount;
        // Уже отправленное недостающим не считается — см. ClientSession::sent().
        if (!receiver->sent().has(i) && (!asked || receiver->wanted().has(i)))
            return i;
        ++i;
    }
    return m_chunkCount;
}

void TransferSession::serveBackfillFromWindow(ClientSession *receiver)
{
    // Источник № 1 из §6 и самый дешёвый: чанк уже в оперативке,
    // источнику за ним идти не надо. Два случая: получатель
    // подключился, когда окно уже ушло вперёд, и докачка с дырками.
    quint64 i = std::max<quint64>(receiver->windowHint(), m_window.firstIndex());
    while (receiver->pendingBytes() < kSocketHighWater) {
        i = nextMissing(receiver, i);
        if (i >= receiver->cursor() || i >= m_window.endIndex())
            break;
        if (!m_window.contains(i)) {
            ++i;
            continue;
        }
        const QByteArray frame = m_window.at(i);
        if (frame.isEmpty())
            break;
        receiver->sendBinary(frame);
        receiver->sent().set(i);
        receiver->countFromWindow();
        ++i;
        // Подсказка двигается ТОЛЬКО за успешной отправкой и только
        // вперёд. Без этого каждый оборот цикла слал бы одно и то же
        // заново, пока не придёт очередной have, — а это целое окно
        // повторов за каждые триста миллисекунд.
        receiver->setWindowHint(i);
    }
}

bool TransferSession::canSeed(const ClientSession *receiver)
{
    return receiver && receiver->speaks(ClientSession::FeatureBackfill);
}

ClientSession *TransferSession::pickPeerFor(quint64 index, const ClientSession *forWhom) const
{
    // Самый свободный из тех, у кого этот чанк есть.
    //
    // «Свободный» меряется очередью в его сокет — то есть тем, сколько мы
    // ему сами ещё не додали. Прямой меры его аплоада у нас нет и быть не
    // может, а эта хотя бы не даёт нагрузить того, кто и так не поспевает.
    ClientSession *best = nullptr;
    for (ClientSession *p : m_receivers) {
        if (p == forWhom)
            continue;
        if (!canSeed(p))
            continue;
        if (p->backfillStrikes() >= kMaxStrikes)
            continue;
        if (!p->have().has(index))
            continue;
        if (p->pendingBytes() >= kSocketHighWater)
            continue;
        if (!best || p->pendingBytes() < best->pendingBytes())
            best = p;
    }
    return best;
}

void TransferSession::requestBackfill()
{
    if (m_chunkCount == 0)
        return;

    const auto full = [this] {
        return qint64(m_backfillInFlight.cardinality()) >= kBackfillInFlight;
    };
    if (full())
        return;

    // Собираем то, чего не хватает хоть кому-то и чего уже нет в окне.
    //
    // Здесь же получается коалесцирование, и оно досталось даром: один и
    // тот же индекс, нужный двоим, попадает в множество «в пути» один
    // раз — и спрашивается у источника один раз.
    QJsonArray senderRanges;
    QHash<ClientSession *, QJsonArray> peerRanges;

    for (ClientSession *r : std::as_const(m_receivers)) {
        if (full())
            break;
        // У кого сокет и так полон — не просим: чанку второй волны негде
        // будет приземлиться, он не ложится в окно.
        if (r->pendingBytes() >= kSocketHighWater)
            continue;

        // Пока отправитель на связи, всё, что за курсором, привезёт живая
        // волна. Без него живой волны нет, и вторая тянет том целиком.
        const quint64 edge = m_sender ? r->cursor() : m_chunkCount;
        quint64 i = r->backfillHint();
        while (!full()) {
            i = nextMissing(r, i);
            if (i >= edge)
                break;
            if (m_window.contains(i) || m_backfillInFlight.has(i)) {
                ++i;
                continue;
            }

            // Пир вперёд отправителя. Это и есть тезис продукта: чанк,
            // который уже есть у кого-то из группы, не должен стоить
            // отправителю второй заливки.
            ClientSession *peer = pickPeerFor(i, r);
            if (!peer && !m_sender) {
                ++i;
                continue;   // взять неоткуда — попробуем на следующем обороте
            }

            m_backfillInFlight.set(i);
            m_askedOf.insert(i, peer);
            if (peer)
                peer->askedToServe().set(i);
            r->setBackfillHint(i);

            QJsonArray range;
            range.append(double(i));
            range.append(double(i));
            if (peer)
                peerRanges[peer].append(range);
            else
                senderRanges.append(range);
            ++i;
        }
    }

    for (auto it = peerRanges.constBegin(); it != peerRanges.constEnd(); ++it) {
        QJsonObject serve;
        serve[QStringLiteral("type")] = QStringLiteral("serve");
        serve[QStringLiteral("ranges")] = it.value();
        serve[QStringLiteral("budget_bytes")] =
            double(qint64(it.value().size()) * qint64(m_chunkSize));
        it.key()->sendJson(serve);
    }

    if (!senderRanges.isEmpty() && m_sender) {
        QJsonObject need;
        need[QStringLiteral("type")] = QStringLiteral("need");
        need[QStringLiteral("ranges")] = senderRanges;
        need[QStringLiteral("budget_bytes")] =
            double(qint64(senderRanges.size()) * qint64(m_chunkSize));
        need[QStringLiteral("lane")] = QStringLiteral("backfill");
        m_sender->sendJson(need);
    }

    if (!peerRanges.isEmpty() || !senderRanges.isEmpty())
        m_backfillAskedMs = QDateTime::currentMSecsSinceEpoch();
}

void TransferSession::deliverBackfillFrame(quint64 index, const QByteArray &frame, bool fromPeer)
{
    // Уезжает всем, кому нужен, и нигде не оседает.
    //
    // В окно такой чанк класть нельзя принципиально: окно непрерывно,
    // и догон опоздавшего выталкивал бы из него тех, кто идёт
    // вовремя, — и они тоже становились бы опоздавшими. Именно
    // поэтому потолок на число чанков в пути маленький: всё, что
    // попросили, обязано уехать прямо сейчас.
    for (ClientSession *r : std::as_const(m_receivers)) {
        if (!r->speaks(ClientSession::FeatureRanges))
            continue;
        if (r->have().has(index) || r->sent().has(index))
            continue;   // есть или уже едет — второй копией (опоздавшей) не засыпаем
        if (m_sender && index >= r->cursor())
            continue;   // это ему привезёт живая волна
        r->sendBinary(frame);
        r->sent().set(index);
        if (fromPeer)
            r->countFromPeer();
        else
            r->countFromSender();
    }
}

void TransferSession::requestFromSender()
{
    if (!m_sender || m_chunkCount == 0)
        return;

    const quint64 capacity = m_window.capacityChunks();
    const quint64 limit = std::min(m_chunkCount, slowestCursor() + capacity);  // граница, не включая

    quint64 from = m_window.endIndex();
    if (m_requestedUpTo >= 0 && quint64(m_requestedUpTo) + 1 > from)
        from = quint64(m_requestedUpTo) + 1;
    if (from >= limit)
        return;

    const quint64 to = limit - 1;

    // Гистерезис: пока в пути больше четверти окна, новый need не шлём.
    // Без этого мы просили бы по чанку на каждый пришедший чанк и залили
    // бы отправителя болтовнёй на ровном месте.
    const bool nothingInFlight = m_requestedUpTo < qint64(m_window.endIndex());
    const quint64 addition = to - (m_requestedUpTo < 0 ? from : quint64(m_requestedUpTo));
    if (!nothingInFlight && addition < std::max<quint64>(1, capacity / 4))
        return;

    QJsonArray range;
    range.append(double(from));
    range.append(double(to));
    QJsonArray ranges;
    ranges.append(range);

    QJsonObject need;
    need[QStringLiteral("type")] = QStringLiteral("need");
    need[QStringLiteral("ranges")] = ranges;
    need[QStringLiteral("budget_bytes")] = double((to - from + 1) * quint64(m_chunkSize));
    m_sender->sendJson(need);

    m_requestedUpTo = qint64(to);
}

QJsonObject TransferSession::peersJson() const
{
    QJsonArray arr;
    for (const ClientSession *r : m_receivers) {
        // Прогресс считается по КОЛИЧЕСТВУ принятого, а не по префиксу,
        // но только у клиента, который умеет говорить диапазонами. У
        // клиента M1 карты на сервере нет и быть не может — он шлёт
        // только ack, — и для него остаётся прежний счёт по префиксу.
        //
        // Разница появится вместе со второй волной: получатель, тянущий
        // начало тома и одновременно хвост, по префиксу выглядел бы
        // стоящим на нуле, хотя у него уже половина.
        const bool ranges = r->speaks(ClientSession::FeatureRanges);
        const quint64 done = ranges ? r->have().cardinality() : r->acked();

        QJsonObject o;
        o[QStringLiteral("id")] = int(r->receiverId());
        o[QStringLiteral("name")] = r->name();
        o[QStringLiteral("progress")] = m_chunkCount ? double(done) / double(m_chunkCount) : 1.0;
        // Три роли, а не две: отправителю важно видеть разницу между тем,
        // кто качает вместе со всеми, и тем, кто догоняет начало второй
        // волной: второй по прогрессу выглядит отстающим, хотя на самом
        // деле просто пришёл позже.
        QString role = QStringLiteral("leech");
        if (done >= m_chunkCount)
            role = QStringLiteral("seed");
        else if (nextMissing(r, 0) < r->cursor())
            role = QStringLiteral("catching");
        o[QStringLiteral("role")] = role;
        // Откуда этот получатель брал чанки — в штуках, а не в долях:
        // отправитель складывает их по всем получателям, и доли от разных
        // знаменателей сложить было бы нельзя.
        QJsonObject src;
        src[QStringLiteral("window")] = double(r->fromWindow());
        src[QStringLiteral("peers")] = double(r->fromPeers());
        src[QStringLiteral("sender")] = double(r->fromSender());
        o[QStringLiteral("src")] = src;
        arr.append(o);
    }
    QJsonObject o;
    o[QStringLiteral("type")] = QStringLiteral("peers");
    o[QStringLiteral("receivers")] = arr;
    return o;
}

QJsonObject TransferSession::statsJson(const ClientSession *receiver) const
{
    const quint64 total =
        receiver->fromWindow() + receiver->fromPeers() + receiver->fromSender();

    QJsonObject src;
    if (total > 0) {
        src[QStringLiteral("window")] = double(receiver->fromWindow()) / double(total);
        src[QStringLiteral("peers")] = double(receiver->fromPeers()) / double(total);
        src[QStringLiteral("sender")] = double(receiver->fromSender()) / double(total);
    } else {
        src[QStringLiteral("window")] = 0.0;
        src[QStringLiteral("peers")] = 0.0;
        src[QStringLiteral("sender")] = 0.0;
    }

    QJsonObject o;
    o[QStringLiteral("type")] = QStringLiteral("stats");
    o[QStringLiteral("src")] = src;
    return o;
}

void TransferSession::broadcastToReceivers(const QJsonObject &obj)
{
    for (ClientSession *r : std::as_const(m_receivers))
        r->sendJson(obj);
}

void TransferSession::closeWith(const QString &reasonCode)
{
    if (m_state == State::Closed)
        return;
    m_state = State::Closed;

    QJsonObject err;
    err[QStringLiteral("type")] = QStringLiteral("error");
    err[QStringLiteral("reason")] = reasonCode;
    broadcastToReceivers(err);
    if (m_sender)
        m_sender->sendJson(err);
    if (m_owner && m_owner != m_sender) {
        m_owner->sendJson(err);
        m_owner->setTransfer(nullptr);
    }
    m_owner = nullptr;
    for (ClientSession *r : std::as_const(m_pending)) {
        r->sendJson(err);
        r->setTransfer(nullptr);
    }
    m_pending.clear();

    // Разрываем связь в ОБЕ стороны. Сессия сейчас уйдёт в deleteLater, а
    // сокеты переживут её на несколько тактов цикла событий — и первый же
    // disconnected постучался бы по освобождённой памяти.
    for (ClientSession *r : std::as_const(m_receivers))
        r->setTransfer(nullptr);
    m_receivers.clear();
    if (m_sender) {
        m_sender->setTransfer(nullptr);
        m_sender = nullptr;
    }

    m_window.clear();
}
