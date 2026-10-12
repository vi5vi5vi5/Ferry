// Накладная раздачи: то, что видит отправитель, пока том уходит.
//
// Всю работу делают два воркера: hash.worker считает BLAKE3 по чанкам,
// send.worker держит ключ, сокет и отдаёт релею ровно то, что тот просит.
// Здесь только картинка: карта переправы с маршрутом к каждому
// получателю, поток, таблица, трюм и условия выдачи.
//
// Про получателей релей говорит немного — имя, доля принятого, роль и
// откуда тот брал чанки. Этого хватает на всё, что здесь нарисовано, и
// ничего сверх этого не выдумывается: курсор на карте тома стоит только
// у того, кто качает подряд с начала, — у догоняющего форма принятого
// неизвестна.
//
// Та же накладная служит и владельцу, который ушёл и вернулся по ссылке
// владельца (#manage=…): тогда груза в вкладке нет, воркеров тоже — один
// сокет с resume_owner, получатели, условия и кнопки управления.

import { fetchMeta, wsUrl, isValidTransferId, MASTER_KEY_SIZE } from './api.js';
import { b64urlDecode } from './b64.js';
import { chunkSizeFor } from './manifest.js';
import { deriveKey, randomSalt } from './password.js';
import { encodeQr, qrPath } from './qr.js';
import { openVolume } from './volume.js';
import { NB, grp, comma, plural, unitOf, inUnit, unitName, size, dur, pct, mbps, copy } from './ui.js';

const $ = (sel) => document.querySelector(sel);
const SVG = 'http://www.w3.org/2000/svg';

const MAX_ROUTES = 6;
const META_EVERY_MS = 5000;
const MAP_CELLS = 120;
const DAY_MS = 24 * 3600 * 1000;
const MAX_TTL_MS = 7 * DAY_MS;
const MORE_USES = 5;

// Эта вкладка отпустила раздачу «работать без меня»: ссылка владельца
// лежит здесь, пока вкладку не закроют, — после перезагрузки главная
// предложит вернуться к управлению.
export const OWNED_KEY = 'ferry.owned';

// Ссылка владельца: …/#manage=<id>.<owner_token>[.<ключ>]. Ключа нет у
// раздачи по паролю — его не знает и сам владелец, пока не введёт пароль.
export function parseManageLink(text) {
    let url;
    try {
        url = new URL(String(text || '').trim());
    } catch {
        return null;
    }
    const m = /^#manage=([A-Za-z0-9_-]+)\.([A-Za-z0-9_-]+)(?:\.([A-Za-z0-9_-]+))?$/.exec(url.hash);
    if (!m || !isValidTransferId(m[1]))
        return null;
    const token = b64urlDecode(m[2]);
    if (!token || token.length < 16)
        return null;
    let key = '';
    if (m[3]) {
        const raw = b64urlDecode(m[3]);
        if (!raw || raw.length !== MASTER_KEY_SIZE)
            return null;
        key = m[3];
    }
    return { origin: url.origin, id: m[1], token: m[2], key };
}

export function manageLink(origin, id, token, key) {
    return `${origin}/#manage=${id}.${token}${key ? '.' + key : ''}`;
}

const PREP = {
    creating: ['Заводим раздачу на релее…',
        'Ключ рождается здесь, в этой вкладке. На релей уходит только проверочный отпечаток, из которого ключ не вывести.'],
    hashing: ['Считаем хеши до ссылки…',
        'Этот релей не умеет хеши на лету, поэтому ссылка появится, когда весь том будет посчитан.'],
    offering: ['Отдаём релею опись…',
        'Опись зашифрована тем же ключом: релей не видит ни имён, ни состава тома.'],
    deriving: ['Выводим ключ из пароля…',
        'Argon2id, 64 МиБ памяти. Это нарочно небыстро: так же небыстро будет и тому, кто захочет подобрать пароль.'],
    resuming: ['Возвращаемся к раздаче…',
        'Ссылка владельца подтверждает права на релее. Тома в этой вкладке нет — он у тех, кто скачал его целиком.'],
};

const ENDS = {
    expired: ['Срок раздачи вышел', 'Релей закрыл раздачу по сроку жизни. Ссылка больше не работает; тот же груз можно раздать заново — с новым ключом.', 'warn', 'срок вышел'],
    network: ['Связь с релеем оборвалась', 'Пропала сеть или релей перезапустили. Без связи с отправителем раздача не живёт: ссылка больше не открывается, и кто не докачал, начнёт заново по новой.', 'danger', 'связь потеряна'],
    server_busy: ['Релей сейчас занят', 'Упёрлись в потолок раздач или памяти на релее. Попробуйте чуть позже.', 'warn', 'релей занят'],
    owner_conflict: ['Релей не признал раздачу', 'Токен владельца не подошёл — так бывает, если черновик раздачи истёк. Начните заново.', 'danger', 'отказ'],
    read: ['Файл изменился или пропал', 'Чтение с диска не удалось, и отдавать дальше нечего. Выберите груз заново.', 'danger', 'груз не читается'],
    unsupported: ['Релей этого не умеет', 'Выбранные условия этот релей не поддерживает. Отправьте без них или обновите релей.', 'warn', 'не поддерживается'],
    revoked: ['Доступ закрыт', 'Раздачу закрыли по ссылке владельца. Ссылка больше не работает.', 'warn', 'доступ закрыт'],
    uses_exhausted: ['Использования кончились', 'Все, кому было можно, забрали том, и раздача закрылась.', 'warn', 'всё забрали'],
    sender_gone: ['Раздавать стало некому', 'Ушли все, кто скачал том целиком, и раздача без отправителя закрылась. Ссылка больше не работает.', 'warn', 'раздача закрыта'],
    transfer_not_found: ['Раздачи больше нет', 'Её закрыл срок, кончились использования, доступ закрыли — или без отправителя её стало некому отдавать.', 'warn', 'раздачи нет'],
};
// Владелец без тома видит то же по-своему.
const ENDS_MANAGE = {
    owner_conflict: ['Управляют не отсюда', 'Ссылку владельца открыли в другой вкладке, или отправитель вернулся с томом. Управлять раздачей можно из одного места.', 'warn', 'управляют не отсюда'],
    network: ['Связь с релеем оборвалась', 'Раздача от этого не остановилась: её отдают те, кто скачал том целиком. Чтобы снова управлять ею, откройте ссылку владельца ещё раз.', 'warn', 'связь потеряна'],
};
const DEFAULT_END = ['Раздача прервалась', 'Что-то пошло не так, и продолжать вслепую мы не стали.', 'danger', 'прервана'];

let V = null;

// ---------------------------------------------------------------- мелочи

function setText(name, value) {
    const v = String(value);
    for (const el of document.querySelectorAll(`[data-t="${name}"]`))
        if (el.textContent !== v)
            el.textContent = v;
}

function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls)
        e.className = cls;
    if (text !== undefined)
        e.textContent = text;
    return e;
}

