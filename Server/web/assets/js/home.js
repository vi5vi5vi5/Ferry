// Главная: релей, погрузка и условия выдачи.
//
// Груз собирается здесь, в странице: объекты File нельзя передать в
// другую вкладку или пережить переход по адресу. Поэтому раздача идёт
// в этой же странице — главную сменяет накладная (sendview.js), а файлы
// читаются с диска по мере того, как их просит релей.

import { parseLink, joinLinkKey, fetchMeta } from './api.js';
import { cryptoAvailable } from './crypto.js';
import { cargoFromFileList, cargoFromDrop, dropEntries, CargoError } from './cargo.js';
import { chunkSizeFor } from './manifest.js';
import { normalize as normalizePassword, problem as passwordProblem } from './password.js';
import { grp, comma, plural, size, files, copy } from './ui.js';
import { startSending, startManaging, parseManageLink, OWNED_KEY } from './sendview.js';

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
    policy: { uses: 0, ttlMs: 86400000, maxConcurrent: 8, requireApproval: false, leaveOpen: false },
    password: false,
    features: [],
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
        const health = await r.json();
        H.features = Array.isArray(health.features) ? health.features : [];
        pill.classList.add('ok');
        setText('relayText', `релей на связи · ${location.host}`);
        // Подтверждение, «без меня» и пароль — только там, где релей их
        // знает: старый молча пропустил бы условия мимо ушей.
        $('#acc').hidden = !H.features.includes('policy');
        $('#pw-row').hidden = !H.features.includes('password');
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
    const p = H.policy;
    setText('leaveText', p.requireApproval && p.leaveOpen
        ? 'Закроете вкладку — раздачу продолжат те, кто уже скачал том целиком, пока хоть один из них на связи. Спрашивать будет некого, поэтому дальше войдёт любой с ключом, в пределах использований. Вернуться к управлению можно по ссылке владельца.'
        : 'Закроете вкладку — раздачу продолжат те, кто уже скачал том целиком, пока хоть один из них на связи. Если таких нет, ссылка погаснет. Вернуться к управлению можно по ссылке владельца.');
    setText('sendFoot', p.leaveOpen
        ? 'Пока открыта эта вкладка, файлы читаются прямо с вашего диска. Без вас раздача живёт, пока на связи хоть один, кто скачал том целиком.'
        : 'Раздача идёт, пока открыта эта вкладка: файлы читаются прямо с вашего диска.');
}

function setSwitch(id, on) {
    $(id).setAttribute('aria-checked', String(on));
}

// Пароль проверяется на лету: кнопка «Отправить» ждёт, пока фраза не
// станет годной. Подсказка говорит, чего не хватает, а не просто «плохо».
function passwordState() {
    if (!H.password)
        return { ok: true, phrase: '' };
    const phrase = normalizePassword($('#pw-new').value);
    const why = phrase ? passwordProblem(phrase) : 'Придумайте фразу хотя бы из пяти слов.';
    return { ok: !why, phrase, why };
}

