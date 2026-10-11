// Главная: релей, погрузка и условия выдачи.
//
// Груз собирается здесь, в странице: объекты File нельзя передать в
// другую вкладку или пережить переход по адресу. Поэтому раздача идёт
// в этой же странице — главную сменяет накладная (sendview.js), а файлы
// читаются с диска по мере того, как их просит релей.

import { parseLink } from './api.js';
import { cryptoAvailable } from './crypto.js';
import { cargoFromFileList, cargoFromDrop, dropEntries, CargoError } from './cargo.js';
import { chunkSizeFor } from './manifest.js';
import { grp, comma, plural, size, files, copy } from './ui.js';
import { startSending } from './sendview.js';

const $ = (sel) => document.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

function setText(name, value) {
    const v = String(value);
    for (const el of $$(`[data-t="${name}"]`))
        if (el.textContent !== v)
            el.textContent = v;
}

const H = {
    cargo: null,
    policy: { uses: 0, ttlMs: 86400000, maxConcurrent: 8 },
    canSend: true,
    os: /Windows/i.test(navigator.userAgent) ? 'win' : 'linux',
};

// ---------------------------------------------------------------- релей

async function checkRelay() {
    const pill = $('#relay-pill');
    try {
        const r = await fetch('/api/health', { cache: 'no-store', credentials: 'omit' });
        if (!r.ok)
            throw new Error(String(r.status));
        await r.json();
        pill.classList.add('ok');
        setText('relayText', `релей на связи · ${location.host}`);
    } catch {
        pill.classList.add('bad');
        setText('relayText', `релей не отвечает · ${location.host}`);
    }
}

// ---------------------------------------------------------------- клиент

// Адрес подставляем на месте: страница статическая, а релей открывают и
// по домену, и по IP, и на нестандартном порту. location.origin несёт
// порт, если он есть, — именно это и нужно.
function renderInstall() {
    const base = location.origin;
    const win = H.os === 'win';
    setText('instPrompt', win ? 'PS>' : '$');
    setText('instCmd', win ? `irm ${base}/install.ps1 | iex` : `curl -fsSL ${base}/install.sh | sh`);
    setText('instCmdK', win
        ? "Add-Type 'using System.Net;public class FerryTls{public static void Ok()"
          + "{ServicePointManager.ServerCertificateValidationCallback=delegate{return true;};}}'\n"
          + "[FerryTls]::Ok(); $env:FERRY_INSECURE = '1'\n"
          + `irm ${base}/install.ps1 | iex`
        : `curl -fsSLk ${base}/install.sh | sh -s -- --insecure`);
    for (const el of $$('[data-os-only]'))
        el.hidden = el.dataset.osOnly !== H.os;
    for (const b of $$('.ostabs button'))
        b.setAttribute('aria-pressed', String(b.dataset.os === H.os));
}

// Судовой журнал на главной — пример вывода ferry send, а не живые
// данные: числа просто ползут, чтобы было видно, что это поток.
function startLog() {
    setText('logLink', `${location.origin}/t/n7Qk2pLm0vXsR7dTnWcAqB`);
    const still = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    let t = 0;
    const bar = (p) => '█'.repeat(Math.round(p * 16)) + '░'.repeat(16 - Math.round(p * 16));
    const pc = (p) => String(Math.round(p * 100)).padStart(2, ' ') + ' %';
    const tick = () => {
        const p1 = Math.min(0.99, 0.62 + t * 0.004);
        const p3 = Math.min(0.99, 0.31 + t * 0.003);
        setText('logSent', comma(7.7 + Math.min(4.6, t * 0.06), 1));
        setText('logRate', comma(112 + ((t * 37) % 13) - 6 + 0.4, 1));
        setText('logBar1', bar(p1));
        setText('logP1', pc(p1));
        setText('logBar3', bar(p3));
        setText('logP3', pc(p3));
        t++;
    };
    tick();
    if (!still)
        setInterval(tick, 1000);
}

// ---------------------------------------------------------------- погрузка

function cardState(state) {
    const card = $('#load');
    card.dataset.state = state;
    for (const b of $$('[data-card]', card))
        b.hidden = b.dataset.card !== state;
    setText('loadTag', state === 'cargo' ? 'груз на причале' : state === 'busy' ? 'принимаю' : 'ждёт груз');
}

function note(text) {
    const n = $('#drop-note');
    n.textContent = text || '';
    n.hidden = !text;
}

