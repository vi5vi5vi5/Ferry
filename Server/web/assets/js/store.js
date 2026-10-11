// Куда пишутся байты (§7 проектного документа).
//
// Две волны требуют записи вне порядка, а отдача другим — ещё и чтения
// того, что уже записано. Поэтому основное место — OPFS: приватная
// файловая система страницы с синхронным произвольным доступом из
// воркера. Она есть во всех нынешних браузерах, переживает перезагрузку
// вкладки (недокачка продолжается) и даёт читать принятое, то есть быть
// источником для опоздавших.
//
// Цена — временно двойное место: по окончании том ещё надо переложить в
// папку или в «Загрузки». Если на двойное места нет, а браузер умеет
// писать в выбранную папку напрямую (Chrome, Edge), включается прямая
// запись: без докачки после перезагрузки и без раздачи, пока идёт приём.
// Совсем маленький том без OPFS можно держать в памяти.
//
// Всё здесь работает внутри воркера: синхронный доступ к OPFS вне
// воркера браузер не даёт.

import { slice } from './manifest.js';

const DIR = 'ferry-recv';
const STATE = 'state.json';
const VOLUME = 'volume';

export const MEMORY_LIMIT = 512 * 1024 * 1024;

export class StoreError extends Error {
    constructor(code, message) {
        super(message);
        this.code = code;
    }
}

function quotaError(e) {
    return e && (e.name === 'QuotaExceededError' || /quota/i.test(String(e.message)));
}

async function writeAll(handle, bytes, at) {
    let done = 0;
    while (done < bytes.length) {
        // В Safari до 17 методы синхронного доступа были асинхронными;
        // await на числе ничего не стоит, а на промисе — спасает.
        const n = await handle.write(done ? bytes.subarray(done) : bytes, { at: at + done });
        if (!n)
            throw new StoreError('io', 'запись на диск вернула ноль байт');
        done += n;
    }
}

export async function opfsDir(create = false) {
    const root = await navigator.storage.getDirectory();
    return root.getDirectoryHandle(DIR, { create });
}

export function opfsSupported() {
    return typeof navigator !== 'undefined' && !!navigator.storage
        && typeof navigator.storage.getDirectory === 'function'
        && typeof FileSystemFileHandle !== 'undefined'
        && 'createSyncAccessHandle' in FileSystemFileHandle.prototype;
}

// Прочитать сохранённое состояние недокачки, не открывая её на запись.
// Нужно странице: показать «продолжить с 42 %» до нажатия кнопки.
export async function readSavedState(id) {
    try {
        const dir = await (await opfsDir(false)).getDirectoryHandle(id);
        const file = await (await dir.getFileHandle(STATE)).getFile();
        return JSON.parse(await file.text());
    } catch {
        return null;
    }
}

// Уборка: недокачки, к которым давно никто не возвращался, и законченные
// тома, которые уже переложены. Храниться в браузере вечно им незачем —
// это чужое место на диске человека.
export async function sweepOld({ keepId = '', incompleteDays = 7, completeHours = 24 } = {}) {
    let dir;
    try {
        dir = await opfsDir(false);
    } catch {
        return;
    }
    const now = Date.now();
    for await (const [name, handle] of dir.entries()) {
        if (name === keepId || handle.kind !== 'directory')
            continue;
        let stale = true;
        try {
            const st = JSON.parse(await (await (await handle.getFileHandle(STATE)).getFile()).text());
            const age = now - (st.updatedAt || 0);
            stale = st.complete ? age > completeHours * 3600e3 : age > incompleteDays * 86400e3;
        } catch {
            // без состояния — обломок; убрать
        }
        if (stale) {
            try {
                await dir.removeEntry(name, { recursive: true });
            } catch {
                // занята другой вкладкой — в следующий раз
            }
        }
    }
}

// ---- OPFS ----

export class OpfsStore {
    constructor(id) {
        this.id = id;
        this.kind = 'opfs';
        this.canRead = true;
        this.persistent = true;
        this.vol = null;     // FileSystemSyncAccessHandle тома
        this.st = null;      // и состояния
        this.dir = null;
        this.volHandle = null;
    }

    async open(plan) {
        const top = await opfsDir(true);
        this.dir = await top.getDirectoryHandle(this.id, { create: true });
        this.volHandle = await this.dir.getFileHandle(VOLUME, { create: true });
        const stHandle = await this.dir.getFileHandle(STATE, { create: true });
        try {
            this.vol = await this.volHandle.createSyncAccessHandle();
            this.st = await stHandle.createSyncAccessHandle();
        } catch (e) {
            if (this.vol)
                await this.vol.close();
            this.vol = null;
            if (e && e.name === 'NoModificationAllowedError')
                throw new StoreError('busy', 'Эту раздачу уже принимает другая вкладка.');
            throw e;
        }

        let saved = null;
        const size = await this.st.getSize();
        if (size > 0 && size < 64 * 1024 * 1024) {
            const buf = new Uint8Array(size);
            await this.st.read(buf, { at: 0 });
            try {
                saved = JSON.parse(new TextDecoder().decode(buf));
            } catch {
                saved = null;
            }
        }

        // Место под весь том — сразу. Квота кончится сейчас, с понятной
        // ошибкой, а не на середине приёма.
        try {
            if ((await this.vol.getSize()) !== plan.total)
                await this.vol.truncate(plan.total);
        } catch (e) {
            if (quotaError(e))
                throw new StoreError('no_space', 'В хранилище браузера не хватает места под том.');
            throw e;
        }
        return saved;
    }

