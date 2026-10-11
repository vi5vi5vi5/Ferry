// Множество чанков и диапазоны — зеркало core/ChunkSet.cpp.
//
// Границы диапазонов включительные с обеих сторон: [5,5] — ровно один
// чанк. Этими диапазонами клиент и сервер разговаривают по проводу, и
// трактовка границ обязана совпадать с ядром, иначе ошибка проявится
// дыркой в томе, а не исключением.

export class ChunkSet {
    constructor(count = 0) {
        this.reset(count);
    }

    reset(count) {
        this.count = count;
        this.bytes = new Uint8Array(Math.ceil(count / 8));
        this.have = 0;
    }

    has(i) {
        return i >= 0 && i < this.count && (this.bytes[i >>> 3] & (1 << (i & 7))) !== 0;
    }

    set(i) {
        if (i < 0 || i >= this.count || this.has(i))
            return false;
        this.bytes[i >>> 3] |= 1 << (i & 7);
        this.have++;
        return true;
    }

    clear(i) {
        if (!this.has(i))
            return false;
        this.bytes[i >>> 3] &= ~(1 << (i & 7));
        this.have--;
        return true;
    }

    setRange(from, to) {
        if (from < 0 || to < from || to >= this.count)
            return false;
        for (let i = from; i <= to; i++)
            this.set(i);
        return true;
    }

    get empty() {
        return this.have === 0;
    }

    get full() {
        return this.count !== 0 && this.have === this.count;
    }

    // Сколько чанков подряд есть с начала.
    prefix() {
        return this.firstMissing(0);
    }

    firstMissing(from) {
        let i = from;
        while (i < this.count) {
            if ((i & 7) === 0 && this.bytes[i >>> 3] === 0xff) {
                i += 8;
                continue;
            }
            if (!this.has(i))
                return i;
            i++;
        }
        return this.count;
    }

    firstPresent(from) {
        let i = from;
        while (i < this.count) {
            if ((i & 7) === 0 && this.bytes[i >>> 3] === 0) {
                i += 8;
                continue;
            }
            if (this.has(i))
                return i;
            i++;
        }
        return this.count;
    }

    ranges() {
        const out = [];
        let i = this.firstPresent(0);
        while (i < this.count) {
            const end = this.firstMissing(i);
            out.push([i, end - 1]);
            i = this.firstPresent(end);
        }
        return out;
    }

    missing() {
        const out = [];
        let i = this.firstMissing(0);
        while (i < this.count) {
            const end = this.firstPresent(i);
            out.push([i, end - 1]);
            i = this.firstMissing(end);
        }
        return out;
    }

    loadBits(bytes) {
        if (bytes.length !== this.bytes.length)
            return false;
        this.bytes.set(bytes);
        // Хвост последнего байта за пределами тома обязан быть пустым.
        const extra = this.bytes.length * 8 - this.count;
        if (extra > 0)
            this.bytes[this.bytes.length - 1] &= 0xff >>> extra;
        let n = 0;
        for (const b of this.bytes) {
            let v = b;
            while (v) {
                v &= v - 1;
                n++;
            }
        }
        this.have = n;
        return true;
    }

    clone() {
        const c = new ChunkSet(this.count);
        c.bytes.set(this.bytes);
        c.have = this.have;
        return c;
    }
}
