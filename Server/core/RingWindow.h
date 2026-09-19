#pragma once

#include <QByteArray>
#include <QList>

// Скользящее окно чанков раздачи (§6).
//
// Это БУФЕР ДЖИТТЕРА, А НЕ КЭШ, и разница здесь важнее всего остального в
// этом классе. При 100 МБ/с окно в 256 МиБ покрывает две с половиной
// секунды разброса между получателями — ровно столько, чтобы быстрый и
// медленный не мешали друг другу. Ни для какого «позднего подключения»
// этого не хватает и хватить не может: опоздавший обслуживается backfill'ом,
// а не окном.
//
// Окно непрерывно: [first, end). Чанки приходят от отправителя по порядку,
// поэтому ни дырок, ни хеш-таблицы здесь не нужно — обычный кольцевой
// список и индекс начала.
class RingWindow
{
public:
    // capacityBytes — потолок, а не резерв: память занимается по мере
    // прихода чанков. Ноль и отрицательное значение означают «один чанк»:
    // совсем без окна раздача не поедет вообще.
    void configure(quint32 chunkSize, qint64 capacityBytes);

    // Индекс обязан быть равен endIndex(): окно непрерывно по построению.
    // Всё остальное — ошибка вызывающего, и её лучше увидеть сразу.
    bool append(quint64 index, const QByteArray &payload);

    bool contains(quint64 index) const { return index >= m_first && index < endIndex(); }

    // Пустой QByteArray, если чанка в окне нет. Возврат по значению
    // бесплатен: QByteArray разделяет данные неявно, и отдать один чанк в
    // N сокетов не копирует ни байта полезной нагрузки.
    QByteArray at(quint64 index) const;

    quint64 firstIndex() const { return m_first; }
    quint64 endIndex() const { return m_first + quint64(m_items.size()); }
    qint64 bytes() const { return m_bytes; }
    qint64 capacity() const { return m_capacity; }
    int count() const { return int(m_items.size()); }

    // Сколько чанков влезает в окно целиком. Нужно планировщику, чтобы не
    // просить у отправителя больше, чем окно способно удержать.
    quint64 capacityChunks() const;

    // Выбросить всё, что получатели уже прочитали. upto — индекс, до
    // которого (не включая) чанки больше никому не нужны.
    void dropBefore(quint64 upto);

    // Ужать окно, когда раздач стало много (§6, защита памяти сервера).
    void setCapacity(qint64 bytes);

    void clear();

private:
    void evict();

    QList<QByteArray> m_items;
    quint64 m_first = 0;
    qint64 m_bytes = 0;
    qint64 m_capacity = 0;
    quint32 m_chunkSize = 0;
};
