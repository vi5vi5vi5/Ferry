#include "network/HttpFileServer.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#include "config/Log.h"
#include "core/Protocol.h"
#include "network/HttpApi.h"
#include "network/HttpRequest.h"
#include "network/TransferServer.h"

HttpFileServer::HttpFileServer(quint16 port, const QString &rootDir, HttpApi *api,
                               TransferServer *ws, bool serveWeb, const QString &publicUrl,
                               QObject *parent)
    : QObject(parent),
      m_api(api),
      m_ws(ws),
      m_root(QDir(rootDir).absolutePath()),
      m_publicUrl(publicUrl),
      m_port(port),
      m_serveWeb(serveWeb)
{
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &HttpFileServer::onNewConnection);

    if (m_server->listen(QHostAddress::Any, port)) {
        if (m_serveWeb) {
            qCInfo(lcApp).noquote()
                << QStringLiteral("HTTP: порт %1, страницы и установщик из %2").arg(port).arg(m_root);
        } else {
            qCInfo(lcApp).noquote()
                << QStringLiteral("HTTP: порт %1, только /api (страницы выключены)").arg(port);
        }
    } else {
        qCritical().noquote() << QStringLiteral("HTTP: не удалось занять порт %1: %2")
                                     .arg(port).arg(m_server->errorString());
    }
}

HttpFileServer::~HttpFileServer()
{
    qDeleteAll(m_parsers);
    m_parsers.clear();
}

bool HttpFileServer::isListening() const
{
    return m_server->isListening();
}

void HttpFileServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, this, &HttpFileServer::onReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            delete m_parsers.take(socket);
            socket->deleteLater();
        });
    }
}

bool HttpFileServer::looksLikeWebSocketUpgrade(const QByteArray &head)
{
    const QByteArray lower = head.toLower();
    if (!lower.startsWith("get "))
        return false;
    if (!lower.contains("upgrade: websocket"))
        return false;
    // Путь проверяет сам TransferServer, но отсечь чужие апгрейды дешевле
    // здесь: иначе любой «GET /wsx» с Upgrade уезжал бы в чужие руки.
    return lower.contains(QByteArray("get ") + ferry::kWsPath + " ");
}

void HttpFileServer::onReadyRead()
{
    auto *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket)
        return;

    // Смена протокола. Проверяем ДО того, как что-либо прочитали: сокет
    // целиком, вместе с непрочитанным рукопожатием, уходит к
    // QWebSocketServer, и он должен получить его нетронутым.
    if (m_ws) {
        const QByteArray peeked = socket->peek(4096);
        const bool headComplete = peeked.contains(QByteArrayLiteral("\r\n\r\n"));
        if (!headComplete && peeked.size() < 4096)
            return;   // заголовки ещё не все — решать рано
        if (looksLikeWebSocketUpgrade(peeked)) {
            // Отцепляемся полностью: и от readyRead, и от лямбды на
            // disconnected, которая удалила бы сокет из-под вебсокета.
            socket->disconnect(this);
            delete m_parsers.take(socket);
            m_ws->adoptConnection(socket);
            return;
        }
    }

    // Запрос может приходить кусками — парсер копит байты между вызовами
    // readyRead, пока не соберёт запрос целиком.
    HttpRequestParser *parser = m_parsers.value(socket);
    if (!parser) {
        parser = new HttpRequestParser;
        m_parsers.insert(socket, parser);
    }

    switch (parser->feed(socket->readAll())) {
    case HttpRequestParser::State::NeedMore:
        return;
    case HttpRequestParser::State::Error: {
        const int code = parser->errorStatus();
        sendResponse(socket, code, statusText(code), "text/plain; charset=utf-8",
                     QByteArray::number(code) + ' ' + statusText(code));
        break;
    }
    case HttpRequestParser::State::Done:
        dispatch(socket, parser);
        break;
    }

    delete m_parsers.take(socket);
}

void HttpFileServer::dispatch(QTcpSocket *socket, HttpRequestParser *parser)
{
    const HttpRequest &req = parser->request();

    if (m_api) {
        QPointer<QTcpSocket> guard(socket);
        const bool handled = m_api->route(req, [this, guard](const ApiResponse &resp) {
            if (guard)
                sendApiResponse(guard, resp);
        });
        if (handled)
            return;
    }

    if (req.method != "GET") {
        sendResponse(socket, 405, statusText(405), "text/plain; charset=utf-8",
                     "405 Method Not Allowed");
        return;
    }

    serveFile(socket, req);
}

