#include "Chunker.h"

namespace ferry {

uint32_t chooseChunkSize(uint64_t totalBytes)
{
    constexpr uint64_t kMiB = 1024ull * 1024ull;
    if (totalBytes < 64 * kMiB)
        return 256 * 1024;
    if (totalBytes <= 4096 * kMiB)
        return 1 * kMiB;
    return 4 * kMiB;
}

ChunkPlan planFor(uint64_t totalBytes)
{
    return planWith(totalBytes, chooseChunkSize(totalBytes));
}

ChunkPlan planWith(uint64_t totalBytes, uint32_t chunkSize)
{
    ChunkPlan p;
    // Границы нарочно шире таблицы из chooseChunkSize: чужой клиент вправе
    // выбрать другой размер, лишь бы он был степенью двойки в разумном
    // диапазоне. А вот произвольное число ломает и карту присутствия, и
    // выравнивание чтения с диска, поэтому его отвергаем.
    if (chunkSize < 64 * 1024 || chunkSize > 64u * 1024u * 1024u)
        return p;
    if ((chunkSize & (chunkSize - 1)) != 0)
        return p;

    p.totalBytes = totalBytes;
    p.chunkSize = chunkSize;
    p.chunkCount = (totalBytes + chunkSize - 1) / chunkSize;
    return p;
}

} // namespace ferry
