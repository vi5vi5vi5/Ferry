// Приём тома — зеркало Cli/Receiver.cpp для браузера.
//
// Порядок тот же, что у CLI: проверка владения ключом, hello с картой
// принятого, приём чанков в любом порядке с проверкой каждого против
// списка хешей, отчёты have, отдача чанков тем, кто пришёл позже, и
// докачка с того же места после обрыва.
//
// Отличия от CLI продиктованы браузером:
//   * обрыв связи — не конец: страница переподключается сама, а принятое
//     лежит в OPFS и переживает даже перезагрузку вкладки;
//   * WebSocket в браузере не умеет обратного давления на чтение, поэтому
//     расшифровка начинается прямо в обработчике сообщения, а проверка и
//     запись идут строго по порядку прихода — так порядок «хеш раньше
//     чанка» сохраняется, а WebCrypto работает параллельно.
//
// Модуль не знает ни про DOM, ни про воркер: события уходят в onEvent, а
// WebSocket и хранилище приходят снаружи. Это позволяет гонять его и в
// воркере страницы, и в node против настоящего релея.

import { b64urlDecode, b64urlEncode } from './b64.js';
import { blake3 } from './blake3.js';
import { ChunkSet } from './chunkset.js';
import { openChunk, sealChunk, proveKeyOwnership, hashSegmentLabel, LABEL_MANIFEST } from './crypto.js';
import { fetchChallenge, RelayError, wsUrl } from './api.js';
import { parseManifest, sameVolume } from './manifest.js';
import { equalBytes, openVolume } from './volume.js';
import { StoreError } from './store.js';

const OP_CHUNK = 1;
const HEADER = 9;
const MAX_BAD_CHUNKS = 64;            // подряд — дальше чинить нечего
const SEED_HIGH_WATER = 2 * 1024 * 1024;
const MAX_RANGES = 4096;              // длиннее релей отвергает
const HASH_SEGMENT_MAX = 8192;
const MAP_CELLS = 120;
const RECONNECT_STEPS = [1, 2, 4, 8, 15, 30];
const HELLO_TIMEOUT_MS = 20000;
// Две минуты: на медленном канале чанк в 4 МиБ честно идёт десятки секунд,
// а сообщение приходит только целиком. Ложная тревога здесь стоит дорого —
// каждое новое подключение списывает использование ссылки.
const STALL_MS = 120000;

// Отказы релея, после которых переподключаться бессмысленно: ответ не
// изменится, сколько ни проси. Остальное (обрыв, занятый сервер) —
// повод попробовать ещё раз.
export const FINAL_ERRORS = new Set([
    'need_key', 'transfer_not_found', 'uses_exhausted', 'expired', 'sender_gone',
    'no_source', 'chunk_mismatch', 'owner_conflict', 'bad_message', 'approval_denied', 'revoked',
    'bad_hashes', 'bad_key', 'bad_manifest', 'bad_meta', 'no_space', 'busy', 'io',
]);

const now = () => (typeof performance !== 'undefined' ? performance.now() : Date.now());

function readBe64(u8, off) {
    let v = 0;
    for (let i = 0; i < 8; i++)
        v = v * 256 + u8[off + i];
    return v;   // за 2^53 точность теряется, но такой индекс и так вне тома
}

class RateMeter {
    constructor() {
        this.last = 0;
        this.bytes = 0;
        this.value = 0;
    }

    add(n) {
        const t = now();
        if (!this.last) {
            this.last = t;
            this.bytes = n;
            return;
        }
        this.bytes += n;
        const dt = t - this.last;
        if (dt >= 500) {
            const inst = (this.bytes * 1000) / dt;
            this.value = this.value <= 0 ? inst : this.value * 0.7 + inst * 0.3;
            this.bytes = 0;
            this.last = t;
        }
    }

    // Тишина дольше двух секунд — значит скорость ноль, а не «последняя
    // известная»: иначе застрявший приём показывал бы бодрые мегабайты.
    current() {
        return this.last && now() - this.last > 2000 ? 0 : this.value;
    }
}

