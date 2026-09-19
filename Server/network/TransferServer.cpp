#include "network/TransferServer.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <QTimer>
#include <QTcpSocket>
#include <QWebSocket>
#include <QWebSocketServer>

#include "config/Log.h"
#include "config/ServerConfig.h"
#include "core/ChallengeStore.h"
#include "core/Link.h"
#include "core/Protocol.h"
#include "core/TransferRegistry.h"
#include "core/TransferSession.h"
#include "network/ClientSession.h"

namespace {

QByteArray fromB64(const QString &s)
{
    return QByteArray::fromBase64(s.toLatin1(),
                                  QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

// Как часто сервер рассказывает отправителю, кто и насколько выкачал, и
// подгоняет застоявшиеся раздачи. Раз в секунду: чаще человеку не нужно,
// а сторожевой прогон по сессиям не бесплатен.
constexpr int kTickMs = 1000;

} // namespace

TransferServer::TransferServer(quint16 port, TransferRegistry *registry,
                               ChallengeStore *challenges, const ServerConfig &config,
                               QObject *parent)
    : QObject(parent), m_registry(registry), m_challenges(challenges), m_config(config),
      m_port(port)
{
    m_server = new QWebSocketServer(QStringLiteral("Ferry"), QWebSocketServer::NonSecureMode, this);
    connect(m_server, &QWebSocketServer::newConnection, this, &TransferServer::onNewConnection);

    if (m_server->listen(QHostAddress::Any, port))
        qCInfo(lcApp).noquote() << QStringLiteral("WebSocket: порт %1, путь %2")
                                       .arg(port).arg(QLatin1String(ferry::kWsPath));
    else
        qCritical().noquote() << QStringLiteral("WebSocket: не удалось занять порт %1: %2")
                                     .arg(port).arg(m_server->errorString());

    m_tick = new QTimer(this);
    m_tick->setInterval(kTickMs);
    connect(m_tick, &QTimer::timeout, this, &TransferServer::onTick);
    m_tick->start();
}

TransferServer::~TransferServer()
{
    m_server->close();
    qDeleteAll(m_sessions);
    m_sessions.clear();
}

bool TransferServer::isListening() const
{
    return m_server->isListening();
}

void TransferServer::adoptConnection(QTcpSocket *socket)
{
    // Дальше всё идёт обычным путём: QWebSocketServer сам разберёт
    // рукопожатие и выдаст newConnection.
    m_server->handleConnection(socket);
}

bool TransferServer::constantTimeEquals(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size() || a.isEmpty())
        return false;
    quint8 diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= quint8(a[i]) ^ quint8(b[i]);
    return diff == 0;
}

void TransferServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QWebSocket *socket = m_server->nextPendingConnection();

        // Путь проверяем сами: QWebSocketServer принимает любой. Чужой путь
        // почти наверняка означает, что клиент говорит не с тем сервером, и
        // лучше сказать об этом сразу, чем ждать непонятного offer.
        if (socket->requestUrl().path() != QLatin1String(ferry::kWsPath)) {
            socket->close(QWebSocketProtocol::CloseCodeBadOperation,
                          QStringLiteral("unknown path"));
            socket->deleteLater();
            continue;
        }

        auto *session = new ClientSession(socket, this);
        m_sessions.insert(session);

        connect(session, &ClientSession::textReceived, this, &TransferServer::onText);
        connect(session, &ClientSession::binaryReceived, this, &TransferServer::onBinary);
        connect(session, &ClientSession::disconnected, this, &TransferServer::onDisconnected);
        connect(session, &ClientSession::bytesWritten, this, &TransferServer::onBytesWritten);
    }
}

void TransferServer::sendError(ClientSession *session, const char *reason, bool closeAfter)
{
    QJsonObject o;
    o[QStringLiteral("type")] = QStringLiteral("error");
    o[QStringLiteral("reason")] = QString::fromLatin1(reason);
    session->sendJson(o);
    if (closeAfter)
        session->close();
}

