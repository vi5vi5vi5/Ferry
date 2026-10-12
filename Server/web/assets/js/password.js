// Режим «ссылка отдельно, пароль отдельно».
//
// Ключ выводится из пароля: K = Argon2id(пароль, соль, 64 МиБ, 3 прохода),
// соль лежит на релее и отдаётся всем. Дальше всё как с обычным ключом.
//
// Выравнивание пароля обязано совпадать с CLI (Cli/Password.cpp) до байта,
// иначе браузер и терминал выведут из одной фразы разные ключи: пробелы по
// краям срезаются, любые промежутки внутри сжимаются в один пробел,
// латиница и кириллица — строчными, «ё» становится «е». NFC — потому что
// macOS охотно отдаёт «й» двумя знаками.

export const MIN_WORDS = 5;
export const MIN_CHARS = 20;
export const SALT_SIZE = 16;

const SPACE = /[\s\u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000]+/gu;

export function normalize(text) {
    let s = String(text || '').normalize('NFC').replace(SPACE, ' ').trim();
    let out = '';
    for (const ch of s) {
        const cp = ch.codePointAt(0);
        if (cp >= 0x41 && cp <= 0x5a)
            out += String.fromCodePoint(cp + 0x20);
        else if (cp >= 0x410 && cp <= 0x42f)
            out += String.fromCodePoint(cp + 0x20);
        else if (cp === 0x401 || cp === 0x451)
            out += 'е';
        else
            out += ch;
    }
    return out;
}

// Пустая строка — фраза годится.
export function problem(normalized) {
    const words = normalized ? normalized.split(' ').length : 0;
    if (words < MIN_WORDS)
        return `Нужно хотя бы ${MIN_WORDS} слов: пароль здесь — единственная защита тома.`;
    if ([...normalized].length < MIN_CHARS)
        return `Слишком коротко: нужно хотя бы ${MIN_CHARS} знаков.`;
    return '';
}

let worker = null;
let seq = 0;
const waiting = new Map();

function argon2() {
    if (!worker) {
        worker = new Worker(new URL('./argon2.worker.js', import.meta.url));
        worker.onmessage = (e) => {
            const { id, key, error } = e.data || {};
            const w = waiting.get(id);
            if (!w)
                return;
            waiting.delete(id);
            if (error)
                w.reject(new Error(error));
            else
                w.resolve(new Uint8Array(key));
        };
        worker.onerror = (e) => {
            for (const w of waiting.values())
                w.reject(new Error(e.message || 'argon2 worker'));
            waiting.clear();
            worker = null;
        };
    }
    return worker;
}

// 32 байта ключа из уже выровненного пароля.
export function deriveKey(normalized, salt) {
    const id = ++seq;
    return new Promise((resolve, reject) => {
        waiting.set(id, { resolve, reject });
        argon2().postMessage({ id, password: normalized, salt });
    });
}

export function randomSalt() {
    return crypto.getRandomValues(new Uint8Array(SALT_SIZE));
}