function svg(tag, attrs) {
    const e = document.createElementNS(SVG, tag);
    for (const [k, v] of Object.entries(attrs || {}))
        e.setAttribute(k, v);
    return e;
}

function kvs(root, rows) {
    root.replaceChildren(...rows.map(([k, v, cls, unit]) => {
        const row = el('div', 'kv');
        const vv = el('span', 'v' + (cls ? ' ' + cls : ''), v);
        if (unit)
            vv.append(' ', el('span', 'u', unit));
        row.append(el('span', 'k', k), el('span', 'dots'), vv);
        return row;
    }));
}

const nameOf = (r) => (r.name && String(r.name).trim()) || `гость ${r.id}`;

const ROLE = {
    leech: ['качает', 'leech'],
    catching: ['догоняет', 'catching'],
    seed: ['раздаёт другим', 'seed'],
};

// Точка на маршруте: квадратичная Безье от релея до причала получателя.
function onRoute(y, f) {
    const P0 = [830, 300];
    const P1 = [1150, y];
    const P2 = [1450, y];
    const u = 1 - f;
    const x = u * u * P0[0] + 2 * u * f * P1[0] + f * f * P2[0];
    const yy = u * u * P0[1] + 2 * u * f * P1[1] + f * f * P2[1];
    const dx = 2 * u * (P1[0] - P0[0]) + 2 * f * (P2[0] - P1[0]);
    const dy = 2 * u * (P1[1] - P0[1]) + 2 * f * (P2[1] - P1[1]);
    return { x: (x / 1600) * 100, y: (yy / 600) * 100, a: (Math.atan2(dy, dx) * 180) / Math.PI };
}

function laneYs(n) {
    if (n <= 1)
        return [300];
    return Array.from({ length: n }, (_, i) => 110 + (i * 380) / (n - 1));
}

const BOAT = (accent, stripe) => `<svg viewBox="0 0 56 28" width="48" height="24" aria-hidden="true"><path d="M20 4 V11" stroke="#5a7181"></path><circle cx="20" cy="3" r="1.8" fill="${accent}"></circle><rect x="10" y="10" width="22" height="7" fill="#a8b7c2"></rect><rect x="24" y="6" width="9" height="4" fill="#e8eef2"></rect><rect x="36" y="11" width="6" height="6" fill="#5a7382"></rect><path d="M2 17 H54 L49 25 Q48 26 46 26 H8 Q6 26 5 24 Z" fill="#05080b" stroke="${accent === '#5ecdbb' ? '#5ecdbb' : '#a8b7c2'}" stroke-width="1"></path><path d="M4 20 H52.6 L51.6 22 H4.8 Z" fill="${stripe}"></path></svg>`;

const MINI_BOAT = (stripe) => `<svg viewBox="0 0 56 28" width="26" height="13" aria-hidden="true"><path d="M2 17 H54 L49 25 Q48 26 46 26 H8 Q6 26 5 24 Z" fill="#05080b" stroke="#a8b7c2" stroke-width="1.6"></path><rect x="10" y="9" width="22" height="8" fill="#a8b7c2"></rect><path d="M4 20 H52.6 L51.6 22 H4.8 Z" fill="${stripe}"></path></svg>`;

// ---------------------------------------------------------------- состояние

class View {
    constructor(cargo, policy, hooks, manage = null) {
        this.cargo = cargo;
        this.policy = policy;
        this.hooks = hooks;
        this.manage = manage;
        this.total = cargo ? cargo.total : 0;
        this.chunkSize = cargo ? chunkSizeFor(cargo.total) : 0;
        this.count = cargo ? Math.ceil(this.total / this.chunkSize) : 0;
        this.name = cargo ? cargo.manifest.name : '';
        this.passwordMode = !!(policy && policy.password);
        this.passwordText = this.passwordMode ? policy.password : '';
        this.passwordShown = this.passwordMode ? (policy.passwordShown || policy.password) : '';
        this.phase = 'preparing';
        this.ownerToken = '';
        this.features = [];
        // Последнее, что релей сказал об условиях: state, resume_ok или
        // метаданные. at — когда, чтобы срок тикал между обновлениями.
        this.cond = null;
        this.knocks = new Map();    // id → { name, at } — ждут у двери
        this.linkLost = false;      // связь с релеем пропала, воркер возвращается
        this.ws = null;
        this.link = '';
        this.key = '';
        this.id = '';
        this.prog = null;
        this.receivers = [];
        this.rx = new Map();        // id → оценка скорости и последний src
        this.finished = new Set();  // кто дошёл до 100 %
        this.meta = null;
        this.metaAt = 0;
        this.timers = [];
        this.workers = [];
        this.confirming = false;
        this.marks = new Map();     // id → суда и подписи на карте
        this.rowEls = new Map();    // id → строка таблицы
    }

    // ---- ход ----

    start() {
        (this.manage ? this.startManage() : this.startSend()).catch((e) => {
            this.onEvent({ ev: 'failed', code: (e && e.code) || 'internal', message: (e && e.message) || String(e) });
        });
        this.timers.push(setInterval(() => this.tick(), 1000));
        this.renderAll();
    }

    async startSend() {
        const { cargo } = this;
        let password = null;
        // Ключ из пароля выводится здесь, до воркеров: Argon2id живёт в
        // своём классическом воркере (hash-wasm собран как UMD), а в
        // send.worker приходят уже 32 байта и соль.
        if (this.passwordMode) {
            this.prepPhase = 'deriving';
            this.renderPrep();
            const salt = randomSalt();
            const key = await deriveKey(this.passwordText, salt);
            if (this.phase !== 'preparing')
                return;
            password = { key, salt };
        }
        const ch = new MessageChannel();
        const hasher = new Worker('/assets/js/hash.worker.js', { type: 'module' });
        const worker = new Worker('/assets/js/send.worker.js', { type: 'module' });
        this.workers = [hasher, worker];
        this.worker = worker;
        worker.onmessage = (e) => this.onEvent(e.data);
        worker.onerror = (e) => this.onEvent({ ev: 'failed', code: 'internal', message: e.message || 'воркер отправки упал' });
        hasher.onerror = (e) => this.onEvent({ ev: 'failed', code: 'internal', message: e.message || 'воркер хешей упал' });
        hasher.postMessage({ cmd: 'start', port: ch.port1, files: cargo.files, manifest: cargo.manifest,
            chunkSize: this.chunkSize }, [ch.port1]);
        const policy = { ...this.policy };
        delete policy.password;
        delete policy.passwordShown;
        worker.postMessage({ cmd: 'start', hashPort: ch.port2, files: cargo.files, manifest: cargo.manifest,
            chunkSize: this.chunkSize, policy, password, origin: location.origin }, [ch.port2]);
        if (password)
            password.key.fill(0);
    }

