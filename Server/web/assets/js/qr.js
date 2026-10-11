// QR-код ссылки — зеркало Cli/third_party/qrcodegen (Nayuki, MIT) для
// браузера, но только та часть, что нужна Ferry: байтовый режим, автоматический
// выбор версии и маски, повышение уровня коррекции, пока данные влезают.
//
// Повторяем qrcodegen_encodeText(text, Ecc_LOW, …, boostEcl=true) из CLI: тот
// же код на экране браузера и в терминале — меньше поводов разойтись. Ссылка
// Ferry всегда содержит строчные буквы, поэтому числовой и буквенно-цифровой
// режимы здесь не нужны.
//
// encodeQr(text) → { size, dark(x, y) }.

const ECC_CODEWORDS_PER_BLOCK = [
    [-1, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30],
    [-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28],
    [-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30, 28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30],
    [-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28, 30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30],
];

const NUM_ERROR_CORRECTION_BLOCKS = [
    [-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 6, 6, 6, 6, 7, 8, 8, 9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25],
    [-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49],
    [-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8, 8, 10, 12, 16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68],
    [-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81],
];

// Уровни коррекции в порядке qrcodegen: LOW, MEDIUM, QUARTILE, HIGH.
const FORMAT_ECL = [1, 0, 3, 2];
const PENALTY_N1 = 3;
const PENALTY_N2 = 3;
const PENALTY_N3 = 40;
const PENALTY_N4 = 10;

function rawDataModules(ver) {
    let result = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        const numAlign = Math.floor(ver / 7) + 2;
        result -= (25 * numAlign - 10) * numAlign - 55;
        if (ver >= 7)
            result -= 36;
    }
    return result;
}

const dataCodewords = (ver, ecl) =>
    Math.floor(rawDataModules(ver) / 8) - ECC_CODEWORDS_PER_BLOCK[ecl][ver] * NUM_ERROR_CORRECTION_BLOCKS[ecl][ver];

function rsMultiply(x, y) {
    let z = 0;
    for (let i = 7; i >= 0; i--) {
        z = ((z << 1) ^ ((z >>> 7) * 0x11D)) & 0xFF;
        z ^= ((y >>> i) & 1) * x;
    }
    return z;
}

function rsDivisor(degree) {
    const result = new Uint8Array(degree);
    result[degree - 1] = 1;
    let root = 1;
    for (let i = 0; i < degree; i++) {
        for (let j = 0; j < degree; j++) {
            result[j] = rsMultiply(result[j], root);
            if (j + 1 < degree)
                result[j] ^= result[j + 1];
        }
        root = rsMultiply(root, 0x02);
    }
    return result;
}

function rsRemainder(data, divisor) {
    const result = new Uint8Array(divisor.length);
    for (const b of data) {
        const factor = b ^ result[0];
        result.copyWithin(0, 1);
        result[result.length - 1] = 0;
        for (let j = 0; j < result.length; j++)
            result[j] ^= rsMultiply(divisor[j], factor);
    }
    return result;
}

function alignmentPositions(ver) {
    if (ver === 1)
        return [];
    const numAlign = Math.floor(ver / 7) + 2;
    const step = Math.floor((ver * 8 + numAlign * 3 + 5) / (numAlign * 4 - 4)) * 2;
    const result = [6];
    for (let i = numAlign - 1, pos = ver * 4 + 10; i >= 1; i--, pos -= step)
        result[i] = pos;
    return result;
}

class Grid {
    constructor(ver) {
        this.size = ver * 4 + 17;
        this.mod = new Uint8Array(this.size * this.size);
        this.fn = new Uint8Array(this.size * this.size);
    }
    get(x, y) {
        return this.mod[y * this.size + x] === 1;
    }
    set(x, y, dark, fn = true) {
        this.mod[y * this.size + x] = dark ? 1 : 0;
        if (fn)
            this.fn[y * this.size + x] = 1;
    }
}

