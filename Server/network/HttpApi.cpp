#include "network/HttpApi.h"

#include <QDateTime>

#include <algorithm>

#include "config/Log.h"
#include "config/ServerConfig.h"
#include "core/ChallengeStore.h"
#include "core/Link.h"
#include "core/Protocol.h"
#include "core/TransferRegistry.h"
#include "core/TransferSession.h"
#include "network/HttpRequest.h"
#include "network/TransferServer.h"

#ifndef FERRY_COMMIT
#define FERRY_COMMIT "unknown"
#endif
#ifndef FERRY_MODIFIED
#define FERRY_MODIFIED 0
#endif
#ifndef FERRY_BUILD_TIME
#define FERRY_BUILD_TIME "unknown"
#endif

ApiResponse ApiResponse::json(int status, const QJsonObject &body)
{
    ApiResponse r;
    r.status = status;
    r.body = body;
    return r;
}

ApiResponse ApiResponse::error(int status, const char *reason)
{
    QJsonObject o;
    o[QStringLiteral("error")] = QString::fromLatin1(reason);
    return json(status, o);
}

HttpApi::HttpApi(TransferRegistry *registry, ChallengeStore *challenges,
                 const ServerConfig &config, QObject *parent)
    : QObject(parent), m_registry(registry), m_challenges(challenges), m_config(config)
{
}

bool HttpApi::route(const HttpRequest &req, const Responder &plainRespond)
{
    if (!req.path.startsWith(QLatin1String("/api/")))
        return false;

    // CORS — каждому ответу, включая отказы: страница на чужом адресе иначе
    // не увидела бы даже кода ошибки, только «сетевой сбой».
    const QList<QPair<QByteArray, QByteArray>> cors = corsHeaders(req);
    const Responder respond = cors.isEmpty()
        ? plainRespond
        : Responder([plainRespond, cors](const ApiResponse &resp) {
              ApiResponse withCors = resp;
              withCors.headers += cors;
              plainRespond(withCors);
          });

    // Предварительный запрос браузера: можно ли прислать сюда такое. Его
    // вызывают заголовок X-Ferry-Features у GET метаданных и JSON в теле
    // POST. Тело пустое, статус 200 — 204 не дал бы прислать Content-Length,
    // а наш транспорт ставит его всегда.
    if (req.method == "OPTIONS") {
        ApiResponse r;
        r.contentType = QByteArrayLiteral("text/plain; charset=utf-8");
        const bool allowed = std::any_of(cors.cbegin(), cors.cend(), [](const auto &h) {
            return h.first == "Access-Control-Allow-Origin";
        });
        if (allowed) {
            r.headers.append({QByteArrayLiteral("Access-Control-Allow-Methods"),
                              QByteArrayLiteral("GET, POST")});
            r.headers.append({QByteArrayLiteral("Access-Control-Allow-Headers"),
                              QByteArrayLiteral("Content-Type, X-Ferry-Features")});
            // Сутки браузер не переспрашивает: ответ зависит только от
            // настроек релея, а они меняются перезапуском.
            r.headers.append({QByteArrayLiteral("Access-Control-Max-Age"), QByteArrayLiteral("86400")});
        }
        respond(r);
        return true;
    }

    if (req.path == QLatin1String("/api/health")) {
        if (req.method != "GET") {
            respond(ApiResponse::error(405, "method_not_allowed"));
            return true;
        }
        handleHealth(respond);
        return true;
    }

    if (req.path == QLatin1String("/api/transfers")) {
        if (req.method != "POST") {
            respond(ApiResponse::error(405, "method_not_allowed"));
            return true;
        }
        handleCreate(req, respond);
        return true;
    }

    // Отправитель закрывает вкладку (/leave) или закрывает доступ
    // (/revoke). Сокет у него в воркере и забит чанками: bye или revoke
    // встали бы в очередь за мегабайтами, а воркер закроют раньше, чем они
    // дойдут. sendBeacon по HTTP успевает. Без этого релей принял бы уход
    // за обрыв и ещё тридцать секунд ждал бы отправителя назад — а после
    // «Закрыть доступ» всё это время пускал бы новых получателей.
    const bool leave = req.path.endsWith(QLatin1String("/leave"));
    const bool revoke = req.path.endsWith(QLatin1String("/revoke"));
    if (req.path.startsWith(QLatin1String("/api/transfers/")) && (leave || revoke)) {
        if (req.method != "POST") {
            respond(ApiResponse::error(405, "method_not_allowed"));
            return true;
        }
        QString rest = req.path.mid(QStringLiteral("/api/transfers/").size());
        rest.chop(leave ? QStringLiteral("/leave").size() : QStringLiteral("/revoke").size());
        if (!ferry::isValidTransferId(rest.toStdString())) {
            respond(ApiResponse::error(404, ferry::err::kNotFound));
            return true;
        }
        handleOwnerBeacon(rest.toLatin1(), req.body, revoke, respond);
        return true;
    }

    if (req.path.startsWith(QLatin1String("/api/transfers/"))) {
        if (req.method != "GET") {
            respond(ApiResponse::error(405, "method_not_allowed"));
            return true;
        }
        QString rest = req.path.mid(QStringLiteral("/api/transfers/").size());
        bool wantsChallenge = false;
        if (rest.endsWith(QLatin1String("/challenge"))) {
            wantsChallenge = true;
            rest.chop(QStringLiteral("/challenge").size());
        }
        const QByteArray id = rest.toLatin1();
        if (!ferry::isValidTransferId(rest.toStdString())) {
            // Форма id проверяется до поиска: иначе по времени ответа можно
            // было бы отличать «такой раздачи нет» от «id вообще не тот».
            respond(ApiResponse::error(404, ferry::err::kNotFound));
            return true;
        }
        if (wantsChallenge) {
            handleChallenge(id, respond);
        } else {
            // У запроса метаданных нет тела, поэтому клиент говорит,
            // что умеет хеши на лету, заголовком.
            const bool streamClient =
                req.header(QByteArrayLiteral("x-ferry-features"))
                    .contains(ferry::kFeatureStreamHashes);
            handleMeta(id, streamClient, respond);
        }
        return true;
    }

    respond(ApiResponse::error(404, "not_found"));
    return true;
}