    // Владелец вернулся без тома: метаданные — чтобы знать размер и режим,
    // ключ из ссылки — чтобы показать имя, и сокет с resume_owner.
    async startManage() {
        const m = this.manage;
        this.id = m.id;
        this.ownerToken = m.token;
        this.key = m.key;
        this.prepPhase = 'resuming';
        this.renderAll();
        let meta;
        try {
            meta = await fetchMeta(location.origin, m.id);
        } catch (e) {
            this.end('failed', e.code || 'network', e.message === e.code ? '' : e.message);
            return;
        }
        this.total = meta.total;
        this.chunkSize = meta.chunkSize;
        this.count = meta.chunks;
        this.passwordMode = meta.mode === 'password';
        if (m.key) {
            try {
                const vol = await openVolume(meta, b64urlDecode(m.key));
                this.name = vol.manifest.name;
            } catch {
                // Ключ не подошёл к манифесту — управлять можно и без имени.
            }
        }
        if (this.phase !== 'preparing')
            return;
        this.renderAll();

        const ws = new WebSocket(wsUrl(location.origin));
        this.ws = ws;
        let lastError = '';
        ws.onopen = () => ws.send(JSON.stringify({ type: 'resume_owner', id: m.id, owner_token: m.token, serve: false }));
        ws.onmessage = (ev) => {
            if (typeof ev.data !== 'string')
                return;
            let v;
            try {
                v = JSON.parse(ev.data);
            } catch {
                return;
            }
            switch (v.type) {
            case 'resume_ok':
                this.features = Array.isArray(v.features) ? v.features : [];
                this.onEvent({ ev: 'ready', id: m.id, link: `${location.origin}/t/${m.id}`, key: m.key,
                    ownerToken: m.token, features: this.features });
                this.onEvent({ ev: 'state', state: v });
                break;
            case 'peers':
                this.onEvent({ ev: 'peers', receivers: Array.isArray(v.receivers) ? v.receivers : [] });
                break;
            case 'approval_request':
                this.onEvent({ ev: 'knock', receiver: v.receiver, name: typeof v.name === 'string' ? v.name : '' });
                break;
            case 'approval_cancel':
                this.onEvent({ ev: 'knockGone', receiver: v.receiver });
                break;
            case 'state':
                this.onEvent({ ev: 'state', state: v });
                break;
            case 'error':
                lastError = String(v.reason || 'unknown');
                this.onEvent({ ev: 'failed', code: lastError });
                break;
            default:
                break;
            }
        };
        ws.onclose = () => {
            if (this.phase === 'live' || this.phase === 'preparing')
                this.onEvent({ ev: 'failed', code: lastError || 'network' });
        };
    }

    // Команда владельца релею: пустить, отказать, поменять условия.
    owner(msg) {
        if (this.phase !== 'live')
            return;
        if (this.manage) {
            if (this.ws && this.ws.readyState === 1)
                this.ws.send(JSON.stringify(msg));
        } else if (this.worker) {
            this.worker.postMessage({ cmd: 'owner', msg });
        }
    }

    // revoke — «Закрыть доступ»: релей закрывает раздачу для всех. Без
    // него закрыть сокет при «работать без меня» значило бы просто уйти.
    stop(revoke = false) {
        const running = this.phase === 'live' || this.phase === 'preparing';
        if (this.manage && running) {
            if (this.ws && this.ws.readyState === 1) {
                if (revoke)
                    this.ws.send(JSON.stringify({ type: 'revoke' }));
                this.ws.close(1000);
            }
            this.end('closed');
        } else if (this.worker && running) {
            if (revoke)
                this.beacon('revoke');
            else
                this.leaveBeacon();
            this.worker.postMessage({ cmd: 'stop', revoke });
            this.end('closed');
        } else {
            this.phase = this.phase === 'preparing' ? 'closed' : this.phase;
            this.dispose();
        }
    }

    // Сколько сейчас на связи тех, у кого весь том: на них раздача и
    // останется без вкладки. Не finished — тот помнит всех, кто когда-либо
    // докачал, в том числе ушедших и не оставшихся раздавать.
    liveSeeds() {
        return this.receivers.filter((r) => Number(r.progress) >= 1).length;
    }

    // Уходим сами — релей должен узнать это сразу, а не ждать нас тридцать
    // секунд, как после обрыва. Маяком по HTTP, а не через сокет: сокет в
    // воркере забит чанками, а при закрытии вкладки воркер не успеет и этого.
    leaveBeacon() {
        if ((this.features || []).includes('resume'))
            this.beacon('leave');
    }

    // what — leave или revoke. «Закрыть доступ» идёт тем же путём: revoke в
    // сокете встал бы за мегабайтами чанков, а воркер закроют раньше, и
    // релей ещё тридцать секунд пускал бы новых. Сокет revoke тоже шлёт —
    // для релея постарше, у которого /revoke нет.
    beacon(what) {
        if (this.manage || !this.id || !this.ownerToken)
            return;
        try {
            navigator.sendBeacon(`${location.origin}/api/transfers/${encodeURIComponent(this.id)}/${what}`,
                new Blob([this.ownerToken], { type: 'text/plain' }));
        } catch {
            // не вышло — остаётся сокет
        }
    }

    // Воркеру даём мгновение закрыть сокет как положено: релей тогда
    // сразу скажет получателям «отправитель ушёл», а не будет ждать таймаута.
    dispose(delay = 0) {
        for (const t of this.timers)
            clearInterval(t);
        this.timers = [];
        const ws = this.workers;
        this.workers = [];
        setTimeout(() => ws.forEach((w) => w.terminate()), delay);
        if (this.ws && this.ws.readyState <= 1)
            this.ws.close(1000);
    }

    onEvent(m) {
        if (this.phase === 'closed' || this.phase === 'failed')
            return;
        switch (m.ev) {
        case 'phase':
            this.prepPhase = m.phase;
            this.renderPrep();
            break;
        case 'ready':
            this.phase = 'live';
            this.id = m.id;
            this.link = m.link;
            this.key = m.key;
            this.ownerToken = m.ownerToken || '';
            this.features = m.features || [];
            if (!this.manage) {
                const p = this.policy;
                this.cond = { usesLeft: p.uses > 0 ? p.uses : -1, expiresInMs: p.ttlMs, at: performance.now(),
                    requireApproval: !!p.requireApproval, leaveOpen: !!p.leaveOpen, state: 'active',
                    maxConcurrent: p.maxConcurrent || 8 };
                if (p.leaveOpen)
                    this.rememberOwned();
            }
            this.renderLink();
            this.pollMeta();
            this.timers.push(setInterval(() => this.pollMeta(), META_EVERY_MS));
            this.renderAll();
            break;
        case 'progress':
            this.prog = m;
            this.renderFlow();
            this.renderMap();
            if (this.phase === 'preparing')
                this.renderPrep();
            break;
        case 'peers':
            this.onPeers(m.receivers);
            break;
        case 'knock':
            this.knocks.set(m.receiver, { name: m.name, at: performance.now() });
            this.renderKnocks();
            break;
        case 'knockGone':
            this.knocks.delete(m.receiver);
            this.renderKnocks();
            break;
        case 'state':
            this.applyState(m.state);
            break;
        case 'link':
            // Связь с релеем пропала, воркер возвращается сам. Раздача на
            // релее ждёт нас: получатели стоят, а не получают отказ.
            this.linkLost = m.state === 'lost';
            this.renderHead();
            break;
        case 'resumed':
            // Вопросы у двери релей пришлёт заново, уже в новый сокет.
            this.knocks.clear();
            this.renderKnocks();
            break;
        case 'failed':
            this.end('failed', m.code, m.message);
            break;
        case 'closed':
            this.end('closed');
            break;
        default:
            break;
        }
    }

