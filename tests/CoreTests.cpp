// ============================================================
//  Самотесты ядра. Без фреймворка: один бинарь, который печатает, что
//  проверил, и возвращает ненулевой код при первом расхождении.
//
//  Главное здесь — векторы BLAKE3 из официального репозитория. Всё
//  остальное ядро можно переписать и заметить ошибку по поведению; хеш
//  так проверить нельзя — неправильная реализация будет стабильно давать
//  стабильно неправильный ответ, и разойдёмся мы с миром только тогда,
//  когда кто-то напишет второй клиент.
// ============================================================
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/Base64Url.h"
#include "core/Chunker.h"
#include "core/Crypto.h"
#include "core/HashList.h"
#include "core/Json.h"
#include "core/Link.h"
#include "core/Manifest.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string &what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  ПРОВАЛ  %s\n", what.c_str());
    }
}

void section(const char *name)
{
    std::printf("\n%s\n", name);
}

std::string toHex(const uint8_t *data, size_t len)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 0x0F];
    }
    return out;
}

// Вход тестовых векторов BLAKE3: повторяющаяся последовательность
// 0, 1, 2, ..., 250, 0, 1, ...
std::vector<uint8_t> vectorInput(size_t len)
{
    std::vector<uint8_t> v(len);
    for (size_t i = 0; i < len; ++i)
        v[i] = static_cast<uint8_t>(i % 251);
    return v;
}

void testBlake3()
{
    section("BLAKE3 — официальные тестовые векторы");
    struct Case { size_t len; const char *hash; };
    // BLAKE3-team/BLAKE3, test_vectors/test_vectors.json (первые 32 байта
    // расширенного вывода — это и есть хеш обычной длины).
    static const Case cases[] = {
        {0,      "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"},
        {1,      "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213"},
        {64,     "4eed7141ea4a5cd4b788606bd23f46e212af9cacebacdc7d1f4c6dc7f2511b98"},
        {1023,   "10108970eeda3eb932baac1428c7a2163b0e924c9a9e25b35bba72b28f70bd11"},
        {1024,   "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7"},
        {1025,   "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444"},
        {2048,   "e776b6028c7cd22a4d0ba182a8bf62205d2ef576467e838ed6f2529b85fba24a"},
        {8192,   "aae792484c8efe4f19e2ca7d371d8c467ffb10748d8a5a1ae579948f718a2a63"},
        {16384,  "f875d6646de28985646f34ee13be9a576fd515f76b5b0a26bb324735041ddde4"},
        {102400, "bc3e3d41a1146b069abffad3c0d44860cf664390afce4d9661f7902e7943e085"},
    };
    for (const Case &c : cases) {
        const std::vector<uint8_t> in = vectorInput(c.len);
        const ferry::Hash32 got = ferry::blake3(in.data(), in.size());
        const std::string hex = toHex(got.data(), got.size());
        check(hex == c.hash, "BLAKE3 длины " + std::to_string(c.len) + ": " + hex);
    }
}

