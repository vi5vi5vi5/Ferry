#include "Base64Url.h"

namespace ferry {
namespace {

constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// -1 — символ не из алфавита. Таблица строится один раз при первом обращении.
const int8_t *decodeTable()
{
    static int8_t table[256];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 256; ++i)
            table[i] = -1;
        for (int i = 0; i < 64; ++i)
            table[static_cast<uint8_t>(kAlphabet[i])] = static_cast<int8_t>(i);
        // Классический алфавит принимаем тоже — см. комментарий в заголовке.
        table[static_cast<uint8_t>('+')] = 62;
        table[static_cast<uint8_t>('/')] = 63;
        ready = true;
    }
    return table;
}

} // namespace

std::string base64UrlEncode(const uint8_t *data, size_t len)
{
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
        i += 3;
    }
    const size_t rest = len - i;
    if (rest == 1) {
        const uint32_t v = uint32_t(data[i]) << 16;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
    } else if (rest == 2) {
        const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
    }
    return out;
}

std::string base64UrlEncode(const std::vector<uint8_t> &data)
{
    return base64UrlEncode(data.data(), data.size());
}

bool base64UrlDecode(const std::string &text, std::vector<uint8_t> &out)
{
    const int8_t *table = decodeTable();

    size_t len = text.size();
    while (len > 0 && text[len - 1] == '=')
        --len;
    // Длина 4k+1 невозможна ни при каком входе: один символ несёт 6 бит,
    // а байт — 8, и остаток в 6 бит не образует ничего.
    if (len % 4 == 1)
        return false;

    out.clear();
    out.reserve(len / 4 * 3 + 2);

    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; ++i) {
        const int8_t v = table[static_cast<uint8_t>(text[i])];
        if (v < 0)
            return false;
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    // Хвостовые биты обязаны быть нулями. Если это не так, строку кто-то
    // подправил руками, и молча проглатывать такое нельзя: в ключе это
    // означало бы, что два разных текста дают один ключ.
    if (bits > 0 && (acc & ((1u << bits) - 1)) != 0)
        return false;
    return true;
}

} // namespace ferry
