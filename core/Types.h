#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Общие имена для сырых байт. Отдельным заголовком, чтобы ядро (Chunker,
// HashList, Manifest, Link) не тянуло за собой Crypto.h, а вместе с ним и
// OpenSSL: серверу криптография не нужна, и это свойство должно держаться
// само, а не на договорённости.
namespace ferry {

using Bytes = std::vector<uint8_t>;
using Key32 = std::array<uint8_t, 32>;    // ключ: K, K_data, K_meta, V
using Hash32 = std::array<uint8_t, 32>;   // BLAKE3

} // namespace ferry