    onPeers(list) {
        const now = performance.now();
        this.receivers = list.slice().sort((a, b) => a.id - b.id);
        for (const r of this.receivers) {
            const p = Math.max(0, Math.min(1, Number(r.progress) || 0));
            let s = this.rx.get(r.id);
            if (!s) {
                s = { p, t: now, rate: 0, src: null, name: nameOf(r) };
                this.rx.set(r.id, s);
            }
            // Скорость — по приросту доли за окно не короче секунды: сервер
            // шлёт peers на каждый have, и мгновенные значения прыгают.
            const dt = (now - s.t) / 1000;
            if (dt >= 1) {
                const inst = Math.max(0, ((p - s.p) * this.total) / dt);
                s.rate = s.rate > 0 ? s.rate * 0.6 + inst * 0.4 : inst;
                s.p = p;
                s.t = now;
            }
            if (p >= 1)
                s.rate = 0;
            if (r.src)
                s.src = r.src;
            s.name = nameOf(r);
            if (p >= 1)
                this.finished.add(r.id);
        }
        this.renderScene();
        this.renderRows();
        this.renderSources();
        this.renderMapCursors();
    }

    applyState(v) {
        const prev = this.cond || {};
        this.cond = {
            state: typeof v.state === 'string' ? v.state : prev.state,
            usesLeft: typeof v.uses_left === 'number' ? v.uses_left : -1,
            expiresInMs: typeof v.expires_in_ms === 'number' ? v.expires_in_ms : 0,
            requireApproval: !!v.require_approval,
            leaveOpen: !!v.leave_open,
            maxConcurrent: typeof v.max_concurrent === 'number' ? v.max_concurrent : prev.maxConcurrent,
            at: performance.now(),
        };
        // Подтверждение сняли — очередь у двери вошла целиком, ответов от
        // нас больше не ждут.
        if (!this.cond.requireApproval)
            this.knocks.clear();
        if (!this.manage && this.cond.leaveOpen)
            this.rememberOwned();
        this.renderKnocks();
        this.renderCond();
        this.renderPol();
        this.renderLink();
    }

    ownerLink() {
        if (!this.id || !this.ownerToken)
            return '';
        return manageLink(location.origin, this.id, this.ownerToken, this.passwordMode ? '' : this.key);
    }

    rememberOwned() {
        try {
            sessionStorage.setItem(OWNED_KEY, JSON.stringify({ url: this.ownerLink(), name: this.name }));
        } catch {
            // хранилище недоступно — ссылка владельца всё равно на экране
        }
    }

    async pollMeta() {
        if (this.phase !== 'live' || !this.id)
            return;
        try {
            const m = await fetchMeta(location.origin, this.id);
            if (this.phase !== 'live')
                return;
            this.meta = m;
            this.metaAt = performance.now();
            const prev = this.cond || {};
            this.cond = { ...prev, state: m.state, usesLeft: m.usesLeft, expiresInMs: m.expiresInMs,
                requireApproval: m.requireApproval, at: this.metaAt };
            this.renderCond();
            this.renderPol();
        } catch {
            // Раздача закрылась — это скажет сокет; сеть мигнула — спросим ещё.
        }
    }

    tick() {
        if (this.phase !== 'live')
            return;
        // Скорость получателя гаснет, если от него давно ничего не было.
        const now = performance.now();
        for (const s of this.rx.values())
            if (now - s.t > 4000)
                s.rate = 0;
        this.renderCond();
        this.renderRows();
        this.renderKnocks();
    }

    end(phase, code = '', message = '') {
        this.phase = phase;
        this.code = code;
        this.dispose(phase === 'closed' ? 300 : 0);
        const box = $('#sx-end');
        box.hidden = false;
        box.className = 'banner';
        if (phase === 'closed') {
            setText('sxEndTitle', 'Доступ закрыт');
            setText('sxEndText', 'Раздача закрыта по вашей команде. Ссылка больше не работает: новые получатели не войдут, а кто качал — остановился.');
        } else {
            const e = this.endOf(code);
            // У чтения своя причина уже в тексте; у остальных подробность
            // из воркера помогает понять, что именно случилось.
            const known = ENDS[code] || (this.manage && ENDS_MANAGE[code]);
            const detail = known ? (message && message !== code && code !== 'read' ? ` (${message})` : '')
                : ` Код: ${code}${message && message !== code ? `, ${message}` : ''}.`;
            let text = e[1];
            // Отправитель с «работать без меня» потерял связь: раздача,
            // может быть, жива — её держат сиды, и вернуться можно.
            if (!this.manage && code === 'network' && this.cond && this.cond.leaveOpen && this.ownerToken)
                text = 'Пропала сеть или релей перезапустили. Если кто-то уже скачал том целиком, раздача продолжается без вас — управлять ею можно по ссылке владельца.';
            setText('sxEndTitle', e[0]);
            setText('sxEndText', text + detail);
            box.classList.add(e[2] === 'warn' ? 'banner' : 'banner--danger');
        }
        this.knocks.clear();
        $('#sx-again').hidden = code === 'read' || !!this.manage;
        $('#sx-manage').hidden = !(!this.manage && phase === 'failed' && code === 'network'
            && this.cond && this.cond.leaveOpen && this.ownerToken);
        this.renderAll();
        box.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
    }

    endOf(code) {
        return (this.manage && ENDS_MANAGE[code]) || ENDS[code] || DEFAULT_END;
    }

    // ---------------------------------------------------------------- картинка

    renderAll() {
        this.renderHead();
        this.renderPrep();
        this.renderFlow();
        this.renderSources();
        this.renderScene();
        this.renderRows();
        this.renderMap();
        this.renderMapCursors();
        this.renderCond();
        this.renderKnocks();
        this.renderPol();
    }