void testBase64Url()
{
    section("base64url");
    struct Case { const char *plain; const char *encoded; };
    static const Case cases[] = {
        {"", ""},
        {"f", "Zg"},
        {"fo", "Zm8"},
        {"foo", "Zm9v"},
        {"foob", "Zm9vYg"},
        {"fooba", "Zm9vYmE"},
        {"foobar", "Zm9vYmFy"},
    };
    for (const Case &c : cases) {
        const std::string enc =
            ferry::base64UrlEncode(reinterpret_cast<const uint8_t *>(c.plain), std::strlen(c.plain));
        check(enc == c.encoded, std::string("кодирование ") + c.plain + " -> " + enc);

        ferry::Bytes back;
        check(ferry::base64UrlDecode(enc, back), std::string("декодирование ") + enc);
        check(std::string(back.begin(), back.end()) == c.plain, "туда-обратно");
    }

    // Символы вне алфавита и невозможная длина — отказ, а не молчаливая порча.
    ferry::Bytes tmp;
    check(!ferry::base64UrlDecode("Zg!", tmp), "отвергает посторонний символ");
    check(!ferry::base64UrlDecode("Z", tmp), "отвергает длину 4k+1");
    // Классический алфавит принимаем: ссылку мог поправить чужой инструмент.
    check(ferry::base64UrlDecode("++//", tmp) && tmp.size() == 3, "принимает + и /");

    // Все 256 байт туда-обратно.
    ferry::Bytes all(256);
    for (int i = 0; i < 256; ++i)
        all[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
    ferry::Bytes back;
    check(ferry::base64UrlDecode(ferry::base64UrlEncode(all), back) && back == all,
          "256 байт туда-обратно");
}

void testChunker()
{
    section("Арифметика тома");
    check(ferry::chooseChunkSize(1024) == 256 * 1024, "мелкий том — чанк 256 КиБ");
    check(ferry::chooseChunkSize(100ull * 1024 * 1024) == 1024 * 1024, "средний том — 1 МиБ");
    check(ferry::chooseChunkSize(8ull * 1024 * 1024 * 1024) == 4 * 1024 * 1024, "крупный том — 4 МиБ");

    const ferry::ChunkPlan p = ferry::planWith(8421376, 1048576);
    check(p.chunkCount == 9, "8 421 376 байт при 1 МиБ — девять чанков");
    check(p.sizeOf(0) == 1048576, "первый чанк полный");
    check(p.sizeOf(8) == 8421376 - 8 * 1048576, "последний чанк неполный");
    check(p.offsetOf(8) == 8 * 1048576, "смещение последнего чанка");

    // Ровное деление: последний чанк тоже полный.
    const ferry::ChunkPlan even = ferry::planWith(4 * 1048576, 1048576);
    check(even.chunkCount == 4 && even.sizeOf(3) == 1048576, "ровное деление");

    // Пустой том — законный случай.
    const ferry::ChunkPlan empty = ferry::planFor(0);
    check(empty.valid() && empty.chunkCount == 0, "пустой том — ноль чанков");

    // Размер не степень двойки приходить не должен.
    check(!ferry::planWith(100, 100000).valid(), "отвергает размер не степень двойки");
    check(!ferry::planWith(100, 1024).valid(), "отвергает слишком мелкий чанк");
}

void testHashList()
{
    section("Список хешей");
    const std::vector<uint8_t> data = vectorInput(3 * 1024 * 1024 + 7);
    const ferry::ChunkPlan plan = ferry::planWith(data.size(), 1024 * 1024);

    ferry::HashList list;
    for (uint64_t i = 0; i < plan.chunkCount; ++i)
        list.append(ferry::blake3(data.data() + plan.offsetOf(i), plan.sizeOf(i)));
    check(list.size() == plan.chunkCount, "хешей столько же, сколько чанков");

    const ferry::Hash32 root = list.root();

    ferry::HashList parsed;
    check(ferry::HashList::parse(list.serialize(), plan.chunkCount, parsed), "разбор списка");
    check(parsed.root() == root, "корень после разбора тот же");
    check(!ferry::HashList::parse(list.serialize(), plan.chunkCount + 1, parsed),
          "отвергает список не той длины");

    check(parsed.verify(0, data.data(), plan.sizeOf(0)), "чанк 0 сходится");
    std::vector<uint8_t> spoiled(data.begin(), data.begin() + plan.sizeOf(0));
    spoiled[100] ^= 0x01;
    check(!parsed.verify(0, spoiled.data(), spoiled.size()), "испорченный чанк не сходится");
}

void testCrypto()
{
    section("Криптография");

    // HKDF-SHA256, RFC 5869, тестовый случай 1.
    const ferry::Bytes ikm(22, 0x0b);
    const ferry::Bytes salt = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                               0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c};
    const ferry::Bytes info = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9};
    const ferry::Bytes okm = ferry::hkdfSha256(ikm.data(), ikm.size(), salt.data(), salt.size(),
                                               std::string(info.begin(), info.end()), 42);
    check(toHex(okm.data(), okm.size())
              == "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                 "34007208d5b887185865",
          "HKDF-SHA256 сходится с RFC 5869");

    // Три ключа из одного K — разные, и это не случайность.
    bool ok = false;
    const ferry::Key32 master = ferry::randomKey32(&ok);
    check(ok, "CSPRNG работает");
    const ferry::TransferKeys keys = ferry::TransferKeys::derive(master);
    check(keys.data != keys.meta && keys.meta != keys.verifier && keys.data != keys.verifier,
          "K_data, K_meta и V различаются");
    const ferry::TransferKeys again = ferry::TransferKeys::derive(master);
    check(again.data == keys.data, "вывод детерминирован");

    // AEAD туда-обратно.
    const uint8_t noncePrefix[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    const std::vector<uint8_t> plain = vectorInput(65536);
    ferry::Bytes cipher;
    check(ferry::sealChunk(keys.data, noncePrefix, 5, 100, plain.data(), plain.size(), cipher),
          "чанк зашифрован");
    check(cipher.size() == plain.size() + 16, "шифротекст длиннее на тег GCM");

    ferry::Bytes back;
    check(ferry::openChunk(keys.data, noncePrefix, 5, 100, cipher.data(), cipher.size(), back),
          "чанк расшифрован");
    check(back == plain, "открытый текст совпал");

    // AAD связывает чанк с его местом в томе. Подменённый индекс, подменённое
    // общее количество (атака обрезания) и чужой ключ — всё три должны
    // отвергаться, а не давать «почти правильный» результат.
    check(!ferry::openChunk(keys.data, noncePrefix, 6, 100, cipher.data(), cipher.size(), back),
          "перестановка чанка отвергнута");
    check(!ferry::openChunk(keys.data, noncePrefix, 5, 99, cipher.data(), cipher.size(), back),
          "обрезание тома отвергнуто");
    check(!ferry::openChunk(keys.meta, noncePrefix, 5, 100, cipher.data(), cipher.size(), back),
          "чужой ключ отвергнут");

    ferry::Bytes damaged = cipher;
    damaged[42] ^= 0x01;
    check(!ferry::openChunk(keys.data, noncePrefix, 5, 100, damaged.data(), damaged.size(), back),
          "битый шифротекст отвергнут");

    // Доказательство владения ключом: сервер знает V и проверяет HMAC,
    // но обратно к K не приходит.
    const ferry::Bytes challenge = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const ferry::Bytes proof = ferry::proveKeyOwnership(keys.verifier, challenge.data(), challenge.size());
    const ferry::Bytes expect = ferry::hmacSha256(keys.verifier.data(), keys.verifier.size(),
                                                  challenge.data(), challenge.size());
    check(proof == expect && proof.size() == 32, "доказательство владения ключом считается");
}