export class Receiver {
    constructor({ origin, id, key, meta, store, WebSocketImpl, onEvent, name = '' }) {
        this.origin = origin;
        this.id = id;
        this.key = key;
        this.meta = meta;
        this.store = store;
        this.WS = WebSocketImpl || WebSocket;
        this.emit = onEvent || (() => {});
        this.name = name;

        this.phase = 'idle';
        this.ws = null;
        this.queue = [];
        this.pumping = null;
        this.timer = null;
        this.reconnectTimer = null;
        this.reconnectAttempt = 0;
        this.reconnectAt = 0;
        this.fatal = null;
        this.stopped = false;
        this.finished = false;
        this.seeding = true;
        this.served = 0;
        this.servedBytes = 0;
        this.serving = false;
    }

    // ---- запуск ----

    async start() {
        const vol = await openVolume(this.meta, this.key);
        this.keys = vol.keys;
        this.plan = vol.plan;
        this.manifest = vol.manifest;
        this.layout = vol.layout;
        const count = this.plan.count;

        this.hashes = vol.hashes || new Uint8Array(count * 32);
        this.hashed = vol.hashes ? count : 0;
        this.hashesComplete = !this.meta.stream;

        this.have = new ChunkSet(count);
        this.pending = new ChunkSet(count);   // на диске, но ещё не сверено
        this.late = new ChunkSet(count);      // пришло второй волной
        this.serveQueue = new ChunkSet(count);
        this.storedRoot = null;

        let saved;
        try {
            saved = await this.store.open(this.plan);
        } catch (e) {
            throw e instanceof StoreError ? e : new StoreError('io', String(e && e.message || e));
        }
        this.resumed = this.restore(saved);
        this.received = this.have.have;
        this.startedAt = Date.now();
        this.meter = new RateMeter();
        this.badInARow = 0;
        this.liveFront = 0;
        this.joinIndex = null;
        this.src = null;
        this.dirtyState = true;
        await this.saveState();

        this.timer = setInterval(() => this.housekeeping(), 100);
        // Том уже целиком здесь и сверен (страницу перезагрузили после
        // приёма) — к серверу не идём: каждое подключение списывает
        // использование ссылки, а одноразовая ссылка после приёма отказала
        // бы и том, лежащий на диске, показался бы потерянным.
        if (this.have.full && this.hashesComplete) {
            await this.checkDone();
            return;
        }
        await this.connect();
    }

    // Недокачка из прошлого раза. Правила те же, что у CLI:
    //   * корень сошёлся с манифестом — карте верим целиком;
    //   * корня нет (хеши ещё едут, или недокачка осталась от такой
    //     раздачи) — каждый её чанк перечитывается и сверяется с хешем,
    //     а до того для остального клиента его как бы нет;
    //   * корень другой — это недокачка чужого тома, начинаем с нуля.
    restore(saved) {
        const p = this.plan;
        if (!saved || saved.v !== 1 || saved.count !== p.count || saved.chunkSize !== p.chunkSize
            || saved.total !== p.total)
            return false;
        const bits = b64urlDecode(saved.bits || '');
        if (!bits || bits.length !== this.have.bytes.length)
            return false;
        const root = saved.root ? b64urlDecode(saved.root) : null;
        if (!this.meta.stream && root && equalBytes(root, this.manifest.root)) {
            this.have.loadBits(bits);
            return this.have.have > 0;
        }
        if (this.meta.stream || !root) {
            this.pending.loadBits(bits);
            this.storedRoot = root;
            return this.pending.have > 0;
        }
        return false;
    }

    // ---- соединение ----

