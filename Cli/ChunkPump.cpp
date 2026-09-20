#include "Cli/ChunkPump.h"

#include <cstring>

#include "core/Crypto.h"
#include "core/Protocol.h"

namespace ferry::cli {

void ChunkPump::init(platform::File fd, const ChunkPlan &plan, const Key32 &dataKey,
                     const uint8_t noncePrefix[kNoncePrefixSize])
{
    m_fd = fd;
    m_plan = plan;
    m_key = dataKey;
    std::memcpy(m_noncePrefix, noncePrefix, kNoncePrefixSize);
    m_plain.assign(plan.chunkSize, 0);
    m_lastPlainSize = 0;
    m_error.clear();
}

bool ChunkPump::send(net::WebSocketClient &ws, uint64_t index)
{
    if (index >= m_plan.chunkCount) {
        m_error = "просят чанк, которого в томе нет";
        return false;
    }

    const uint32_t len = m_plan.sizeOf(index);
    const int64_t got = platform::fileReadAt(m_fd, m_plain.data(), len, m_plan.offsetOf(index));
    if (got != int64_t(len)) {
        m_error = "файл перестал читаться — его изменили или удалили во время раздачи";
        return false;
    }

    Bytes cipher;
    if (!sealChunk(m_key, m_noncePrefix, index, m_plan.chunkCount, m_plain.data(), size_t(len),
                   cipher)) {
        m_error = "не удалось зашифровать чанк";
        return false;
    }

    // Заголовок внутри той же последовательности байт, что и шифротекст:
    // сервер раздаёт фрейм получателям как есть, не копируя полезную
    // нагрузку.
    std::vector<uint8_t> frame(kBinaryHeaderSize + cipher.size());
    frame[0] = OpChunk;
    for (int i = 0; i < 8; ++i)
        frame[1 + size_t(i)] = uint8_t(index >> (56 - 8 * i));
    std::memcpy(frame.data() + kBinaryHeaderSize, cipher.data(), cipher.size());

    ws.sendBinary(frame.data(), frame.size());
    m_lastPlainSize = len;
    return true;
}

} // namespace ferry::cli