void testJson()
{
    section("JSON");
    ferry::json::Value v;
    std::string err;
    const std::string text =
        R"({"v":1,"kind":"file","name":"отчёт \"за год\".pdf","total":5368709120,)"
        R"("nested":{"a":[1,2.5,true,null,"\u0041\ud83d\ude00"]}})";
    check(ferry::json::Value::parse(text, v, &err), "разбор: " + err);
    check(v["v"].toInt() == 1, "целое поле");
    check(v["name"].toString() == "отчёт \"за год\".pdf", "строка с кавычками и кириллицей");
    // 5 ГиБ через double потеряли бы младшие байты — проверяем именно это.
    check(v["total"].toInt() == 5368709120LL, "большое целое не потеряло точность");
    check(v["nested"]["a"].size() == 5, "вложенный массив");
    check(v["nested"]["a"].at(1).toDouble() == 2.5, "дробное");
    check(v["nested"]["a"].at(2).toBool(), "true");
    check(v["nested"]["a"].at(3).isNull(), "null");
    check(v["nested"]["a"].at(4).toString() == "A\xF0\x9F\x98\x80", "суррогатная пара в UTF-8");
    check(v["нет-такого"].isNull(), "отсутствующее поле — null, а не падение");

    // Печать и повторный разбор дают то же самое.
    ferry::json::Value again;
    check(ferry::json::Value::parse(v.dump(), again, &err), "печать разбирается обратно: " + err);
    check(again["name"].toString() == v["name"].toString(), "имя пережило печать");
    check(again["total"].toInt() == v["total"].toInt(), "размер пережил печать");

    check(!ferry::json::Value::parse("{\"a\":1,}", v), "хвостовая запятая отвергнута");
    check(!ferry::json::Value::parse("{'a':1}", v), "одинарные кавычки отвергнуты");
    check(!ferry::json::Value::parse("{\"a\":1} лишнее", v), "мусор после значения отвергнут");
}