    async connect() {
        if (this.stopped || this.fatal)
            return;
        clearTimeout(this.reconnectTimer);
        this.reconnectTimer = null;
        // Сообщения прошлого сокета дорабатываем до конца: это проверенные
        // чанки, выбрасывать их незачем.
        if (this.pumping)
            await this.pumping;
        this.setPhase('connecting');

        let challenge;
        try {
            challenge = await fetchChallenge(this.origin, this.id);
        } catch (e) {
            const code = e instanceof RelayError ? e.code : 'network';
            // Том уже у нас, просились только раздавать — не пустили, и ладно.
            if (this.finished)
                return this.seedRefusedWith(code);
            if (code === 'network' || code.startsWith('http_5'))
                return this.scheduleReconnect();
            return this.fail(code);
        }
        const proof = await proveKeyOwnership(this.keys, challenge);

        // Сегменты хешей сервер присылает новому сокету с самого начала —
        // значит и считать их начинаем заново. Список, досчитанный целиком,
        // просить незачем.
        const wantStream = this.meta.stream && !this.hashesComplete;
        if (wantStream)
            this.hashed = 0;

        const ws = new this.WS(wsUrl(this.origin));
        ws.binaryType = 'arraybuffer';
        this.ws = ws;
        this.accepted = false;
        this.waitingApproval = false;
        this.ranges = false;
        this.sentHave = -1;
        this.lastAck = 0;
        this.sessionStart = this.have.prefix();
        this.socketOpenedAt = now();
        this.lastMessageAt = now();

        ws.onopen = () => {
            const features = ['ranges', 'stream_hashes'];
            // Отдавать умеем, только если можем читать принятое. Объявить
            // backfill без этого — значит позвать сервер просить у нас то,
            // чего мы не отдадим, и повесить чужой догон.
            if (this.store.canRead)
                features.push('backfill');
            const hello = {
                type: 'hello',
                id: this.id,
                challenge: b64urlEncode(challenge),
                proof: b64urlEncode(proof),
                have_upto: this.have.prefix(),
                have: this.have.ranges().slice(0, MAX_RANGES),
                features,
            };
            if (this.name)
                hello.name = this.name;
            if (wantStream)
                hello.hashes = 'stream';
            ws.send(JSON.stringify(hello));
        };
        ws.onmessage = (ev) => {
            if (ws !== this.ws)
                return;
            this.lastMessageAt = now();
            this.enqueue(ev.data);
        };
        ws.onclose = () => this.socketGone(ws);
        ws.onerror = () => {};
    }

    // Сокет умер — своей смертью или мы его бросили. Решаем, что дальше.
    socketGone(ws) {
        if (ws !== this.ws)
            return;
        this.ws = null;
        // Сервер отвечает ошибкой и сразу закрывает сокет: причина уже
        // лежит в очереди — даём ей обработаться, прежде чем решать.
        Promise.resolve(this.pumping).then(() => {
            if (this.fatal || this.stopped)
                return;
            if (this.finished) {
                this.emitSeed();
                return;
            }
            this.scheduleReconnect();
        });
    }

    // Бросить сокет, не дожидаясь, пока он закроется сам. Пропавшая сеть в
    // браузере может «закрываться» минутами: TCP ещё ждёт, а приём стоит.
    abandonSocket() {
        const ws = this.ws;
        if (!ws)
            return;
        this.socketGone(ws);
        try {
            ws.close();
        } catch {
            // уже закрыт
        }
    }

    scheduleReconnect() {
        if (this.stopped || this.fatal || this.finished)
            return;
        const step = RECONNECT_STEPS[Math.min(this.reconnectAttempt, RECONNECT_STEPS.length - 1)];
        this.reconnectAttempt++;
        this.reconnectAt = Date.now() + step * 1000;
        this.setPhase('reconnecting');
        clearTimeout(this.reconnectTimer);
        this.reconnectTimer = setTimeout(() => this.connect(), step * 1000);
        this.emitProgress();
    }

    reconnectNow() {
        if (this.phase === 'reconnecting')
            this.connect();
    }

    // Сеть вернулась (браузер сказал online). Старый сокет почти наверняка
    // мёртв, даже если ещё не знает об этом: не ждём сторожа.
    networkBack() {
        if (this.finished || this.fatal || this.stopped)
            return;
        if (this.phase === 'reconnecting')
            this.connect();
        else if (this.ws && now() - this.lastMessageAt > 3000)
            this.abandonSocket();
    }

