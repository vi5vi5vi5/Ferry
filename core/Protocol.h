#pragma once

#include <cstdint>

// Константы протокола, общие для сервера, CLI и (позже) веб-клиента.
// Всё, что здесь лежит, попадает в провод, поэтому менять значения можно
// только вместе с kProtocolVersion.
namespace ferry {

// Версия формата. Идёт в AAD каждого чанка (§4), поэтому смена версии
// автоматически делает старые чанки нечитаемыми — это и нужно.
inline constexpr uint8_t kProtocolVersion = 1;

// Пути на сервере.
inline constexpr const char *kWsPath = "/wsf";
inline constexpr const char *kApiTransfers = "/api/transfers";

// Первый байт бинарного фрейма.
enum BinaryOp : uint8_t {
    OpChunk = 1,   // [op:1][chunk_index:8 BE][шифротекст + тег GCM 16 байт]
};

// Заголовок бинарного фрейма: op + индекс.
inline constexpr size_t kBinaryHeaderSize = 1 + 8;

// Тег GCM. Прибавляется к каждому чанку и к каждому шифротексту метаданных.
inline constexpr size_t kGcmTagSize = 16;

// Длины ключевого материала.
inline constexpr size_t kMasterKeySize = 32;   // K из фрагмента ссылки
inline constexpr size_t kTransferIdSize = 16;  // публичный id раздачи
inline constexpr size_t kOwnerTokenSize = 32;  // токен владельца
inline constexpr size_t kNoncePrefixSize = 4;  // случайный префикс nonce на раздачу
inline constexpr size_t kChallengeSize = 16;   // challenge сервера
inline constexpr size_t kVerifierSize = 32;    // V = HKDF(K, "ferry/v1/verifier")
inline constexpr size_t kPasswordSaltSize = 16;

// Метки HKDF. Разные метки — разные ключи из одного K; сервер получает
// только производную verifier и не может вернуться от неё к K.
inline constexpr const char *kInfoData = "ferry/v1/data";
inline constexpr const char *kInfoMeta = "ferry/v1/meta";
inline constexpr const char *kInfoVerifier = "ferry/v1/verifier";

// Манифест и список хешей шифруются тем же AEAD, что и чанки, но ключом
// K_meta и с этими «индексами». Пересечься с индексом настоящего чанка они
// не могут: том в 2^64-2 чанка по 256 КиБ — это больше, чем бывает материи.
inline constexpr uint64_t kMetaLabelManifest = 0xFFFFFFFFFFFFFFFFull;
inline constexpr uint64_t kMetaLabelHashList = 0xFFFFFFFFFFFFFFFEull;

// Коды ошибок (§8). Строками, а не числами: они уходят в JSON и читаются
// человеком в логе клиента.
namespace err {
inline constexpr const char *kNeedKey = "need_key";
inline constexpr const char *kNotFound = "transfer_not_found";
inline constexpr const char *kUsesExhausted = "uses_exhausted";
inline constexpr const char *kExpired = "expired";
inline constexpr const char *kTooManyReceivers = "too_many_receivers";
inline constexpr const char *kApprovalDenied = "approval_denied";
inline constexpr const char *kSenderGone = "sender_gone";
inline constexpr const char *kNoSource = "no_source";
inline constexpr const char *kChunkMismatch = "chunk_mismatch";
inline constexpr const char *kServerBusy = "server_busy";
inline constexpr const char *kOwnerConflict = "owner_conflict";
inline constexpr const char *kBadMessage = "bad_message";
} // namespace err

} // namespace ferry
