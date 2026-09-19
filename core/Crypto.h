#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Protocol.h"
#include "Types.h"

// Криптография Ferry (§4). Живёт отдельной целью сборки (ferry-crypto),
// потому что СЕРВЕРУ ЭТО НЕ НУЖНО: он никогда ничего не расшифровывает, у
// него на руках только шифротекст и односторонняя производная ключа. Если
// однажды окажется, что серверный бинарь тянет за собой libcrypto, — значит
// кто-то случайно позвал отсюда функцию, и это повод разобраться, а не
// добавить библиотеку в образ.
namespace ferry {

// CSPRNG операционной системы. false — генератор недоступен; в этом случае
// продолжать нельзя ни при каких обстоятельствах (ключ из предсказуемых
// байт хуже отсутствия шифрования, потому что создаёт ложное чувство).
bool randomBytes(uint8_t *out, size_t len);
bool randomBytes(Bytes &out, size_t len);
Key32 randomKey32(bool *ok = nullptr);

// HKDF-SHA256 (RFC 5869) целиком: extract + expand. Реализован здесь на
// HMAC, а не через EVP_PKEY_HKDF, чтобы одинаково собираться и на
// OpenSSL 1.1.1 (debian bullseye, где собирается клиент), и на 3.x.
Bytes hkdfSha256(const uint8_t *ikm, size_t ikmLen,
                 const uint8_t *salt, size_t saltLen,
                 const std::string &info, size_t outLen);

Bytes hmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *msg, size_t msgLen);

// Три ключа из одного K. Соль пустая: K и так равномерно случаен, а метка
// в info разводит производные между собой.
struct TransferKeys
{
    Key32 data{};       // шифрование чанков
    Key32 meta{};       // шифрование манифеста и списка хешей
    Key32 verifier{};   // V — отдаётся серверу, обратно не выводится

    static TransferKeys derive(const Key32 &master);
};

// Ответ на challenge сервера: HMAC-SHA256(V, challenge). Сервер знает V и
// может проверить, но получить из V ключ K не может — HKDF односторонний.
Bytes proveKeyOwnership(const Key32 &verifier, const uint8_t *challenge, size_t len);

// AES-256-GCM по §4.
//   nonce = [ noncePrefix : 4 ][ index : 8 BE ]
//   AAD   = [ version : 1 ][ index : 8 BE ][ totalChunks : 8 BE ]
// index в AAD закрывает перестановку чанков, totalChunks — обрезание тома.
// out получает шифротекст с приклеенным тегом (16 байт) в конце.
bool sealChunk(const Key32 &key, const uint8_t noncePrefix[kNoncePrefixSize],
               uint64_t index, uint64_t totalChunks,
               const uint8_t *plain, size_t plainLen, Bytes &out);

// Обратная операция. false — тег не сошёлся; открытый текст в этом случае
// НЕ отдаётся наружу даже частично.
bool openChunk(const Key32 &key, const uint8_t noncePrefix[kNoncePrefixSize],
               uint64_t index, uint64_t totalChunks,
               const uint8_t *cipher, size_t cipherLen, Bytes &out);

// Argon2id для парольного режима (§4) — появится в M4 вместе с самим
// режимом. Объявление стоит здесь, чтобы было видно место.
// Bytes argon2id(const std::string &password, const uint8_t *salt, size_t saltLen);

} // namespace ferry