void TransferServer::onText(ClientSession *session, const QString &text)
{
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isObject()) {
        sendError(session, ferry::err::kBadMessage);
        return;
    }
    const QJsonObject msg = doc.object();
    const QString type = msg.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("offer")) {
        handleOffer(session, msg);
    } else if (type == QLatin1String("hello")) {
        handleHello(session, msg);
    } else if (type == QLatin1String("ack")) {
        handleAck(session, msg);
    } else if (type == QLatin1String("bye")) {
        session->close();
    } else if (type == QLatin1String("subscribe_live")) {
        // В M1 подписка на живой поток происходит сразу при hello: других
        // режимов у получателя пока нет. Сообщение принимаем молча, чтобы
        // клиент, написанный по §8, работал без оговорок.
        if (session->transfer())
            session->transfer()->pump();
    } else {
        sendError(session, ferry::err::kBadMessage);
    }
}

void TransferServer::handleOffer(ClientSession *session, const QJsonObject &msg)
{
    if (session->role() != ClientSession::Role::Unknown) {
        sendError(session, ferry::err::kBadMessage);
        return;
    }

    const QByteArray id = msg.value(QStringLiteral("id")).toString().toLatin1();
    TransferSession *transfer = m_registry->find(id);
    if (!transfer) {
        sendError(session, ferry::err::kNotFound);
        return;
    }
    // Черновик живёт минуту: если отправитель шёл до WebSocket дольше,
    // раздачи уже нет, и это честнее, чем оживить её задним числом.
    if (transfer->hasOffer() || transfer->sender()) {
        sendError(session, ferry::err::kOwnerConflict);
        return;
    }

    const QByteArray token = fromB64(msg.value(QStringLiteral("owner_token")).toString());
    if (!constantTimeEquals(token, transfer->ownerToken())) {
        sendError(session, ferry::err::kOwnerConflict);
        return;
    }

    QString errorCode;
    if (!transfer->applyOffer(msg, &errorCode)) {
        sendError(session, errorCode.toLatin1().constData());
        return;
    }

    transfer->attachSender(session);

    QJsonObject ok;
    ok[QStringLiteral("type")] = QStringLiteral("offer_ok");
    ok[QStringLiteral("chunks")] = double(transfer->chunkCount());
    session->sendJson(ok);

    qInfo().noquote() << QStringLiteral("раздача принята: %1, %2 чанков")
                             .arg(Log::keepsIdentifiers() ? QString::fromLatin1(id)
                                                          : QStringLiteral("<id скрыт>"))
                             .arg(transfer->chunkCount());

    // Окно наполняется, не дожидаясь получателей: когда первый придёт,
    // начало тома уже будет лежать в оперативке.
    transfer->pump();
}

