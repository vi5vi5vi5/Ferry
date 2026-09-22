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

// ---- Хеши на лету ----
//
// Отправитель не читает том целиком до ссылки: хеши считаются в фоне и
// уходят сегментами, а раздача начинается сразу. Для этого нужны ещё две
// метки, и ни одна из них не имеет права совпасть с уже занятыми.
//
// Промежуточный манифест (без корня — корня ещё нет) и итоговый (с корнем)
// — разные открытые тексты. Зашифровать их под одним nonce значило бы
// повторить nonce в GCM, а это раскрывает и поток ключа, и ключ
// аутентификации. Поэтому промежуточный идёт под своей меткой, а
// kMetaLabelManifest и kMetaLabelHashList достаются итоговым — ровно в
// том виде, в каком их ждут клиенты, не знающие про хеши на лету.
inline constexpr uint64_t kMetaLabelStreamManifest = 0xFFFFFFFFFFFFFFFDull;

// Сегмент списка хешей, начинающийся с чанка from, шифруется под меткой
// база + from. Сегменты идут встык и не пересекаются, так что метки
// уникальны; до 0xFF… база не дотянется ни при каком реальном томе
// (для этого понадобилось бы 2^60 чанков).
inline constexpr uint64_t kMetaLabelHashSegmentBase = 0xF000000000000000ull;
inline constexpr uint64_t hashSegmentLabel(uint64_t from)
{
    return kMetaLabelHashSegmentBase + from;
}

// Потолок одного сегмента: 8192 хеша — 256 КиБ открытого текста. Больше
// в одно сообщение класть незачем, а релею нужен предел, которому он
// может не верить на слово.
inline constexpr uint64_t kHashSegmentMax = 8192;

// Клиент говорит релею, что умеет хеши на лету: по HTTP — этим
// заголовком (у запроса метаданных нет тела), по WebSocket — строкой в
// features. Старый клиент ни того, ни другого не пришлёт и получит том
// только целиком посчитанным.
inline constexpr const char *kFeaturesHeader = "X-Ferry-Features";
inline constexpr const char *kFeatureStreamHashes = "stream_hashes";

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
// Отправитель ещё считает хеши, а клиент не умеет получать их на лету.
// Не отказ, а «зайдите через минуту»: как только список досчитан, раздача
// для такого клиента ничем не отличается от обычной.
inline constexpr const char *kPreparing = "preparing";
} // namespace err

} // namespace ferry
