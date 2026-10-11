// Манифест, раскладка тома и арифметика чанков — зеркало core/Manifest.cpp,
// core/VolumeLayout.cpp и core/Chunker.cpp.
//
// Разбор строгий, как в ядре: манифест — это то, по чему получатель будет
// создавать файлы на своём диске, поэтому «почти правильный» отвергается
// целиком, а не чинится. Разойдись эта копия с C++ хоть на байт в
// раскладке — хеши сойдутся, а файлы окажутся склеены со сдвигом.

import { b64urlDecode, b64urlEncode } from './b64.js';

const MAX_ENTRIES = 100000;
const MAX_NAME_BYTES = 1024;
const MAX_PATH_BYTES = 4096;

const enc = new TextEncoder();
const byteLen = (s) => enc.encode(s).length;

export class ManifestError extends Error {}

// ---- чанки ----

// Размер чанка пришёл снаружи (из метаданных раздачи). null — размер
// невозможен: не степень двойки в разумных пределах.
export function planWith(total, chunkSize) {
    if (!Number.isSafeInteger(total) || total < 0)
        return null;
    if (!Number.isInteger(chunkSize) || chunkSize < 64 * 1024 || chunkSize > 64 * 1024 * 1024)
        return null;
    if ((chunkSize & (chunkSize - 1)) !== 0)
        return null;
    const count = Math.ceil(total / chunkSize);
    return {
        total,
        chunkSize,
        count,
        offsetOf: (i) => i * chunkSize,
        sizeOf: (i) => (i + 1 < count ? chunkSize : i + 1 === count ? total - i * chunkSize : 0),
    };
}

// ---- пути ----

function isReservedWindowsName(segment) {
    const dot = segment.indexOf('.');
    const base = (dot < 0 ? segment : segment.slice(0, dot)).toLowerCase();
    if (base === 'con' || base === 'prn' || base === 'aux' || base === 'nul')
        return true;
    return base.length === 4 && (base.startsWith('com') || base.startsWith('lpt'))
        && base[3] >= '1' && base[3] <= '9';
}

function segmentProblem(segment) {
    if (segment === '')
        return 'пустой сегмент пути';
    if (segment === '.' || segment === '..')
        return 'сегмент пути выходит за корень';
    for (let i = 0; i < segment.length; i++) {
        const c = segment.charCodeAt(i);
        if (c < 0x20 || c === 0x7f)
            return 'управляющий символ в пути';
        if ('<>:"|?*\\'.includes(segment[i]))
            return 'недопустимый символ в пути';
    }
    const last = segment[segment.length - 1];
    if (last === '.' || last === ' ')
        return 'сегмент пути кончается точкой или пробелом';
    if (isReservedWindowsName(segment))
        return 'зарезервированное имя Windows в пути';
    return null;
}

// Санитизация пути из описи (класс Zip Slip). null — путь годен, иначе
// причина отказа.
export function relPathProblem(path) {
    if (typeof path !== 'string' || path === '' || byteLen(path) > MAX_PATH_BYTES)
        return 'путь пуст или слишком длинный';
    if (path[0] === '/')
        return 'абсолютный путь';
    if (path.length >= 2 && path[1] === ':')
        return 'путь с буквой диска';
    for (const segment of path.split('/')) {
        const why = segmentProblem(segment);
        if (why)
            return why;
    }
    return null;
}

// Имя одиночного файла. Здесь, наоборот, чиним: отправитель мог прислать
// путь целиком, а получателю нужно одно безопасное имя. Пустая строка —
// из имени не осталось ничего пригодного.
export function safeFileName(name) {
    let cut = 0;
    for (let i = 0; i < name.length; i++) {
        if (name[i] === '/' || name[i] === '\\' || name[i] === ':')
            cut = i + 1;
    }
    let out = '';
    for (const ch of name.slice(cut)) {
        const c = ch.codePointAt(0);
        if (c < 0x20 || c === 0x7f)
            continue;
        out += '<>:"|?*\\/'.includes(ch) ? '_' : ch;
    }
    out = out.replace(/[. ]+$/, '').replace(/^ +/, '');
    if (out === '' || out === '.' || out === '..')
        return '';
    if (isReservedWindowsName(out))
        out = '_' + out;
    while (byteLen(out) > MAX_NAME_BYTES)
        out = Array.from(out).slice(0, -1).join('');
    return out;
}

// ---- манифест ----

function int(v) {
    return Number.isSafeInteger(v) ? v : null;
}