void TransferServer::handleHello(ClientSession *session, const QJsonObject &msg)
{
    if (session->role() != ClientSession::Role::Unknown) {
        sendError(session, ferry::err::kBadMessage);
        return;
    }

    const QByteArray id = msg.value(QStringLiteral("id")).toString().toLatin1();
    TransferSession *transfer = m_registry->find(id);
    if (!transfer || !transfer->hasOffer()) {
        sendError(session, ferry::err::kNotFound);
        return;
    }

    // ---- доказательство владения ключом (§5) ----
    // Порядок важен: пока эта проверка не прошла, НИЧЕГО не списывается.
    // Превью-бот мессенджера, дёрнувший ссылку без фрагмента, до политики
    // не доходит и одноразовую раздачу не сжигает.
    const QByteArray challenge = fromB64(msg.value(QStringLiteral("challenge")).toString());
    const QByteArray proof = fromB64(msg.value(QStringLiteral("proof")).toString());
    if (challenge.size() != int(ferry::kChallengeSize) || proof.size() != 32) {
        sendError(session, ferry::err::kNeedKey);
        return;
    }
    if (!m_challenges->consume(challenge)) {
        // Challenge не наш, просрочен или уже использован. Повторное
        // предъявление подслушанного ответа не проходит.
        sendError(session, ferry::err::kNeedKey);
        return;
    }
    const QByteArray expected = QMessageAuthenticationCode::hash(challenge, transfer->verifier(),
                                                                 QCryptographicHash::Sha256);
    if (!constantTimeEquals(proof, expected)) {
        sendError(session, ferry::err::kNeedKey);
        return;
    }

    QString name = msg.value(QStringLiteral("name")).toString().trimmed();
    if (name.size() > 64)
        name.truncate(64);
    session->setName(name);

    // Докачка: получатель говорит, сколько чанков подряд у него уже есть.
    const double haveUptoRaw = msg.value(QStringLiteral("have_upto")).toDouble(0);
    const quint64 haveUpto = haveUptoRaw > 0 ? quint64(haveUptoRaw) : 0;

    QString errorCode;
    if (!transfer->attachReceiver(session, haveUpto, QDateTime::currentMSecsSinceEpoch(),
                                  &errorCode)) {
        sendError(session, errorCode.toLatin1().constData());
        return;
    }

    QJsonObject ok;
    ok[QStringLiteral("type")] = QStringLiteral("hello_ok");
    ok[QStringLiteral("receiver_id")] = int(session->receiverId());
    ok[QStringLiteral("state")] = QStringLiteral("active");
    // Сидировать в M1 нельзя никому: reflected backfill приходит в M2.
    // Врать об этом нельзя — получатель должен знать, чего от него не ждут.
    ok[QStringLiteral("can_seed")] = false;
    ok[QStringLiteral("chunks")] = double(transfer->chunkCount());
    ok[QStringLiteral("uses_left")] = transfer->usesLeft();
    ok[QStringLiteral("from_chunk")] = double(session->cursor());
    session->sendJson(ok);

    if (TransferSession *t = session->transfer()) {
        if (t->sender())
            t->sender()->sendJson(t->peersJson());
    }

    qInfo().noquote() << QStringLiteral("получатель подключился к %1")
                             .arg(Log::keepsIdentifiers() ? QString::fromLatin1(id)
                                                          : QStringLiteral("<id скрыт>"));

    transfer->pump();
}

void TransferServer::handleAck(ClientSession *session, const QJsonObject &msg)
{
    TransferSession *transfer = session->transfer();
    if (!transfer || session->role() != ClientSession::Role::Receiver)
        return;
    const double upto = msg.value(QStringLiteral("upto")).toDouble(-1);
    if (upto < 0)
        return;
    transfer->onReceiverAck(session, quint64(upto));
}

void TransferServer::onBinary(ClientSession *session, const QByteArray &data)
{
    TransferSession *transfer = session->transfer();
    if (!transfer || session->role() != ClientSession::Role::Sender) {
        // Бинарь от получателя — это будущий reflected backfill (M2).
        // Сейчас его быть не может, и принимать его молча нельзя: иначе
        // любой подключившийся смог бы подмешивать байты в чужую раздачу.
        sendError(session, ferry::err::kBadMessage);
        return;
    }

    QString errorCode;
    if (!transfer->onSenderFrame(data, &errorCode)) {
        sendError(session, errorCode.toLatin1().constData());
        return;
    }
}

void TransferServer::onBytesWritten(ClientSession *session)
{
    // Сокет разгрузился — самое время дослать следующие чанки. Это и есть
    // backpressure: быстрый получатель едет быстро, медленный медленно, и
    // друг с другом они не связаны.
    if (TransferSession *transfer = session->transfer())
        transfer->pump();
}

void TransferServer::onDisconnected(ClientSession *session)
{
    if (TransferSession *transfer = session->transfer())
        transfer->detach(session);

    m_sessions.remove(session);
    session->deleteLater();
}

void TransferServer::onTick()
{
    m_challenges->sweep();
    m_registry->sweep();

    // Сводка отправителю и страховочный прогон: если из-за гонки сигналов
    // раздача где-то встала, следующий тик её сдвинет.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSet<TransferSession *> seen;
    for (ClientSession *cs : std::as_const(m_sessions)) {
        TransferSession *t = cs->transfer();
        if (!t || seen.contains(t))
            continue;
        seen.insert(t);
        t->tick(now);
        t->pump();
        if (t->sender())
            t->sender()->sendJson(t->peersJson());
    }
}