    send(obj) {
        if (this.ws && this.ws.readyState === 1)
            this.ws.send(JSON.stringify(obj));
    }

    // ---- очередь сообщений ----

    enqueue(data) {
        if (typeof data === 'string') {
            this.queue.push({ text: data });
        } else {
            const u8 = new Uint8Array(data);
            const item = { bin: true, u8 };
            if (u8.length < HEADER) {
                item.broken = 'сервер прислал обрезанный фрейм';
            } else if (u8[0] !== OP_CHUNK) {
                item.skip = true;
            } else {
                item.index = readBe64(u8, 1);
                if (item.index >= this.plan.count) {
                    item.broken = 'сервер прислал чанк с несуществующим номером';
                } else if (this.have.has(item.index)) {
                    item.skip = true;   // повтор — не беда
                } else {
                    // Расшифровка стартует сразу: она не зависит от хешей, а
                    // WebCrypto выполняет её параллельно с остальным.
                    item.plain = openChunk(this.keys.data, this.meta.noncePrefix, item.index,
                        this.plan.count, u8.subarray(HEADER));
                }
            }
            this.queue.push(item);
        }
        if (!this.pumping)
            this.pumping = this.pump().finally(() => {
                this.pumping = null;
            });
    }

    async pump() {
        while (this.queue.length && !this.fatal) {
            const item = this.queue.shift();
            try {
                if (item.bin)
                    await this.onChunk(item);
                else
                    await this.onText(item.text);
            } catch (e) {
                if (e instanceof StoreError)
                    return this.fail(e.code, e.message);
                return this.fail('io', String(e && e.message || e));
            }
        }
    }

    async onText(text) {
        let v;
        try {
            v = JSON.parse(text);
        } catch {
            return;
        }
        if (!v || typeof v !== 'object')
            return;
        switch (v.type) {
        case 'approval_pending':
            // Ключ принят, решает отправитель. Сколько он будет думать,
            // неизвестно, поэтому сторож на hello здесь молчит; уйти можно
            // кнопкой «Остановить», и использование при этом не списано.
            this.waitingApproval = true;
            this.lastMessageAt = now();
            this.setPhase('approval');
            break;
        case 'state':
            // Отправитель ушёл, разрешив раздаче жить без него, — или
            // вернулся. Приёму это ничего не меняет, человеку стоит знать.
            if (typeof v.state === 'string')
                this.relayState = v.state;
            if (typeof v.uses_left === 'number')
                this.usesLeft = v.uses_left;
            this.emitProgress();
            break;
        case 'hello_ok':
            this.accepted = true;
            this.waitingApproval = false;
            if (typeof v.state === 'string')
                this.relayState = v.state;
            this.reconnectAttempt = 0;
            this.ranges = Array.isArray(v.features) && v.features.includes('ranges');
            this.canSeed = !!v.can_seed;
            if (typeof v.uses_left === 'number')
                this.usesLeft = v.uses_left;
            this.setPhase(this.finished ? 'done' : 'receiving');
            if (this.finished)
                this.emitSeed();
            // Что нам нужно: сервер по этому выбирает, что тянуть второй
            // волной.
            if (this.ranges && !this.finished) {
                const want = this.have.missing();
                if (want.length) {
                    this.send({ type: 'request', ranges: want.slice(0, MAX_RANGES),
                        budget_bytes: this.plan.chunkSize * 8 });
                }
            }
            this.checkDone();
            break;
        case 'error':
            // Отправитель ушёл, а у нас всё на месте — не беда: том уже здесь.
            if (this.finished)
                return this.seedRefusedWith(String(v.reason || 'unknown'));
            return this.fail(String(v.reason || 'unknown'));
        case 'stats': {
            const s = v.src || {};
            const w = +s.window || 0;
            const p = +s.peers || 0;
            const snd = +s.sender || 0;
            if (w + p + snd > 0)
                this.src = { window: w, peers: p, sender: snd };
            break;
        }
        case 'hashes':
            return this.onHashSegment(v);
        case 'hashes_done':
            return this.onHashesDone(v);
        case 'serve':
            if (!Array.isArray(v.ranges))
                return;
            for (const r of v.ranges) {
                if (Array.isArray(r) && r.length >= 2 && Number.isInteger(r[0]) && Number.isInteger(r[1])
                    && r[0] >= 0 && r[1] >= r[0])
                    this.serveQueue.setRange(r[0], Math.min(r[1], this.plan.count - 1));
            }
            this.serve();
            break;
        default:
            break;
        }
    }