    async write(offset, bytes) {
        try {
            await writeAll(this.vol, bytes, offset);
        } catch (e) {
            if (quotaError(e))
                throw new StoreError('no_space', 'В хранилище браузера кончилось место.');
            throw e;
        }
    }

    async read(offset, len) {
        if (this.vol) {
            const buf = new Uint8Array(len);
            const n = await this.vol.read(buf, { at: offset });
            return n === len ? buf : null;
        }
        const file = await this.volHandle.getFile();
        const buf = new Uint8Array(await file.slice(offset, offset + len).arrayBuffer());
        return buf.length === len ? buf : null;
    }

    async saveState(state) {
        if (!this.st)
            return;
        const bytes = new TextEncoder().encode(JSON.stringify(state));
        await this.st.truncate(0);
        await writeAll(this.st, bytes, 0);
        await this.st.flush();
    }

    async flush() {
        if (this.vol)
            await this.vol.flush();
    }

    // Приём окончен: закрываем запись. Дальше том читается как обычный
    // File — так его можно отдать в «Загрузки» или переложить в папку.
    async finish() {
        if (this.vol) {
            await this.vol.flush();
            await this.vol.close();
            this.vol = null;
        }
        if (this.st) {
            await this.st.close();
            this.st = null;
        }
    }

    async volumeBlob() {
        await this.finish();
        return this.volHandle.getFile();
    }

    async close() {
        try {
            await this.finish();
        } catch {
            // уже закрыто
        }
    }

    async discard() {
        await this.close();
        try {
            await (await opfsDir(false)).removeEntry(this.id, { recursive: true });
        } catch {
            // нечего удалять
        }
    }
}

// ---- память ----

export class MemoryStore {
    constructor() {
        this.kind = 'memory';
        this.canRead = true;
        this.persistent = false;
        this.chunks = [];
        this.plan = null;
    }

    async open(plan) {
        this.plan = plan;
        this.chunks = new Array(plan.count).fill(null);
        return null;
    }

    async write(offset, bytes) {
        this.chunks[offset / this.plan.chunkSize] = bytes;
    }

    async read(offset, len) {
        const c = this.chunks[offset / this.plan.chunkSize];
        return c && c.length === len ? c : null;
    }

    async saveState() {}
    async flush() {}
    async finish() {}

    async volumeBlob() {
        return new Blob(this.chunks.map((c) => c || new Uint8Array(0)));
    }

    async close() {}

    async discard() {
        this.chunks = [];
    }
}

// ---- прямая запись в выбранную папку (Chrome, Edge) ----
//
// Пишем сразу в конечные файлы. createWritable() копит данные во
// временном файле и подменяет настоящий только при close(), поэтому
// прочитать принятое до конца приёма нельзя — отсюда canRead = false и
// никакой раздачи другим, пока идёт приём. По той же причине недокачка
// не переживает закрытие вкладки: незакрытый временный файл браузер
// выбрасывает.

const MAX_OPEN_WRITERS = 32;

export class DirectStore {
    constructor(dirHandle, manifest, layout, name) {
        this.kind = 'fsa';
        this.canRead = false;
        this.persistent = false;
        this.root = dirHandle;
        this.manifest = manifest;
        this.layout = layout;
        this.name = name;
        this.target = null;      // папка тома (для дерева) или сам корень
        this.handles = new Map();  // индекс файла раскладки → FileSystemFileHandle
        this.writers = new Map();  // индекс → { w, last }
        this.left = layout.files.map((f) => f.size);
        this.reopen = new Set();   // файлы, которые уже закрывали недописанными
        this.tick = 0;
    }

    async open() {
        if (this.manifest.kind === 'tree') {
            this.target = await this.root.getDirectoryHandle(this.name, { create: true });
            // Пустые папки и файлы нулевой длины — сразу: байт они не несут,
            // и по ходу приёма до них дело не дойдёт.
            for (const e of this.manifest.entries) {
                if (e.isDir)
                    await mkdirs(this.target, e.path.split('/'));
                else if (e.size === 0)
                    await (await fileIn(this.target, e.path, true)).createWritable().then((w) => w.close());
            }
        } else {
            this.target = this.root;
            if (this.layout.files.length === 0) {
                const h = await this.target.getFileHandle(this.name, { create: true });
                await (await h.createWritable()).close();
            }
        }
        return null;
    }

