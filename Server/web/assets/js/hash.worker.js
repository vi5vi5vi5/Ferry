// Хеши тома: BLAKE3 каждого открытого чанка — зеркало Cli/Hasher.cpp.
//
// Отдельный воркер, потому что хеш — это чистый процессор: в одном потоке
// с сокетом он отнимал бы у сети паузы между сообщениями. Отправитель
// получает хеши через свой порт пачками и сам решает, когда объявить их
// получателям.
//
// Читаем крупными блоками, но границы блоков совпадают с границами чанков:
// так каждый чанк хешируется ровно из того буфера, в который прочитан, без
// склейки.

import { blake3 } from './blake3.js';
import { buildLayout } from './manifest.js';
import { readRange, readProblem } from './volread.js';

const BLOCK = 16 * 1024 * 1024;
const POST_EVERY_MS = 100;

let port = null;
let stopped = false;

async function run({ files, manifest, chunkSize }) {
    const layout = buildLayout(manifest);
    const total = manifest.total;
    const count = Math.ceil(total / chunkSize);
    const perBlock = Math.max(1, Math.floor(BLOCK / chunkSize));

    let pendingFrom = 0;
    let pending = [];
    let lastPost = 0;
    let bytesDone = 0;
    const post = (force) => {
        const now = performance.now();
        if (!pending.length || (!force && now - lastPost < POST_EVERY_MS))
            return;
        const data = new Uint8Array(pending.length * 32);
        pending.forEach((h, k) => data.set(h, k * 32));
        port.postMessage({ type: 'hashes', from: pendingFrom, count: pending.length, data, bytes: bytesDone },
            [data.buffer]);
        pendingFrom += pending.length;
        pending = [];
        lastPost = now;
    };

    for (let first = 0; first < count && !stopped; first += perBlock) {
        const last = Math.min(count, first + perBlock);
        const from = first * chunkSize;
        const length = Math.min(total, last * chunkSize) - from;
        let block;
        try {
            block = await readRange(files, layout, from, length);
        } catch (e) {
            port.postMessage({ type: 'error', message: readProblem(e) });
            return;
        }
        for (let i = first; i < last; i++) {
            const off = (i - first) * chunkSize;
            const len = Math.min(chunkSize, total - i * chunkSize);
            pending.push(blake3(block.subarray(off, off + len)));
            bytesDone += len;
        }
        post(false);
    }
    post(true);
    if (!stopped)
        port.postMessage({ type: 'done' });
}

self.onmessage = (ev) => {
    const msg = ev.data;
    if (msg.cmd === 'start') {
        port = msg.port;
        // Остановить могут и страница, и отправитель — через тот же порт.
        port.onmessage = (e) => {
            if (e.data && e.data.type === 'stop')
                stopped = true;
        };
        run(msg).catch((e) => port.postMessage({ type: 'error', message: readProblem(e) }));
    } else if (msg.cmd === 'stop') {
        stopped = true;
    }
};