    renderHead() {
        const stamp = $('#sx-stamp');
        stamp.className = 'stamp';
        let a = 'готовим ссылку';
        let b = 'груз на причале';
        if (this.manage && this.phase === 'preparing') {
            a = 'возвращаемся';
            b = 'ссылка владельца';
        } else if (this.manage && this.phase === 'live') {
            a = 'без отправителя';
            b = this.receivers.length ? 'раздают скачавшие' : 'ждём получателей';
        } else if (this.phase === 'live' && this.linkLost) {
            stamp.classList.add('stamp--warn');
            a = 'связь пропала';
            b = 'возвращаемся, получатели ждут';
        } else if (this.phase === 'live') {
            a = 'раздача идёт';
            b = this.receivers.length ? 'принято к перевозке' : 'ждём получателей';
        } else if (this.phase === 'closed') {
            stamp.classList.add('stamp--wait');
            a = 'доступ закрыт';
            b = 'по команде отправителя';
        } else if (this.phase === 'failed') {
            const e = this.endOf(this.code);
            stamp.classList.add(e[2] === 'warn' ? 'stamp--warn' : 'stamp--danger');
            a = e[3];
            b = 'раздача закрыта';
        } else {
            stamp.classList.add('stamp--wait');
        }
        setText('sxStamp', a);
        setText('sxStampSub', b);
        $('#sx-idbox').hidden = !this.id;
        setText('sxId', this.id);
        const ended = this.phase === 'closed' || this.phase === 'failed';
        $('#sx-ctl').hidden = ended || this.confirming;
        $('#sx-confirm').hidden = ended || !this.confirming;
        $('#sx-close').disabled = this.phase !== 'live';
        $('#send').classList.toggle('sx-ended', ended);
        $('#send').classList.toggle('sx-manage', !!this.manage);
        setText('sxPort', this.manage ? 'отправителя нет' : 'ваш порт');
        const n = ended ? 0 : this.knocks.size;
        document.title = `${n ? `(${n}) Пустить? · ` : ''}${ended ? 'Раздача закрыта' : this.manage ? 'Управление' : 'Раздача'}`
            + `${this.name ? ` · ${this.name}` : ''} — Ferry`;
    }

    renderPrep() {
        const ready = !!this.link;
        $('#sx-prep').hidden = ready || this.phase === 'failed' || this.phase === 'closed';
        $('#sx-linkbox').hidden = !ready;
        if (ready)
            return;
        const [t, sub] = PREP[this.prepPhase || 'creating'] || PREP.creating;
        let title = t;
        if (this.prepPhase === 'hashing' && this.prog && this.count)
            title = `Считаем хеши до ссылки: ${comma((this.prog.hashed / this.count) * 100, 0)}${NB}%`;
        setText('sxPrep', title);
        setText('sxPrepSub', sub);
    }

    fullLink() {
        return this.key ? `${this.link}#${this.key}` : this.link;
    }

    renderLink() {
        if (!this.link)
            return;
        setText('sxLink', this.link);
        setText('sxKey', this.key ? '#' + this.key : '');
        setText('sxKeyOnly', this.key);
        $('#sx-copy-bare').hidden = !this.key;
        if (!this.key)
            $('#sx-split').hidden = true;
        let note = 'Часть после решётки — ключ. На сервер она не уходит: её нет ни в запросе, ни в логах, ни в Referer. Кто получил ссылку целиком — получил том.';
        if (this.passwordMode)
            note = 'Ссылка без ключа: ключ получатель выведет из пароля. Этот режим слабее ключа — у кого ссылка, тот может перебирать пароли сколько угодно, не спрашивая релей.';
        else if (!this.key)
            note = 'В ссылке владельца не было ключа, поэтому здесь ссылка без него. Получателю нужна та, что с ключом, — её вы отправляли раньше.';
        setText('sxLinkNote', note);
        setText('sxLinkHead', this.passwordMode ? 'ссылка и пароль' : 'ссылка и ключ');
        $('#sx-pw').hidden = !(this.passwordMode && this.passwordText);
        const c = this.cond;
        $('#sx-owner').hidden = !!this.manage || !this.ownerToken || !(c && c.leaveOpen)
            || !this.features.includes('policy');
        const qr = encodeQr(this.fullLink());
        const box = $('#sx-qr');
        box.setAttribute('viewBox', `-2 -2 ${qr.size + 4} ${qr.size + 4}`);
        box.querySelector('path').setAttribute('d', qrPath(qr));
        this.renderHead();
        this.renderPrep();
    }

    // Очередь у двери. Строки не пересоздаются на каждый тик, иначе
    // кнопка уходила бы из-под курсора.
    renderKnocks() {
        const box = $('#sx-knocks');
        const live = this.phase === 'live';
        box.hidden = !live || this.knocks.size === 0;
        const list = $('#sx-knock-list');
        const now = performance.now();
        const want = live ? [...this.knocks.keys()] : [];
        for (const row of Array.from(list.children))
            if (!this.knocks.has(Number(row.dataset.id)) || !live)
                row.remove();
        want.forEach((id, i) => {
            const k = this.knocks.get(id);
            let row = list.querySelector(`[data-id="${id}"]`);
            if (!row) {
                row = el('div', 'sx-knock');
                row.dataset.id = String(id);
                const who = el('div', 'who');
                who.append(el('b', '', (k.name && k.name.trim()) || `гость ${id}`), el('span', 'when'));
                const yes = el('button', 'btn btn-p btn-sm', 'Пустить');
                yes.type = 'button';
                yes.addEventListener('click', () => this.answer(id, true));
                const no = el('button', 'btn btn-sm', 'Отказать');
                no.type = 'button';
                no.addEventListener('click', () => this.answer(id, false));
                row.append(who, yes, no);
            }
            if (list.children[i] !== row)
                list.insertBefore(row, list.children[i] || null);
            row.querySelector('.when').textContent = `ключ верный · ждёт ${dur(now - k.at)}`;
        });
        const n = this.knocks.size;
        setText('sxKnockTitle', n === 1 ? 'Ждёт вашего решения' : `Ждут вашего решения: ${grp(n)}`);
        this.renderHead();
    }

    answer(id, ok) {
        this.owner({ type: 'approve', receiver: id, ok });
        // Ответ ушёл — строку убираем сразу; если релей не пустит по
        // своим причинам, получатель узнает это сам.
        this.knocks.delete(id);
        this.renderKnocks();
    }

    // Ручки условий: подтверждение, «без меня», «продлить», «ещё раз».
    renderPol() {
        const live = this.phase === 'live';
        const can = live && this.features.includes('policy');
        const c = this.cond || {};
        $('#sx-pol').hidden = !can;
        $('#sx-sw-approve').setAttribute('aria-checked', String(!!c.requireApproval));
        $('#sx-sw-leave').setAttribute('aria-checked', String(!!c.leaveOpen));
        $('#sx-leave-row').hidden = !!this.manage;
        $('#sx-extend').hidden = !can;
        $('#sx-more').hidden = !can || !(c.usesLeft >= 0);
        const left = this.timeLeft();
        $('#sx-extend').disabled = left >= MAX_TTL_MS - 3600 * 1000;
    }

    timeLeft() {
        const c = this.cond;
        if (!c)
            return this.policy ? this.policy.ttlMs : 0;
        return Math.max(0, c.expiresInMs - (performance.now() - c.at));
    }

