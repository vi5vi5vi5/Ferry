#include "core/ChallengeStore.h"

#include <QDateTime>
#include <QRandomGenerator>

#include "core/Protocol.h"

QByteArray ChallengeStore::issue()
{
    if (m_issued.size() >= kMaxIssued) {
        sweep();
        if (m_issued.size() >= kMaxIssued)
            return {};
    }

    QByteArray c(int(ferry::kChallengeSize), 0);
    QRandomGenerator::system()->generate(c.begin(), c.end());
    m_issued.insert(c, QDateTime::currentMSecsSinceEpoch() + kTtlMs);
    return c;
}

bool ChallengeStore::consume(const QByteArray &challenge)
{
    const auto it = m_issued.constFind(challenge);
    if (it == m_issued.constEnd())
        return false;
    const qint64 expiresAt = it.value();
    m_issued.erase(m_issued.find(challenge));   // одноразовый
    return QDateTime::currentMSecsSinceEpoch() < expiresAt;
}

void ChallengeStore::sweep()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_issued.begin(); it != m_issued.end();) {
        if (now >= it.value())
            it = m_issued.erase(it);
        else
            ++it;
    }
}
