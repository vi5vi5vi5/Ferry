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

import { fetchMeta } from './api.js';
import { chunkSizeFor } from './manifest.js';
import { encodeQr, qrPath } from './qr.js';
import { NB, grp, comma, plural, unitOf, inUnit, unitName, size, dur, pct, mbps, copy } from './ui.js';

const $ = (sel) => document.querySelector(sel);
const SVG = 'http://www.w3.org/2000/svg';

const MAX_ROUTES = 6;
const META_EVERY_MS = 5000;
const MAP_CELLS = 120;

const PREP = {
    creating: ['Заводим раздачу на релее…',
        'Ключ рождается здесь, в этой вкладке. На релей уходит только проверочный отпечаток, из которого ключ не вывести.'],
    hashing: ['Считаем хеши до ссылки…',
        'Этот релей не умеет хеши на лету, поэтому ссылка появится, когда весь том будет посчитан.'],
    offering: ['Отдаём релею опись…',
        'Опись зашифрована тем же ключом: релей не видит ни имён, ни состава тома.'],
};

const ENDS = {
    expired: ['Срок раздачи вышел', 'Релей закрыл раздачу по сроку жизни. Ссылка больше не работает; тот же груз можно раздать заново — с новым ключом.', 'warn', 'срок вышел'],
    network: ['Связь с релеем оборвалась', 'Пропала сеть или релей перезапустили. Без связи с отправителем раздача не живёт: ссылка больше не открывается, и кто не докачал, начнёт заново по новой.', 'danger', 'связь потеряна'],
    server_busy: ['Релей сейчас занят', 'Упёрлись в потолок раздач или памяти на релее. Попробуйте чуть позже.', 'warn', 'релей занят'],
    owner_conflict: ['Релей не признал раздачу', 'Токен владельца не подошёл — так бывает, если черновик раздачи истёк. Начните заново.', 'danger', 'отказ'],
    read: ['Файл изменился или пропал', 'Чтение с диска не удалось, и отдавать дальше нечего. Выберите груз заново.', 'danger', 'груз не читается'],
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
    constructor(cargo, policy, hooks) {
        this.cargo = cargo;
        this.policy = policy;
        this.hooks = hooks;
        this.total = cargo.total;
        this.chunkSize = chunkSizeFor(cargo.total);
        this.count = Math.ceil(this.total / this.chunkSize);
        this.phase = 'preparing';
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
        const { cargo } = this;
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
        worker.postMessage({ cmd: 'start', hashPort: ch.port2, files: cargo.files, manifest: cargo.manifest,
            chunkSize: this.chunkSize, policy: this.policy, origin: location.origin }, [ch.port2]);
        this.timers.push(setInterval(() => this.tick(), 1000));
        this.renderAll();
    }

    stop() {
        if (this.worker && (this.phase === 'live' || this.phase === 'preparing')) {
            this.worker.postMessage({ cmd: 'stop' });
            this.end('closed');
        } else {
            this.dispose();
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

    async pollMeta() {
        if (this.phase !== 'live' || !this.id)
            return;
        try {
            const m = await fetchMeta(location.origin, this.id);
            if (this.phase !== 'live')
                return;
            this.meta = m;
            this.metaAt = performance.now();
            this.renderCond();
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
            const e = ENDS[code] || DEFAULT_END;
            // У чтения своя причина уже в тексте; у остальных подробность
            // из воркера помогает понять, что именно случилось.
            const detail = ENDS[code] ? (message && message !== code && code !== 'read' ? ` (${message})` : '')
                : ` Код: ${code}${message && message !== code ? `, ${message}` : ''}.`;
            setText('sxEndTitle', e[0]);
            setText('sxEndText', e[1] + detail);
            box.classList.add(e[2] === 'warn' ? 'banner' : 'banner--danger');
        }
        $('#sx-again').hidden = code === 'read';
        this.renderAll();
        box.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
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
    }

    renderHead() {
        const stamp = $('#sx-stamp');
        stamp.className = 'stamp';
        let a = 'готовим ссылку';
        let b = 'груз на причале';
        if (this.phase === 'live') {
            a = 'раздача идёт';
            b = this.receivers.length ? 'принято к перевозке' : 'ждём получателей';
        } else if (this.phase === 'closed') {
            stamp.classList.add('stamp--wait');
            a = 'доступ закрыт';
            b = 'по команде отправителя';
        } else if (this.phase === 'failed') {
            const e = ENDS[this.code] || DEFAULT_END;
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
        document.title = `${ended ? 'Раздача закрыта' : 'Раздача'} · ${this.cargo.manifest.name} — Ferry`;
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

    renderLink() {
        setText('sxLink', this.link);
        setText('sxKey', '#' + this.key);
        setText('sxKeyOnly', this.key);
        const qr = encodeQr(`${this.link}#${this.key}`);
        const box = $('#sx-qr');
        box.setAttribute('viewBox', `-2 -2 ${qr.size + 4} ${qr.size + 4}`);
        box.querySelector('path').setAttribute('d', qrPath(qr));
        this.renderHead();
        this.renderPrep();
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
        const seats = this.policy.maxConcurrent || 8;
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
        const m = this.meta;
        const usesLeft = m ? m.usesLeft : (this.policy.uses > 0 ? this.policy.uses : -1);
        if (usesLeft < 0)
            parts.push(part('без лимита', 'использований'));
        else if (usesLeft === 0)
            parts.push(part('использований', 'не осталось'));
        else
            parts.push(part(`осталось ${grp(usesLeft)}`, plural(usesLeft, 'использование', 'использования', 'использований')));
        let left = this.policy.ttlMs;
        if (m)
            left = Math.max(0, m.expiresInMs - (performance.now() - this.metaAt));
        if (this.phase === 'live')
            parts.push(part(`истекает через ${dur(left)}`));
        else if (this.phase === 'preparing')
            parts.push(part(`срок ${dur(left)}`));
        parts.push(part(`не более ${grp(this.policy.maxConcurrent || 8)}`, 'одновременно'));
        if (this.finished.size)
            parts.push(part(`забрали целиком: ${grp(this.finished.size)}`));
        const out = [];
        parts.forEach((p, i) => {
            if (i)
                out.push(' · ');
            out.push(p);
        });
        box.replaceChildren(...out);

        let note = 'Раздача идёт, пока открыта эта вкладка.';
        if (this.phase === 'closed' || this.phase === 'failed')
            note = 'Раздача закрыта. На релее от неё ничего не осталось.';
        else if (usesLeft === 0)
            note = 'Новые получатели не войдут. Кто качает сейчас — докачает, пока открыта эта вкладка.';
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
            copy(`${V.link}#${V.key}`, e.currentTarget);
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
        V.stop();
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
    window.addEventListener('beforeunload', (e) => {
        if (V && (V.phase === 'live' || V.phase === 'preparing')) {
            e.preventDefault();
            e.returnValue = '';
        }
    });
}

function resetDom() {
    $('#sx-end').hidden = true;
    $('#sx-split').hidden = true;
    $('#sx-again').hidden = false;
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
