// Отправитель в браузере — зеркало Cli/Sender.cpp.
//
// Страница передаёт сюда опись и объекты File; дальше всё здесь: ключ,
// черновик раздачи, offer, хеши на лету и ровно те чанки, которые попросил
// сервер. Ключ рождается в этом воркере и уходит только на страницу —
// показать ссылку. На сервер он не попадает ни в каком виде: туда идёт V,
// из которого K обратно не вывести.
//
// Хеши считает соседний воркер (hash.worker.js) и присылает их через свой
// порт пачками. Чанк не уходит раньше своего хеша — сервер это проверяет,
// и на этом правиле держится всё, что получатель знает о целостности.

import { createTransfer, wsUrl, RelayError, NONCE_PREFIX_SIZE, MASTER_KEY_SIZE } from './api.js';
import {
    deriveKeys, verifierBytes, sealChunk,
    LABEL_MANIFEST, LABEL_HASH_LIST, LABEL_STREAM_MANIFEST, hashSegmentLabel,
} from './crypto.js';
import { blake3 } from './blake3.js';
import { b64urlEncode } from './b64.js';
import { ChunkSet } from './chunkset.js';
import { buildLayout, manifestToJson } from './manifest.js';
import { readRange, readProblem } from './volread.js';

// Сколько держим в очереди сокета. Меньше — на быстром канале между
// чанками появляются паузы; больше — читаем и шифруем то, что уедет через
// полминуты, и зря держим это в памяти.
const HIGH_WATER = 16 * 1024 * 1024;
// Сколько чанков готовится заранее: чтение с диска и шифрование идут,
// пока предыдущий чанк ещё в сокете.
const AHEAD = 4;
// Доля догона в аплоаде — три десятых, как в CLI (§6).
const LIVE_SHARE = 7;
const BACKFILL_SHARE = 3;
const SEGMENT_MAX = 8192;
const OFFER_TIMEOUT_MS = 20000;
const PROGRESS_MS = 250;
const MAP_CELLS = 120;

const post = (msg, transfer) => self.postMessage(msg, transfer || []);

class RateMeter {
    constructor() {
        this.last = 0;
        this.bytes = 0;
        this.value = 0;
    }
    add(bytes) {
        const t = performance.now();
        if (this.last === 0) {
            this.last = t;
            this.bytes = bytes;
            return;
        }
        this.bytes += bytes;
        const dt = t - this.last;
        if (dt >= 500) {
            const instant = (this.bytes * 1000) / dt;
            this.value = this.value <= 0 ? instant : this.value * 0.7 + instant * 0.3;
            this.bytes = 0;
            this.last = t;
        }
    }
    // Скорость падает до нуля, если байты перестали идти, а не застывает
    // на последнем значении.
    read() {
        if (this.last && performance.now() - this.last > 2000)
            return 0;
        return this.value;
    }
}

class SendError extends Error {
    constructor(code, message) {
        super(message || code);
        this.code = code;
    }
}

function putBe64(out, off, v) {
    let x = BigInt(v);
    for (let i = 7; i >= 0; i--) {
        out[off + i] = Number(x & 0xffn);
        x >>= 8n;
    }
}

class Sender {
    constructor(msg) {
        this.files = msg.files;
        this.manifest = msg.manifest;
        this.policy = msg.policy || {};
        this.origin = msg.origin;
        this.hashPort = msg.hashPort;
        this.chunkSize = msg.chunkSize;
        this.total = this.manifest.total;
        this.count = Math.ceil(this.total / this.chunkSize);
        this.layout = buildLayout(this.manifest);

        this.hashes = new Uint8Array(this.count * 32);
        this.hashed = 0;            // посчитано подряд с начала
        this.hashBytes = 0;
        this.hashingDone = false;
        this.hashMeter = new RateMeter();
        this.announced = 0;         // объявлено получателям
        this.hashesDoneSent = false;
        this.lastSegment = 0;
        this.hashStarted = performance.now();
        this.hashTook = -1;

        this.live = new ChunkSet(this.count);
        this.backfill = new ChunkSet(this.count);
        this.liveHint = 0;
        this.backfillHint = 0;
        this.sentLive = 0;
        this.sentBackfill = 0;

        this.sent = new ChunkSet(this.count);
        this.sentBytes = 0;
        this.distinctBytes = 0;
        this.meter = new RateMeter();

        this.inflight = 0;
        this.inflightBytes = 0;
        this.chain = Promise.resolve();

        this.ws = null;
        this.accepted = false;
        this.stopping = false;
        this.finished = false;
        this.pumping = false;
        this.again = false;
        this.timer = 0;
        this.lastProgress = 0;
        this.startedAt = 0;
    }

