// Общее для страниц: числа по-русски и буфер обмена.
//
// Числа — с неразрывным пробелом между разрядами и между числом и единицей:
// «12,4 ГиБ» не должно переноситься посередине.

export const NB = ' ';
const UNITS = ['Б', 'КиБ', 'МиБ', 'ГиБ', 'ТиБ', 'ПиБ'];

export const grp = (n) => String(Math.round(n)).replace(/\B(?=(\d{3})+(?!\d))/g, NB);
export const comma = (x, d) => x.toFixed(d).replace('.', ',');

export function plural(n, one, few, many) {
    const a = n % 10;
    const b = n % 100;
    if (a === 1 && b !== 11)
        return one;
    if (a >= 2 && a <= 4 && (b < 12 || b > 14))
        return few;
    return many;
}

export function unitOf(bytes) {
    let i = 0;
    let v = bytes;
    while (v >= 1024 && i < UNITS.length - 1) {
        v /= 1024;
        i++;
    }
    return i;
}

export function inUnit(bytes, u) {
    const v = bytes / 1024 ** u;
    return u === 0 ? grp(v) : comma(v, v >= 100 ? 0 : 1);
}

export const unitName = (u) => UNITS[u];

export const size = (bytes) => {
    const u = unitOf(bytes);
    return inUnit(bytes, u) + NB + UNITS[u];
};

export function dur(ms) {
    const s = Math.max(0, Math.round(ms / 1000));
    if (s < 60)
        return `${s}${NB}с`;
    const m = Math.floor(s / 60);
    if (m < 60)
        return `${m}${NB}мин ${String(s % 60).padStart(2, '0')}${NB}с`;
    const h = Math.floor(m / 60);
    if (h < 48)
        return `${h}${NB}ч ${String(m % 60).padStart(2, '0')}${NB}мин`;
    return `${Math.floor(h / 24)}${NB}дн ${h % 24}${NB}ч`;
}

export const pct = (x) => `${(Math.max(0, Math.min(1, x)) * 100).toFixed(2)}%`;
export const files = (n) => `${grp(n)} ${plural(n, 'файл', 'файла', 'файлов')}`;
export const dirs = (n) => `${grp(n)} ${plural(n, 'папка', 'папки', 'папок')}`;
export const chunks = (n) => `${grp(n)} ${plural(n, 'чанк', 'чанка', 'чанков')}`;

// Скорость в мегабайтах — как её пишут провайдеры и как её печатает CLI.
export const mbps = (bytesPerSec) => comma(bytesPerSec / 1e6, bytesPerSec >= 1e8 ? 0 : 1);

export async function copy(text, btn, label = 'Скопировано') {
    try {
        await navigator.clipboard.writeText(text);
    } catch {
        const ta = document.createElement('textarea');
        ta.value = text;
        document.body.append(ta);
        ta.select();
        document.execCommand('copy');
        ta.remove();
    }
    if (btn) {
        // Подпись может лежать во вложенном span рядом с иконкой.
        const target = btn.querySelector('[data-label]') || btn;
        if (!target.dataset.was)
            target.dataset.was = target.textContent;
        target.textContent = label;
        clearTimeout(btn._copyTimer);
        btn._copyTimer = setTimeout(() => {
            target.textContent = target.dataset.was;
        }, 1500);
    }
}
