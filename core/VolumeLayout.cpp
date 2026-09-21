#include "core/VolumeLayout.h"

#include <algorithm>

namespace ferry {

void VolumeLayout::build(const Manifest &manifest)
{
    m_files.clear();
    m_total = 0;

    if (!manifest.isTree()) {
        // Один файл во весь том. Имя берём из манифеста как есть: чинить
        // его — дело получателя (safeFileName), а раскладке всё равно.
        if (manifest.total > 0) {
            m_files.push_back({manifest.name, manifest.total, 0, 0});
            m_total = manifest.total;
        }
        return;
    }

    m_files.reserve(manifest.entries.size());
    for (const ManifestEntry &e : manifest.entries) {
        if (e.isDir || e.size == 0)
            continue;
        m_files.push_back({e.path, e.size, m_total, e.mtime});
        m_total += e.size;
    }
}

std::vector<VolumeLayout::Piece> VolumeLayout::slice(uint64_t from, uint64_t length) const
{
    std::vector<Piece> out;
    if (length == 0 || from >= m_total || from + length > m_total)
        return out;

    // Двоичным поиском по началам файлов: у тома из сорока тысяч файлов
    // линейный поиск на каждый чанк — это уже заметно.
    size_t lo = 0, hi = m_files.size();
    while (lo + 1 < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (m_files[mid].start <= from)
            lo = mid;
        else
            hi = mid;
    }

    uint64_t left = length;
    for (size_t i = lo; i < m_files.size() && left > 0; ++i) {
        const FileSpan &f = m_files[i];
        if (from >= f.start + f.size)
            continue;
        const uint64_t inFile = from - f.start;
        const uint64_t take = std::min(left, f.size - inFile);
        out.push_back({i, inFile, take});
        from += take;
        left -= take;
    }
    return out;
}

} // namespace ferry
