#include "core/TransferSession.h"

#include <QDateTime>
#include <QJsonArray>
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
    m_mode = msg.value(QStringLiteral("mode")).toString(QStringLiteral("key"));

    // ---- политика (§5) ----
    const QJsonObject policy = msg.value(QStringLiteral("policy")).toObject();
    const QJsonValue uses = policy.value(QStringLiteral("uses"));
    m_usesLeft = uses.isNull() || uses.isUndefined() ? -1 : std::max(1, uses.toInt(-1));

    qint64 ttl = qint64(policy.value(QStringLiteral("ttl_ms")).toDouble(kDefaultTtlMs));
    ttl = std::clamp<qint64>(ttl, 60 * 1000, kMaxTtlMs);
    m_expiresAtMs = QDateTime::currentMSecsSinceEpoch() + ttl;

    m_maxConcurrent = std::clamp(policy.value(QStringLiteral("max_concurrent")).toInt(8), 1, 64);

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
    o[QStringLiteral("state")] = m_sender ? QStringLiteral("active") : QStringLiteral("closed");
    o[QStringLiteral("receivers")] = m_receivers.size();
    o[QStringLiteral("uses_left")] = m_usesLeft;
    o[QStringLiteral("expires_in_ms")] = double(std::max<qint64>(0, m_expiresAtMs - nowMs));
    o[QStringLiteral("require_approval")] = false;   // M4
    return o;
}

void TransferSession::attachSender(ClientSession *sender)
{
    m_sender = sender;
    sender->setRole(ClientSession::Role::Sender);
    sender->setTransfer(this);
}

bool TransferSession::attachReceiver(ClientSession *receiver, const QJsonObject &hello,
                                     qint64 nowMs, QString *errorCode)
{
    const auto fail = [&](const char *code) {
        if (errorCode)
            *errorCode = QString::fromLatin1(code);
        return false;
    };

    if (m_state != State::Active || !m_hasOffer)
        return fail(ferry::err::kSenderGone);
    if (nowMs >= m_expiresAtMs)
        return fail(ferry::err::kExpired);
    if (m_usesLeft == 0)
        return fail(ferry::err::kUsesExhausted);
    if (m_receivers.size() >= m_maxConcurrent)
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

    // Курсор живого потока ставится на конец непрерывного НАЧАЛА, а не на
    // количество принятого: чанк, лежащий за дыркой, живому потоку не
    // помогает — он поедет второй волной.
    quint64 haveUpto = std::min<quint64>(receiver->have().prefix(), m_chunkCount);

    // Опоздавший больше НЕ получает отказ.
    //
    // Здесь стоял `no_source`, и он был честным ровно до тех пор, пока
    // достать начало тома было неоткуда. Теперь есть откуда:
    // получатель СРАЗУ подписывается на живой поток — хвост и так
    // летит всем, серверу это не стоит ни одного обращения к источнику, —
    // а пропущенное начало тянется второй волной (§6).
    //
    // Курсор живой волны ставится туда, где для этого получателя
    // начинается бесплатное: либо сразу за его непрерывным началом,
    // либо с начала окна, если он опоздал сильнее.
    receiver->setRole(ClientSession::Role::Receiver);
    receiver->setTransfer(this);
    receiver->setReceiverId(m_nextReceiverId++);
    //
    // И одна оговорка, без которой всё это стало бы хуже честного
    // отказа. Вторая волна держится на том, что получатель говорит,
    // что у него есть. Клиент M1 этого не умеет — значит достать ему
    // пропущенное неоткуда, и пустить его дальше значило бы отдать том
    // с дырой молча. Отказ здесь остаётся — но только для него.
    if (!receiver->speaks(ClientSession::FeatureRanges) && m_window.firstIndex() > haveUpto)
        return fail(ferry::err::kNoSource);

    receiver->setCursor(std::max<quint64>(haveUpto, m_window.firstIndex()));
    receiver->setAcked(haveUpto);
    receiver->setWindowHint(0);
    receiver->setBackfillHint(0);
    m_receivers.append(receiver);

    // Использование списывается ЗДЕСЬ — то есть после того, как получатель
    // доказал владение ключом (§5). Бот, дёрнувший ссылку без фрагмента,
    // до этой строки не доходит и ничего не сжигает.
    if (m_usesLeft > 0)
        --m_usesLeft;

    return true;
}

void TransferSession::detach(ClientSession *session)
{
    if (session == m_sender) {
        m_sender = nullptr;
        // M1: отправитель ушёл — раздача кончилась. Делегирование сиду
        // (§5, состояние DELEGATED) приходит в M4 вместе с политикой.
        // Тем, кто уже выкачал всё, говорить нечего — они и так закончили.
        for (ClientSession *r : std::as_const(m_receivers)) {
            if (r->cursor() < m_chunkCount) {
                QJsonObject err;
                err[QStringLiteral("type")] = QStringLiteral("error");
                err[QStringLiteral("reason")] = QString::fromLatin1(ferry::err::kSenderGone);
                r->sendJson(err);
            }
        }
        session->setTransfer(nullptr);
        emit needsClosing(this);
        return;
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
    for (ClientSession *r : std::as_const(m_receivers))
        r->setBackfillHint(0);

    // Ушёл самый медленный — остальным можно ехать дальше.
    pump();
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

    if (!m_hasOffer || m_state != State::Active)
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
    if (!m_backfillInFlight.has(index) || m_askedOf.value(index, nullptr) != peer)
        return fail(ferry::err::kBadMessage);

    const ferry::ChunkPlan plan = ferry::planWith(m_totalBytes, m_chunkSize);
    const qint64 expect = qint64(ferry::kBinaryHeaderSize) + plan.sizeOf(index)
                          + qint64(ferry::kGcmTagSize);
    if (frame.size() != expect)
        return fail(ferry::err::kBadMessage);

    m_backfillInFlight.clear(index);
    m_askedOf.remove(index);
    m_servedBy.insert(index, {peer, QDateTime::currentMSecsSinceEpoch()});
    m_lastGrowthMs = QDateTime::currentMSecsSinceEpoch();

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
    if (m_state != State::Active || !m_sender || !m_hasOffer)
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
    if (m_backfillInFlight.empty() || m_backfillAskedMs == 0)
        return;
    if (nowMs - m_backfillAskedMs < kStallMs)
        return;
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
    if (m_state != State::Active || !m_hasOffer)
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
            // при докачке с дырками это целые мегабайты впустую.
            if (!r->have().has(r->cursor())) {
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

        quint64 i = r->backfillHint();
        while (!full()) {
            i = nextMissing(r, i);
            if (i >= r->cursor())
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
        if (r->have().has(index))
            continue;
        if (index >= r->cursor())
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