export function parseManifest(text) {
    let o;
    try {
        o = JSON.parse(text);
    } catch (e) {
        throw new ManifestError('манифест не разбирается');
    }
    if (!o || typeof o !== 'object' || Array.isArray(o))
        throw new ManifestError('манифест не объект');
    if (o.v !== 1)
        throw new ManifestError('незнакомая версия манифеста');
    if (o.kind !== 'file' && o.kind !== 'tree')
        throw new ManifestError('незнакомый вид тома');
    if (typeof o.name !== 'string' || o.name === '' || byteLen(o.name) > MAX_NAME_BYTES)
        throw new ManifestError('пустое или слишком длинное имя тома');
    const total = int(o.total);
    if (total === null || total < 0)
        throw new ManifestError('размер тома отсутствует или отрицателен');

    const m = { v: 1, kind: o.kind, name: o.name, total, root: null, stream: false, entries: [] };

    if (o.hashes !== undefined && o.hashes !== 'stream')
        throw new ManifestError('незнакомый способ доставки хешей');
    m.stream = o.hashes === 'stream';
    if (m.stream) {
        if (o.root !== undefined && o.root !== null)
            throw new ManifestError('у манифеста с хешами на лету не бывает корня');
    } else {
        const root = b64urlDecode(o.root);
        if (!root || root.length !== 32)
            throw new ManifestError('корневой хеш отсутствует или не той длины');
        m.root = root;
    }

    if (m.kind === 'tree') {
        if (!Array.isArray(o.entries))
            throw new ManifestError('у дерева нет описи');
        if (o.entries.length > MAX_ENTRIES)
            throw new ManifestError('в описи слишком много записей');
        let sum = 0;
        for (const item of o.entries) {
            if (!item || typeof item !== 'object')
                throw new ManifestError('запись описи не объект');
            const e = {
                path: typeof item.p === 'string' ? item.p : '',
                size: int(item.s),
                mtime: int(item.m) ?? 0,
                isDir: item.d === true,
            };
            if (e.size === null || e.size < 0)
                throw new ManifestError('у записи описи нет размера');
            const why = relPathProblem(e.path);
            if (why)
                throw new ManifestError('недопустимый путь в описи: ' + why);
            if (e.isDir && e.size !== 0)
                throw new ManifestError('у директории в описи ненулевой размер');
            sum += e.size;
            m.entries.push(e);
        }
        if (sum !== m.total)
            throw new ManifestError('сумма размеров описи не сходится с размером тома');
    }
    return m;
}

// Обратная сторона parseManifest — для отправителя. Поля и их порядок те
// же, что у Manifest::toJson в ядре: root === null означает промежуточный
// манифест раздачи с хешами на лету.
export function manifestToJson(m, root) {
    const o = { v: 1, kind: m.kind, name: m.name, total: m.total };
    if (root)
        o.root = b64urlEncode(root);
    else
        o.hashes = 'stream';
    if (m.kind === 'tree') {
        o.entries = m.entries.map((e) => {
            const item = { p: e.path, s: e.size };
            if (e.mtime)
                item.m = e.mtime;
            if (e.isDir)
                item.d = true;
            return item;
        });
    }
    return JSON.stringify(o);
}

// Порядок описи — побайтовое сравнение путей в UTF-8, как std::string в
// ядре. Сравнение строк JavaScript идёт по UTF-16 и расходится с ним на
// символах за пределами BMP.
export function comparePaths(a, b) {
    const x = enc.encode(a);
    const y = enc.encode(b);
    const n = Math.min(x.length, y.length);
    for (let i = 0; i < n; i++)
        if (x[i] !== y[i])
            return x[i] - y[i];
    return x.length - y.length;
}

export const LIMITS = { entries: MAX_ENTRIES, nameBytes: MAX_NAME_BYTES, pathBytes: MAX_PATH_BYTES };

// Размер чанка по размеру тома — как chooseChunkSize в ядре.
export function chunkSizeFor(total) {
    const MiB = 1024 * 1024;
    if (total < 64 * MiB)
        return 256 * 1024;
    if (total <= 4096 * MiB)
        return MiB;
    return 4 * MiB;
}

// Описывают ли два манифеста один и тот же том, если не смотреть на
// корень. Нужно на hashes_done: итоговый манифест обязан совпасть с
// промежуточным во всём, кроме корня, — до байта в ядре, до поля здесь.
export function sameVolume(a, b) {
    const shape = (m) => JSON.stringify([m.v, m.kind, m.name, m.total,
        m.entries.map((e) => [e.path, e.size, e.mtime, e.isDir])]);
    return shape(a) === shape(b);
}

// ---- раскладка ----

// Какой байт тома какому файлу принадлежит. Том — конкатенация файлов
// описи без выравнивания; пустые папки и файлы нулевой длины байт не
// несут и в раскладку не попадают.
export function buildLayout(m) {
    const files = [];
    let total = 0;
    if (m.kind !== 'tree') {
        if (m.total > 0)
            files.push({ path: m.name, size: m.total, start: 0, mtime: 0 });
        return { files, total: m.total };
    }
    for (const e of m.entries) {
        if (e.isDir || e.size === 0)
            continue;
        files.push({ path: e.path, size: e.size, start: total, mtime: e.mtime });
        total += e.size;
    }
    return { files, total };
}

// Разложить диапазон тома [from, from + length) по файлам.
export function slice(layout, from, length) {
    const out = [];
    const files = layout.files;
    if (length === 0 || from >= layout.total || from + length > layout.total)
        return out;
    let lo = 0;
    let hi = files.length;
    while (lo + 1 < hi) {
        const mid = (lo + hi) >>> 1;
        if (files[mid].start <= from)
            lo = mid;
        else
            hi = mid;
    }
    let left = length;
    for (let i = lo; i < files.length && left > 0; i++) {
        const f = files[i];
        if (from >= f.start + f.size)
            continue;
        const inFile = from - f.start;
        const take = Math.min(left, f.size - inFile);
        out.push({ file: i, offset: inFile, length: take });
        from += take;
        left -= take;
    }
    return out;
}

// Сводка для экрана: сколько файлов и папок, без учёта корня.
export function summarize(m) {
    if (m.kind !== 'tree')
        return { files: 1, dirs: 0, emptyDirs: 0 };
    let files = 0;
    let emptyDirs = 0;
    const dirs = new Set();
    for (const e of m.entries) {
        if (e.isDir) {
            emptyDirs++;
            dirs.add(e.path);
        } else {
            files++;
        }
        const parts = e.path.split('/');
        for (let i = 1; i < parts.length; i++)
            dirs.add(parts.slice(0, i).join('/'));
    }
    return { files, dirs: dirs.size, emptyDirs };
}
