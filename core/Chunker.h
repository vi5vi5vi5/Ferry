#pragma once

#include <cstdint>

// Арифметика тома. Всё, что раздаётся, — непрерывное байтовое пространство
// (один файл или конкатенация файлов папки), нарезанное на чанки
// фиксированного размера; последний чанк неполный.
//
// Это единственное место, где живёт выбор размера чанка, и сервер о нём не
// думает вовсе: размер приходит к нему в offer и дальше он видит просто
// N штук одинаковых кусков.
namespace ferry {

// §3 проектного документа. Порог смещён в сторону крупного чанка, потому что
// накладные расходы у нас линейны по числу чанков сразу в трёх местах:
// 16 байт тега AEAD, 32 байта хеша в списке и по одному биту в карте
// присутствия у каждого получателя.
uint32_t chooseChunkSize(uint64_t totalBytes);

struct ChunkPlan
{
    uint64_t totalBytes = 0;
    uint32_t chunkSize = 0;
    uint64_t chunkCount = 0;

    uint64_t offsetOf(uint64_t index) const { return index * uint64_t(chunkSize); }

    // Размер конкретного чанка: все полные, кроме последнего.
    uint32_t sizeOf(uint64_t index) const
    {
        if (index + 1 < chunkCount)
            return chunkSize;
        if (index + 1 == chunkCount) {
            const uint64_t rest = totalBytes - offsetOf(index);
            return static_cast<uint32_t>(rest);
        }
        return 0;
    }

    bool valid() const { return chunkSize > 0 && (totalBytes == 0 || chunkCount > 0); }
};

// Размер чанка выбирается сам — так делает отправитель.
ChunkPlan planFor(uint64_t totalBytes);

// Размер чанка пришёл снаружи (из offer) — так делают сервер и получатель.
// Возвращает план с chunkSize == 0, если размер невозможен: не степень двойки
// в разумных пределах или не сходится с количеством чанков.
ChunkPlan planWith(uint64_t totalBytes, uint32_t chunkSize);

// Пустой том — законный случай (файл нулевой длины): чанков ноль, манифест
// есть, получатель создаёт пустой файл. Отдельная функция, чтобы это
// решение было видно в коде, а не подразумевалось.
inline bool isEmptyVolume(const ChunkPlan &p) { return p.totalBytes == 0; }

} // namespace ferry
