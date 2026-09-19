#pragma once

#include <cstdint>
#include <vector>

#include "Types.h"

// Целостность (§4): плоский список BLAKE3-хешей открытых чанков и корень
// над этим списком.
//
// Почему не дерево Меркла: при чанке в 1 МиБ список для тома в 20 ГБ весит
// 640 КБ — его дешевле передать целиком один раз, чем таскать доказательства
// к каждому чанку. Зато проверка любого чанка становится O(1) и не зависит
// от порядка прихода — а это ровно то, что нужно для двух волн и для приёма
// чанков от других получателей: подсунуть мусор нельзя, подмена ловится на
// месте, а не в конце.
namespace ferry {

Hash32 blake3(const uint8_t *data, size_t len);

class HashList
{
public:
    void clear() { m_hashes.clear(); }
    void reserve(size_t n) { m_hashes.reserve(n); }
    void append(const Hash32 &h) { m_hashes.push_back(h); }

    size_t size() const { return m_hashes.size(); }
    bool empty() const { return m_hashes.empty(); }
    const Hash32 &at(size_t i) const { return m_hashes[i]; }

    // root = BLAKE3(конкатенация всех хешей). Лежит в ЗАШИФРОВАННОМ
    // манифесте, а не в ссылке: иначе сервер получил бы content-id и мог бы
    // сопоставлять разные раздачи одного и того же файла.
    Hash32 root() const;

    // Провод: просто конкатенация, 32 байта на чанк. Шифруется K_meta.
    std::vector<uint8_t> serialize() const;

    // expectedCount приходит из offer. Длина, не кратная 32, или не сходящаяся
    // с количеством чанков — это сломанный или подменённый список, и дальше
    // идти нельзя: проверять чанки будет не против чего.
    static bool parse(const std::vector<uint8_t> &raw, uint64_t expectedCount, HashList &out);

    // Совпал ли открытый чанк со своим хешем.
    bool verify(uint64_t index, const uint8_t *plain, size_t len) const;

private:
    std::vector<Hash32> m_hashes;
};

} // namespace ferry
