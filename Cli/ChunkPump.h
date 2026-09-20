#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Cli/net/WebSocketClient.h"
#include "Cli/platform/Platform.h"
#include "core/Chunker.h"
#include "core/Protocol.h"
#include "core/Types.h"

// Отдать чанк: прочитать с диска по смещению, зашифровать, собрать фрейм,
// отправить. Одно и то же и у отправителя, и у получателя, который стал
// источником для чужого догона.
//
// Почему у получателя получается тот же самый шифротекст, а не «тоже
// правильный». AES-256-GCM детерминирован: тот же ключ, тот же nonce
// (prefix ‖ index), тот же AAD и тот же открытый текст дают побайтово ту
// же запись. Ключ K_data и nonce-префикс у получателя те же, что у
// отправителя, а открытый текст он проверил по списку хешей — значит
// шифротекст сойдётся с исходным до последнего байта.
//
// Повторное использование nonce здесь не ослабление: катастрофа GCM — это
// один nonce на ДВА РАЗНЫХ открытых текста, а здесь текст тот же самый, и
// наружу выходит ровно та запись, которая и так уже ездила по сети.
//
// Отсюда же и главное следствие: сид не хранит шифротекст. Ему достаточно
// собственной недокачки, а значит сидирование не стоит ни байта сверх.
namespace ferry::cli {

class ChunkPump
{
public:
    // fd остаётся во владении вызывающего: у отправителя это исходный
    // файл, у получателя — его же недокачка, открытая на чтение и запись.
    void init(platform::File fd, const ChunkPlan &plan, const Key32 &dataKey,
              const uint8_t noncePrefix[kNoncePrefixSize]);

    // false — чанк не ушёл, причина в error(). Ошибка здесь всегда
    // означает беду с файлом: его подменили, урезали или он кончился.
    bool send(net::WebSocketClient &ws, uint64_t index);

    // Размер последнего отданного чанка в открытом виде — для счётчиков.
    uint32_t lastPlainSize() const { return m_lastPlainSize; }

    const std::string &error() const { return m_error; }

private:
    platform::File m_fd = platform::kInvalidFile;
    ChunkPlan m_plan;
    Key32 m_key{};
    uint8_t m_noncePrefix[kNoncePrefixSize] = {};
    std::vector<uint8_t> m_plain;
    uint32_t m_lastPlainSize = 0;
    std::string m_error;
};

} // namespace ferry::cli
