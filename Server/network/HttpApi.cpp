#include "network/HttpApi.h"

#include <QDateTime>

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

bool HttpApi::route(const HttpRequest &req, const Responder &respond)
{
    if (!req.path.startsWith(QLatin1String("/api/")))
        return false;

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
    respond(ApiResponse::json(200, o));
}
