#include "core/TransferRegistry.h"

#include <QDateTime>
#include <QRandomGenerator>

#include <algorithm>

#include "config/Log.h"
#include "config/ServerConfig.h"
#include "core/Protocol.h"
#include "core/TransferSession.h"

TransferRegistry::TransferRegistry(const ServerConfig &config, QObject *parent)
    : QObject(parent), m_config(config)
{
}

QByteArray TransferRegistry::makeId() const
{
    // Системный генератор, а не обычный: идентификатор раздачи — это
    // половина того, что защищает раздачу (вторая половина — ключ), и
    // предсказуемый id позволил бы перебирать чужие раздачи.
    QByteArray raw(int(ferry::kTransferIdSize), 0);
    QRandomGenerator::system()->generate(raw.begin(), raw.end());
    return raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

bool TransferRegistry::ipQuotaAllows(const QString &peerAddress, QString *errorCode)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    IpCounter &c = m_byIp[peerAddress];

    if (now - c.windowStartedMs >= 60 * 1000) {
        c.windowStartedMs = now;
        c.createdInWindow = 0;
    }
    if (c.createdInWindow >= m_config.createPerIpPerMin) {
        if (errorCode)
            *errorCode = QString::fromLatin1(ferry::err::kServerBusy);
        return false;
    }
    if (c.alive >= m_config.maxTransfersPerIp) {
        if (errorCode)
            *errorCode = QString::fromLatin1(ferry::err::kServerBusy);
        return false;
    }
    ++c.createdInWindow;
    return true;
}

TransferSession *TransferRegistry::create(const QString &peerAddress, QString *errorCode)
{
    if (m_byId.size() >= m_config.maxTransfers) {
        if (errorCode)
            *errorCode = QString::fromLatin1(ferry::err::kServerBusy);
        return nullptr;
    }
    if (!ipQuotaAllows(peerAddress, errorCode))
        return nullptr;

    QByteArray id = makeId();
    // Коллизия на 128 битах невозможна практически, но проверка стоит
    // одного поиска в хеше, а её отсутствие — чужой раздачи.
    while (m_byId.contains(id))
        id = makeId();

    QByteArray ownerToken(int(ferry::kOwnerTokenSize), 0);
    QRandomGenerator::system()->generate(ownerToken.begin(), ownerToken.end());

    auto *session = new TransferSession(id, ownerToken, this);
    session->setWindowCapacity(m_config.windowBytes());
    m_byId.insert(id, session);
    m_sessionIp.insert(session, peerAddress);
    ++m_byIp[peerAddress].alive;

    connect(session, &TransferSession::needsClosing, this, &TransferRegistry::remove);

    rebalanceWindows();
    return session;
}

TransferSession *TransferRegistry::find(const QByteArray &id) const
{
    return m_byId.value(id, nullptr);
}

void TransferRegistry::remove(TransferSession *session)
{
    if (!session || !m_byId.contains(session->id()))
        return;

    const QByteArray id = session->id();
    session->closeWith(QString::fromLatin1(ferry::err::kSenderGone));
    m_byId.remove(id);

    const QString ip = m_sessionIp.take(session);
    const auto it = m_byIp.find(ip);
    if (it != m_byIp.end()) {
        it->alive = std::max(0, it->alive - 1);
        // Адрес без живых раздач и с истёкшим окном забываем совсем:
        // хранить его дальше — значит вести историю, которой не обещали.
        if (it->alive == 0
            && QDateTime::currentMSecsSinceEpoch() - it->windowStartedMs >= 60 * 1000)
            m_byIp.erase(it);
    }

    emit transferClosed(id);
    session->deleteLater();
    rebalanceWindows();
}

qint64 TransferRegistry::windowBytesTotal() const
{
    qint64 sum = 0;
    for (const TransferSession *s : m_byId)
        sum += s->windowBytes();
    return sum;
}

void TransferRegistry::rebalanceWindows()
{
    // Делим бюджет поровну между живыми раздачами, но не больше
    // настроенного окна на каждую. Так первая раздача не съедает всю
    // память и не заставляет вторую получить server_busy на ровном месте.
    const qint64 n = std::max<qint64>(1, m_byId.size());
    const qint64 share = m_config.ramBudgetBytes() / n;
    const qint64 cap = std::min(share, m_config.windowBytes());
    for (TransferSession *s : m_byId)
        s->setWindowCapacity(cap);
}

void TransferRegistry::sweep()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QList<TransferSession *> doomed;
    for (TransferSession *s : m_byId) {
        if (now >= s->expiresAtMs())
            doomed.append(s);
        else if (s->state() == TransferSession::State::Closed)
            doomed.append(s);
    }
    for (TransferSession *s : doomed) {
        const bool draft = !s->hasOffer();
        s->closeWith(QString::fromLatin1(draft ? ferry::err::kNotFound : ferry::err::kExpired));
        remove(s);
    }

    // Чужие адреса, за которыми ничего не осталось, забываем.
    for (auto it = m_byIp.begin(); it != m_byIp.end();) {
        if (it->alive == 0 && now - it->windowStartedMs >= 60 * 1000)
            it = m_byIp.erase(it);
        else
            ++it;
    }

    // Раздачи живут в оперативке и только там. Если их стало столько, что
    // окна вышли за бюджет, — ужимаем окна, а не уходим в своп: своп у
    // релея утягивает за собой всех сразу.
    if (windowBytesTotal() > m_config.ramBudgetBytes())
        rebalanceWindows();
}
