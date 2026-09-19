#include "Crypto.h"

#include <algorithm>
#include <cstring>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace ferry {
namespace {

// RAII вокруг контекста шифра: у EVP полтора десятка мест, где можно выйти
// по ошибке, и забыть free в одном из них — вопрос времени.
struct CipherCtx
{
    EVP_CIPHER_CTX *p = EVP_CIPHER_CTX_new();

    CipherCtx() = default;
    ~CipherCtx()
    {
        if (p)
            EVP_CIPHER_CTX_free(p);
    }
    CipherCtx(const CipherCtx &) = delete;
    CipherCtx &operator=(const CipherCtx &) = delete;
};

void putBe64(uint8_t *out, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        out[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
}

// nonce = префикс раздачи + индекс чанка. Повтор невозможен: индекс уникален
// внутри тома, префикс — между томами.
void buildNonce(const uint8_t noncePrefix[kNoncePrefixSize], uint64_t index, uint8_t out[12])
{
    std::memcpy(out, noncePrefix, kNoncePrefixSize);
    putBe64(out + kNoncePrefixSize, index);
}

void buildAad(uint64_t index, uint64_t totalChunks, uint8_t out[17])
{
    out[0] = kProtocolVersion;
    putBe64(out + 1, index);
    putBe64(out + 9, totalChunks);
}

} // namespace

bool randomBytes(uint8_t *out, size_t len)
{
    if (len == 0)
        return true;
    return RAND_bytes(out, static_cast<int>(len)) == 1;
}

bool randomBytes(Bytes &out, size_t len)
{
    out.assign(len, 0);
    return randomBytes(out.data(), len);
}

Key32 randomKey32(bool *ok)
{
    Key32 k{};
    const bool good = randomBytes(k.data(), k.size());
    if (ok)
        *ok = good;
    if (!good)
        k.fill(0);
    return k;
}

Bytes hmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *msg, size_t msgLen)
{
    Bytes out(32, 0);
    unsigned int outLen = 0;
    // Односложный HMAC() объявлен устаревшим в OpenSSL 3.0, но работает и
    // там, и в 1.1.1. Переход на EVP_MAC развёл бы сборку на две ветки ради
    // одной строки — см. -Wno-deprecated-declarations в CMakeLists.
    const uint8_t dummy = 0;
    if (!HMAC(EVP_sha256(), key, static_cast<int>(keyLen),
              msgLen ? msg : &dummy, msgLen, out.data(), &outLen))
        return {};
    if (outLen != 32)
        return {};
    return out;
}

Bytes hkdfSha256(const uint8_t *ikm, size_t ikmLen,
                 const uint8_t *salt, size_t saltLen,
                 const std::string &info, size_t outLen)
{
    if (outLen == 0 || outLen > 255 * 32)
        return {};

    // Extract. Пустая соль по RFC — это блок нулей длиной с хеш.
    static const uint8_t kZeroSalt[32] = {};
    const uint8_t *s = (salt && saltLen) ? salt : kZeroSalt;
    const size_t sLen = (salt && saltLen) ? saltLen : sizeof(kZeroSalt);
    const Bytes prk = hmacSha256(s, sLen, ikm, ikmLen);
    if (prk.size() != 32)
        return {};

    // Expand.
    Bytes out;
    out.reserve(outLen);
    Bytes t;
    uint8_t counter = 1;
    while (out.size() < outLen) {
        Bytes block;
        block.reserve(t.size() + info.size() + 1);
        block.insert(block.end(), t.begin(), t.end());
        block.insert(block.end(), info.begin(), info.end());
        block.push_back(counter);

        t = hmacSha256(prk.data(), prk.size(), block.data(), block.size());
        if (t.size() != 32)
            return {};

        const size_t take = std::min(t.size(), outLen - out.size());
        out.insert(out.end(), t.begin(), t.begin() + static_cast<long>(take));
        ++counter;
    }
    return out;
}

TransferKeys TransferKeys::derive(const Key32 &master)
{
    TransferKeys keys;
    const auto one = [&](const char *info, Key32 &dst) {
        const Bytes b = hkdfSha256(master.data(), master.size(), nullptr, 0, info, 32);
        if (b.size() == 32)
            std::memcpy(dst.data(), b.data(), 32);
    };
    one(kInfoData, keys.data);
    one(kInfoMeta, keys.meta);
    one(kInfoVerifier, keys.verifier);
    return keys;
}

