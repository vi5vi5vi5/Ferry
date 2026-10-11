// ZIP без сжатия (store, zip64) — как папка уезжает в «Загрузки».
//
// Браузер без доступа к папкам (Firefox, Safari, телефоны) умеет сохранить
// только один файл. Дерево поэтому упаковывается в ZIP — без сжатия: том
// чаще всего уже сжат или шифрован отправителем, а сжимать гигабайты в
// вкладке — минуты процессора ради нескольких процентов.
//
// Архив собирается как Blob из заголовков и СРЕЗОВ тома: байты файлов не
// копируются в память, браузер читает их из OPFS в момент сохранения.
// Единственный проход по данным — CRC32, без которой ZIP не обходится.

const enc = new TextEncoder();

const CRC_TABLE = (() => {
    const t = new Uint32Array(256 * 8);
    for (let n = 0; n < 256; n++) {
        let c = n;
        for (let k = 0; k < 8; k++)
            c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
        t[n] = c >>> 0;
    }
    for (let n = 0; n < 256; n++) {
        let c = t[n];
        for (let k = 1; k < 8; k++) {
            c = t[c & 0xff] ^ (c >>> 8);
            t[k * 256 + n] = c >>> 0;
        }
    }
    return t;
})();

// CRC32 с продолжением: crc32(b, crc32(a)) === crc32(a ‖ b). Восемь байт
// за шаг — так это не самое медленное место экспорта.
export function crc32(bytes, crc = 0) {
    const t = CRC_TABLE;
    let c = ~crc >>> 0;
    let i = 0;
    const n = bytes.length;
    for (; i + 8 <= n; i += 8) {
        const a = (bytes[i] | (bytes[i + 1] << 8) | (bytes[i + 2] << 16) | (bytes[i + 3] << 24)) ^ c;
        c = t[1792 + (a & 0xff)] ^ t[1536 + ((a >>> 8) & 0xff)] ^ t[1280 + ((a >>> 16) & 0xff)]
            ^ t[1024 + (a >>> 24)] ^ t[768 + bytes[i + 4]] ^ t[512 + bytes[i + 5]]
            ^ t[256 + bytes[i + 6]] ^ t[bytes[i + 7]];
    }
    for (; i < n; i++)
        c = t[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
    return ~c >>> 0;
}

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

const MAX32 = 0xffffffff;
const READ_STEP = 8 * 1024 * 1024;

// Собрать ZIP. top — имя корневой папки внутри архива; blob — весь том;
// onProgress(сделано, всего) — по байтам CRC-прохода.
export async function buildZip(manifest, layout, blob, top, onProgress = () => {}) {
    const spans = new Map(layout.files.map((f) => [f.path, f]));
    const parts = [];
    const central = [];
    let offset = 0;
    let done = 0;
    const total = Math.max(1, layout.total);

    const entries = [{ path: '', isDir: true, size: 0, mtime: 0 }, ...manifest.entries];
    for (const e of entries) {
        const name = enc.encode(e.path === '' ? `${top}/` : `${top}/${e.path}${e.isDir ? '/' : ''}`);
        const span = e.isDir || e.size === 0 ? null : spans.get(e.path);
        let crc = 0;
        if (span) {
            for (let at = 0; at < span.size; at += READ_STEP) {
                const n = Math.min(READ_STEP, span.size - at);
                const chunk = new Uint8Array(await blob.slice(span.start + at, span.start + at + n).arrayBuffer());
                crc = crc32(chunk, crc);
                done += n;
                onProgress(done, total);
            }
        }
        const size = e.isDir ? 0 : e.size;
        const big = size >= MAX32;
        const farOffset = offset >= MAX32;
        const { time, date } = dosTime(e.mtime);
        const version = big || farOffset ? 45 : 20;

        const lh = new Writer(30 + name.length + (big ? 20 : 0));
        lh.u32(0x04034b50);
        lh.u16(version);
        lh.u16(0x0800);           // имена в UTF-8
        lh.u16(0);                // store
        lh.u16(time);
        lh.u16(date);
        lh.u32(crc);
        lh.u32(big ? MAX32 : size);
        lh.u32(big ? MAX32 : size);
        lh.u16(name.length);
        lh.u16(big ? 20 : 0);
        lh.bytes(name);
        if (big) {
            lh.u16(0x0001);
            lh.u16(16);
            lh.u64(size);
            lh.u64(size);
        }
        parts.push(lh.buf);
        if (span)
            parts.push(blob.slice(span.start, span.start + span.size));
        central.push({ name, crc, size, big, offset, farOffset, time, date, version, isDir: e.isDir });
        offset += lh.buf.length + size;
    }

    const cdStart = offset;
    for (const c of central) {
        const extraLen = (c.big ? 16 : 0) + (c.farOffset ? 8 : 0);
        const ch = new Writer(46 + c.name.length + (extraLen ? 4 + extraLen : 0));
        ch.u32(0x02014b50);
        ch.u16((3 << 8) | 45);    // сделано в unix: права из external attrs
        ch.u16(c.version);
        ch.u16(0x0800);
        ch.u16(0);
        ch.u16(c.time);
        ch.u16(c.date);
        ch.u32(c.crc);
        ch.u32(c.big ? MAX32 : c.size);
        ch.u32(c.big ? MAX32 : c.size);
        ch.u16(c.name.length);
        ch.u16(extraLen ? 4 + extraLen : 0);
        ch.u16(0);                // комментарий
        ch.u16(0);                // диск
        ch.u16(0);                // внутренние атрибуты
        ch.u32(c.isDir ? ((0o40755 << 16) | 0x10) : (0o100644 << 16));
        ch.u32(c.farOffset ? MAX32 : c.offset);
        ch.bytes(c.name);
        if (extraLen) {
            ch.u16(0x0001);
            ch.u16(extraLen);
            if (c.big) {
                ch.u64(c.size);
                ch.u64(c.size);
            }
            if (c.farOffset)
                ch.u64(c.offset);
        }
        parts.push(ch.buf);
        offset += ch.buf.length;
    }
    const cdSize = offset - cdStart;
    const count = central.length;
    const zip64 = count >= 0xffff || cdStart >= MAX32 || cdSize >= MAX32;

    if (zip64) {
        const z = new Writer(56 + 20);
        z.u32(0x06064b50);
        z.u64(44);
        z.u16((3 << 8) | 45);
        z.u16(45);
        z.u32(0);
        z.u32(0);
        z.u64(count);
        z.u64(count);
        z.u64(cdSize);
        z.u64(cdStart);
        z.u32(0x07064b50);        // локатор
        z.u32(0);
        z.u64(offset);
        z.u32(1);
        parts.push(z.buf);
    }
    const end = new Writer(22);
    end.u32(0x06054b50);
    end.u16(0);
    end.u16(0);
    end.u16(zip64 ? 0xffff : count);
    end.u16(zip64 ? 0xffff : count);
    end.u32(zip64 ? MAX32 : cdSize);
    end.u32(zip64 ? MAX32 : cdStart);
    end.u16(0);
    parts.push(end.buf);
    onProgress(total, total);
    return new Blob(parts, { type: 'application/zip' });
}