    renderFlow() {
        const p = this.prog;
        const rate = p && this.phase === 'live' ? p.rate : 0;
        setText('sxRate', mbps(rate));
        $('.sx-map-box').classList.toggle('sx-idle', rate <= 0);
        const sent = p ? p.distinctBytes : 0;
        const again = p ? Math.max(0, p.sentBytes - p.distinctBytes) : 0;
        const rows = [
            ['отдано', size(sent), '', `из ${size(this.total)}`],
            ['повторно', again > 0 ? size(again) : 'ничего', again > 0 ? '' : 'v--ok'],
        ];
        if (p && !p.hashesDone) {
            const f = this.count ? p.hashed / this.count : 0;
            rows.push(['хеши', `${comma(f * 100, 0)}${NB}%`, '',
                p.hashRate > 0 ? `${mbps(p.hashRate)}${NB}МБ/с` : 'считаются']);
        } else if (p && p.hashTook >= 0) {
            rows.push(['хеши', 'посчитаны', 'v--ok', p.hashTook < 1000 ? 'меньше чем за секунду' : `за ${dur(p.hashTook)}`]);
        }
        rows.push(['на диске релея', '0', '', 'байт']);
        kvs($('#sx-flow'), rows);
    }

    renderSources() {
        let w = 0;
        let pe = 0;
        let se = 0;
        for (const s of this.rx.values()) {
            if (!s.src)
                continue;
            w += Number(s.src.window) || 0;
            pe += Number(s.src.peers) || 0;
            se += Number(s.src.sender) || 0;
        }
        const all = w + pe + se;
        const bars = $('#sx-src').children;
        const share = (x) => (all ? x / all : 0);
        bars[0].style.width = pct(share(w));
        bars[1].style.width = pct(share(pe));
        bars[2].style.width = pct(share(se));
        const t = (x) => (all ? `${comma(share(x) * 100, 0)}${NB}%` : '—');
        setText('srcWindow', t(w));
        setText('srcPeers', t(pe));
        setText('srcSender', t(se));
    }

    renderScene() {
        const g = $('#sx-routes');
        const marks = $('#sx-marks');
        const ended = this.phase === 'closed' || this.phase === 'failed';
        const list = this.receivers;
        const shown = list.slice(0, MAX_ROUTES);
        const ys = laneYs(Math.max(1, shown.length));
        const parts = [];

        if (shown.length === 0) {
            parts.push(svg('rect', { x: 1440, y: 294, width: 90, height: 12, fill: '#22303a' }));
            parts.push(svg('path', { d: 'M830 300 Q1150 300 1450 300', fill: 'none', stroke: '#5a7181',
                'stroke-width': 1.2, 'stroke-dasharray': '4 7', opacity: '.6' }));
        }
        let wait = marks.querySelector('.sx-wait');
        if (!wait) {
            wait = el('div', 'sx-wait');
            wait.append(el('b'), el('span'));
            marks.append(wait);
        }
        wait.hidden = shown.length > 0;
        wait.firstChild.textContent = ended ? 'никого' : 'ждём';
        wait.lastChild.textContent = ended ? 'раздача закрыта' : 'получатели появятся здесь';

        // Суда и подписи живут между обновлениями: тогда они плывут по
        // маршруту, а не прыгают с места на место.
        const alive = new Set();
        shown.forEach((r, i) => {
            const y = ys[i];
            const d = `M830 300 Q1150 ${y} 1450 ${y}`;
            const p = Math.max(0, Math.min(1, Number(r.progress) || 0));
            const [roleText, roleCls] = ROLE[r.role] || ROLE.leech;
            const peerish = roleCls !== 'leech';
            parts.push(svg('rect', { x: 1440, y: y - 6, width: 90, height: 12, fill: '#22303a' }));
            parts.push(svg('path', { d, fill: 'none', stroke: '#5a7181', 'stroke-width': 1.2, 'stroke-dasharray': '4 7' }));
            parts.push(svg('path', { d, pathLength: 1000, fill: 'none', stroke: peerish ? '#2ba192' : '#5a7382',
                'stroke-width': 3.5, 'stroke-dasharray': `${(p * 1000).toFixed(1)} 1000` }));

            alive.add(r.id);
            let m = this.marks.get(r.id);
            if (!m) {
                m = { boat: el('div', 'sx-boat'), hull: el('div', 'hull'), tag: el('span', 'tag'),
                    who: el('div', 'sx-who'), name: el('b'), role: el('span', 'role'), kind: '' };
                m.boat.append(m.hull, m.tag);
                m.who.append(m.name, m.role);
                marks.append(m.boat, m.who);
                this.marks.set(r.id, m);
            }
            const pos = onRoute(y, p);
            m.boat.style.left = `${pos.x.toFixed(3)}%`;
            m.boat.style.top = `${pos.y.toFixed(3)}%`;
            m.hull.style.transform = `rotate(${(p >= 1 ? 0 : pos.a).toFixed(1)}deg)`;
            const kind = roleCls === 'seed' ? 'seed' : 'go';
            if (m.kind !== kind) {
                m.kind = kind;
                m.hull.innerHTML = kind === 'seed' ? BOAT('#5ecdbb', '#35b3a0') : BOAT('#ff8c47', '#ff6a1f');
                m.boat.classList.toggle('seed', kind === 'seed');
            }
            m.tag.textContent = `${comma(p * 100, 0)}${NB}%`;
            // Подпись — над своим фарватером: у причала её место занимает
            // судно, когда доходит до конца.
            m.who.style.top = `${(((y - 62) / 600) * 100).toFixed(3)}%`;
            m.role.hidden = shown.length > 3;
            m.name.textContent = nameOf(r);
            m.role.textContent = roleText;
            m.role.className = `role ${roleCls === 'seed' ? 'v--seed' : 'dim'}`;
        });
        for (const [id, m] of this.marks)
            if (!alive.has(id)) {
                m.boat.remove();
                m.who.remove();
                this.marks.delete(id);
            }

        // Сид отдаёт начало тому, кто догоняет, — пунктир между причалами.
        const yOf = (r) => ys[shown.indexOf(r)];
        const seed = shown.find((r) => r.role === 'seed');
        const late = shown.find((r) => r.role === 'catching');
        if (seed && late) {
            const down = yOf(late) > yOf(seed);
            const a = yOf(seed) + (down ? 16 : -16);
            const b = yOf(late) + (down ? -14 : 14);
            const mid = (a + b) / 2;
            parts.push(svg('path', { class: 'fx-flow-slow', d: `M1452 ${a} Q1372 ${mid} 1452 ${b}`, fill: 'none',
                stroke: '#35b3a0', 'stroke-width': 2, 'stroke-dasharray': '6 6' }));
            const t = svg('text', { x: 1372, y: mid + 4, 'text-anchor': 'end', 'font-family': 'IBM Plex Sans, sans-serif',
                'font-style': 'italic', 'font-size': 12, fill: '#5ecdbb' });
            t.textContent = `сид → ${nameOf(late)}`;
            parts.push(t);
        }
        if (list.length > shown.length) {
            const t = svg('text', { x: 1450, y: 560, 'text-anchor': 'end', 'font-family': 'IBM Plex Mono, monospace',
                'font-size': 13, fill: '#a8b7c2' });
            t.textContent = `и ещё ${grp(list.length - shown.length)} — в таблице`;
            parts.push(t);
        }
        g.replaceChildren(...parts);
        this.renderHead();
    }

