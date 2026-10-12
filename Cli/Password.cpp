#include "Cli/Password.h"

#include <cstdint>
#include <vector>

#include "argon2.h"
#include "core/Protocol.h"
#include "core/Unicode.h"

namespace ferry::cli {
namespace {

// UTF-8 в кодовые точки и обратно. Битую последовательность пропускаем как
// есть, байтом: исправлять за человека его пароль мы не будем, а
// выравнивать в ней нечего.
std::vector<uint32_t> decode(const std::string &s)
{
    std::vector<uint32_t> out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = uint8_t(s[i]);
        int len = 1;
        uint32_t cp = c;
        if (c >= 0xF0 && c < 0xF8) {
            len = 4;
            cp = c & 0x07;
        } else if (c >= 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if (c >= 0xC0) {
            len = 2;
            cp = c & 0x1F;
        }
        if (len > 1 && i + size_t(len) <= s.size()) {
            bool ok = true;
            for (int k = 1; k < len; ++k) {
                const uint8_t cc = uint8_t(s[i + size_t(k)]);
                if ((cc & 0xC0) != 0x80) {
                    ok = false;
                    break;
                }
                cp = (cp << 6) | (cc & 0x3F);
            }
            if (ok) {
                out.push_back(cp);
                i += size_t(len);
                continue;
            }
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

void encode(uint32_t cp, std::string &out)
{
    if (cp < 0x80) {
        out.push_back(char(cp));
    } else if (cp < 0x800) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

bool isSpace(uint32_t cp)
{
    // Пробельные из ASCII, неразрывный пробел и пробелы общего блока:
    // мессенджеры охотно подменяют обычный пробел неразрывным. U+FEFF (BOM)
    // — потому что его считает пробелом \s браузера, а Блокнот кладёт его
    // в начало файла, из которого фразу потом копируют.
    return cp == 0x20 || (cp >= 0x09 && cp <= 0x0D) || cp == 0xA0 || cp == 0x1680
           || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F
           || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

uint32_t fold(uint32_t cp)
{
    if (cp >= 'A' && cp <= 'Z')
        return cp + 0x20;
    if (cp >= 0x0410 && cp <= 0x042F)   // А..Я
        return cp + 0x20;
    if (cp == 0x0401 || cp == 0x0451)   // Ё, ё
        return 0x0435;                  // е
    return cp;
}

} // namespace

std::string normalizePassword(const std::string &utf8)
{
    // Сначала NFC, как normalize('NFC') в браузере: иначе «й», вставленная
    // двумя знаками (и + U+0306), дала бы другой ключ.
    std::vector<uint32_t> cps = decode(utf8);
    toNfc(cps);
    std::string out;
    bool pendingSpace = false;
    for (uint32_t cp : cps) {
        if (isSpace(cp)) {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(' ');
            pendingSpace = false;
        }
        encode(fold(cp), out);
    }
    return out;
}

std::string passwordProblem(const std::string &normalized)
{
    int words = normalized.empty() ? 0 : 1;
    for (char c : normalized)
        if (c == ' ')
            ++words;
    if (words < kPasswordMinWords)
        return "нужна фраза хотя бы из " + std::to_string(kPasswordMinWords)
               + " слов: пароль здесь — единственная защита тома";
    if (decode(normalized).size() < kPasswordMinChars)
        return "фраза слишком короткая: нужно хотя бы " + std::to_string(kPasswordMinChars)
               + " знаков";
    return {};
}

bool passwordKey(const std::string &normalized, const uint8_t *salt, size_t saltLen, Key32 &out)
{
    if (saltLen != kPasswordSaltSize)
        return false;
    return argon2id_hash_raw(kArgon2Iterations, kArgon2MemoryKiB, kArgon2Lanes,
                             normalized.data(), normalized.size(), salt, saltLen, out.data(),
                             out.size())
           == ARGON2_OK;
}

} // namespace ferry::cli