    // Сегмент списка: встык к уже известным, под K_meta и с меткой от
    // своего начала — переставить или подсунуть чужой сегмент нельзя.
    async onHashSegment(v) {
        const from = v.from;
        const n = v.count;
        const bad = () => this.fail('bad_hashes', 'Сегмент списка хешей не сошёлся — том не принимаем.');
        if (!this.meta.stream || this.hashesComplete || from !== this.hashed || !Number.isInteger(n)
            || n < 1 || n > HASH_SEGMENT_MAX || from + n > this.plan.count)
            return bad();
        const sealed = b64urlDecode(v.data);
        if (!sealed)
            return bad();
        const opened = await openChunk(this.keys.meta, this.meta.noncePrefix, hashSegmentLabel(from),
            this.plan.count, sealed);
        if (!opened || opened.length !== n * 32)
            return bad();
        this.hashes.set(opened, from * 32);
        this.hashed = from + n;
    }

    // Отправитель досчитал. Итоговый манифест — тот же том, но с корнем, и
    // корень обязан сойтись с тем, что собрано из сегментов.
    async onHashesDone(v) {
        const bad = () => this.fail('bad_hashes', 'Корень не сошёлся со списком хешей — том не принимаем.');
        if (!this.meta.stream)
            return bad();
        if (this.hashesComplete)
            return;   // повтор после переподключения
        if (this.hashed !== this.plan.count)
            return bad();
        const sealed = b64urlDecode(v.manifest);
        if (!sealed)
            return bad();
        const opened = await openChunk(this.keys.meta, this.meta.noncePrefix, LABEL_MANIFEST,
            this.plan.count, sealed);
        if (!opened)
            return bad();
        let final;
        try {
            final = parseManifest(new TextDecoder('utf-8', { fatal: true }).decode(opened));
        } catch {
            return bad();
        }
        if (final.stream || !equalBytes(final.root, blake3(this.hashes)) || !sameVolume(final, this.manifest))
            return bad();

        this.manifest.root = final.root;
        this.hashesComplete = true;
        this.dirtyState = true;

        // Недокачка этого же тома, досчитанного когда-то раньше: корни
        // совпали — карте можно верить целиком, перечитывать незачем.
        if (!this.pending.empty && this.storedRoot && equalBytes(this.storedRoot, final.root)) {
            for (let i = this.pending.firstPresent(0); i < this.plan.count; i = this.pending.firstPresent(i + 1)) {
                if (this.have.set(i))
                    this.received++;
            }
            this.pending.reset(this.plan.count);
        }
        this.checkDone();
    }

    async onChunk(item) {
        if (item.broken)
            return this.fail('bad_message', item.broken);
        if (item.skip || this.have.has(item.index))
            return;
        const index = item.index;
        const plain = await item.plain;
        if (!plain)
            return this.reject(index, 'aead');
        if (plain.length !== this.plan.sizeOf(index))
            return this.reject(index, 'length');
        // Проверка против списка хешей — за O(1) и независимо от порядка.
        // Именно она делает безопасным приём чанков от кого угодно.
        if (index >= this.hashed || !this.hashMatches(index, plain))
            return this.reject(index, 'hash');
        this.badInARow = 0;

        await this.store.write(this.plan.offsetOf(index), plain);
        this.have.set(index);
        this.pending.clear(index);
        this.received++;
        this.dirtyState = true;

        // Какой волной приехал чанк. Сервер этого не говорит и не должен,
        // но волна видна по порядку: всё, что пришло ПОЗАДИ уже виденного,
        // — вторая волна.
        if (this.joinIndex === null)
            this.joinIndex = index > this.sessionStart ? index : -1;
        if (index >= this.liveFront)
            this.liveFront = index;
        else
            this.late.set(index);
        this.meter.add(plain.length);
        this.checkDone();
    }

