#include "core/ChunkSet.h"

#include <algorithm>

namespace ferry {
namespace {

// Таблица вместо __builtin_popcount: ядро собирается тремя разными
// компиляторами (gcc на Linux, MinGW под Windows, плюс то, чем однажды
// соберут чужой порт), и двести пятьдесят шесть байт дешевле, чем
// развилка по компиляторам в общем коде.
constexpr uint8_t popcount8(uint8_t v)
{
    v = uint8_t((v & 0x55) + ((v >> 1) & 0x55));
    v = uint8_t((v & 0x33) + ((v >> 2) & 0x33));
    return uint8_t((v & 0x0F) + ((v >> 4) & 0x0F));
}

inline size_t byteOf(uint64_t index)
{
    return size_t(index >> 3);
}

inline uint8_t maskOf(uint64_t index)
{
    return uint8_t(1u << (index & 7));
}

} // namespace

void ChunkSet::reset(uint64_t count)
{
    m_count = count;
    m_bits.assign(size_t((count + 7) / 8), 0);
    m_have = 0;
}

bool ChunkSet::has(uint64_t index) const
{
    if (index >= m_count)
        return false;
    return (m_bits[byteOf(index)] & maskOf(index)) != 0;
}

bool ChunkSet::set(uint64_t index)
{
    if (index >= m_count)
        return false;
    uint8_t &byte = m_bits[byteOf(index)];
    const uint8_t mask = maskOf(index);
    if (byte & mask)
        return false;
    byte = uint8_t(byte | mask);
    ++m_have;
    return true;
}

bool ChunkSet::clear(uint64_t index)
{
    if (index >= m_count)
        return false;
    uint8_t &byte = m_bits[byteOf(index)];
    const uint8_t mask = maskOf(index);
    if (!(byte & mask))
        return false;
    byte = uint8_t(byte & ~mask);
    --m_have;
    return true;
}

bool ChunkSet::setRange(const ChunkRange &range)
{
    if (!range.valid() || range.to >= m_count)
        return false;
    for (uint64_t i = range.from; i <= range.to; ++i)
        set(i);
    return true;
}

bool ChunkSet::applyRanges(const std::vector<ChunkRange> &ranges)
{
    // Сначала проверяем весь список, и только потом трогаем карту —
    // см. пояснение в заголовке.
    for (const ChunkRange &r : ranges) {
        if (!r.valid() || r.to >= m_count)
            return false;
    }
    for (const ChunkRange &r : ranges)
        setRange(r);
    return true;
}

uint64_t ChunkSet::prefix() const
{
    // Целыми байтами, пока они полные: у тома в терабайт при чанке 4 МиБ
    // это 32 КиБ карты, и перебирать её по биту на каждом ack — заметно.
    uint64_t index = 0;
    const uint64_t wholeBytes = m_count / 8;
    size_t b = 0;
    while (b < wholeBytes && m_bits[b] == 0xFF) {
        ++b;
        index += 8;
    }
    while (index < m_count && has(index))
        ++index;
    return index;
}

std::vector<ChunkRange> ChunkSet::ranges() const
{
    std::vector<ChunkRange> out;
    uint64_t i = 0;
    while (i < m_count) {
        // Пропускаем пустые байты целиком.
        while (i < m_count && (i & 7) == 0 && i + 8 <= m_count && m_bits[byteOf(i)] == 0)
            i += 8;
        if (i >= m_count)
            break;
        if (!has(i)) {
            ++i;
            continue;
        }
        const uint64_t from = i;
        while (i < m_count && has(i))
            ++i;
        out.push_back({from, i - 1});
    }
    return out;
}

std::vector<ChunkRange> ChunkSet::missing() const
{
    std::vector<ChunkRange> out;
    uint64_t i = 0;
    while (i < m_count) {
        while (i < m_count && (i & 7) == 0 && i + 8 <= m_count && m_bits[byteOf(i)] == 0xFF)
            i += 8;
        if (i >= m_count)
            break;
        if (has(i)) {
            ++i;
            continue;
        }
        const uint64_t from = i;
        while (i < m_count && !has(i))
            ++i;
        out.push_back({from, i - 1});
    }
    return out;
}

uint64_t ChunkSet::firstMissing(uint64_t from) const
{
    uint64_t i = from;
    while (i < m_count) {
        if ((i & 7) == 0 && i + 8 <= m_count && m_bits[byteOf(i)] == 0xFF) {
            i += 8;
            continue;
        }
        if (!has(i))
            return i;
        ++i;
    }
    return m_count;
}

bool ChunkSet::loadBits(const uint8_t *data, size_t len)
{
    if (!data || len != m_bits.size())
        return false;
    std::copy(data, data + len, m_bits.begin());

    // Гасим хвост последнего байта: биты за пределами тома не значат
    // ничего, а посчитались бы наравне с настоящими.
    const uint64_t tail = m_count & 7;
    if (tail != 0 && !m_bits.empty())
        m_bits.back() = uint8_t(m_bits.back() & ((1u << tail) - 1));

    m_have = 0;
    for (uint8_t byte : m_bits)
        m_have += popcount8(byte);
    return true;
}

bool ChunkSet::normalize(std::vector<ChunkRange> &ranges, uint64_t count)
{
    for (const ChunkRange &r : ranges) {
        if (!r.valid() || r.to >= count)
            return false;
    }
    if (ranges.empty())
        return true;

    std::vector<ChunkRange> sorted = ranges;
    std::sort(sorted.begin(), sorted.end(), [](const ChunkRange &a, const ChunkRange &b) {
        return a.from != b.from ? a.from < b.from : a.to < b.to;
    });

    std::vector<ChunkRange> merged;
    merged.push_back(sorted.front());
    for (size_t i = 1; i < sorted.size(); ++i) {
        ChunkRange &last = merged.back();
        // Склеиваем не только пересекающиеся, но и соседние: [0,3] и
        // [4,7] — это [0,7]. Иначе карта тома, пришедшая по проводу,
        // распухала бы на ровном месте, а у получателя, качающего по
        // одному чанку, список диапазонов был бы длиной в том.
        //
        // last.to + 1 не переполнится: to < count, а count у нас заведомо
        // меньше UINT64_MAX — том такого размера не существует.
        if (sorted[i].from <= last.to + 1) {
            last.to = std::max(last.to, sorted[i].to);
        } else {
            merged.push_back(sorted[i]);
        }
    }
    ranges.swap(merged);
    return true;
}

} // namespace ferry
