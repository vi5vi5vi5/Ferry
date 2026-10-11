// base64url без выравнивания — зеркало core/Base64Url.cpp.
//
// Строгость та же, что в C++: длина 4k+1 и ненулевые хвостовые биты
// отвергаются. В ключе это не формальность — иначе два разных текста
// давали бы один и тот же ключ.

const ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';

const DECODE = new Int8Array(256).fill(-1);
for (let i = 0; i < 64; i++)
    DECODE[ALPHABET.charCodeAt(i)] = i;
// Классический алфавит принимаем тоже, как и ядро.
DECODE['+'.charCodeAt(0)] = 62;
DECODE['/'.charCodeAt(0)] = 63;

export function b64urlEncode(bytes) {
    const b = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    let out = '';
    let i = 0;
    for (; i + 3 <= b.length; i += 3) {
        const v = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
        out += ALPHABET[(v >> 18) & 63] + ALPHABET[(v >> 12) & 63]
            + ALPHABET[(v >> 6) & 63] + ALPHABET[v & 63];
    }
    const rest = b.length - i;
    if (rest === 1) {
        const v = b[i] << 16;
        out += ALPHABET[(v >> 18) & 63] + ALPHABET[(v >> 12) & 63];
    } else if (rest === 2) {
        const v = (b[i] << 16) | (b[i + 1] << 8);
        out += ALPHABET[(v >> 18) & 63] + ALPHABET[(v >> 12) & 63] + ALPHABET[(v >> 6) & 63];
    }
    return out;
}

// null — строка не base64url.
export function b64urlDecode(text) {
    if (typeof text !== 'string')
        return null;
    let len = text.length;
    while (len > 0 && text.charCodeAt(len - 1) === 61 /* = */)
        len--;
    if (len % 4 === 1)
        return null;
    const out = new Uint8Array(Math.floor(len * 3 / 4));
    let acc = 0;
    let bits = 0;
    let o = 0;
    for (let i = 0; i < len; i++) {
        const c = text.charCodeAt(i);
        const v = c < 256 ? DECODE[c] : -1;
        if (v < 0)
            return null;
        acc = ((acc << 6) | v) & 0xffffff;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (acc >> bits) & 0xff;
        }
    }
    if (bits > 0 && (acc & ((1 << bits) - 1)) !== 0)
        return null;
    return o === out.length ? out : out.subarray(0, o);
}
