#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPair>

#include <functional>

class ChallengeStore;
class TransferRegistry;
struct HttpRequest;
struct ServerConfig;

// Ответ API. Обычно это JSON; contentType задан — значит отдаём сырое тело
// (установщик клиента, бинарь клиента).
struct ApiResponse
{
    int status = 200;
    QJsonObject body;
    QByteArray contentType;
    QByteArray rawBody;
    QByteArray cacheControl;
    QList<QPair<QByteArray, QByteArray>> headers;

    static ApiResponse json(int status, const QJsonObject &body);
    static ApiResponse error(int status, const char *reason);
};

// JSON API Ferry. Всё, что здесь есть, умещается в четыре ручки — и это не
// мало для прототипа, а ровно столько, сколько нужно: том едет по
// WebSocket, а HTTP отвечает только на вопросы «заведи раздачу», «что это
// за раздача» и «дай challenge».
//
//   POST /api/transfers               завести раздачу     -> {id, owner_token}
//   GET  /api/transfers/<id>          метаданные          ничего не сжигает
//   GET  /api/transfers/<id>/challenge  16 случайных байт
//   GET  /api/health                  жив ли, и на каких настройках
//
// Всем ответам /api дописываются заголовки CORS по server.cors_origins, а
// предварительный запрос браузера (OPTIONS) получает ответ здесь же — см.
// corsHeaders.
class HttpApi : public QObject
{
    Q_OBJECT
public:
    using Responder = std::function<void(const ApiResponse &)>;

    HttpApi(TransferRegistry *registry, ChallengeStore *challenges,
            const ServerConfig &config, QObject *parent = nullptr);

    // true — путь наш и ответ будет отдан через respond (возможно, позже).
    bool route(const HttpRequest &req, const Responder &respond);

private:
    void handleCreate(const HttpRequest &req, const Responder &respond);
    void handleMeta(const QByteArray &id, bool streamClient, const Responder &respond);
    void handleChallenge(const QByteArray &id, const Responder &respond);
    void handleOwnerBeacon(const QByteArray &id, const QByteArray &body, bool revoke,
                           const Responder &respond);
    void handleHealth(const Responder &respond);

    // Заголовки CORS для этого запроса: разрешён его Origin — Allow-Origin
    // (и Vary, если список не «*»), нет — пусто, и браузер ответ не покажет.
    QList<QPair<QByteArray, QByteArray>> corsHeaders(const HttpRequest &req) const;

    TransferRegistry *m_registry;      // не владеет
    ChallengeStore *m_challenges;      // не владеет
    const ServerConfig &m_config;      // живёт в main дольше нас
};