function renderPassword() {
    $('#pw-box').hidden = !H.password;
    setSwitch('#sw-pw', H.password);
    const st = passwordState();
    const input = $('#pw-new');
    const typed = input.value.trim() !== '';
    input.setAttribute('aria-invalid', String(H.password && typed && !st.ok));
    const hint = $('#pw-hint');
    hint.classList.toggle('caption--warn', H.password && typed && !st.ok);
    setText('pwHint', !H.password || st.ok
        ? 'Регистр, лишние пробелы и «ё» не важны. Пять слов, которые не встречаются вместе, — надёжнее, чем одно сложное.'
        : st.why);
    $('#send-go').disabled = !st.ok;
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
    const pw = passwordState();
    if (!pw.ok) {
        $('#pw-new').focus();
        return;
    }
    const policy = { ...H.policy };
    if (!H.features.includes('policy')) {
        policy.requireApproval = false;
        policy.leaveOpen = false;
    }
    if (H.password && H.features.includes('password')) {
        policy.password = pw.phrase;
        // Показывать на накладной — как набрал человек: выровненную
        // строчными без «ё» он бы не узнал.
        policy.passwordShown = $('#pw-new').value.trim().replace(/\s+/g, ' ');
    }
    $('#home').hidden = true;
    $('#send').hidden = false;
    window.scrollTo(0, 0);
    startSending(H.cargo, policy, {
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
    $('#sw-approve').addEventListener('click', () => {
        H.policy.requireApproval = !H.policy.requireApproval;
        setSwitch('#sw-approve', H.policy.requireApproval);
        polHint();
    });
    $('#sw-leave').addEventListener('click', () => {
        H.policy.leaveOpen = !H.policy.leaveOpen;
        setSwitch('#sw-leave', H.policy.leaveOpen);
        polHint();
    });
    $('#sw-pw').addEventListener('click', () => {
        H.password = !H.password;
        renderPassword();
        if (H.password)
            $('#pw-new').focus();
    });
    $('#pw-new').addEventListener('input', renderPassword);
    $('#pw-new').addEventListener('keydown', (e) => {
        if (e.key === 'Enter')
            go();
    });

    $('#get-form').addEventListener('submit', (e) => {
        e.preventDefault();
        const input = $('#get-in');
        const v = joinLinkKey(input.value);
        const hint = $('#get-hint');
        // Ссылку владельца тоже можно вставить сюда: она ведёт на эту же
        // страницу, только с другим фрагментом.
        if (parseManageLink(v)) {
            location.href = v;
            if (new URL(v).pathname === location.pathname)
                location.reload();
            return;
        }
        const l = parseLink(v);
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

// Ссылка владельца: #manage=id.token[.ключ]. Главная тогда не нужна —
// сразу накладная без груза, управление раздачей, которая идёт без нас.
function bootManage() {
    const m = parseManageLink(location.href);
    if (!m)
        return false;
    $('#home').hidden = true;
    $('#send').hidden = false;
    setText('host', location.host);
    // «Отправить другое» отсюда — просто чистая главная: груза в этой
    // вкладке не было, держать нечего.
    startManaging(m, { onNew: () => location.assign(location.pathname) });
    return true;
}

// Эта вкладка недавно отпустила раздачу «работать без меня» — например,
// её перезагрузили. Напомним, что раздача, может быть, ещё жива.
async function rememberOwned() {
    let owned = null;
    try {
        owned = JSON.parse(sessionStorage.getItem(OWNED_KEY) || 'null');
    } catch {
        return;
    }
    const m = owned && parseManageLink(owned.url || '');
    if (!m)
        return;
    const forget = () => {
        try {
            sessionStorage.removeItem(OWNED_KEY);
        } catch {
            // хранилище недоступно — и так забудется
        }
        $('#own-left').hidden = true;
    };
    try {
        const meta = await fetchMeta(location.origin, m.id);
        if (meta.state !== 'delegated')
            return;
    } catch (e) {
        if (e && /not_found|expired|uses_exhausted|revoked|sender_gone/.test(e.code || ''))
            forget();
        return;
    }
    setText('ownLeftTitle', owned.name ? `Раздача «${owned.name}» идёт без вас.` : 'Ваша раздача идёт без вас.');
    $('#own-left-go').href = owned.url;
    $('#own-left-forget').onclick = forget;
    $('#own-left').hidden = false;
}

function boot() {
    if (bootManage())
        return;
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
    renderPassword();
    rememberOwned();
    setText('host', location.host);
}

// Ссылку владельца могут открыть и в уже открытой главной — тогда
// меняется только фрагмент, и страница не перезагружается.
window.addEventListener('hashchange', () => {
    if (parseManageLink(location.href) && $('#send').hidden)
        location.reload();
});

boot();
