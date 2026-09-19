#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

struct ServerConfig;
class TransferSession;

// Реестр живых раздач: id -> сессия. Всё в оперативке, ничего на диске.
//
// Здесь же живёт то, что в проектном документе названо «анти-абьюз без
// хранения» (§10): счётчики по IP, потолок числа раздач и общий потолок
// памяти под окна. Все они обнуляются при перезапуске — истории нет по
// построению, а не по настройке.
class TransferRegistry : public QObject
{
    Q_OBJECT
public:
    explicit TransferRegistry(const ServerConfig &config, QObject *parent = nullptr);

    // Создаёт черновик раздачи: id и токен владельца есть, offer ещё нет.
    // Черновик живёт минуту — столько у отправителя, чтобы дойти до
    // WebSocket. nullptr — упёрлись в потолок (вызывающий отвечает
    // server_busy или too_many_receivers по errorCode).
    TransferSession *create(const QString &peerAddress, QString *errorCode);

    TransferSession *find(const QByteArray &id) const;

    void remove(TransferSession *session);

    int count() const { return m_byId.size(); }
    qint64 windowBytesTotal() const;

    // Вызов по таймеру: убирает протухшие и закрытые, при нехватке памяти
    // ужимает окна.
    void sweep();

signals:
    void transferClosed(const QByteArray &id);

private:
    QByteArray makeId() const;
    bool ipQuotaAllows(const QString &peerAddress, QString *errorCode);
    void rebalanceWindows();

    const ServerConfig &m_config;
    QHash<QByteArray, TransferSession *> m_byId;

    // Сколько раздач за последнюю минуту завёл каждый адрес и сколько
    // держит сейчас. Протекающее ведро без истории: минута прошла — счёт
    // обнулился, адрес забыт.
    struct IpCounter
    {
        int createdInWindow = 0;
        qint64 windowStartedMs = 0;
        int alive = 0;
    };
    QHash<QString, IpCounter> m_byIp;
    QHash<TransferSession *, QString> m_sessionIp;
};