    hashMatches(index, plain) {
        const got = blake3(plain);
        const off = index * 32;
        for (let i = 0; i < 32; i++) {
            if (got[i] !== this.hashes[off + i])
                return false;
        }
        return true;
    }

    // Несошедшийся чанк больше не валит передачу: его выбрасывают, сервер
    // узнаёт, что источник солгал, и просит тот же индекс у другого.
    reject(index, why) {
        this.badInARow++;
        if (this.ranges)
            this.send({ type: 'bad_chunk', index, reason: why });
        if (this.badInARow >= MAX_BAD_CHUNKS)
            this.fail('chunk_mismatch');
    }

    // ---- фон ----

    async housekeeping() {
        if (this.inHousekeeping)
            return;
        this.inHousekeeping = true;
        try {
            await this.verifyPending();
            const t = now();
            // Сторож. Пока идёт приём, сервер не молчит подолгу: чанки,
            // stats, повторный need отправителю через пять секунд тишины
            // оборачивается данными. Тишина в STALL_MS — это мёртвая сеть,
            // о которой браузер ещё не знает.
            if (this.ws && !this.finished
                && ((!this.accepted && !this.waitingApproval
                     && t - this.socketOpenedAt > HELLO_TIMEOUT_MS)
                    || (this.accepted && t - this.lastMessageAt > STALL_MS)))
                this.abandonSocket();
            if (this.accepted && this.ws && !this.finished && t - this.lastAck > 300)
                this.reportHave();
            if (this.dirtyState && (!this.lastSave || t - this.lastSave > 500))
                await this.saveState();
            if (!this.lastEmit || t - this.lastEmit > 250)
                this.emitProgress();
            this.serve();
        } catch (e) {
            if (e instanceof StoreError)
                this.fail(e.code, e.message);
        } finally {
            this.inHousekeeping = false;
        }
    }

    // Сверка недокачки, которой нельзя было верить на слово. Не дольше
    // 30 мс за оборот: сеть не должна стоять, пока мы перечитываем диск.
    async verifyPending() {
        if (this.pending.empty || this.fatal)
            return;
        const until = now() + 30;
        while (now() < until) {
            const i = this.pending.firstPresent(0);
            if (i >= this.plan.count || i >= this.hashed)
                break;
            this.pending.clear(i);
            const len = this.plan.sizeOf(i);
            const buf = await this.store.read(this.plan.offsetOf(i), len);
            if (buf && this.hashMatches(i, buf) && this.have.set(i)) {
                this.received++;
                this.dirtyState = true;
            }
        }
        this.checkDone();
    }

    reportHave() {
        if (!this.ranges) {
            const prefix = this.have.prefix();
            if (prefix !== this.sentHave) {
                this.send({ type: 'ack', upto: prefix });
                this.sentHave = prefix;
                this.lastAck = now();
            }
            return;
        }
        if (this.have.have !== this.sentHave) {
            this.send({ type: 'have', ranges: this.have.ranges().slice(0, MAX_RANGES) });
            this.sentHave = this.have.have;
            this.lastAck = now();
        }
    }

    async saveState() {
        this.dirtyState = false;
        this.lastSave = now();
        // Несверенные чанки пишутся в карту вместе с настоящими: иначе обрыв
        // до сверки выбросил бы их, и в следующий раз их качали бы заново.
        const bits = this.have.bytes.slice();
        for (let i = 0; i < bits.length; i++)
            bits[i] |= this.pending.bytes[i];
        await this.store.saveState({
            v: 1,
            id: this.id,
            origin: this.origin,
            name: this.manifest.name,
            kind: this.manifest.kind,
            total: this.plan.total,
            chunkSize: this.plan.chunkSize,
            count: this.plan.count,
            root: this.hashesComplete && this.manifest.root ? b64urlEncode(this.manifest.root) : null,
            bits: b64urlEncode(bits),
            have: this.have.have,
            complete: this.finished,
            updatedAt: Date.now(),
        });
    }