    // ---------------------------------------------------------------- ход

    async start() {
        this.hashPort.onmessage = (ev) => this.onHashes(ev.data);

        const master = crypto.getRandomValues(new Uint8Array(MASTER_KEY_SIZE));
        this.noncePrefix = crypto.getRandomValues(new Uint8Array(NONCE_PREFIX_SIZE));
        this.keys = await deriveKeys(master);
        const verifier = await verifierBytes(master);

        let created = await this.create();
        this.stream = created.features.includes('stream_hashes');

        // Релей без хешей на лету: как раньше, весь том до ссылки. Черновик
        // живёт минуту, а подсчёт может идти дольше — тогда нужен свежий.
        if (!this.stream) {
            post({ ev: 'phase', phase: 'hashing' });
            const createdAt = performance.now();
            await new Promise((resolve) => {
                this.onAllHashed = resolve;
                if (this.hashingDone)
                    resolve();
            });
            if (this.finished)
                return;
            if (performance.now() - createdAt > 45000)
                created = await this.create();
        }
        this.created = created;

        const base = created.publicUrl || this.origin;
        this.link = `${base}/t/${created.id}`;
        this.keyText = b64urlEncode(master);
        master.fill(0);

        const offer = {
            type: 'offer',
            id: created.id,
            owner_token: created.ownerToken,
            total: this.total,
            chunk_size: this.chunkSize,
            chunks: this.count,
            nonce_prefix: b64urlEncode(this.noncePrefix),
            verifier: b64urlEncode(verifier),
            mode: 'key',
            policy: {},
        };
        verifier.fill(0);
        if (this.stream) {
            offer.hash_mode = 'stream';
            offer.manifest = b64urlEncode(await this.sealMeta(LABEL_STREAM_MANIFEST,
                new TextEncoder().encode(manifestToJson(this.manifest, null))));
        } else {
            const { manifest, list } = await this.finalMeta();
            offer.manifest = b64urlEncode(manifest);
            offer.hash_list = b64urlEncode(list);
            this.announced = this.count;
            this.hashesDoneSent = true;
            this.hashTook = performance.now() - this.hashStarted;
        }
        if (this.policy.uses > 0)
            offer.policy.uses = this.policy.uses;
        if (this.policy.ttlMs > 0)
            offer.policy.ttl_ms = this.policy.ttlMs;
        if (this.policy.maxConcurrent > 0)
            offer.policy.max_concurrent = this.policy.maxConcurrent;

        await this.connect(offer);
    }

    async create() {
        post({ ev: 'phase', phase: 'creating' });
        try {
            return await createTransfer(this.origin);
        } catch (e) {
            throw new SendError(e.code || 'network', e.message);
        }
    }

    connect(offer) {
        return new Promise((resolve, reject) => {
            post({ ev: 'phase', phase: 'offering' });
            const ws = new WebSocket(wsUrl(this.origin));
            ws.binaryType = 'arraybuffer';
            this.ws = ws;
            const timeout = setTimeout(() => {
                if (!this.accepted)
                    reject(new SendError('network', 'сервер не подтвердил раздачу'));
            }, OFFER_TIMEOUT_MS);
            ws.onopen = () => ws.send(JSON.stringify(offer));
            ws.onmessage = (ev) => {
                if (typeof ev.data !== 'string')
                    return;
                let v;
                try {
                    v = JSON.parse(ev.data);
                } catch {
                    return;
                }
                if (v.type === 'offer_ok' && !this.accepted) {
                    this.accepted = true;
                    clearTimeout(timeout);
                    this.onAccepted();
                    resolve();
                } else if (v.type === 'need') {
                    this.rememberNeed(v);
                    this.kick();
                } else if (v.type === 'peers') {
                    post({ ev: 'peers', receivers: Array.isArray(v.receivers) ? v.receivers : [] });
                } else if (v.type === 'error') {
                    this.lastError = String(v.reason || 'unknown');
                    if (!this.accepted) {
                        clearTimeout(timeout);
                        reject(new SendError(this.lastError));
                    } else {
                        this.fail(this.lastError);
                    }
                }
            };
            ws.onclose = () => {
                clearTimeout(timeout);
                if (!this.accepted) {
                    reject(new SendError(this.lastError || 'network', 'не удалось подключиться к релею'));
                    return;
                }
                if (!this.stopping)
                    this.fail(this.lastError || 'network');
            };
        });
    }

    onAccepted() {
        this.startedAt = performance.now();
        post({
            ev: 'ready',
            id: this.created.id,
            link: this.link,
            key: this.keyText,
            chunkSize: this.chunkSize,
            chunks: this.count,
            stream: this.stream,
            policy: this.policy,
        });
        this.timer = setInterval(() => this.kick(), 50);
        this.kick();
    }