    renderRows() {
        const rows = $('#sx-rows');
        const n = this.receivers.length;
        const seats = (this.cond && this.cond.maxConcurrent) || (this.policy && this.policy.maxConcurrent) || 8;
        setText('sxSeats', `${grp(n)} из ${grp(seats)} ${plural(seats, 'места', 'мест', 'мест')}`);
        const ended = this.phase === 'closed' || this.phase === 'failed';
        $('#sx-none').hidden = n > 0;
        setText('sxNone', ended ? 'Раздача закрыта.'
            : this.phase === 'live' ? 'Пока никого. Пришлите ссылку — получатель откроет её в браузере или заберёт командой ferry get.'
                : 'Получатели появятся, как только будет ссылка.');
        const alive = new Set();
        this.receivers.forEach((r, i) => {
            alive.add(r.id);
            const p = Math.max(0, Math.min(1, Number(r.progress) || 0));
            const [roleText, roleCls] = ROLE[r.role] || ROLE.leech;
            const s = this.rx.get(r.id);
            const rate = s ? s.rate : 0;
            let e = this.rowEls.get(r.id);
            if (!e) {
                e = { row: el('div', 'sx-trow'), who: el('span', 'who'), lane: el('div', 'sx-lane'), fill: el('i'),
                    pc: el('span', 'num'), rate: el('span', 'num dim hide-sm'), eta: el('span', 'num dim hide-sm'),
                    rl: el('span', 'rl'), kind: '' };
                e.lane.append(e.fill);
                const cell = el('div', 'lane-cell');
                cell.append(e.lane);
                e.row.append(e.who, cell, e.pc, e.rate, e.eta, e.rl);
                this.rowEls.set(r.id, e);
            }
            if (rows.children[i] !== e.row)
                rows.insertBefore(e.row, rows.children[i] || null);
            e.who.textContent = nameOf(r);
            e.who.title = nameOf(r);
            if (e.kind !== roleCls) {
                e.kind = roleCls;
                e.lane.className = `sx-lane ${roleCls}`;
                if (e.boat)
                    e.boat.remove();
                e.lane.insertAdjacentHTML('beforeend', MINI_BOAT(roleCls === 'leech' ? '#ff6a1f' : '#35b3a0'));
                e.boat = e.lane.lastElementChild;
            }
            e.fill.style.width = pct(p);
            e.boat.style.left = pct(p);
            e.pc.textContent = `${comma(p * 100, 0)}${NB}%`;
            e.rate.textContent = rate > 0 ? `${mbps(rate)}${NB}МБ/с` : '—';
            e.eta.textContent = p >= 1 ? '—' : rate > 0 ? dur(((1 - p) * this.total * 1000) / rate) : 'считаю…';
            e.rl.className = `rl ${roleCls}`;
            e.rl.replaceChildren(el('i'), roleText);
        });
        for (const [id, e] of this.rowEls)
            if (!alive.has(id)) {
                e.row.remove();
                this.rowEls.delete(id);
            }
    }

    renderMap() {
        const map = $('#sx-map');
        const n = Math.min(MAP_CELLS, this.count);
        if (map.children.length !== n) {
            map.replaceChildren(...Array.from({ length: n }, () => el('i')));
            const ticks = $('#sx-ticks');
            const u = unitOf(this.total);
            ticks.replaceChildren(...Array.from({ length: 7 }, (_, i) => {
                const s = el('span', i % 2 ? 'hide-sm' : '');
                s.textContent = i === 0 ? '0' : inUnit((this.total * i) / 6, u) + NB + unitName(u);
                return s;
            }));
        }
        const cells = this.prog && this.prog.cells;
        if (cells)
            Array.from(map.children).forEach((c, i) => {
                const v = cells[i] | 0;
                const cls = v === 2 ? 'c2' : v === 1 ? 'c1' : '';
                if (c.className !== cls)
                    c.className = cls;
            });
        const sent = this.prog ? this.prog.distinctBytes : 0;
        setText('sxMapAside', `отдано релею ${size(sent)} из ${size(this.total)}`);
    }

    renderMapCursors() {
        const box = $('#sx-cursors');
        const leech = this.receivers.filter((r) => r.role === 'leech' && r.progress < 1).slice(0, 6);
        box.replaceChildren(...leech.map((r) => {
            const x = Math.max(0, Math.min(1, Number(r.progress) || 0));
            const c = el('div', 'sx-cur' + (x < 0.07 ? ' l' : x > 0.93 ? ' r' : ''));
            c.style.left = pct(x);
            c.append(el('span', '', nameOf(r)), el('i'));
            return c;
        }));
    }

    renderCond() {
        const box = $('#sx-cond');
        const parts = [];
        const part = (num, unit) => {
            const s = el('span');
            s.append(num);
            if (unit)
                s.append(NB, el('span', 'u', unit));
            return s;
        };
        const c = this.cond;
        const usesLeft = c ? c.usesLeft : (this.policy && this.policy.uses > 0 ? this.policy.uses : -1);
        if (usesLeft < 0)
            parts.push(part('без лимита', 'использований'));
        else if (usesLeft === 0)
            parts.push(part('использований', 'не осталось'));
        else
            parts.push(part(`осталось ${grp(usesLeft)}`, plural(usesLeft, 'использование', 'использования', 'использований')));
        const left = this.timeLeft();
        if (this.phase === 'live')
            parts.push(part(`истекает через ${dur(left)}`));
        else if (this.phase === 'preparing' && !this.manage)
            parts.push(part(`срок ${dur(left)}`));
        const seats = (c && c.maxConcurrent) || (this.policy && this.policy.maxConcurrent) || 8;
        parts.push(part(`не более ${grp(seats)}`, 'одновременно'));
        if (c && c.requireApproval)
            parts.push(part('вход', 'с вашего разрешения'));
        if (this.finished.size)
            parts.push(part(`забрали целиком: ${grp(this.finished.size)}`));
        const out = [];
        parts.forEach((p, i) => {
            if (i)
                out.push(' · ');
            out.push(p);
        });
        box.replaceChildren(...out);

        const seeds = this.liveSeeds();
        let note = 'Раздача идёт, пока открыта эта вкладка.';
        if (this.phase === 'closed' || this.phase === 'failed')
            note = this.manage ? 'Раздача закрыта или управляют ею не отсюда.' : 'Раздача закрыта. На релее от неё ничего не осталось.';
        else if (this.manage)
            note = 'Отправителя нет: том отдают те, кто скачал его целиком, пока хоть один из них на связи. Эту вкладку можно закрыть — раздача не остановится.';
        else if (usesLeft === 0)
            note = 'Новые получатели не войдут. Кто качает сейчас — докачает, пока открыта эта вкладка.';
        else if (c && c.leaveOpen)
            note = seeds
                ? `Закроете вкладку — раздачу продолжат те, кто скачал том целиком (сейчас ${grp(seeds)}).`
                : 'Закроете вкладку — раздачу продолжат те, кто скачал том целиком. Пока таких нет: закрыть сейчас значит закрыть раздачу.';
        setText('sxFootNote', note);
    }
}