function kv(k, v, cls) {
    const row = document.createElement('div');
    row.className = 'kv';
    const a = document.createElement('span');
    a.className = 'k';
    a.textContent = k;
    const d = document.createElement('span');
    d.className = 'dots';
    const b = document.createElement('span');
    b.className = 'v' + (cls ? ' ' + cls : '');
    b.textContent = v;
    row.append(a, d, b);
    return row;
}

function showCargo(cargo) {
    if (cargo.total === 0)
        throw new CargoError(cargo.manifest.kind === 'file'
            ? 'Файл пустой — возить нечего.'
            : 'Том пустой: во всех файлах ноль байт — возить нечего.');
    H.cargo = cargo;
    const m = cargo.manifest;
    const chunk = chunkSizeFor(cargo.total);
    const count = Math.ceil(cargo.total / chunk);
    setText('cargoKind', m.kind === 'file' ? 'файл' : cargo.loose ? 'несколько файлов — одним томом' : 'папка');
    setText('cargoName', m.name);

    const rows = [];
    if (m.kind === 'tree') {
        let comp = files(cargo.fileCount);
        if (cargo.emptyDirs)
            comp += ` · ${grp(cargo.emptyDirs)} ${plural(cargo.emptyDirs, 'пустая папка', 'пустые папки', 'пустых папок')}`;
        rows.push(kv('состав', comp));
    }
    rows.push(kv('размер', size(cargo.total)));
    rows.push(kv('чанки', `${grp(count)} по ${size(chunk)}`));
    $('#cargo-kvs').replaceChildren(...rows);

    const sk = cargo.skipped;
    $('#cargo-skipped').hidden = sk.length === 0;
    if (sk.length) {
        setText('skippedText', `Не поедут ${grp(sk.length)} ${plural(sk.length, 'файл', 'файла', 'файлов')}.`);
        $('#skipped-list').replaceChildren(...sk.slice(0, 500).map((s) => {
            const row = document.createElement('div');
            row.className = 'frow';
            const p = document.createElement('span');
            p.className = 'p';
            p.textContent = s.path;
            p.title = s.path;
            const w = document.createElement('span');
            w.className = 's';
            w.textContent = s.why;
            row.append(p, w);
            return row;
        }));
    }
    note('');
    cardState('cargo');
    $('#send-go').focus({ preventScroll: true });
}

function fail(e) {
    cardState('drop');
    if (e instanceof CargoError)
        note(e.message);
    else
        note(`Не получилось прочитать то, что выбрали: ${(e && e.message) || e}`);
}

function fromList(list) {
    if (!list || list.length === 0)
        return;
    try {
        showCargo(cargoFromFileList(list));
    } catch (e) {
        fail(e);
    }
}

async function fromDrop(drop) {
    cardState('busy');
    setText('busyText', 'Читаю опись…');
    try {
        showCargo(await cargoFromDrop(drop));
    } catch (e) {
        fail(e);
    }
}

function resetCargo() {
    H.cargo = null;
    $('#in-files').value = '';
    $('#in-dir').value = '';
    note('');
    cardState('drop');
}

// Отправлять из браузера можно только там, где есть WebCrypto и модульные
// воркеры. Что не так — говорим сразу, а не после выбора файла на 40 ГБ.
function checkCapabilities() {
    let why = '';
    if (!window.isSecureContext || !cryptoAvailable())
        why = 'Отправлять из браузера можно только со страницы по https: без неё браузер не даёт шифрования. Откройте релей по https или отправьте из терминала — команда ниже.';
    else if (typeof Worker !== 'function' || typeof MessageChannel !== 'function')
        why = 'Этот браузер не умеет отправлять том: нужны модульные воркеры. Они есть в свежих Chrome, Edge, Firefox и Safari.';
    if (!why)
        return;
    H.canSend = false;
    note(why);
    for (const b of [$('#pick-files'), $('#pick-dir')])
        b.disabled = true;
}

function setSeg(group, value) {
    for (const b of $$(`[data-seg="${group}"] button`))
        b.setAttribute('aria-pressed', String(b.dataset.v === String(value)));
}

function polHint() {
    const u = H.policy.uses;
    setText('polHint', u === 1
        ? 'Ссылка сгорит на первом получателе. Использование тратит тот, кто доказал владение ключом, — превью-бот мессенджера его не сжигает.'
        : 'Использование тратит тот, кто доказал владение ключом. Превью-бот мессенджера его не сжигает.');
}