function drawFunctionPatterns(g, ver) {
    const n = g.size;
    for (let i = 0; i < n; i++) {
        g.set(6, i, i % 2 === 0);
        g.set(i, 6, i % 2 === 0);
    }
    for (const [cx, cy] of [[3, 3], [n - 4, 3], [3, n - 4]]) {
        for (let dy = -4; dy <= 4; dy++) {
            for (let dx = -4; dx <= 4; dx++) {
                const dist = Math.max(Math.abs(dx), Math.abs(dy));
                const x = cx + dx;
                const y = cy + dy;
                if (x >= 0 && x < n && y >= 0 && y < n)
                    g.set(x, y, dist !== 2 && dist !== 4);
            }
        }
    }
    const pos = alignmentPositions(ver);
    const na = pos.length;
    for (let i = 0; i < na; i++) {
        for (let j = 0; j < na; j++) {
            if ((i === 0 && j === 0) || (i === 0 && j === na - 1) || (i === na - 1 && j === 0))
                continue;
            for (let dy = -2; dy <= 2; dy++)
                for (let dx = -2; dx <= 2; dx++)
                    g.set(pos[i] + dx, pos[j] + dy, Math.max(Math.abs(dx), Math.abs(dy)) !== 1);
        }
    }
    // Формат резервируем сейчас, рисуем после выбора маски.
    drawFormatBits(g, 0, 0);
    if (ver >= 7) {
        let rem = ver;
        for (let i = 0; i < 12; i++)
            rem = (rem << 1) ^ ((rem >>> 11) * 0x1F25);
        let bits = (ver << 12) | rem;
        for (let i = 0; i < 6; i++) {
            for (let j = 0; j < 3; j++) {
                const k = n - 11 + j;
                g.set(k, i, (bits & 1) !== 0);
                g.set(i, k, (bits & 1) !== 0);
                bits >>>= 1;
            }
        }
    }
}

function drawFormatBits(g, ecl, mask) {
    const data = (FORMAT_ECL[ecl] << 3) | mask;
    let rem = data;
    for (let i = 0; i < 10; i++)
        rem = (rem << 1) ^ ((rem >>> 9) * 0x537);
    const bits = ((data << 10) | rem) ^ 0x5412;
    const bit = (i) => ((bits >>> i) & 1) !== 0;
    for (let i = 0; i <= 5; i++)
        g.set(8, i, bit(i));
    g.set(8, 7, bit(6));
    g.set(8, 8, bit(7));
    g.set(7, 8, bit(8));
    for (let i = 9; i < 15; i++)
        g.set(14 - i, 8, bit(i));
    const n = g.size;
    for (let i = 0; i < 8; i++)
        g.set(n - 1 - i, 8, bit(i));
    for (let i = 8; i < 15; i++)
        g.set(8, n - 15 + i, bit(i));
    g.set(8, n - 8, true);
}

function drawCodewords(g, data) {
    const n = g.size;
    let i = 0;
    for (let right = n - 1; right >= 1; right -= 2) {
        if (right === 6)
            right = 5;
        for (let vert = 0; vert < n; vert++) {
            for (let j = 0; j < 2; j++) {
                const x = right - j;
                const upward = ((right + 1) & 2) === 0;
                const y = upward ? n - 1 - vert : vert;
                if (!g.fn[y * n + x] && i < data.length * 8) {
                    g.set(x, y, ((data[i >>> 3] >>> (7 - (i & 7))) & 1) !== 0, false);
                    i++;
                }
            }
        }
    }
}

function maskBit(mask, x, y) {
    switch (mask) {
        case 0: return (x + y) % 2 === 0;
        case 1: return y % 2 === 0;
        case 2: return x % 3 === 0;
        case 3: return (x + y) % 3 === 0;
        case 4: return (Math.floor(x / 3) + Math.floor(y / 2)) % 2 === 0;
        case 5: return ((x * y) % 2) + ((x * y) % 3) === 0;
        case 6: return (((x * y) % 2) + ((x * y) % 3)) % 2 === 0;
        default: return (((x + y) % 2) + ((x * y) % 3)) % 2 === 0;
    }
}

function applyMask(g, mask) {
    const n = g.size;
    for (let y = 0; y < n; y++)
        for (let x = 0; x < n; x++)
            if (!g.fn[y * n + x] && maskBit(mask, x, y))
                g.mod[y * n + x] ^= 1;
}

function finderAddHistory(run, history, n) {
    if (history[0] === 0)
        run += n;
    history.copyWithin(1, 0, 6);
    history[0] = run;
}

function finderCount(history) {
    const n = history[1];
    const core = n > 0 && history[2] === n && history[3] === n * 3 && history[4] === n && history[5] === n;
    return (core && history[0] >= n * 4 && history[6] >= n ? 1 : 0)
        + (core && history[6] >= n * 4 && history[0] >= n ? 1 : 0);
}

function finderTerminate(color, run, history, n) {
    if (color) {
        finderAddHistory(run, history, n);
        run = 0;
    }
    run += n;
    finderAddHistory(run, history, n);
    return finderCount(history);
}