void testManifest()
{
    section("Манифест");
    ferry::Manifest m;
    m.kind = "file";
    m.name = "дамп базы.sql.zst";
    m.total = 5368709120ull;
    m.root = ferry::blake3(reinterpret_cast<const uint8_t *>("x"), 1);

    ferry::Manifest back;
    std::string err;
    check(ferry::Manifest::fromJson(m.toJson(), back, &err), "манифест туда-обратно: " + err);
    check(back.name == m.name, "имя сохранилось");
    check(back.total == m.total, "размер сохранился");
    check(back.root == m.root, "корневой хеш сохранился");

    // Санитизация путей (§3): отвергаем, а не чиним.
    check(ferry::sanitizeRelPath("src/main.cpp"), "обычный путь");
    check(ferry::sanitizeRelPath("2024/отчёты/январь.xlsx"), "кириллица в пути");
    check(!ferry::sanitizeRelPath("/etc/passwd"), "абсолютный путь отвергнут");
    check(!ferry::sanitizeRelPath("../../etc/passwd"), "выход за корень отвергнут");
    check(!ferry::sanitizeRelPath("a/../b"), "точки посреди пути отвергнуты");
    check(!ferry::sanitizeRelPath("C:/windows/system32"), "буква диска отвергнута");
    check(!ferry::sanitizeRelPath("dir\\file"), "обратный слэш отвергнут");
    check(!ferry::sanitizeRelPath("com1/x"), "резервное имя Windows отвергнуто");
    check(!ferry::sanitizeRelPath("NUL.txt"), "резервное имя с расширением отвергнуто");
    check(!ferry::sanitizeRelPath("имя."), "хвостовая точка отвергнута");
    check(!ferry::sanitizeRelPath("a//b"), "пустой сегмент отвергнут");

    // Имя одиночного файла, наоборот, чиним.
    check(ferry::safeFileName("/home/user/dump.sql") == "dump.sql", "путь срезан");
    check(ferry::safeFileName("C:\\temp\\dump.sql") == "dump.sql", "windows-путь срезан");
    check(ferry::safeFileName("a?b*c.txt") == "a_b_c.txt", "недопустимые символы заменены");
    check(ferry::safeFileName("NUL") == "_NUL", "резервное имя обезврежено");
    check(ferry::safeFileName("...").empty(), "из одних точек ничего не осталось");

    // Сумма описи обязана сходиться с размером тома.
    ferry::Manifest tree;
    tree.kind = "tree";
    tree.name = "project";
    tree.total = 100;
    tree.entries.push_back({"a.txt", 60, 0, false});
    tree.entries.push_back({"b.txt", 40, 0, false});
    check(ferry::Manifest::fromJson(tree.toJson(), back, &err), "дерево разбирается: " + err);
    tree.total = 101;
    check(!ferry::Manifest::fromJson(tree.toJson(), back, &err), "расхождение суммы отвергнуто");
}

void testLink()
{
    section("Ссылка");
    ferry::TransferLink link;
    link.host = "ferry.example.ru";
    link.id = ferry::base64UrlEncode(ferry::Bytes(16, 0xAB));
    bool ok = false;
    link.key = ferry::randomKey32(&ok);
    link.hasKey = true;

    const std::string text = link.toString();
    check(text.find("#") != std::string::npos, "ключ во фрагменте");
    check(text.find(link.id) != std::string::npos, "id в пути");

    ferry::TransferLink back;
    std::string err;
    check(ferry::TransferLink::parse(text, back, &err), "разбор ссылки: " + err);
    check(back.host == link.host && back.id == link.id, "хост и id совпали");
    check(back.hasKey && back.key == link.key, "ключ совпал");

    // Ссылка без фрагмента: так выглядит парольный режим и так же выглядит
    // ссылка, у которой решётку съел мессенджер. Разбирается, но без ключа.
    ferry::TransferLink noKey;
    check(ferry::TransferLink::parse(link.origin() + "/t/" + link.id, noKey, &err),
          "ссылка без ключа разбирается");
    check(!noKey.hasKey, "и честно говорит, что ключа нет");

    check(!ferry::TransferLink::parse("ferry.example.ru/t/abc", back), "без схемы отвергнута");
    check(!ferry::TransferLink::parse("https://ferry.example.ru/x/" + link.id, back),
          "чужой путь отвергнут");
    check(!ferry::TransferLink::parse("https://ferry.example.ru/t/короткий", back),
          "негодный id отвергнут");
    check(!ferry::TransferLink::parse("https://ferry.example.ru/t/" + link.id + "#короткий", back),
          "негодный ключ отвергнут");
    // Пробелы по краям — след копирования из мессенджера.
    check(ferry::TransferLink::parse("  " + text + "\n", back, &err), "пробелы по краям срезаны");
}