Bytes proveKeyOwnership(const Key32 &verifier, const uint8_t *challenge, size_t len)
{
    return hmacSha256(verifier.data(), verifier.size(), challenge, len);
}

bool sealChunk(const Key32 &key, const uint8_t noncePrefix[kNoncePrefixSize],
               uint64_t index, uint64_t totalChunks,
               const uint8_t *plain, size_t plainLen, Bytes &out)
{
    CipherCtx ctx;
    if (!ctx.p)
        return false;

    uint8_t nonce[12];
    uint8_t aad[17];
    buildNonce(noncePrefix, index, nonce);
    buildAad(index, totalChunks, aad);

    if (EVP_EncryptInit_ex(ctx.p, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        return false;
    if (EVP_CIPHER_CTX_ctrl(ctx.p, EVP_CTRL_GCM_SET_IVLEN, sizeof(nonce), nullptr) != 1)
        return false;
    if (EVP_EncryptInit_ex(ctx.p, nullptr, nullptr, key.data(), nonce) != 1)
        return false;

    int len = 0;
    if (EVP_EncryptUpdate(ctx.p, nullptr, &len, aad, sizeof(aad)) != 1)
        return false;

    out.assign(plainLen + kGcmTagSize, 0);
    if (plainLen > 0) {
        if (EVP_EncryptUpdate(ctx.p, out.data(), &len, plain, static_cast<int>(plainLen)) != 1)
            return false;
    }
    int total = len;
    if (EVP_EncryptFinal_ex(ctx.p, out.data() + total, &len) != 1)
        return false;
    total += len;
    if (static_cast<size_t>(total) != plainLen)
        return false;

    if (EVP_CIPHER_CTX_ctrl(ctx.p, EVP_CTRL_GCM_GET_TAG, kGcmTagSize, out.data() + plainLen) != 1)
        return false;
    return true;
}

bool openChunk(const Key32 &key, const uint8_t noncePrefix[kNoncePrefixSize],
               uint64_t index, uint64_t totalChunks,
               const uint8_t *cipher, size_t cipherLen, Bytes &out)
{
    if (cipherLen < kGcmTagSize)
        return false;
    const size_t bodyLen = cipherLen - kGcmTagSize;

    CipherCtx ctx;
    if (!ctx.p)
        return false;

    uint8_t nonce[12];
    uint8_t aad[17];
    buildNonce(noncePrefix, index, nonce);
    buildAad(index, totalChunks, aad);

    if (EVP_DecryptInit_ex(ctx.p, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        return false;
    if (EVP_CIPHER_CTX_ctrl(ctx.p, EVP_CTRL_GCM_SET_IVLEN, sizeof(nonce), nullptr) != 1)
        return false;
    if (EVP_DecryptInit_ex(ctx.p, nullptr, nullptr, key.data(), nonce) != 1)
        return false;

    int len = 0;
    if (EVP_DecryptUpdate(ctx.p, nullptr, &len, aad, sizeof(aad)) != 1)
        return false;

    Bytes plain(bodyLen, 0);
    if (bodyLen > 0) {
        if (EVP_DecryptUpdate(ctx.p, plain.data(), &len, cipher, static_cast<int>(bodyLen)) != 1)
            return false;
    }
    int total = len;

    // Тег ставим до Final: именно Final его и проверяет.
    uint8_t tag[kGcmTagSize];
    std::memcpy(tag, cipher + bodyLen, kGcmTagSize);
    if (EVP_CIPHER_CTX_ctrl(ctx.p, EVP_CTRL_GCM_SET_TAG, kGcmTagSize, tag) != 1)
        return false;

    if (EVP_DecryptFinal_ex(ctx.p, plain.data() + total, &len) != 1) {
        // Тег не сошёлся. Открытый текст наружу не отдаём вообще — он не
        // «почти правильный», он не аутентифицирован, и работать с ним
        // нельзя даже для диагностики.
        return false;
    }
    total += len;
    if (static_cast<size_t>(total) != bodyLen)
        return false;

    out = std::move(plain);
    return true;
}

} // namespace ferry
