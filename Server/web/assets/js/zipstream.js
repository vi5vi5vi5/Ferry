// ZIP без сжатия, собираемый на лету, — для приёма потоком (путь C).
//
// zip.js собирает архив из тома, который уже лежит целиком: CRC считается
// заранее и пишется в заголовок. Здесь тома нет — байты идут из сети по
// порядку и сразу уходят в «Загрузки». Поэтому CRC каждого файла
// становится известен только в его конце и пишется в дескриптор данных
// (бит 3), а заголовок перед данными несёт нули. Размеры же известны
// заранее из описи — значит заранее известен и размер всего архива до
// байта: менеджер загрузок показывает честный процент.
//
// Порядок записей — порядок описи, тот же, что у zip.js и у раскладки тома:
// байты файлов в потоке тома идут ровно в этом порядке, и архив пишется
// одним проходом без единого возврата назад.

import { crc32 } from './zip.js';

const enc = new TextEncoder();
const MAX32 = 0xffffffff;

function dosTime(sec) {
    const d = new Date(sec > 0 ? sec * 1000 : Date.now());
    const year = Math.max(1980, d.getFullYear());
    return {
        time: (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1),
        date: ((year - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate(),
    };
}

class Writer {
    constructor(size) {
        this.buf = new Uint8Array(size);
        this.dv = new DataView(this.buf.buffer);
        this.o = 0;
    }
    u16(v) { this.dv.setUint16(this.o, v, true); this.o += 2; }
    u32(v) { this.dv.setUint32(this.o, v >>> 0, true); this.o += 4; }
    u64(v) {
        this.dv.setUint32(this.o, v % 0x100000000, true);
        this.dv.setUint32(this.o + 4, Math.floor(v / 0x100000000), true);
        this.o += 8;
    }
    bytes(b) { this.buf.set(b, this.o); this.o += b.length; }
}

export class ZipStream {
    // top — имя корневой папки внутри архива; emit(u8) — куда отдавать байты.
    constructor(manifest, top, emit) {
        this.emit = emit;
        this.entries = [];
        let offset = 0;
        const all = [{ path: '', isDir: true, size: 0, mtime: 0 }, ...manifest.entries];
        for (const e of all) {
            const name = enc.encode(e.path === '' ? `${top}/` : `${top}/${e.path}${e.isDir ? '/' : ''}`);
            const size = e.isDir ? 0 : e.size;
            // Дескриптор — только там, где есть данные: у пустого файла и у
            // папки CRC известен заранее (ноль), и заголовок пишется честно.
            const desc = size > 0;
            const big = size >= MAX32;
            const head = 30 + name.length + (big ? 20 : 0);
            const tail = desc ? (big ? 24 : 16) : 0;
            const { time, date } = dosTime(e.mtime);
            const farOffset = offset >= MAX32;
            this.entries.push({
                name, size, desc, big, offset, farOffset, time, date, isDir: !!e.isDir,
                version: big || farOffset ? 45 : 20, crc: 0,
            });
            offset += head + size + tail;
        }
        this.cdStart = offset;
        let cd = 0;
        for (const c of this.entries) {
            const extraLen = (c.big ? 16 : 0) + (c.farOffset ? 8 : 0);
            cd += 46 + c.name.length + (extraLen ? 4 + extraLen : 0);
        }
        this.cdSize = cd;
        this.zip64 = this.entries.length >= 0xffff || this.cdStart >= MAX32 || this.cdSize >= MAX32;
        this.size = this.cdStart + this.cdSize + (this.zip64 ? 76 : 0) + 22;

        this.k = 0;          // текущая запись
        this.inFile = 0;     // сколько её данных уже прошло
        this.crc = 0;
        this.started = false;
    }

    // Записи без данных (папки, пустые файлы) — сразу, как до них дошла
    // очередь: ждать им нечего.
    skipEmpty() {
        while (this.k < this.entries.length && !this.entries[this.k].desc) {
            this.emit(this.localHeader(this.entries[this.k]));
            this.k++;
        }
    }

    // Очередной кусок тома — строго следом за предыдущим.
    feed(bytes) {
        if (!this.started) {
            this.started = true;
            this.skipEmpty();
        }
        let p = 0;
        while (p < bytes.length) {
            const e = this.entries[this.k];
            if (!e)
                throw new Error('данных больше, чем в описи');
            if (this.inFile === 0)
                this.emit(this.localHeader(e));
            const take = Math.min(bytes.length - p, e.size - this.inFile);
            const piece = bytes.subarray(p, p + take);
            this.crc = crc32(piece, this.crc);
            this.emit(piece);
            this.inFile += take;
            p += take;
            if (this.inFile === e.size) {
                e.crc = this.crc;
                this.emit(this.descriptor(e));
                this.k++;
                this.inFile = 0;
                this.crc = 0;
                this.skipEmpty();
            }
        }
    }

    // Всё пришло: хвостовые пустые записи, центральный каталог, конец.
    finish() {
        if (!this.started) {
            this.started = true;
            this.skipEmpty();
        }
        if (this.k !== this.entries.length || this.inFile !== 0)
            throw new Error('архив не дописан: данных меньше, чем в описи');
        for (const c of this.entries)
            this.emit(this.centralHeader(c));
        if (this.zip64) {
            const z = new Writer(76);
            z.u32(0x06064b50);
            z.u64(44);
            z.u16((3 << 8) | 45);
            z.u16(45);
            z.u32(0);
            z.u32(0);
            z.u64(this.entries.length);
            z.u64(this.entries.length);
            z.u64(this.cdSize);
            z.u64(this.cdStart);
            z.u32(0x07064b50);        // локатор
            z.u32(0);
            z.u64(this.cdStart + this.cdSize);
            z.u32(1);
            this.emit(z.buf);
        }
        const end = new Writer(22);
        end.u32(0x06054b50);
        end.u16(0);
        end.u16(0);
        end.u16(this.zip64 ? 0xffff : this.entries.length);
        end.u16(this.zip64 ? 0xffff : this.entries.length);
        end.u32(this.zip64 ? MAX32 : this.cdSize);
        end.u32(this.zip64 ? MAX32 : this.cdStart);
        end.u16(0);
        this.emit(end.buf);
    }

    localHeader(e) {
        const w = new Writer(30 + e.name.length + (e.big ? 20 : 0));
        w.u32(0x04034b50);
        w.u16(e.version);
        w.u16(e.desc ? 0x0808 : 0x0800);   // UTF-8; дескриптор после данных
        w.u16(0);                          // store
        w.u16(e.time);
        w.u16(e.date);
        // С дескриптором CRC и размеры здесь — нули (APPNOTE 4.4.9); у
        // большого файла — 0xFFFFFFFF и zip64-поле, тогда дескриптор с
        // восьмибайтными размерами.
        w.u32(0);
        w.u32(e.big ? MAX32 : 0);
        w.u32(e.big ? MAX32 : 0);
        w.u16(e.name.length);
        w.u16(e.big ? 20 : 0);
        w.bytes(e.name);
        if (e.big) {
            w.u16(0x0001);
            w.u16(16);
            w.u64(0);
            w.u64(0);
        }
        return w.buf;
    }

    descriptor(e) {
        const w = new Writer(e.big ? 24 : 16);
        w.u32(0x08074b50);
        w.u32(e.crc);
        if (e.big) {
            w.u64(e.size);
            w.u64(e.size);
        } else {
            w.u32(e.size);
            w.u32(e.size);
        }
        return w.buf;
    }

    centralHeader(c) {
        const extraLen = (c.big ? 16 : 0) + (c.farOffset ? 8 : 0);
        const w = new Writer(46 + c.name.length + (extraLen ? 4 + extraLen : 0));
        w.u32(0x02014b50);
        w.u16((3 << 8) | 45);    // сделано в unix: права из external attrs
        w.u16(c.version);
        w.u16(c.desc ? 0x0808 : 0x0800);
        w.u16(0);
        w.u16(c.time);
        w.u16(c.date);
        w.u32(c.crc);
        w.u32(c.big ? MAX32 : c.size);
        w.u32(c.big ? MAX32 : c.size);
        w.u16(c.name.length);
        w.u16(extraLen ? 4 + extraLen : 0);
        w.u16(0);                // комментарий
        w.u16(0);                // диск
        w.u16(0);                // внутренние атрибуты
        w.u32(c.isDir ? ((0o40755 << 16) | 0x10) : (0o100644 << 16));
        w.u32(c.farOffset ? MAX32 : c.offset);
        w.bytes(c.name);
        if (extraLen) {
            w.u16(0x0001);
            w.u16(extraLen);
            if (c.big) {
                w.u64(c.size);
                w.u64(c.size);
            }
            if (c.farOffset)
                w.u64(c.offset);
        }
        return w.buf;
    }
}