// Сквозной проход: то, что делает отправитель, и то, что делает получатель.
// Без сети и без сервера — только форматы.
void testEndToEnd()
{
    section("Сквозной проход: отправитель -> получатель");

    const std::vector<uint8_t> volume = vectorInput(5 * 1024 * 1024 + 12345);

    // --- отправитель ---
    bool ok = false;
    const ferry::Key32 master = ferry::randomKey32(&ok);
    const ferry::TransferKeys keys = ferry::TransferKeys::derive(master);
    uint8_t noncePrefix[4];
    ferry::randomBytes(noncePrefix, sizeof(noncePrefix));

    const ferry::ChunkPlan plan = ferry::planFor(volume.size());
    ferry::HashList hashes;
    for (uint64_t i = 0; i < plan.chunkCount; ++i)
        hashes.append(ferry::blake3(volume.data() + plan.offsetOf(i), plan.sizeOf(i)));

    ferry::Manifest manifest;
    manifest.name = "test.bin";
    manifest.total = volume.size();
    manifest.root = hashes.root();

    const std::string manifestJson = manifest.toJson();
    ferry::Bytes encManifest, encHashes;
    check(ferry::sealChunk(keys.meta, noncePrefix, ferry::kMetaLabelManifest, plan.chunkCount,
                           reinterpret_cast<const uint8_t *>(manifestJson.data()),
                           manifestJson.size(), encManifest),
          "манифест зашифрован");
    const ferry::Bytes rawHashes = hashes.serialize();
    check(ferry::sealChunk(keys.meta, noncePrefix, ferry::kMetaLabelHashList, plan.chunkCount,
                           rawHashes.data(), rawHashes.size(), encHashes),
          "список хешей зашифрован");

    // --- получатель ---
    const ferry::TransferKeys rkeys = ferry::TransferKeys::derive(master);
    ferry::Bytes plainManifest, plainHashes;
    check(ferry::openChunk(rkeys.meta, noncePrefix, ferry::kMetaLabelManifest, plan.chunkCount,
                           encManifest.data(), encManifest.size(), plainManifest),
          "манифест расшифрован");
    check(ferry::openChunk(rkeys.meta, noncePrefix, ferry::kMetaLabelHashList, plan.chunkCount,
                           encHashes.data(), encHashes.size(), plainHashes),
          "список хешей расшифрован");

    ferry::Manifest gotManifest;
    std::string err;
    check(ferry::Manifest::fromJson(std::string(plainManifest.begin(), plainManifest.end()),
                                    gotManifest, &err),
          "манифест разобран: " + err);

    ferry::HashList gotHashes;
    check(ferry::HashList::parse(plainHashes, plan.chunkCount, gotHashes), "список хешей разобран");
    // Корень лежит в манифесте: пока он не сошёлся, списку хешей верить нельзя.
    check(gotHashes.root() == gotManifest.root, "корень сошёлся с манифестом");

    // Чанки приходят вне порядка (две волны) — и каждый проверяется сам по себе.
    std::vector<uint8_t> received(gotManifest.total, 0);
    std::vector<uint64_t> order;
    for (uint64_t i = plan.chunkCount; i > 0; --i)
        order.push_back(i - 1);

    bool allGood = true;
    for (uint64_t i : order) {
        ferry::Bytes cipher;
        if (!ferry::sealChunk(keys.data, noncePrefix, i, plan.chunkCount,
                              volume.data() + plan.offsetOf(i), plan.sizeOf(i), cipher)) {
            allGood = false;
            break;
        }
        ferry::Bytes plainChunk;
        if (!ferry::openChunk(rkeys.data, noncePrefix, i, plan.chunkCount, cipher.data(),
                              cipher.size(), plainChunk)) {
            allGood = false;
            break;
        }
        if (!gotHashes.verify(i, plainChunk.data(), plainChunk.size())) {
            allGood = false;
            break;
        }
        std::memcpy(received.data() + plan.offsetOf(i), plainChunk.data(), plainChunk.size());
    }
    check(allGood, "все чанки прошли шифрование, расшифровку и проверку");
    check(received == volume, "собранный том побайтово равен исходному");
}

} // namespace

int main()
{
    std::printf("Ferry — самотесты ядра\n");

    testBlake3();
    testBase64Url();
    testChunker();
    testHashList();
    testCrypto();
    testJson();
    testManifest();
    testLink();
    testEndToEnd();

    std::printf("\n%s: проверок %d, провалов %d\n",
                g_failures == 0 ? "ВСЁ СОШЛОСЬ" : "ЕСТЬ РАСХОЖДЕНИЯ", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