// ---------------------------------------------------------------- связки

let wired = false;

function wire() {
    if (wired)
        return;
    wired = true;
    $('#sx-copy').addEventListener('click', (e) => {
        if (V && V.link)
            copy(V.fullLink(), e.currentTarget);
    });
    $('#sx-copy-owner').addEventListener('click', (e) => {
        if (V && V.ownerLink())
            copy(V.ownerLink(), e.currentTarget);
    });
    $('#sx-pw-show').addEventListener('click', (e) => {
        if (!V)
            return;
        const on = e.currentTarget.getAttribute('aria-pressed') !== 'true';
        e.currentTarget.setAttribute('aria-pressed', String(on));
        e.currentTarget.textContent = on ? 'Скрыть' : 'Показать пароль';
        setText('sxPwText', on ? V.passwordShown : '••••••••••••');
    });
    $('#sx-allow-all').addEventListener('click', () => {
        if (V)
            V.owner({ type: 'approve', ok: true, all: true });
    });
    $('#sx-sw-approve').addEventListener('click', () => {
        if (V && V.cond)
            V.owner({ type: 'policy', require_approval: !V.cond.requireApproval });
    });
    $('#sx-sw-leave').addEventListener('click', () => {
        if (!V || !V.cond)
            return;
        const on = !V.cond.leaveOpen;
        V.owner(on ? { type: 'policy', leave_open: true, on_owner_leave: 'freeze' } : { type: 'policy', leave_open: false });
    });
    // Продлить — на сутки от того, что осталось, но не дальше недели от
    // сейчас: дальше релей всё равно не даст.
    $('#sx-extend').addEventListener('click', () => {
        if (V)
            V.owner({ type: 'policy', ttl_ms: Math.min(V.timeLeft() + DAY_MS, MAX_TTL_MS) });
    });
    $('#sx-more').addEventListener('click', () => {
        if (V && V.cond && V.cond.usesLeft >= 0)
            V.owner({ type: 'policy', uses: V.cond.usesLeft + MORE_USES });
    });
    // Ссылка владельца отличается от главной только фрагментом: браузер
    // сменит адрес, но страницу не перезагрузит, а home.js на hashchange
    // перезагружает лишь пустую главную. Здесь лист раздачи открыт — значит
    // перезагрузить нужно самим.
    $('#sx-manage').addEventListener('click', () => {
        const link = V && V.ownerLink();
        if (!link)
            return;
        location.assign(link);
        if (location.href === new URL(link).href)
            location.reload();
    });
    $('#sx-copy-bare').addEventListener('click', (e) => {
        if (!V || !V.link)
            return;
        copy(V.link, e.currentTarget);
        $('#sx-split').hidden = false;
    });
    $('#sx-copy-key').addEventListener('click', (e) => {
        if (V && V.key)
            copy(V.key, e.currentTarget);
    });
    $('#sx-close').addEventListener('click', () => {
        if (!V || V.phase !== 'live')
            return;
        V.confirming = true;
        const n = V.receivers.filter((r) => r.progress < 1).length;
        setText('sxConfirm', n
            ? `Закрыть? ${n === 1 ? 'Один получатель ещё качает и не докачает' : `${grp(n)} ${plural(n, 'получатель', 'получателя', 'получателей')} ещё качают и не докачают`}.`
            : 'Закрыть? Ссылка перестанет работать.');
        V.renderHead();
        $('#sx-close-no').focus();
    });
    $('#sx-close-no').addEventListener('click', () => {
        if (!V)
            return;
        V.confirming = false;
        V.renderHead();
    });
    $('#sx-close-yes').addEventListener('click', () => {
        if (!V)
            return;
        V.confirming = false;
        V.stop(true);
        try {
            sessionStorage.removeItem(OWNED_KEY);
        } catch {
            // нечего забывать
        }
    });
    $('#sx-again').addEventListener('click', () => {
        if (!V)
            return;
        const { cargo, policy, hooks } = V;
        startSending(cargo, policy, hooks);
    });
    $('#sx-new').addEventListener('click', () => {
        if (!V)
            return;
        const { hooks } = V;
        V.stop();
        V = null;
        hooks.onNew();
    });
    // Закрыть вкладку — закрыть раздачу: файлы читаются из неё и больше
    // ниоткуда. Спрашиваем, как спрашивает любой загрузчик.
    // С «работать без меня» и хотя бы одним сидом на связи уход ничего не
    // ломает — тогда не спрашиваем. Владельцу без тома уходить можно всегда.
    // Вкладка закрывается (вопрос выше уже задан и принят): сказать релею,
    // что это уход, а не обрыв.
    window.addEventListener('pagehide', () => {
        if (V && !V.manage && V.phase === 'live')
            V.leaveBeacon();
    });
    window.addEventListener('beforeunload', (e) => {
        const free = V && (V.manage || (V.cond && V.cond.leaveOpen && V.liveSeeds() > 0));
        if (V && !free && (V.phase === 'live' || V.phase === 'preparing')) {
            e.preventDefault();
            e.returnValue = '';
        }
    });
}

function resetDom() {
    $('#sx-end').hidden = true;
    $('#sx-split').hidden = true;
    $('#sx-again').hidden = false;
    $('#sx-manage').hidden = true;
    $('#sx-knocks').hidden = true;
    $('#sx-knock-list').replaceChildren();
    $('#sx-owner').hidden = true;
    $('#sx-pw').hidden = true;
    $('#sx-pol').hidden = true;
    $('#sx-extend').hidden = true;
    $('#sx-more').hidden = true;
    $('#sx-pw-show').setAttribute('aria-pressed', 'false');
    $('#sx-pw-show').textContent = 'Показать пароль';
    setText('sxPwText', '••••••••••••');
    setText('sxLink', '');
    setText('sxKey', '');
    $('#sx-rows').replaceChildren();
    $('#sx-cursors').replaceChildren();
    $('#sx-map').replaceChildren();
    $('#sx-routes').replaceChildren();
    $('#sx-marks').replaceChildren();
}

export function startSending(cargo, policy, hooks) {
    wire();
    if (V)
        V.stop();
    resetDom();
    V = new View(cargo, policy, hooks);
    V.start();
    window.scrollTo(0, 0);
    return V;
}

// m — разобранная ссылка владельца (parseManageLink).
export function startManaging(m, hooks) {
    wire();
    if (V)
        V.stop();
    resetDom();
    V = new View(null, null, hooks, m);
    V.start();
    window.scrollTo(0, 0);
    return V;
}