void HttpFileServer::serveFile(QTcpSocket *socket, const HttpRequest &req)
{
    const QString urlPath = req.path;
    if (!m_serveWeb) {
        sendResponse(socket, 404, statusText(404), "text/plain; charset=utf-8",
                     "На этом сервере Ferry страницы выключены.\n"
                     "Работает только API и WebSocket-релей.\n");
        return;
    }

    QString rel = urlPath;
    if (rel.isEmpty() || rel == QLatin1String("/"))
        rel = QStringLiteral("/index.html");

    // Страница раздачи одна на все id. Ключ лежит во фрагменте, до сервера
    // он не доходит, и собрать человеку готовую команду может только сама
    // страница — уже у него в браузере.
    if (rel.startsWith(QLatin1String("/t/")))
        rel = QStringLiteral("/t.html");

    const QString full = QDir::cleanPath(m_root + rel);
    if (!full.startsWith(m_root)) {
        sendResponse(socket, 403, statusText(403), "text/plain; charset=utf-8", "403 Forbidden");
        return;
    }

    QFile file(full);
    if (!QFileInfo(full).isFile() || !file.open(QIODevice::ReadOnly)) {
        sendResponse(socket, 404, statusText(404), "text/plain; charset=utf-8", "404 Not Found");
        return;
    }

    // Бинарь клиента и установщик кэшировать нельзя: они обновляются вместе
    // с сервером, и человек, поймавший суточный кэш, получил бы клиента от
    // прошлой версии протокола.
    const bool volatileAsset = rel.startsWith(QLatin1String("/dl/"))
                               || rel == QLatin1String("/install.sh");
    const QByteArray cache = (!volatileAsset && rel.startsWith(QLatin1String("/assets/")))
                                 ? QByteArrayLiteral("public, max-age=86400")
                                 : QByteArrayLiteral("no-store");

    QByteArray body = file.readAll();

    // Установщик клиента — единственный файл, который отдаётся не как
    // есть. В него подставляется адрес, по которому человек СЕЙЧАС пришёл:
    // именно он попадёт в ~/.config/ferry/config, и именно поэтому после
    // `curl … | sh` команда `ferry send файл` работает без флагов.
    //
    // Адрес берём из заголовка Host, а не из настроек: за прокси сервер
    // своего внешнего имени не знает, а Host — знает всегда.
    if (rel == QLatin1String("/install.sh")) {
        QByteArray host = req.header(QByteArrayLiteral("host"));
        if (host.isEmpty())
            host = QByteArrayLiteral("localhost");

        // Схему знать сложнее, чем хост, и ошибиться здесь дорого:
        // неправильная схема в конфиге клиента означает, что `ferry send`
        // не соединится вообще.
        //
        // 1) server.public_url — владелец сервера сказал прямо, верим ему;
        // 2) X-Forwarded-Proto — наш nginx его ставит (см. proxy/);
        // 3) ничего нет — значит к нам пришли напрямую, а напрямую мы
        //    говорим только по http.
        QByteArray scheme;
        if (!m_publicUrl.isEmpty()) {
            const QUrl declared(m_publicUrl);
            if (!declared.scheme().isEmpty())
                scheme = declared.scheme().toLatin1();
            if (!declared.host().isEmpty()) {
                host = declared.host().toLatin1();
                if (declared.port() > 0)
                    host += ':' + QByteArray::number(declared.port());
            }
        }
        if (scheme.isEmpty())
            scheme = req.header(QByteArrayLiteral("x-forwarded-proto"));
        if (scheme != QByteArrayLiteral("https") && scheme != QByteArrayLiteral("http"))
            scheme = QByteArrayLiteral("http");

        body.replace("@FERRY_RELAY@", host);
        body.replace("@FERRY_SCHEME@", scheme);
    }

    sendResponse(socket, 200, statusText(200), mimeFor(full), body, cache);
}

void HttpFileServer::sendApiResponse(QTcpSocket *socket, const ApiResponse &resp)
{
    if (!resp.contentType.isEmpty()) {
        sendResponse(socket, resp.status, statusText(resp.status), resp.contentType, resp.rawBody,
                     resp.cacheControl.isEmpty() ? QByteArrayLiteral("no-store") : resp.cacheControl,
                     resp.headers);
        return;
    }
    sendResponse(socket, resp.status, statusText(resp.status), "application/json; charset=utf-8",
                 QJsonDocument(resp.body).toJson(QJsonDocument::Compact),
                 QByteArrayLiteral("no-store"), resp.headers);
}

void HttpFileServer::sendResponse(QTcpSocket *socket, int code, const QByteArray &status,
                                  const QByteArray &contentType, const QByteArray &body,
                                  const QByteArray &cacheControl,
                                  const QList<QPair<QByteArray, QByteArray>> &extraHeaders)
{
    QByteArray header;
    header += "HTTP/1.1 " + QByteArray::number(code) + ' ' + status + "\r\n";
    header += "Content-Type: " + contentType + "\r\n";
    header += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    header += "Cache-Control: " + cacheControl + "\r\n";
    for (const auto &kv : extraHeaders)
        header += kv.first + ": " + kv.second + "\r\n";
    header += "Connection: close\r\n\r\n";

    socket->write(header);
    socket->write(body);
    socket->flush();
    socket->disconnectFromHost();
}

QByteArray HttpFileServer::statusText(int code)
{
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Content Too Large";
    case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
    }
}

QByteArray HttpFileServer::mimeFor(const QString &path)
{
    if (path.endsWith(QLatin1String(".html")))  return "text/html; charset=utf-8";
    if (path.endsWith(QLatin1String(".js")))    return "application/javascript; charset=utf-8";
    if (path.endsWith(QLatin1String(".css")))   return "text/css; charset=utf-8";
    if (path.endsWith(QLatin1String(".json")))  return "application/json; charset=utf-8";
    if (path.endsWith(QLatin1String(".svg")))   return "image/svg+xml";
    if (path.endsWith(QLatin1String(".png")))   return "image/png";
    if (path.endsWith(QLatin1String(".ico")))   return "image/x-icon";
    if (path.endsWith(QLatin1String(".woff2"))) return "font/woff2";
    // Установщик открывают и глазами («что я сейчас запущу?»), поэтому
    // именно text/plain, а не x-shellscript: браузер должен показать, а не
    // предложить скачать.
    if (path.endsWith(QLatin1String(".sh")))    return "text/plain; charset=utf-8";
    if (path.endsWith(QLatin1String(".txt")))   return "text/plain; charset=utf-8";
    return "application/octet-stream";
}