QList<QPair<QByteArray, QByteArray>> HttpApi::corsHeaders(const HttpRequest &req) const
{
    QList<QPair<QByteArray, QByteArray>> h;
    if (m_config.corsOrigins.isEmpty())
        return h;
    if (m_config.corsOrigins.contains(QStringLiteral("*"))) {
        h.append({QByteArrayLiteral("Access-Control-Allow-Origin"), QByteArrayLiteral("*")});
        return h;
    }
    // Список: ответ зависит от Origin, и кэш по дороге обязан это знать —
    // иначе отдал бы разрешение одного сайта другому.
    h.append({QByteArrayLiteral("Vary"), QByteArrayLiteral("Origin")});
    const QByteArray origin = req.header(QByteArrayLiteral("origin"));
    if (!origin.isEmpty() && m_config.corsOrigins.contains(QString::fromLatin1(origin)))
        h.append({QByteArrayLiteral("Access-Control-Allow-Origin"), origin});
    return h;
}

void HttpApi::handleCreate(const HttpRequest &req, const Responder &respond)
{
    Q_UNUSED(req)

    // Адрес пира сюда не доходит: HttpFileServer знает его, а мы нет.
    // Для M1 достаточно общих потолков реестра; лимиты по конкретному IP
    // включатся, когда адрес начнёт прокидываться из транспорта вместе с
    // X-Forwarded-For от nginx.
    QString errorCode;
    TransferSession *session = m_registry->create(QStringLiteral("-"), &errorCode);
    if (!session) {
        respond(ApiResponse::error(503, errorCode.toLatin1().constData()));
        return;
    }

    QJsonObject o;
    o[QStringLiteral("id")] = QString::fromLatin1(session->id());
    o[QStringLiteral("owner_token")] = QString::fromLatin1(session->ownerToken().toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    // Публичный адрес нужен клиенту, чтобы собрать ссылку. Если владелец
    // сервера его не задал, клиент подставит тот адрес, по которому пришёл.
    o[QStringLiteral("public_url")] = m_config.publicUrl;
    // Что умеет релей — отправителю нужно знать это ДО offer: от ответа
    // зависит, считать ли хеши заранее или отдавать их на лету. Релей,
    // который этого поля не пришлёт, получит offer по-старому.
    o[QStringLiteral("features")] = TransferServer::serverFeatures();
    respond(ApiResponse::json(201, o));

    qInfo().noquote() << QStringLiteral("раздача заведена: %1")
                             .arg(Log::keepsIdentifiers() ? QString::fromLatin1(session->id())
                                                          : QStringLiteral("<id скрыт>"));
}

void HttpApi::handleMeta(const QByteArray &id, bool streamClient, const Responder &respond)
{
    TransferSession *session = m_registry->find(id);
    // Черновик (POST был, offer ещё нет) снаружи выглядит как отсутствие
    // раздачи: показывать нечего, а знать о его существовании незачем.
    if (!session || !session->hasOffer()) {
        respond(ApiResponse::error(404, ferry::err::kNotFound));
        return;
    }
    // Эта ручка НИЧЕГО НЕ СЖИГАЕТ — ни использования, ни challenge. По ней
    // ходят превью-боты мессенджеров, и это нормально: имени файла здесь
    // нет (оно внутри зашифрованного манифеста), а счётчик не трогается.
    // Отправитель ещё считает хеши, а клиент не умеет принимать их
    // сегментами. 409, а не 404: раздача есть, просто пока не для него.
    if (session->streamingHashes() && !streamClient) {
        respond(ApiResponse::error(409, ferry::err::kPreparing));
        return;
    }
    respond(ApiResponse::json(
        200, session->metaJson(QDateTime::currentMSecsSinceEpoch(), streamClient)));
}

void HttpApi::handleOwnerBeacon(const QByteArray &id, const QByteArray &body, bool revoke,
                                const Responder &respond)
{
    TransferSession *session = m_registry->find(id);
    if (!session || !session->hasOffer()) {
        respond(ApiResponse::error(404, ferry::err::kNotFound));
        return;
    }
    // Тело — owner_token, как его выдал POST /api/transfers. Уйти за
    // отправителя или закрыть раздачу может только тот, у кого его токен.
    const QByteArray token = QByteArray::fromBase64(
        body.trimmed(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    if (!TransferServer::constantTimeEquals(token, session->ownerToken())) {
        respond(ApiResponse::error(403, ferry::err::kOwnerConflict));
        return;
    }
    if (revoke)
        session->revoke();
    else
        session->senderLeaving();
    QJsonObject o;
    o[QStringLiteral("ok")] = true;
    respond(ApiResponse::json(200, o));
}

void HttpApi::handleChallenge(const QByteArray &id, const Responder &respond)
{
    TransferSession *session = m_registry->find(id);
    if (!session || !session->hasOffer()) {
        respond(ApiResponse::error(404, ferry::err::kNotFound));
        return;
    }

    const QByteArray challenge = m_challenges->issue();
    if (challenge.isEmpty()) {
        respond(ApiResponse::error(503, ferry::err::kServerBusy));
        return;
    }

    QJsonObject o;
    o[QStringLiteral("challenge")] = QString::fromLatin1(
        challenge.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    respond(ApiResponse::json(200, o));
}

void HttpApi::handleHealth(const Responder &respond)
{
    QJsonObject o;
    o[QStringLiteral("name")] = m_config.name;
    o[QStringLiteral("product")] = QStringLiteral("ferry");
    o[QStringLiteral("protocol")] = int(ferry::kProtocolVersion);
    o[QStringLiteral("commit")] = QStringLiteral(FERRY_COMMIT);
    o[QStringLiteral("modified")] = bool(FERRY_MODIFIED);
    o[QStringLiteral("built")] = QStringLiteral(FERRY_BUILD_TIME);
    o[QStringLiteral("transfers")] = m_registry->count();
    o[QStringLiteral("max_transfers")] = m_config.maxTransfers;
    o[QStringLiteral("window_mb")] = m_config.windowMb;
    o[QStringLiteral("ram_budget_mb")] = m_config.ramBudgetMb;
    o[QStringLiteral("window_bytes_used")] = double(m_registry->windowBytesTotal());
    o[QStringLiteral("max_concurrent")] = m_config.maxConcurrent;
    o[QStringLiteral("public_url")] = m_config.publicUrl;
    // Обещание приватности должно быть проверяемым, а не подразумеваемым:
    // человек видит, ведёт ли этот сервер журнал с идентификаторами.
    o[QStringLiteral("keeps_identifiers")] = Log::keepsIdentifiers();
    // На диске у Ferry нет ничего, и это тоже часть обещания (§10).
    o[QStringLiteral("stores_on_disk")] = false;
    // Что релей умеет — тот же список, что в offer_ok. Главной странице он
    // нужен раньше, чем заведена раздача: ручки, которых релей не поймёт,
    // ей лучше не показывать вовсе.
    o[QStringLiteral("features")] = TransferServer::serverFeatures();
    respond(ApiResponse::json(200, o));
}