    rememberNeed(v) {
        const backfill = v.lane === 'backfill';
        const queue = backfill ? this.backfill : this.live;
        for (const r of Array.isArray(v.ranges) ? v.ranges : []) {
            if (!Array.isArray(r) || r.length < 2)
                continue;
            const from = r[0];
            const to = r[1];
            if (!Number.isInteger(from) || !Number.isInteger(to) || from < 0 || to < from)
                continue;
            if (!queue.setRange(from, Math.min(to, this.count - 1)))
                continue;
            if (backfill)
                this.backfillHint = Math.min(this.backfillHint, from);
            else
                this.liveHint = Math.min(this.liveHint, from);
        }
    }

    // ---------------------------------------------------------------- хеши

    onHashes(m) {
        if (this.finished)
            return;
        if (m.type === 'hashes') {
            if (m.from !== this.hashed)
                return this.fail('internal', 'хеши пришли не по порядку');
            this.hashes.set(m.data, m.from * 32);
            this.hashed = m.from + m.count;
            this.hashMeter.add(m.bytes - this.hashBytes);
            this.hashBytes = m.bytes;
            if (this.accepted)
                this.kick();
            else
                this.progress(false);
        } else if (m.type === 'done') {
            this.hashingDone = true;
            if (this.onAllHashed)
                this.onAllHashed();
            this.kick();
        } else if (m.type === 'error') {
            this.fail('read', m.message);
        }
    }

    async sealMeta(label, plain) {
        return sealChunk(this.keys.meta, this.noncePrefix, label, this.count, plain);
    }

    async finalMeta() {
        const root = blake3(this.hashes);
        const json = new TextEncoder().encode(manifestToJson(this.manifest, root));
        return {
            manifest: await this.sealMeta(LABEL_MANIFEST, json),
            list: await this.sealMeta(LABEL_HASH_LIST, this.hashes),
        };
    }

    // Сегменты уходят пачками: по 256 хешей или раз в 300 мс. Но если
    // живая волна упёрлась в чанк, чей хеш ещё не объявлен, — сразу, иначе
    // данные стояли бы ради красоты пачки.
    async announce() {
        if (!this.stream || this.hashesDoneSent)
            return;
        const ready = this.hashed;
        if (ready > this.announced) {
            const nextLive = this.live.empty ? this.count : this.live.firstPresent(0);
            const starving = nextLive >= this.announced && nextLive < ready;
            const batch = ready - this.announced >= 256;
            const stale = performance.now() - this.lastSegment >= 300;
            if (starving || batch || stale || ready === this.count) {
                while (this.announced < ready) {
                    const from = this.announced;
                    const n = Math.min(ready - from, SEGMENT_MAX);
                    const sealed = await this.sealMeta(hashSegmentLabel(from),
                        this.hashes.subarray(from * 32, (from + n) * 32));
                    if (this.finished)
                        return;
                    this.ws.send(JSON.stringify({ type: 'hashes', from, count: n, data: b64urlEncode(sealed) }));
                    this.announced = from + n;
                }
                this.lastSegment = performance.now();
            }
        }
        // Всё посчитано и объявлено — итоговые манифест с корнем и список
        // целиком. С этой минуты раздача открыта и тем, кто хеши на лету не
        // умеет.
        if (this.hashingDone && this.announced === this.count && !this.hashesDoneSent) {
            this.hashesDoneSent = true;
            const { manifest, list } = await this.finalMeta();
            if (this.finished)
                return;
            this.ws.send(JSON.stringify({
                type: 'hashes_done', manifest: b64urlEncode(manifest), hash_list: b64urlEncode(list),
            }));
            this.hashTook = performance.now() - this.hashStarted;
        }
    }

    // ---------------------------------------------------------------- чанки

    // Следующий чанк к отправке, или -1. Живая волна идёт первой, но догону
    // достаётся до трёх десятых: иначе при получателях быстрее нашего канала
    // опоздавший не получил бы ничего вообще.
    pick() {
        let live = !this.live.empty;
        if (live && !this.backfill.empty && this.sentBackfill * LIVE_SHARE < this.sentLive * BACKFILL_SHARE)
            live = false;
        const queue = live ? this.live : this.backfill;
        if (queue.empty)
            return -1;
        let index = queue.firstPresent(live ? this.liveHint : this.backfillHint);
        if (index >= this.count)
            index = queue.firstPresent(0);
        if (index >= this.count)
            return -1;
        if (live)
            this.liveHint = index;
        else
            this.backfillHint = index;
        // Чанк не уходит раньше своего хеша.
        if (index >= this.announced)
            return -1;
        queue.clear(index);
        if (live)
            this.sentLive++;
        else
            this.sentBackfill++;
        return index;
    }

