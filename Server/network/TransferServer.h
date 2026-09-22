#pragma once

#include <QJsonArray>
#include <QObject>
#include <QSet>

class QTcpSocket;
class QWebSocketServer;
class QJsonObject;
class QTimer;
class ChallengeStore;
class ClientSession;
class TransferRegistry;
struct ServerConfig;

// Ядро релея. Принимает WebSocket-соединения на /wsf, заворачивает каждое в
// ClientSession и маршрутизирует сообщения:
//
//   текст (JSON)  — управление: offer / hello / ack / have / request / bye;
//   бинарь        — чанки от отправителя, которые тут же уезжают получателям.
//
// Сервер никогда не заглядывает внутрь чанка: у него шифротекст и тег GCM,
// ключа нет и быть не может. Его работа — окно, курсоры и кредит.
class TransferServer : public QObject
{
    Q_OBJECT
public:
    TransferServer(quint16 port, TransferRegistry *registry, ChallengeStore *challenges,
                   const ServerConfig &config, QObject *parent = nullptr);
    ~TransferServer() override;

    bool isListening() const;
    quint16 port() const { return m_port; }

    // Принять соединение, которое пришло на ПОРТ HTTP и оказалось запросом
    // на смену протокола.
    //
    // Зачем: так у релея снаружи один порт, а не два. Клиенту достаточно
    // знать адрес — «а вебсокет у нас на 9000» не нужно ни писать в
    // конфиге, ни объяснять человеку. nginx при этом всё равно сводит всё
    // на 443, но без прокси (локальный запуск, сеть на даче) тоже
    // работает, и это стоит одного вызова.
    void adoptConnection(QTcpSocket *socket);

    // Что умеет ЭТОТ сервер. Уезжает в offer_ok и hello_ok, и клиент по
    // нему решает, какие сообщения вообще посылать. А ещё — в ответ на
    // POST /api/transfers: отправителю это нужно знать раньше, чем он
    // соберёт offer (см. хеши на лету).
    //
    // Объявление нужно из-за строгости разбора: неизвестное сообщение —
    // это bad_message, а не «промолчу». Строгость правильная (мусор на
    // проводе должен быть виден сразу), но за неё приходится платить
    // честным ответом на вопрос «а ты меня поймёшь». Без него новый
    // клиент, встретив старый релей, получал бы отказ на первом же have.
    static QJsonArray serverFeatures();

private slots:
    void onNewConnection();
    void onText(ClientSession *session, const QString &text);
    void onBinary(ClientSession *session, const QByteArray &data);
    void onDisconnected(ClientSession *session);
    void onBytesWritten(ClientSession *session);
    void onTick();

private:
    void handleOffer(ClientSession *session, const QJsonObject &msg);
    void handleHello(ClientSession *session, const QJsonObject &msg);
    void handleAck(ClientSession *session, const QJsonObject &msg);
    void handleHashes(ClientSession *session, const QJsonObject &msg, bool done);
    void handleHave(ClientSession *session, const QJsonObject &msg);
    void handleRequest(ClientSession *session, const QJsonObject &msg);
    void handleBadChunk(ClientSession *session, const QJsonObject &msg);

    void sendError(ClientSession *session, const char *reason, bool closeAfter = true);

    // Сравнение без утечки по времени. Токен владельца и доказательство
    // владения ключом — это секреты на 32 байта; побайтовое сравнение с
    // ранним выходом теоретически позволяет их подбирать, и хотя через сеть
    // это почти нереально, писать заведомо худший вариант незачем.
    static bool constantTimeEquals(const QByteArray &a, const QByteArray &b);

    QWebSocketServer *m_server;
    TransferRegistry *m_registry;   // не владеет
    ChallengeStore *m_challenges;   // не владеет
    const ServerConfig &m_config;   // живёт в main дольше нас
    QSet<ClientSession *> m_sessions;
    QTimer *m_tick;
    quint16 m_port;
};
