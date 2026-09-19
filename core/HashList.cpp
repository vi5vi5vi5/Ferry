#include "HashList.h"

#include <cstring>

extern "C" {
#include "third_party/blake3/blake3.h"
}

namespace ferry {

Hash32 blake3(const uint8_t *data, size_t len)
{
    Hash32 out{};
    blake3_hasher h;
    blake3_hasher_init(&h);
    if (len > 0)
        blake3_hasher_update(&h, data, len);
    blake3_hasher_finalize(&h, out.data(), out.size());
    return out;
}

Hash32 HashList::root() const
{
    blake3_hasher h;
    blake3_hasher_init(&h);
    for (const Hash32 &item : m_hashes)
        blake3_hasher_update(&h, item.data(), item.size());
    Hash32 out{};
    blake3_hasher_finalize(&h, out.data(), out.size());
    return out;
}

std::vector<uint8_t> HashList::serialize() const
{
    std::vector<uint8_t> raw(m_hashes.size() * 32);
    size_t off = 0;
    for (const Hash32 &item : m_hashes) {
        std::memcpy(raw.data() + off, item.data(), 32);
        off += 32;
    }
    return raw;
}

bool HashList::parse(const std::vector<uint8_t> &raw, uint64_t expectedCount, HashList &out)
{
    if (raw.size() % 32 != 0)
        return false;
    if (raw.size() / 32 != expectedCount)
        return false;

    out.clear();
    out.reserve(static_cast<size_t>(expectedCount));
    for (size_t off = 0; off < raw.size(); off += 32) {
        Hash32 h{};
        std::memcpy(h.data(), raw.data() + off, 32);
        out.append(h);
    }
    return true;
}

bool HashList::verify(uint64_t index, const uint8_t *plain, size_t len) const
{
    if (index >= m_hashes.size())
        return false;
    const Hash32 got = blake3(plain, len);
    // Сравнение не обязано быть постоянного времени: хеш открытого чанка —
    // не секрет, его знает и тот, кто чанк прислал.
    return std::memcmp(got.data(), m_hashes[index].data(), 32) == 0;
}

} // namespace ferry