// ---------------------------------------------------------------- раздача

function showHome() {
    $('#send').hidden = true;
    $('#home').hidden = false;
    document.title = 'Ferry — паром для файлов';
    window.scrollTo(0, 0);
}

function go() {
    if (!H.cargo || !H.canSend)
        return;
    $('#home').hidden = true;
    $('#send').hidden = false;
    window.scrollTo(0, 0);
    startSending(H.cargo, { ...H.policy }, {
        onNew() {
            resetCargo();
            showHome();
        },
    });
}

// ---------------------------------------------------------------- связки

function wire() {
    $('#pick-files').addEventListener('click', () => $('#in-files').click());
    $('#pick-dir').addEventListener('click', () => $('#in-dir').click());
    $('#in-files').addEventListener('change', (e) => fromList(e.target.files));
    $('#in-dir').addEventListener('change', (e) => fromList(e.target.files));

    // Бросить груз можно куда угодно на главной: промах мимо причала не
    // должен открывать файл в браузере вместо отправки.
    const drop = $('#drop');
    let depth = 0;
    const accepting = () => H.canSend && !$('#home').hidden && $('#load').dataset.state !== 'busy';
    window.addEventListener('dragenter', (e) => {
        if (!e.dataTransfer || !Array.from(e.dataTransfer.types || []).includes('Files'))
            return;
        e.preventDefault();
        depth++;
        if (accepting())
            drop.classList.add('is-over');
    });
    window.addEventListener('dragleave', () => {
        depth = Math.max(0, depth - 1);
        if (depth === 0)
            drop.classList.remove('is-over');
    });
    window.addEventListener('dragover', (e) => {
        e.preventDefault();
        if (e.dataTransfer)
            e.dataTransfer.dropEffect = accepting() ? 'copy' : 'none';
    });
    window.addEventListener('drop', (e) => {
        e.preventDefault();
        depth = 0;
        drop.classList.remove('is-over');
        if (!accepting() || !e.dataTransfer)
            return;
        // Записи достаём синхронно: после первого await список пуст.
        const entries = dropEntries(e.dataTransfer);
        if ($('#load').dataset.state === 'cargo')
            resetCargo();
        $('#load').scrollIntoView({ block: 'nearest', behavior: 'smooth' });
        fromDrop(entries);
    });

    for (const b of $$('[data-seg] button'))
        b.addEventListener('click', () => {
            const group = b.closest('[data-seg]').dataset.seg;
            const v = Number(b.dataset.v);
            if (group === 'uses')
                H.policy.uses = v;
            else if (group === 'ttl')
                H.policy.ttlMs = v;
            setSeg(group, v);
            polHint();
        });
    $('#send-go').addEventListener('click', go);
    $('#cargo-reset').addEventListener('click', resetCargo);

    $('#get-form').addEventListener('submit', (e) => {
        e.preventDefault();
        const input = $('#get-in');
        const v = input.value.trim();
        const l = parseLink(v);
        const hint = $('#get-hint');
        if (!l) {
            hint.textContent = 'Это не похоже на ссылку Ferry: нужен адрес вида https://…/t/…#ключ.';
            hint.style.color = 'var(--warn-text)';
            return;
        }
        // Ссылка без ключа тоже открывается: страница приёма сама спросит
        // ключ, если его прислали отдельно.
        location.href = v;
    });
    $('#get-in').addEventListener('input', () => {
        const hint = $('#get-hint');
        hint.textContent = 'Ключ — часть ссылки после #. На сервер она не уходит: её нет ни в запросе, ни в логах.';
        hint.style.color = '';
    });

    for (const b of $$('.ostabs button'))
        b.addEventListener('click', () => {
            H.os = b.dataset.os;
            renderInstall();
        });
    $('#inst-copy').addEventListener('click', (e) => {
        const cmd = $('[data-t="instCmd"]').textContent;
        copy(cmd, e.currentTarget);
    });
}

function boot() {
    wire();
    renderInstall();
    // Блок про самоподписанный сертификат — только там, где он уместен: по
    // http или по адресу-числу настоящего сертификата не бывает.
    const looksLikeIp = /^\d{1,3}(\.\d{1,3}){3}$/.test(location.hostname) || location.hostname.startsWith('[');
    $('#selfsigned').hidden = !(location.protocol !== 'https:' || looksLikeIp);
    checkCapabilities();
    startLog();
    checkRelay();
    polHint();
    setText('host', location.host);
}

boot();