    async handleFor(i) {
        let h = this.handles.get(i);
        if (!h) {
            h = this.manifest.kind === 'tree'
                ? await fileIn(this.target, this.layout.files[i].path, true)
                : await this.target.getFileHandle(this.name, { create: true });
            this.handles.set(i, h);
        }
        return h;
    }

    async writerFor(i) {
        let entry = this.writers.get(i);
        if (!entry) {
            if (this.writers.size >= MAX_OPEN_WRITERS) {
                // Закрываем самый давний: недописанный файл фиксируется как
                // есть, а при следующем обращении откроется с сохранением.
                let oldest = null;
                for (const [k, v] of this.writers) {
                    if (!oldest || v.last < oldest[1].last)
                        oldest = [k, v];
                }
                this.writers.delete(oldest[0]);
                await oldest[1].w.close();
                this.reopen.add(oldest[0]);
            }
            const h = await this.handleFor(i);
            entry = { w: await h.createWritable({ keepExistingData: this.reopen.has(i) }), last: 0 };
            this.writers.set(i, entry);
        }
        entry.last = ++this.tick;
        return entry.w;
    }

    async write(offset, bytes) {
        try {
            for (const p of slice(this.layout, offset, bytes.length)) {
                const w = await this.writerFor(p.file);
                const start = this.layout.files[p.file].start + p.offset - offset;
                await w.write({ type: 'write', position: p.offset, data: bytes.subarray(start, start + p.length) });
                this.left[p.file] -= p.length;
                if (this.left[p.file] <= 0) {
                    this.writers.delete(p.file);
                    await w.close();
                }
            }
        } catch (e) {
            if (quotaError(e))
                throw new StoreError('no_space', 'На диске кончилось место.');
            throw e;
        }
    }

    async read() {
        return null;
    }

    async saveState() {}
    async flush() {}

    async finish() {
        for (const [, entry] of this.writers)
            await entry.w.close();
        this.writers.clear();
    }

    async volumeBlob() {
        return null;
    }

    async close() {
        // Незакрытые писатели при уходе выбрасываются браузером сами:
        // полуфайл под настоящим именем хуже, чем ничего.
        for (const [, entry] of this.writers) {
            try {
                await entry.w.abort();
            } catch {
                // уже закрыт
            }
        }
        this.writers.clear();
    }

    async discard() {
        await this.close();
    }
}

async function mkdirs(dir, parts) {
    let d = dir;
    for (const p of parts)
        d = await d.getDirectoryHandle(p, { create: true });
    return d;
}

async function fileIn(dir, path, create) {
    const parts = path.split('/');
    const name = parts.pop();
    const parent = await mkdirs(dir, parts);
    return parent.getFileHandle(name, { create });
}

// Имя, которого в папке ещё нет: «том», «том (2)», «том (3)»…
export async function freeName(dir, name, isDir) {
    const exists = async (n) => {
        try {
            if (isDir)
                await dir.getDirectoryHandle(n);
            else
                await dir.getFileHandle(n);
            return true;
        } catch {
            try {
                // занято сущностью другого вида — тоже занято
                if (isDir)
                    await dir.getFileHandle(n);
                else
                    await dir.getDirectoryHandle(n);
                return true;
            } catch {
                return false;
            }
        }
    };
    if (!(await exists(name)))
        return name;
    const dot = isDir ? -1 : name.lastIndexOf('.');
    const stem = dot > 0 ? name.slice(0, dot) : name;
    const ext = dot > 0 ? name.slice(dot) : '';
    for (let k = 2; k < 1000; k++) {
        const n = `${stem} (${k})${ext}`;
        if (!(await exists(n)))
            return n;
    }
    throw new StoreError('exists', 'В папке слишком много одноимённых томов.');
}

// Переложить принятый том из OPFS (или памяти) в выбранную папку.
// blob — весь том одним куском; файлы дерева — его срезы, без копирования
// в память.
export async function exportToDirectory(dir, manifest, layout, blob, name, onProgress) {
    const total = Math.max(1, layout.total);
    if (manifest.kind !== 'tree') {
        const w = await (await dir.getFileHandle(name, { create: true })).createWritable();
        await w.write(blob);
        await w.close();
        onProgress(total, total);
        return;
    }
    const top = await dir.getDirectoryHandle(name, { create: true });
    const spans = new Map(layout.files.map((f) => [f.path, f]));
    let done = 0;
    for (const e of manifest.entries) {
        if (e.isDir) {
            await mkdirs(top, e.path.split('/'));
            continue;
        }
        const w = await (await fileIn(top, e.path, true)).createWritable();
        if (e.size > 0) {
            const span = spans.get(e.path);
            await w.write(blob.slice(span.start, span.start + span.size));
            done += e.size;
        }
        await w.close();
        onProgress(done, total);
    }
    onProgress(total, total);
}