function penalty(g) {
    const n = g.size;
    let result = 0;
    for (let pass = 0; pass < 2; pass++) {
        for (let a = 0; a < n; a++) {
            let color = false;
            let run = 0;
            const history = new Int32Array(7);
            for (let b = 0; b < n; b++) {
                const v = pass === 0 ? g.get(b, a) : g.get(a, b);
                if (v === color) {
                    run++;
                    if (run === 5)
                        result += PENALTY_N1;
                    else if (run > 5)
                        result++;
                } else {
                    finderAddHistory(run, history, n);
                    if (!color)
                        result += finderCount(history) * PENALTY_N3;
                    color = v;
                    run = 1;
                }
            }
            result += finderTerminate(color, run, history, n) * PENALTY_N3;
        }
    }
    for (let y = 0; y < n - 1; y++) {
        for (let x = 0; x < n - 1; x++) {
            const c = g.get(x, y);
            if (c === g.get(x + 1, y) && c === g.get(x, y + 1) && c === g.get(x + 1, y + 1))
                result += PENALTY_N2;
        }
    }
    let dark = 0;
    for (const m of g.mod)
        dark += m;
    const total = n * n;
    const k = Math.ceil(Math.abs(dark * 20 - total * 10) / total) - 1;
    return result + k * PENALTY_N4;
}

export function encodeQr(text) {
    const bytes = new TextEncoder().encode(text);
    let ecl = 0;
    let ver = 1;
    let usedBits = 0;
    for (;; ver++) {
        if (ver > 40)
            throw new Error('QR: ссылка не помещается');
        usedBits = 4 + (ver <= 9 ? 8 : 16) + bytes.length * 8;
        if (usedBits <= dataCodewords(ver, ecl) * 8)
            break;
    }
    for (let e = 1; e <= 3; e++)
        if (usedBits <= dataCodewords(ver, e) * 8)
            ecl = e;

    // Биты данных: режим «байты», длина, сами байты, терминатор, выравнивание
    // и чередующиеся байты-заполнители.
    const capacity = dataCodewords(ver, ecl) * 8;
    const bits = [];
    const push = (val, len) => {
        for (let i = len - 1; i >= 0; i--)
            bits.push((val >>> i) & 1);
    };
    push(0x4, 4);
    push(bytes.length, ver <= 9 ? 8 : 16);
    for (const b of bytes)
        push(b, 8);
    push(0, Math.min(4, capacity - bits.length));
    push(0, (8 - (bits.length % 8)) % 8);
    for (let pad = 0xEC; bits.length < capacity; pad ^= 0xEC ^ 0x11)
        push(pad, 8);
    const data = new Uint8Array(bits.length / 8);
    for (let i = 0; i < bits.length; i++)
        data[i >>> 3] |= bits[i] << (7 - (i & 7));

    // Блоки, коррекция и чередование.
    const numBlocks = NUM_ERROR_CORRECTION_BLOCKS[ecl][ver];
    const eccLen = ECC_CODEWORDS_PER_BLOCK[ecl][ver];
    const raw = Math.floor(rawDataModules(ver) / 8);
    const numShort = numBlocks - (raw % numBlocks);
    const shortLen = Math.floor(raw / numBlocks) - eccLen;
    const divisor = rsDivisor(eccLen);
    const blocks = [];
    for (let i = 0, k = 0; i < numBlocks; i++) {
        const len = shortLen + (i < numShort ? 0 : 1);
        const dat = data.subarray(k, k + len);
        k += len;
        blocks.push({ dat, ecc: rsRemainder(dat, divisor) });
    }
    const out = [];
    for (let i = 0; i <= shortLen; i++)
        for (let j = 0; j < numBlocks; j++)
            if (i < shortLen || j >= numShort)
                out.push(blocks[j].dat[i]);
    for (let i = 0; i < eccLen; i++)
        for (let j = 0; j < numBlocks; j++)
            out.push(blocks[j].ecc[i]);

    const g = new Grid(ver);
    drawFunctionPatterns(g, ver);
    drawCodewords(g, Uint8Array.from(out));

    let best = 0;
    let bestScore = Infinity;
    for (let m = 0; m < 8; m++) {
        applyMask(g, m);
        drawFormatBits(g, ecl, m);
        const score = penalty(g);
        if (score < bestScore) {
            best = m;
            bestScore = score;
        }
        applyMask(g, m);
    }
    applyMask(g, best);
    drawFormatBits(g, ecl, best);
    return { size: g.size, dark: (x, y) => g.get(x, y) };
}

// SVG-путь для тёмных модулей: по прямоугольнику на каждую горизонтальную
// серию, а не по квадрату на модуль, — путь выходит в разы короче.
export function qrPath(qr) {
    const parts = [];
    for (let y = 0; y < qr.size; y++) {
        let x = 0;
        while (x < qr.size) {
            if (!qr.dark(x, y)) {
                x++;
                continue;
            }
            const start = x;
            while (x < qr.size && qr.dark(x, y))
                x++;
            parts.push(`M${start} ${y}h${x - start}v1h-${x - start}z`);
        }
    }
    return parts.join('');
}