    sizeOf(index) {
        return Math.min(this.chunkSize, this.total - index * this.chunkSize);
    }

    async frame(index) {
        let plain;
        try {
            plain = await readRange(this.files, this.layout, index * this.chunkSize, this.sizeOf(index));
        } catch (e) {
            throw new SendError('read', readProblem(e));
        }
        const cipher = await sealChunk(this.keys.data, this.noncePrefix, index, this.count, plain);
        const out = new Uint8Array(9 + cipher.length);
        out[0] = 1;
        putBe64(out, 1, index);
        out.set(cipher, 9);
        return out.buffer;
    }

    fill() {
        while (!this.finished && this.inflight < AHEAD
               && this.ws.bufferedAmount + this.inflightBytes < HIGH_WATER) {
            const index = this.pick();
            if (index < 0)
                return;
            const len = this.sizeOf(index);
            this.inflight++;
            this.inflightBytes += len;
            const ready = this.frame(index);
            // Шлём в том порядке, в каком выбирали: чтение и шифрование идут
            // параллельно, а в сокет чанки встают по очереди.
            this.chain = this.chain.then(() => ready).then((buf) => {
                this.inflight--;
                this.inflightBytes -= len;
                if (this.finished)
                    return;
                this.ws.send(buf);
                this.meter.add(len);
                this.sentBytes += len;
                if (this.sent.set(index))
                    this.distinctBytes += len;
                this.kick();
            }, (e) => {
                this.inflight--;
                this.inflightBytes -= len;
                this.fail(e.code || 'read', e.message);
            });
        }
    }

    kick() {
        if (this.finished || !this.accepted)
            return;
        if (this.pumping) {
            this.again = true;
            return;
        }
        this.pumping = true;
        (async () => {
            do {
                this.again = false;
                await this.announce();
                if (this.finished)
                    break;
                this.fill();
            } while (this.again && !this.finished);
            this.pumping = false;
            this.progress(false);
        })().catch((e) => {
            this.pumping = false;
            this.fail(e.code || 'internal', e.message);
        });
    }

    // ---------------------------------------------------------------- наружу

    cells() {
        const n = Math.min(MAP_CELLS, this.count);
        const out = new Uint8Array(n);
        for (let c = 0; c < n; c++) {
            const a = Math.floor((c * this.count) / n);
            const b = Math.max(a + 1, Math.floor(((c + 1) * this.count) / n));
            let have = 0;
            for (let i = a; i < b; i++)
                if (this.sent.has(i))
                    have++;
            out[c] = have === 0 ? 0 : have === b - a ? 2 : 1;
        }
        return out;
    }

    progress(force) {
        const now = performance.now();
        if (!force && now - this.lastProgress < PROGRESS_MS)
            return;
        this.lastProgress = now;
        post({
            ev: 'progress',
            sentChunks: this.sent.have,
            distinctBytes: this.distinctBytes,
            sentBytes: this.sentBytes,
            rate: this.meter.read(),
            queued: this.ws ? this.ws.bufferedAmount : 0,
            hashed: this.hashed,
            hashBytes: this.hashBytes,
            hashRate: this.hashMeter.read(),
            hashesDone: this.hashesDoneSent,
            hashTook: this.hashTook,
            elapsed: this.startedAt ? now - this.startedAt : 0,
            cells: this.cells(),
        });
    }

    fail(code, message) {
        if (this.finished)
            return;
        this.finished = true;
        clearInterval(this.timer);
        this.progress(true);
        post({ ev: 'failed', code, message: message || '' });
        this.shutdown();
    }

    stop() {
        if (this.finished)
            return;
        this.stopping = true;
        this.finished = true;
        clearInterval(this.timer);
        this.progress(true);
        this.shutdown();
        post({ ev: 'closed' });
    }

    shutdown() {
        try {
            this.hashPort.postMessage({ type: 'stop' });
        } catch {
            // хешер уже закрыт
        }
        if (this.ws && this.ws.readyState <= 1) {
            this.stopping = true;
            this.ws.close(1000);
        }
    }
}

let sender = null;

self.onmessage = (ev) => {
    const msg = ev.data;
    if (msg.cmd === 'start' && !sender) {
        sender = new Sender(msg);
        sender.start().catch((e) => {
            if (e instanceof SendError || e instanceof RelayError)
                sender.fail(e.code, e.message === e.code ? '' : e.message);
            else
                sender.fail('internal', (e && e.message) || String(e));
        });
    } else if (msg.cmd === 'stop' && sender) {
        sender.stop();
    }
};
