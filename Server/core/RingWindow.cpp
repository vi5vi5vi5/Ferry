#include "core/RingWindow.h"

#include <algorithm>

void RingWindow::configure(quint32 chunkSize, qint64 capacityBytes)
{
    clear();
    m_chunkSize = chunkSize ? chunkSize : 1;
    setCapacity(capacityBytes);
}

void RingWindow::setCapacity(qint64 bytes)
{
    // Ниже одного чанка опускаться нельзя: окно перестало бы вмещать даже
    // то, что сейчас отдаётся, и раздача встала бы намертво вместо того,
    // чтобы просто замедлиться.
    m_capacity = std::max<qint64>(bytes, qint64(m_chunkSize));
    evict();
}

quint64 RingWindow::capacityChunks() const
{
    if (m_chunkSize == 0)
        return 0;
    const quint64 n = quint64(m_capacity) / m_chunkSize;
    return n ? n : 1;
}

bool RingWindow::append(quint64 index, const QByteArray &payload)
{
    if (index != endIndex())
        return false;
    m_items.append(payload);
    m_bytes += payload.size();
    evict();
    return true;
}

QByteArray RingWindow::at(quint64 index) const
{
    if (!contains(index))
        return {};
    return m_items.at(int(index - m_first));
}

void RingWindow::dropBefore(quint64 upto)
{
    while (m_first < upto && !m_items.isEmpty()) {
        m_bytes -= m_items.first().size();
        m_items.removeFirst();
        ++m_first;
    }
}

void RingWindow::evict()
{
    // Один чанк остаётся всегда — см. setCapacity. Выбрасываем с головы:
    // окно едет вперёд со скоростью отправителя, и самый старый чанк — это
    // тот, который дальше всех от происходящего.
    while (m_bytes > m_capacity && m_items.size() > 1) {
        m_bytes -= m_items.first().size();
        m_items.removeFirst();
        ++m_first;
    }
}

void RingWindow::clear()
{
    m_items.clear();
    m_first = 0;
    m_bytes = 0;
}