    // Отдаём то, что у нас попросили, — по чанку за раз и с оглядкой на
    // свою же очередь: чужой догон не должен мешать собственному приёму.
    async serve() {
        if (this.serving || !this.store.canRead || !this.seeding)
            return;
        this.serving = true;
        try {
            while (!this.serveQueue.empty && this.ws && this.ws.readyState === 1
                && this.ws.bufferedAmount < SEED_HIGH_WATER) {
                const i = this.serveQueue.firstPresent(0);
                if (i >= this.plan.count)
                    break;
                this.serveQueue.clear(i);
                if (!this.have.has(i))
                    continue;   // сервер найдёт другой источник
                const len = this.plan.sizeOf(i);
                const plain = await this.store.read(this.plan.offsetOf(i), len);
                if (!plain)
                    continue;
                // Шифруем заново: AES-GCM детерминирован, и шифротекст выходит
                // байт в байт тем, что прислал отправитель. Сид не хранит
                // шифротекст — ему достаточно своего тома.
                const sealed = await sealChunk(this.keys.data, this.meta.noncePrefix, i, this.plan.count, plain);
                const frame = new Uint8Array(HEADER + sealed.length);
                frame[0] = OP_CHUNK;
                let v = i;
                for (let k = 8; k >= 1; k--) {
                    frame[k] = v % 256;
                    v = Math.floor(v / 256);
                }
                frame.set(sealed, HEADER);
                if (!this.ws || this.ws.readyState !== 1)
                    break;
                this.ws.send(frame);
                this.served++;
                this.servedBytes += len;
            }
        } catch {
            // Свою загрузку из-за чужой не роняем: перестаём отдавать, и только.
            this.serveQueue.reset(this.plan.count);
        } finally {
            this.serving = false;
        }
    }

    // ---- конец ----

    async checkDone() {
        if (this.finished || this.fatal || this.received < this.plan.count || !this.hashesComplete
            || !this.pending.empty)
            return;
        this.finished = true;
        // Последний have — обязательно: небольшой том уезжает быстрее, чем
        // идут отчёты, и сервер иначе не узнал бы, что у нас всё есть, — а
        // сидировать нас после этого не позвали бы никогда.
        this.sentHave = -1;
        this.reportHave();
        await this.store.flush();
        await this.saveState();
        this.finishedAt = Date.now();
        this.setPhase('done');
        if (!this.store.canRead || !this.seeding)
            this.closeSocket();
        this.emitProgress();
        this.emit({ ev: 'done', summary: this.summary() });
        this.emitSeed();
    }

    summary() {
        const took = Math.max(1, (this.finishedAt || Date.now()) - this.startedAt);
        return {
            took,
            rate: (this.plan.total * 1000) / took,
            count: this.plan.count,
            total: this.plan.total,
            src: this.src,
            resumed: this.resumed,
        };
    }

    closeSocket() {
        const ws = this.ws;
        if (!ws)
            return;
        this.ws = null;
        try {
            if (ws.readyState === 1)
                ws.send(JSON.stringify({ type: 'bye' }));
            ws.close(1000);
        } catch {
            // уже закрыт
        }
    }

    setSeeding(on) {
        this.seeding = !!on;
        if (on)
            this.seedRefused = '';
        if (!on && this.finished)
            this.closeSocket();
        // Том собран без сервера (см. start) — раздавать значит подключиться.
        // Только по явной просьбе: это стоит использования.
        if (on && this.finished && !this.ws && this.store.canRead && !this.stopped && !this.fatal)
            this.connect();
        this.emitSeed();
    }

    emitSeed() {
        this.emit({ ev: 'seed', on: this.seeding && !!this.ws, served: this.served,
            servedBytes: this.servedBytes, canRead: this.store.canRead, refused: this.seedRefused || '' });
    }

