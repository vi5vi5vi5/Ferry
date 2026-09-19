#include "core/TransferSession.h"

#include <QDateTime>
#include <QJsonArray>
#include <algorithm>

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
    m_encryptedManifest = QByteArray::fromBase64(
        msg.value(QStringLiteral("manifest")).toString().toLatin1(),
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    m_encryptedHashList = QByteArray::fromBase64(
        msg.value(QStringLiteral("hash_list")).toString().toLatin1(),
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    if (m_encryptedManifest.isEmpty() || m_encryptedManifest.size() > 16 * 1024 * 1024)
        return fail(ferry::err::kBadMessage);
    const qint64 expectHashBytes = qint64(plan.chunkCount) * 32 + qint64(ferry::kGcmTagSize);
    if (m_encryptedHashList.size() != expectHashBytes)
        return fail(ferry::err::kBadMessage);

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
    m_lastGrowthMs = QDateTime::currentMSecsSinceEpoch();
    m_hasOffer = true;
    m_state = State::Active;
    return true;
}

QJsonObject TransferSession::metaJson(qint64 nowMs) const
{
    QJsonObject o;
    o[QStringLiteral("id")] = QString::fromLatin1(m_id);
    o[QStringLiteral("total")] = double(m_totalBytes);
    o[QStringLiteral("chunk_size")] = double(m_chunkSize);
    o[QStringLiteral("chunks")] = double(m_chunkCount);
    o[QStringLiteral("manifest")] = QString::fromLatin1(m_encryptedManifest.toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    o[QStringLiteral("hash_list")] = QString::fromLatin1(m_encryptedHashList.toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
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

bool TransferSession::attachReceiver(ClientSession *receiver, quint64 haveUpto, qint64 nowMs,
                                     QString *errorCode)
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

    haveUpto = std::min(haveUpto, m_chunkCount);

    // Опоздавший. В M1 отдать ему недостающее начало неоткуда: окно — это
    // буфер джиттера на секунды, а backfill от других получателей появится
    // в M2. Отказать честно лучше, чем отдать том с дырой.
    //
    // Заметьте, что докачка проходит эту проверку: у кого начало уже есть,
    // тому его и не нужно.
    if (m_window.firstIndex() > haveUpto)
        return fail(ferry::err::kNoSource);

    receiver->setRole(ClientSession::Role::Receiver);
    receiver->setTransfer(this);
    receiver->setReceiverId(m_nextReceiverId++);
    receiver->setCursor(haveUpto);
    receiver->setAcked(haveUpto);
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
    // Ушёл самый медленный — остальным можно ехать дальше.
    pump();
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

    // Длина обязана сойтись ровно: чанк плюс тег GCM. Проверка нужна не
    // ради аккуратности, а ради памяти — иначе отправитель мог бы прислать
    // «чанк» на гигабайт и занять им окно.
    const ferry::ChunkPlan plan = ferry::planWith(m_totalBytes, m_chunkSize);
    const qint64 expect = qint64(ferry::kBinaryHeaderSize) + plan.sizeOf(index)
                          + qint64(ferry::kGcmTagSize);
    if (frame.size() != expect)
        return fail(ferry::err::kBadMessage);

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
    // видит честный прогресс.
    receiver->setAcked(std::min(upto, m_chunkCount));
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

void TransferSession::pump()
{
    if (m_state != State::Active || !m_hasOffer)
        return;

    // 1. Отдать каждому получателю то, что для него уже лежит в окне.
    //    У каждого свой курсор и свой канал; быстрый не ждёт медленного.
    QList<ClientSession *> lost;
    for (ClientSession *r : std::as_const(m_receivers)) {
        // Выпал из окна. В M1 достать это неоткуда: backfill появится в M2.
        // Молча ждать нельзя — получатель завис бы навсегда, глядя на
        // остановившийся прогресс и не понимая, почему.
        if (r->cursor() < m_window.firstIndex()) {
            lost.append(r);
            continue;
        }
        while (r->cursor() < m_window.endIndex() && r->pendingBytes() < kSocketHighWater) {
            const QByteArray frame = m_window.at(r->cursor());
            if (frame.isEmpty())
                break;
            r->sendBinary(frame);
            r->setCursor(r->cursor() + 1);
        }
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
        QJsonObject o;
        o[QStringLiteral("id")] = int(r->receiverId());
        o[QStringLiteral("name")] = r->name();
        o[QStringLiteral("progress")] =
            m_chunkCount ? double(r->acked()) / double(m_chunkCount) : 1.0;
        o[QStringLiteral("role")] = r->acked() >= m_chunkCount ? QStringLiteral("seed")
                                                               : QStringLiteral("leech");
        arr.append(o);
    }
    QJsonObject o;
    o[QStringLiteral("type")] = QStringLiteral("peers");
    o[QStringLiteral("receivers")] = arr;
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
