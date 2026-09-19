#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>

class QTcpServer;
class QTcpSocket;
class QJsonObject;
class HttpApi;
class HttpRequestParser;
struct HttpRequest;
class TransferServer;
struct ApiResponse;

// HTTP-транспорт трёх ролей:
//   1) JSON API (/api/...) — разбор здесь, маршрутизация в HttpApi;
//   2) страница раздачи /t/<id> и установщик клиента /install.sh, /dl/*;
//   3) веб-клиент из web/ — появится в M3.
//
// Страница /t/<id> отдаётся одним и тем же файлом для любого id, и это не
// упрощение: ключ раздачи живёт во фрагменте ссылки, а фрагмент до сервера
// не доходит вообще. Значит собрать человеку готовую команду может только
// страница у него в браузере — сервер этого физически не может.
class HttpFileServer : public QObject
{
    Q_OBJECT
public:
    // publicUrl — то, каким адресом релей представляется снаружи
    // (server.public_url). Нужен ровно в одном месте: подставить в
    // install.sh. Пусто — берём адрес из заголовков запроса.
    HttpFileServer(quint16 port, const QString &rootDir, HttpApi *api, TransferServer *ws,
                   bool serveWeb, const QString &publicUrl, QObject *parent = nullptr);
    ~HttpFileServer() override;

    bool isListening() const;
    quint16 port() const { return m_port; }

private slots:
    void onNewConnection();
    void onReadyRead();

private:
    void dispatch(QTcpSocket *socket, HttpRequestParser *parser);
    void serveFile(QTcpSocket *socket, const HttpRequest &req);
    void sendApiResponse(QTcpSocket *socket, const ApiResponse &resp);
    void sendResponse(QTcpSocket *socket, int code, const QByteArray &status,
                      const QByteArray &contentType, const QByteArray &body,
                      const QByteArray &cacheControl = QByteArrayLiteral("no-store"),
                      const QList<QPair<QByteArray, QByteArray>> &extraHeaders = {});
    static QByteArray statusText(int code);
    static QByteArray mimeFor(const QString &path);

    // Похоже ли начало запроса на смену протокола для /wsf. Решаем по
    // ПОДСМОТРЕННЫМ байтам: читать их нельзя, дальше этот же сокет будет
    // разбирать QWebSocketServer, и он должен увидеть рукопожатие целиком.
    static bool looksLikeWebSocketUpgrade(const QByteArray &head);

    QTcpServer *m_server;
    HttpApi *m_api;                                       // не владеет
    TransferServer *m_ws;                                 // не владеет
    QHash<QTcpSocket *, HttpRequestParser *> m_parsers;   // парсер на соединение
    QString m_root;
    QString m_publicUrl;
    quint16 m_port;
    bool m_serveWeb;
};