    seedRefusedWith(code) {
        this.seedRefused = code;
        this.seeding = false;
        this.emitSeed();
    }

    async stop() {
        this.stopped = true;
        clearTimeout(this.reconnectTimer);
        this.closeSocket();
        if (this.pumping)
            await this.pumping;
        clearInterval(this.timer);
        try {
            await this.saveState();
            await this.store.close();
        } catch {
            // хранилище уже закрыто
        }
        this.setPhase(this.finished ? 'done' : 'stopped');
        this.emitProgress();
    }

    fail(code, message) {
        if (this.fatal)
            return;
        this.fatal = { code, message: message || '' };
        clearTimeout(this.reconnectTimer);
        this.closeSocket();
        this.setPhase('failed');
        this.saveState().catch(() => {});
        this.emitProgress();
        this.emit({ ev: 'error', code, message: message || '', retry: !FINAL_ERRORS.has(code) });
    }

    setPhase(p) {
        if (this.phase === p)
            return;
        this.phase = p;
        this.emit({ ev: 'phase', phase: p });
    }

    // ---- снимок для экрана ----

    emitProgress() {
        this.lastEmit = now();
        this.emit({ ev: 'progress', ...this.snapshot() });
    }

    snapshot() {
        const p = this.plan;
        const count = p.count;
        const lastHave = count > 0 && this.have.has(count - 1);
        const bytes = count === 0 ? 0
            : this.have.have * p.chunkSize - (lastHave ? p.chunkSize - p.sizeOf(count - 1) : 0);
        const rate = this.meter.current();
        const left = p.total - bytes;

        // Две волны: хвост идёт со всеми с места, где мы вошли, начало
        // догоняется второй волной.
        const join = this.joinIndex !== null && this.joinIndex > 0 ? this.joinIndex : 0;
        let w1 = 0;
        let w2 = 0;
        if (count > 0) {
            const inRange = (a, b) => {
                let n = 0;
                for (let i = a; i < b; i++)
                    if (this.have.has(i))
                        n++;
                return n;
            };
            if (join > 0) {
                w2 = inRange(0, join) / join;
                w1 = inRange(join, count) / (count - join);
            } else {
                w1 = this.have.have / count;
            }
        }

        // Карта тома: не больше MAP_CELLS ячеек, в каждой — доля принятого
        // и какой волной.
        const cells = new Uint8Array(Math.min(MAP_CELLS, Math.max(count, 1)));
        if (count > 0) {
            for (let c = 0; c < cells.length; c++) {
                const a = Math.floor((c * count) / cells.length);
                const b = Math.max(a + 1, Math.floor(((c + 1) * count) / cells.length));
                let got = 0;
                let late = 0;
                for (let i = a; i < b; i++) {
                    if (this.have.has(i)) {
                        got++;
                        if (this.late.has(i))
                            late++;
                    }
                }
                // 0 — нет, 1 — частично, 2 — есть, 3 — есть, вторая волна
                cells[c] = got === 0 ? 0 : got < b - a ? 1 : late * 2 > got ? 3 : 2;
            }
        }

        return {
            phase: this.phase,
            received: this.received,
            count,
            bytes,
            total: p.total,
            rate,
            eta: rate > 1 && left > 0 ? (left / rate) * 1000 : left > 0 ? -1 : 0,
            verified: this.received,
            pending: this.pending.have,
            src: this.src,
            join,
            w1,
            w2,
            cells,
            bits: this.have.bytes.slice(),
            hashed: this.hashed,
            hashesComplete: this.hashesComplete,
            reconnectAt: this.phase === 'reconnecting' ? this.reconnectAt : 0,
            reconnectAttempt: this.reconnectAttempt,
            relayState: this.relayState || '',
            served: this.served,
            servedBytes: this.servedBytes,
            seeding: this.seeding && this.store.canRead && !!this.ws,
            canRead: this.store.canRead,
            store: this.store.kind,
            usesLeft: this.usesLeft,
            resumed: this.resumed,
            fatal: this.fatal,
        };
    }
}
