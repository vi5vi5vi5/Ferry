// Груз: из того, что человек выбрал или перетащил, собрать опись тома —
// зеркало Cli/Tree.cpp.
//
// Правила те же, что у CLI, потому что получатель один и тот же:
// - один файл — том вида file, папка — вида tree с её именем;
// - пути относительные, POSIX, отсортированы побайтово в UTF-8;
// - пустая папка — отдельная запись, иначе она не доедет;
// - имя, которое не пройдёт на другой стороне, видно до ссылки.
//
// Отличие одно, и оно в пользу человека: CLI на непригодном имени
// останавливается, а здесь такой файл уходит в «пропущено» с причиной.
// Переименовать файл из вкладки браузера нельзя, а отказ целиком из-за
// одного Thumbs:db в папке на сорок тысяч файлов — плохая сделка.

import { relPathProblem, safeFileName, comparePaths, LIMITS } from './manifest.js';

const MAX_DEPTH = 64;

export class CargoError extends Error {}

// Имя тома для нескольких файлов без общей папки.
function looseName() {
    const d = new Date();
    const p = (n) => String(n).padStart(2, '0');
    return `ferry-${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`;
}

class Collector {
    constructor() {
        this.entries = [];
        this.skipped = [];
    }

    file(path, file) {
        const why = relPathProblem(path);
        if (why) {
            this.skipped.push({ path, why });
            return false;
        }
        if (this.entries.length >= LIMITS.entries)
            throw new CargoError(`В томе больше ${LIMITS.entries.toLocaleString('ru-RU')} файлов — столько за раз не возим.`);
        this.entries.push({ path, size: file.size, mtime: Math.floor((file.lastModified || 0) / 1000), isDir: false, file });
        return true;
    }

    emptyDir(path) {
        const why = relPathProblem(path);
        if (why) {
            this.skipped.push({ path: path + '/', why });
            return;
        }
        this.entries.push({ path, size: 0, mtime: 0, isDir: true, file: null });
    }
}

function finish(c, kind, name, loose = false) {
    if (kind === 'file') {
        const e = c.entries[0];
        return {
            manifest: { v: 1, kind: 'file', name, total: e.size, entries: [] },
            files: e.size > 0 ? [e.file] : [],
            skipped: c.skipped,
            fileCount: 1,
            emptyDirs: 0,
            total: e.size,
        };
    }
    if (c.entries.length === 0)
        throw new CargoError(c.skipped.length
            ? 'Ни один файл не пройдёт на другой стороне — везти нечего.'
            : 'Папка пуста — везти нечего.');
    c.entries.sort((a, b) => comparePaths(a.path, b.path));
    let total = 0;
    let fileCount = 0;
    let emptyDirs = 0;
    const files = [];
    for (const e of c.entries) {
        if (e.isDir) {
            emptyDirs++;
            continue;
        }
        fileCount++;
        total += e.size;
        if (e.size > 0)
            files.push(e.file);
    }
    return {
        manifest: {
            v: 1, kind: 'tree', name, total,
            entries: c.entries.map(({ path, size, mtime, isDir }) => ({ path, size, mtime, isDir })),
        },
        files,
        skipped: c.skipped,
        fileCount,
        emptyDirs,
        total,
        loose,
    };
}

function checkName(name) {
    if (!name)
        throw new CargoError('Из имени не получилось ничего пригодного для другой стороны.');
    return name;
}

// <input type="file" multiple> или <input webkitdirectory>.
export function cargoFromFileList(list) {
    const all = Array.from(list || []);
    if (all.length === 0)
        throw new CargoError('Ничего не выбрано.');
    const rel = (f) => f.webkitRelativePath || '';

    // Папка: у каждого файла путь вида «Папка/внутри/файл».
    if (rel(all[0]).includes('/')) {
        const top = rel(all[0]).split('/')[0];
        const c = new Collector();
        for (const f of all) {
            const parts = rel(f).split('/');
            if (parts[0] !== top || parts.length < 2)
                continue;
            if (parts.length - 1 > MAX_DEPTH)
                throw new CargoError(`Дерево глубже ${MAX_DEPTH} уровней.`);
            c.file(parts.slice(1).join('/'), f);
        }
        return finish(c, 'tree', checkName(safeFileName(top)));
    }

    if (all.length === 1) {
        const name = checkName(safeFileName(all[0].name));
        const c = new Collector();
        c.entries.push({ path: name, size: all[0].size, mtime: 0, isDir: false, file: all[0] });
        return finish(c, 'file', name);
    }

    const c = new Collector();
    for (const f of all)
        c.file(f.name, f);
    return finish(c, 'tree', looseName(), true);
}

// ---- перетаскивание ----

const entryFile = (entry) => new Promise((resolve, reject) => entry.file(resolve, reject));

async function readAll(dirEntry) {
    const reader = dirEntry.createReader();
    const out = [];
    // readEntries отдаёт записи порциями (в Chrome — по сто) и пустой
    // массив в конце; один вызов — это не весь каталог.
    for (;;) {
        const batch = await new Promise((resolve, reject) => reader.readEntries(resolve, reject));
        if (batch.length === 0)
            return out;
        out.push(...batch);
    }
}

async function walk(c, entry, path, depth) {
    if (depth > MAX_DEPTH)
        throw new CargoError(`Дерево глубже ${MAX_DEPTH} уровней — похоже на цикл по ссылкам.`);
    if (entry.isFile) {
        let file;
        try {
            file = await entryFile(entry);
        } catch {
            c.skipped.push({ path, why: 'не читается' });
            return false;
        }
        return c.file(path, file);
    }
    let children;
    try {
        children = await readAll(entry);
    } catch {
        c.skipped.push({ path: path + '/', why: 'папка не читается' });
        return false;
    }
    let had = false;
    for (const child of children) {
        const childPath = path ? `${path}/${child.name}` : child.name;
        if (child.isDirectory) {
            had = true;
            await walk(c, child, childPath, depth + 1);
        } else if (await walk(c, child, childPath, depth + 1)) {
            had = true;
        }
    }
    if (!had && path)
        c.emptyDir(path);
    return had;
}

// DataTransfer из события drop. Записи надо достать синхронно, внутри
// обработчика: после первого await список очищается.
export function dropEntries(dataTransfer) {
    const out = [];
    for (const item of Array.from(dataTransfer.items || [])) {
        if (item.kind !== 'file')
            continue;
        const entry = item.webkitGetAsEntry ? item.webkitGetAsEntry() : null;
        if (entry)
            out.push(entry);
    }
    if (out.length === 0 && dataTransfer.files && dataTransfer.files.length)
        return { files: dataTransfer.files };
    return { entries: out };
}

export async function cargoFromDrop(drop) {
    if (drop.files)
        return cargoFromFileList(drop.files);
    const entries = drop.entries;
    if (entries.length === 0)
        throw new CargoError('Перетащили что-то, что не похоже на файл или папку.');

    if (entries.length === 1 && entries[0].isFile) {
        let file;
        try {
            file = await entryFile(entries[0]);
        } catch {
            throw new CargoError('Файл не читается.');
        }
        const name = checkName(safeFileName(file.name));
        const c = new Collector();
        c.entries.push({ path: name, size: file.size, mtime: 0, isDir: false, file });
        return finish(c, 'file', name);
    }

    const c = new Collector();
    if (entries.length === 1) {
        await walk(c, entries[0], '', 0);
        return finish(c, 'tree', checkName(safeFileName(entries[0].name)));
    }
    for (const e of entries)
        await walk(c, e, e.name, 1);
    return finish(c, 'tree', looseName(), true);
}
