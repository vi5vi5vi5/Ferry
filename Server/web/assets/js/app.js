// Страница приёма: /t/<id>#<ключ>.
//
// Здесь только экран. Сеть, расшифровка, BLAKE3 и запись живут в воркере
// (receive.worker.js); страница читает ссылку, показывает опись, помогает
// выбрать, куда выгрузить том, и рисует то, что воркер присылает.
//
// Ключ из фрагмента не уходит никуда, кроме воркера этой же вкладки: ни в
// запросы, ни в логи, ни в Referer.

import { parseLink, fetchMeta } from './api.js';
import { cryptoAvailable } from './crypto.js';
import { openVolume } from './volume.js';
import { summarize } from './manifest.js';
import { readSavedState, sweepOld, MEMORY_LIMIT } from './store.js';
import { b64urlDecode } from './b64.js';
import { NB, grp, comma, plural, unitOf, inUnit, unitName, size, dur, pct, files, dirs, chunks, copy } from './ui.js';

const $ = (sel) => document.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

function popcount(bytes) {
    let n = 0;
    for (const b of bytes) {
        let v = b;
        while (v) {
            v &= v - 1;
            n++;
        }
    }
    return n;
}

const hasBit = (bits, i) => (bits[i >>> 3] & (1 << (i & 7))) !== 0;

// ---------------------------------------------------------------- привязки

const textCache = new Map();
function setText(name, value) {
    let els = textCache.get(name);
    if (!els) {
        els = $$(`[data-t="${name}"]`);
        textCache.set(name, els);
    }
    const v = String(value);
    for (const el of els)
        if (el.textContent !== v)
            el.textContent = v;
}

const listCache = new Map();
function all(sel) {
    let els = listCache.get(sel);
    if (!els) {
        els = $$(sel);
        listCache.set(sel, els);
    }
    return els;
}

function setLeft(name, x) {
    for (const el of all(`[data-left="${name}"]`))
        el.style.left = x;
}

function setSpan(name, from, width) {
    for (const el of all(`[data-span="${name}"]`)) {
        el.style.left = from;
        el.style.width = width;
    }
}

function setIf(name, on) {
    for (const el of all(`[data-if="${name}"]`))
        el.hidden = !on;
}

function kvs(root, rows) {
    root.replaceChildren(...rows.filter(Boolean).map(([k, v, cls]) => {
        const row = document.createElement('div');
        row.className = 'kv';
        const kk = document.createElement('span');
        kk.className = 'k';
        kk.textContent = k;
        const dots = document.createElement('span');
        dots.className = 'dots';
        const vv = document.createElement('span');
        vv.className = 'v' + (cls ? ' ' + cls : '');
        vv.textContent = v;
        row.append(kk, dots, vv);
        return row;
    }));
}

function fileRow(n, path, sizeText, state, cls = 'frow') {
    const row = document.createElement('div');
    row.className = cls;
    const a = document.createElement('span');
    a.className = 'n';
    a.textContent = n;
    const b = document.createElement('span');
    b.className = 'p';
    b.textContent = path;
    b.title = path;
    const c = document.createElement('span');
    c.className = 's';
    c.textContent = sizeText;
    row.append(a, b, c);
    if (state) {
        const d = document.createElement('span');
        d.className = 'st' + (state.cls ? ' ' + state.cls : '');
        d.append(document.createElement('i'), state.text);
        row.append(d);
    }
    return row;
}

function remember(key, value) {
    try {
        if (value === undefined)
            return localStorage.getItem(key);
        localStorage.setItem(key, value);
    } catch {
        // приватное окно — не беда
    }
    return null;
}

// ---------------------------------------------------------------- состояние

const S = {
    view: 'boot',
    link: null,
    keyText: '',
    meta: null,
    vol: null,          // { manifest, layout, plan }
    sum: null,
    saved: null,        // недокачка из прошлого раза
    savedHave: 0,
    opfs: false,
    fsa: typeof window.showDirectoryPicker === 'function',
    free: null,
    dest: 'download',
    dir: null,
    store: null,
    worker: null,
    alive: false,
    snap: null,
    phase: 'idle',
    dropped: false,
    summary: null,
    target: '',
    seed: { on: false, served: 0, servedBytes: 0, canRead: true },
    seedWant: null,
    exporting: false,
    exported: null,
    exportBlob: null,
    urls: [],
    closed: false,
    spans: null,
    showAll: false,
};

const VIEWS = ['boot', 'preview', 'rx', 'done', 'err'];

function show(view) {
    const was = S.view;
    S.view = view;
    for (const v of VIEWS)
        $(`#v-${v}`).hidden = v !== view;
    $('#rx-scene').hidden = !(view === 'boot' || view === 'preview' || view === 'rx');
    $('#rx-scene').classList.toggle('scene-idle', view !== 'rx');
    $('#done-scene').hidden = view !== 'done';
    if (was !== view && (view === 'done' || view === 'err'))
        window.scrollTo(0, 0);
    updateTitle();
}

function updateTitle() {
    const name = S.vol ? S.vol.manifest.name : '';
    if (S.view === 'rx' && S.snap && S.snap.total)
        document.title = `${comma((S.snap.bytes / S.snap.total) * 100, 0)} % · ${name} — Ferry`;
    else if (S.view === 'done')
        document.title = `Доставлено · ${name} — Ferry`;
    else if (S.view === 'err')
        document.title = 'Отказ — Ferry';
    else
        document.title = name ? `${name} — Ferry` : 'Ferry — приём тома';
}

// ---------------------------------------------------------------- отказы

// Картинка, тон штампа и что сказать человеку. Ошибка не выглядит как его
// вина: каждый отказ говорит, что произошло и куда идти дальше.
const ERRORS = {
    need_key: {
        pic: 'key', tone: 'warn', tag: 'ключ не предъявлен', title: 'Ссылка неполная',
        text: 'В ссылке нет части после решётки — а это и есть ключ. Сервер ключа не знает и расшифровать том не может. Использование раздачи при этом не списано: страница без ключа к раздаче даже не подключалась.',
        exit: 'Вставьте ссылку целиком или ключ, если его прислали отдельно', act: ['link'],
    },
    bad_key: {
        pic: 'key', tone: 'warn', tag: 'ключ не подошёл', title: 'Ключ в ссылке не подходит',
        text: 'Часть ссылки после решётки не открыла опись тома: скорее всего, ссылку обрезали при копировании или в ней опечатка. Использование не списано — к раздаче мы не подключались.',
        exit: 'Скопируйте ссылку заново, целиком', act: ['link'],
    },
    bad_link: {
        pic: 'key', tone: 'warn', tag: 'не ссылка ferry', title: 'Это не похоже на ссылку Ferry',
        text: 'Ссылка на раздачу выглядит как адрес сервера, /t/, номер раздачи и ключ после решётки. Здесь чего-то из этого нет.',
        exit: 'Вставьте ссылку от отправителя целиком', act: ['link'],
    },
    bad_manifest: {
        pic: 'key', tone: 'danger', tag: 'опись не читается', title: 'Опись тома не разобралась',
        text: 'Ключ подошёл, но внутри не опись Ferry, которую мы умеем читать. Принимать такой том мы не будем: лучше ничего, чем непонятно что.',
        exit: 'Попросите отправителя раздать том заново', act: ['request'],
    },
    bad_meta: {
        pic: 'key', tone: 'danger', tag: 'размеры не сходятся', title: 'Сервер описал том неправдоподобно',
        text: 'Размер тома и число чанков, которые назвал сервер, не сходятся друг с другом. С таким описанием приём не начать.',
        exit: 'Попросите отправителя раздать том заново', act: ['request'],
    },
    transfer_not_found: {
        pic: 'fog', tone: 'wait', tag: 'раздачи нет', title: 'Такой раздачи нет',
        text: 'Сервер не знает этой ссылки: раздачу закрыли, у неё вышел срок или сервер перезапускали. Тома на сервере не остаётся — он живёт, только пока отправитель на связи.',
        exit: 'Нужна новая ссылка от отправителя', act: ['request'],
    },
    sender_gone: {
        pic: 'fog', tone: 'wait', tag: 'источник отключён', title: 'Отправитель ушёл',
        text: 'Отправитель отключился, и продолжать раздачу некому. Тот, кто уже скачал том целиком, доступ выдать не может — он канал, а не полномочие.',
        exit: 'Нужна новая ссылка от отправителя', act: ['request'],
    },
    uses_exhausted: {
        pic: 'fog', tone: 'danger', tag: 'лимит исчерпан', title: 'Лимит использований исчерпан',
        text: 'У ссылки было ограниченное число использований, и они кончились. Каждое подключение тратит одно — и первое, и переподключение после обрыва.',
        exit: 'Попросите отправителя выдать новую ссылку', act: ['request'],
    },
    expired: {
        pic: 'fog', tone: 'danger', tag: 'срок вышел', title: 'Срок раздачи вышел',
        text: 'У раздачи был срок жизни, и он закончился. Сервер забыл её вместе со всем, что о ней знал.',
        exit: 'Попросите отправителя запустить раздачу заново', act: ['request'],
    },
    approval_denied: {
        pic: 'fog', tone: 'danger', tag: 'не подтверждено', title: 'Отправитель не подтвердил доступ',
        text: 'Эта раздача выдаётся только с подтверждения отправителя, и он его не дал.',
        exit: 'Напишите отправителю напрямую', act: ['request'],
    },
    no_source: {
        pic: 'fog', tone: 'warn', tag: 'начало уплыло', title: 'Начало тома взять неоткуда',
        text: 'Раздача ушла дальше окна сервера, а тех, у кого есть начало тома, сейчас нет на связи. Отправитель тоже отдать его не может.',
        exit: 'Попробуйте позже или попросите отправителя раздать заново', act: ['retry', 'request'],
    },
    too_many_receivers: {
        pic: 'buoys', tone: 'warn', tag: 'мест нет', title: 'Все места заняты',
        text: 'У раздачи стоит потолок одновременных получателей. Он защищает канал отправителя и сервер, а не ограничивает вас лично. Вы стоите у буя — место освободится, как только кто-то причалит.',
        exit: 'Попробуйте через несколько минут', act: ['retry'],
    },
    preparing: {
        pic: 'buoys', tone: 'wait', tag: 'готовится', title: 'Отправитель ещё готовит том',
        text: 'Отправитель досчитывает контрольные суммы тома. Это ненадолго: как только он закончит, раздача откроется.',
        exit: 'Попробуйте через минуту', act: ['retry'],
    },
    busy: {
        pic: 'buoys', tone: 'wait', tag: 'занято', title: 'Этот том уже принимается в другой вкладке',
        text: 'Хранилище тома занято другой вкладкой этого браузера. Две вкладки в один том писать не могут.',
        exit: 'Вернитесь в ту вкладку или закройте её и попробуйте снова', act: ['retry'],
    },
    server_busy: {
        pic: 'storm', tone: 'danger', tag: 'нет памяти под окно', title: 'Сервер занят',
        text: 'Сервер держит окно каждой раздачи в оперативной памяти и не уходит в своп: когда память кончилась, он честно отказывает новым, а не замедляет текущие.',
        exit: 'Попробуйте позже — сейчас идёт слишком много раздач', act: ['retry'],
    },
    network: {
        pic: 'storm', tone: 'warn', tag: 'нет связи', title: 'Сервер не отвечает',
        text: 'Не получилось достучаться до сервера Ferry. Возможно, пропал интернет или сервер перезапускается.',
        exit: 'Проверьте связь и попробуйте снова', act: ['retry'],
    },
    chunk_mismatch: {
        pic: 'storm', tone: 'danger', tag: 'не сошлось', title: 'Данные не сошлись с контрольной суммой',
        text: 'Чанки раз за разом приходили не такими, какими их отправили, и BLAKE3 их не пропустил. Лучше ничего, чем тихо испорченный том.',
        exit: 'Начните приём заново', act: ['restart'],
    },
    bad_hashes: {
        pic: 'storm', tone: 'danger', tag: 'суммы не сошлись', title: 'Список контрольных сумм не сошёлся',
        text: 'Список BLAKE3, по которому проверяется каждый чанк, не совпал с корнем из описи. Принимать том без проверки мы не будем.',
        exit: 'Попросите отправителя раздать том заново', act: ['request', 'restart'],
    },
    bad_message: {
        pic: 'storm', tone: 'danger', tag: 'протокол', title: 'Сервер и страница не договорились',
        text: 'Сервер прислал то, чего страница не ждала, или отверг то, что она прислала. Обычно так бывает, когда сервер обновили, а страница в браузере осталась старой.',
        exit: 'Обновите страницу; не поможет — заберите том из терминала', act: ['reload'], cli: true,
    },
    no_space: {
        pic: 'storm', tone: 'warn', tag: 'нет места', title: 'Не хватает места на диске',
        text: 'Браузеру не хватает места под том.',
        exit: 'Освободите место на диске и попробуйте снова', act: ['back'], cli: true,
    },
    no_store: {
        pic: 'storm', tone: 'warn', tag: 'нет хранилища', title: 'Браузеру некуда сложить том',
        text: 'Хранилища для больших файлов этот браузер странице не даёт — так бывает в приватном окне. Без него том такого размера принять негде.',
        exit: 'Откройте ссылку в обычном окне или заберите том из терминала', act: [], cli: true,
    },
    io: {
        pic: 'storm', tone: 'danger', tag: 'запись', title: 'Не получилось записать на диск',
        text: 'Браузер отказал в записи. Так бывает, когда место кончилось на ходу или у папки забрали разрешение.',
        exit: 'Попробуйте снова или заберите том из терминала', act: ['retry'], cli: true,
    },
    insecure: {
        pic: 'key', tone: 'warn', tag: 'нет https', title: 'Страница открыта без защиты',
        text: 'Расшифровывать в браузере можно только на защищённой странице (https). По обычному http браузер шифрования не даёт — и правильно делает.',
        exit: 'Откройте ссылку по https или заберите том из терминала', act: [], cli: true,
    },
    unsupported: {
        pic: 'storm', tone: 'warn', tag: 'браузер', title: 'Этот браузер не умеет принимать том',
        text: 'Для приёма нужны модульные воркеры и WebCrypto. Они есть в свежих Chrome, Edge, Firefox и Safari.',
        exit: 'Обновите браузер или заберите том из терминала', act: [], cli: true,
    },
    owner_conflict: {
        pic: 'storm', tone: 'danger', tag: 'конфликт', title: 'Раздача занята',
        text: 'Сервер считает, что этой раздачей уже распоряжается кто-то другой.',
        exit: 'Попросите отправителя раздать том заново', act: ['request'],
    },
};

const DEFAULT_ERROR = {
    pic: 'storm', tone: 'danger', tag: 'сбой', title: 'Приём прервался',
    text: 'Что-то пошло не так, и продолжать вслепую мы не стали.',
    exit: 'Попробуйте снова; не поможет — заберите том из терминала', act: ['retry'], cli: true,
};

function showError(code, message = '') {
    const e = ERRORS[code] || DEFAULT_ERROR;
    S.error = { code, message };
    for (const pic of $$('.err-pic'))
        pic.hidden = pic.dataset.pic !== e.pic;
    const box = $('#err-code');
    box.classList.toggle('wait', e.tone === 'wait');
    box.classList.toggle('danger', e.tone === 'danger');
    setText('errCode', code);
    $('#v-err .head-id').hidden = !S.link;
    setText('errTag', e.tag);
    let title = e.title;
    if (code === 'too_many_receivers' && S.meta && S.meta.receivers > 0) {
        const n = S.meta.receivers;
        title = n === 1 ? 'Сейчас качает один человек'
            : `Сейчас качают ${grp(n)} ${plural(n, 'человек', 'человека', 'человек')}`;
    }
    setText('errTitle', title);
    let text = e.text;
    if (code === 'no_space' && S.meta) {
        text = `Тому нужно ${size(S.meta.total)}, а браузер может отдать под хранилище `
            + `${S.free === null ? 'меньше' : size(S.free)}.`;
        if (S.fsa)
            text += ' Если выбрать папку, том пойдёт прямо в неё — без второй копии, но и без раздачи другим.';
    }
    setText('errText', text);
    setText('errExit', e.exit);
    setText('errDetail', message && message !== code ? message : '');

    // Что из принятого уцелело.
    const got = S.snap && S.snap.received > 0 ? S.snap.received / S.snap.count : 0;
    const saved = $('#err-saved');
    saved.hidden = true;
    if (got > 0 && got < 1 && S.store === 'opfs' && !['chunk_mismatch', 'bad_hashes'].includes(code)) {
        const keep = (e.act || []).includes('retry');
        setText('errSaved', keep
            ? `Принятые ${comma(got * 100, 0)} % сохранены в этом браузере: если раздача ещё жива, продолжим с того же места.`
            : `Принятые ${comma(got * 100, 0)} % лежат в хранилище браузера. С новой ссылкой они не пригодятся: новая раздача шифруется новым ключом.`);
        saved.hidden = false;
    }

    const form = $('#err-link');
    form.hidden = !(e.act || []).includes('link');
    const btns = $('#err-btns');
    btns.replaceChildren();
    const add = (label, fn, primary) => {
        const b = document.createElement('button');
        b.type = 'button';
        b.className = primary ? 'btn btn-p' : 'btn';
        b.textContent = label;
        b.onclick = () => fn(b);
        btns.append(b);
        return b;
    };
    for (const a of e.act || []) {
        if (a === 'retry')
            add('Попробовать снова', retry, true);
        else if (a === 'reload')
            add('Обновить страницу', () => location.reload(), true);
        else if (a === 'back' && S.vol)
            add('Вернуться к выбору места', () => { renderPreview(); show('preview'); }, true);
        else if (a === 'restart' && S.worker)
            add('Начать заново', restartFromScratch, true);
        else if (a === 'request')
            add('Скопировать текст запроса', (b) => copy(requestText(code, title), b));
    }
    if (!(e.act || []).includes('retry') && got > 0 && S.store === 'opfs' && S.worker)
        add('Удалить принятое из браузера', (b) => {
            b.disabled = true;
            b.textContent = 'Удалено';
            S.worker.postMessage({ cmd: 'discard' });
            saved.hidden = true;
        });

    $('#err-cli').hidden = !(e.cli && S.link && S.link.key);
    stopTicking();
    show('err');
}

function requestText(code, title) {
    const name = S.vol ? ` на «${S.vol.manifest.name}»` : '';
    return `Привет! Ссылка Ferry${name} у меня не открывается: «${title}» (${code}). `
        + 'Можешь раздать ещё раз и прислать новую ссылку?';
}

function retry() {
    if (S.worker && S.store)
        startWorker();
    else
        location.reload();
}

// ---------------------------------------------------------------- начало

async function boot() {
    wire();
    const link = parseLink(location.href);
    if (!link) {
        S.link = null;
        return showError('bad_link');
    }
    S.link = link;
    S.keyText = location.hash.slice(1);
    setText('id', link.id);
    setText('hudEyebrow', `приём тома · ${location.host}`);
    const full = `${location.origin}/t/${link.id}#${S.keyText}`;
    setText('cliCmd', `ferry get "${full}"`);
    setText('installCmd', /Windows/i.test(navigator.userAgent)
        ? `irm ${location.origin}/install.ps1 | iex`
        : `curl -fsSL ${location.origin}/install.sh | sh`);

    if (link.badKey)
        return showError('bad_key');
    if (!link.key)
        return showError('need_key');
    if (!window.isSecureContext || !cryptoAvailable())
        return showError('insecure');
    if (typeof Worker !== 'function')
        return showError('unsupported');

    setText('bootText', 'Спрашиваю сервер о раздаче…');
    try {
        S.meta = await fetchMeta(link.origin, link.id);
    } catch (e) {
        return showError(e.code || 'network', e.message);
    }

    setText('bootText', 'Открываю опись тома ключом из ссылки…');
    try {
        S.vol = await openVolume(S.meta, link.key);
    } catch (e) {
        return showError(e.code || 'bad_meta', e.message);
    }
    S.sum = summarize(S.vol.manifest);
    S.spans = S.vol.layout.files.map((f) => ({
        path: f.path,
        size: f.size,
        c0: Math.floor(f.start / S.vol.plan.chunkSize),
        c1: Math.floor((f.start + f.size - 1) / S.vol.plan.chunkSize),
    }));

    S.opfs = await probeOpfs();
    if (S.opfs) {
        S.saved = await readSavedState(link.id);
        const sv = S.saved;
        if (sv && (sv.v !== 1 || sv.total !== S.meta.total || sv.chunkSize !== S.meta.chunkSize
            || sv.count !== S.meta.chunks))
            S.saved = null;
        if (S.saved) {
            const bits = b64urlDecode(S.saved.bits || '');
            S.savedHave = bits ? popcount(bits) : 0;
            if (!S.savedHave && !S.saved.complete)
                S.saved = null;
        }
        sweepOld({ keepId: link.id }).catch(() => {});
    }

    // Отправителя нет — раздача мертва. Если только том не лежит у нас
    // целиком: тогда сервер не нужен вовсе.
    if (S.meta.state !== 'active' && !(S.saved && S.saved.complete))
        return showError('sender_gone');

    const pref = remember('ferry.dest');
    S.dest = S.fsa && pref !== 'download' ? 'dir' : 'download';
    S.free = await freeSpace();
    paintScene();
    renderPreview();
    show('preview');
}

async function probeOpfs() {
    try {
        if (!navigator.storage || typeof navigator.storage.getDirectory !== 'function')
            return false;
        await navigator.storage.getDirectory();
        return true;
    } catch {
        return false;
    }
}

async function freeSpace() {
    try {
        const e = await navigator.storage.estimate();
        if (typeof e.quota !== 'number')
            return null;
        return Math.max(0, e.quota - (e.usage || 0));
    } catch {
        return null;
    }
}

// ---------------------------------------------------------------- превью

const PREVIEW_ROWS = 8;
const ALL_ROWS = 2000;

function pickStore() {
    const total = S.meta.total;
    // Недокачка уже заняла своё место, второй раз его не попросит.
    const need = (S.saved ? 0 : total) + 16 * 1024 * 1024;
    if (S.opfs && (S.free === null || S.free >= need))
        return 'opfs';
    if (S.saved && S.opfs)
        return 'opfs';
    if (S.dest === 'dir' && S.fsa)
        return 'fsa';
    if (total <= MEMORY_LIMIT)
        return 'memory';
    return null;
}

function renderPreview() {
    const m = S.vol.manifest;
    const meta = S.meta;
    const plan = S.vol.plan;
    const tree = m.kind === 'tree';

    setText('volName', m.name);
    setText('volCaption', tree
        ? `${files(S.sum.files)} · ${dirs(S.sum.dirs)} · ${size(meta.total)}`
        : `один файл · ${size(meta.total)}`);

    // Штамп и недокачка.
    const stamp = $('#pv-stamp');
    stamp.className = 'stamp';
    const banner = $('#pv-resume');
    banner.hidden = true;
    let go = tree ? 'Принять том' : 'Принять файл';
    if (S.saved && S.saved.complete) {
        stamp.classList.add('stamp--ok');
        setText('pvStamp', 'принято');
        setText('pvStampSub', 'лежит в браузере');
        setText('resumeText', 'Этот том уже целиком принят в этом браузере.');
        setText('resumeSub', 'Сервер больше не нужен: остаётся выгрузить его в папку или в «Загрузки».');
        banner.hidden = false;
        go = 'Открыть принятое';
    } else if (S.saved) {
        const p = S.savedHave / plan.count;
        stamp.classList.add('stamp--wait');
        setText('pvStamp', 'недокачка');
        setText('pvStampSub', `${comma(p * 100, 0)} % в браузере`);
        setText('resumeText', `В этом браузере уже есть ${comma(p * 100, 0)} % этого тома.`);
        setText('resumeSub', 'Продолжим с того же места: принятое качать заново не придётся.');
        banner.hidden = false;
        go = 'Продолжить приём';
    } else {
        setText('pvStamp', 'к выдаче');
        setText('pvStampSub', 'раздача активна');
    }

    kvs($('#pv-vol'), [
        ['вид', tree ? `папка · ${files(S.sum.files)}${S.sum.dirs ? ', ' + dirs(S.sum.dirs) : ''}` : 'один файл'],
        ['размер', `${grp(meta.total)} байт`],
        ['в человеческом виде', size(meta.total)],
        ['чанк', `${size(plan.chunkSize)} · ${grp(plan.count)} шт`],
        ['проверка', meta.stream
            ? `BLAKE3 · суммы досчитываются (${grp(meta.hashed)} из ${grp(plan.count)})`
            : 'BLAKE3 · список сумм сошёлся с корнем', meta.stream ? '' : 'v--ok'],
    ]);

    const uses = meta.usesLeft;
    kvs($('#pv-terms'), [
        ['состояние раздачи', meta.state === 'active' ? 'активна' : 'закрыта', meta.state === 'active' ? '' : 'v--warn'],
        ['отправитель', meta.state === 'active' ? 'на связи' : 'ушёл', meta.state === 'active' ? 'v--ok' : 'v--warn'],
        ['осталось использований', uses < 0 ? 'без лимита' : grp(uses), uses >= 0 && uses <= 1 ? 'v--warn' : ''],
        ['истекает через', meta.expiresInMs > 0 ? dur(meta.expiresInMs) : '—'],
        ['получателей сейчас', grp(meta.receivers)],
        meta.requireApproval ? ['доступ', 'с подтверждения отправителя', 'v--warn'] : null,
    ]);
    setText('termsNote', uses >= 0
        ? 'Каждое подключение тратит одно использование — и первое, и переподключение после обрыва. Открыть эту страницу ничего не стоит: использование списывается, только когда начинается приём.'
        : 'Открыть эту страницу ничего не стоит: сервер считает подключения, только когда начинается приём.');

    renderPreviewList();
    renderDest();
    $('#pv-go').textContent = go;
}

function renderPreviewList() {
    const m = S.vol.manifest;
    const root = $('#pv-files');
    const more = $('#pv-more');
    if (m.kind !== 'tree') {
        root.className = 'files';
        root.replaceChildren(fileRow('0001', m.name, size(m.total), null, 'frow frow--3'));
        setText('listNote', 'один файл — описи не требуется');
        more.hidden = true;
        return;
    }
    const entries = m.entries;
    const limit = S.showAll ? ALL_ROWS : PREVIEW_ROWS;
    const rows = [];
    const n = Math.min(limit, entries.length);
    const pad = Math.max(4, String(entries.length).length);
    for (let i = 0; i < n; i++) {
        const e = entries[i];
        const row = fileRow(String(i + 1).padStart(pad, '0'), e.isDir ? e.path + '/' : e.path,
            e.isDir ? 'папка' : size(e.size), null, 'frow frow--3' + (e.isDir ? ' frow--dir' : ''));
        rows.push(row);
    }
    root.className = S.showAll ? 'files files--all' : 'files';
    root.replaceChildren(...rows);
    setText('listNote', `показаны ${grp(n)} из ${grp(entries.length)}`);
    more.hidden = entries.length <= PREVIEW_ROWS;
    more.textContent = S.showAll
        ? 'Свернуть опись'
        : `Показать всю опись · ${grp(entries.length)}${entries.length > ALL_ROWS ? ` (первые ${grp(ALL_ROWS)})` : ''}`;
}

function renderDest() {
    const m = S.vol.manifest;
    const tree = m.kind === 'tree';
    const dirIn = $('#dest-dir');
    const dlIn = $('#dest-dl');
    dirIn.disabled = !S.fsa;
    dirIn.checked = S.dest === 'dir';
    dlIn.checked = S.dest === 'download';
    setText('optDirText', S.fsa
        ? (S.dir ? `Папка «${S.dir.name}». ` : 'Папку выберете после нажатия «Принять». ')
          + (tree ? 'Том ляжет в неё деревом файлов, с пустыми папками.' : 'Файл ляжет в неё под своим именем.')
        : 'Этот браузер не умеет писать в папку — так умеют Chrome и Edge.');
    setText('optDlText', tree
        ? 'Браузер умеет сохранять только файлы, поэтому папка приедет одним ZIP без сжатия.'
        : 'Файл сохранится как обычная загрузка.');

    const store = pickStore();
    const total = S.meta.total;
    const label = {
        opfs: 'через хранилище браузера',
        fsa: 'прямо в папку',
        memory: 'в памяти вкладки',
    };
    kvs($('#pv-space'), [
        ['нужно места', size(total)],
        ['браузер может занять', S.free === null ? 'не говорит' : size(S.free),
            S.free === null ? '' : S.free >= total ? 'v--ok' : 'v--warn'],
        ['как пишем', store ? label[store] : 'места не хватает', store ? '' : 'v--warn'],
        ['раздавать другим', store && store !== 'fsa' ? 'можно, пока вкладка открыта' : 'нет',
            store && store !== 'fsa' ? 'v--seed' : ''],
        ['докачка после перезагрузки', store === 'opfs' ? 'да, с того же чанка' : 'нет', store === 'opfs' ? 'v--ok' : 'v--warn'],
    ]);

    let note;
    if (store === 'opfs') {
        note = 'Том собирается в хранилище браузера: так работают две волны, докачка после перезагрузки и раздача опоздавшим. '
            + (S.dest === 'dir'
                ? 'В конце он сам ляжет в выбранную папку.'
                : `В конце браузер сохранит ${tree ? 'ZIP' : 'файл'} в «Загрузки».`)
            + ' Копия в браузере удалится, когда нажмёте «Закрыть», — до тех пор место занято дважды.';
    } else if (store === 'fsa') {
        note = 'Хранилищу браузера места под том не хватает, поэтому запись пойдёт прямо в выбранную папку. '
            + 'Цена: раздавать другим не получится, а после перезагрузки страницы приём начнётся заново.';
    } else if (store === 'memory') {
        note = 'Том небольшой и соберётся в памяти вкладки. Перезагрузка страницы начнёт приём заново.';
    } else {
        note = `Места не хватает: тому нужно ${size(total)}, а браузер может занять `
            + `${S.free === null ? 'меньше' : size(S.free)}. Освободите место на диске`
            + (S.fsa ? ' или выберите выгрузку в папку — тогда запись пойдёт прямо в неё.' : '.');
    }
    $('#pv-space-note').textContent = note;
    $('#pv-go').disabled = !store;
}

// ---------------------------------------------------------------- приём

async function onGo() {
    const btn = $('#pv-go');
    btn.disabled = true;
    try {
        const complete = S.saved && S.saved.complete;
        if (S.dest === 'dir' && !S.dir && S.fsa) {
            try {
                S.dir = await window.showDirectoryPicker({ id: 'ferry', mode: 'readwrite', startIn: 'downloads' });
            } catch (e) {
                if (e && e.name === 'AbortError') {
                    btn.disabled = false;
                    return;
                }
                throw e;
            }
            renderDest();
        }
        S.free = await freeSpace();
        S.store = pickStore();
        if (!S.store)
            return showError(S.opfs ? 'no_space' : 'no_store');
        if (S.store === 'opfs' && !complete && navigator.storage && navigator.storage.persist)
            navigator.storage.persist().catch(() => {});
        startWorker();
    } catch (e) {
        showError('io', String(e && e.message || e));
    } finally {
        btn.disabled = false;
    }
}

function startWorker() {
    if (S.worker)
        S.worker.terminate();
    S.alive = false;
    S.snap = null;
    S.phase = 'connecting';
    S.dropped = false;
    S.summary = null;
    S.exported = null;
    S.exportBlob = null;
    const w = new Worker('/assets/js/receive.worker.js', { type: 'module' });
    S.worker = w;
    w.onmessage = (e) => {
        if (w !== S.worker)
            return;
        S.alive = true;
        onWorker(e.data);
    };
    w.onerror = (e) => {
        if (w !== S.worker)
            return;
        e.preventDefault?.();
        showError(S.alive ? 'io' : 'unsupported', e.message || '');
    };
    w.postMessage({
        cmd: 'start',
        origin: S.link.origin,
        id: S.link.id,
        key: S.link.key,
        meta: S.meta,
        store: S.store,
        dir: S.store === 'fsa' ? S.dir : null,
    });
    initRx();
    show('rx');
    startTicking();
}

function restartFromScratch(btn) {
    if (btn)
        btn.disabled = true;
    S.restartAfterDiscard = true;
    S.worker.postMessage({ cmd: 'discard' });
}

function onWorker(ev) {
    switch (ev.ev) {
    case 'phase':
        S.phase = ev.phase;
        if (ev.phase === 'reconnecting')
            S.dropped = true;
        else if (ev.phase === 'receiving')
            S.dropped = false;
        if (S.view === 'rx')
            renderRx();
        break;
    case 'progress':
        S.snap = ev;
        if (S.view === 'done') {
            S.seed = { ...S.seed, on: ev.seeding, served: ev.served, canRead: ev.canRead };
            renderSeed();
        }
        break;
    case 'done':
        S.summary = ev.summary;
        enterDone();
        break;
    case 'error':
        if (ev.where === 'export')
            return exportFailed(ev);
        if (S.view === 'done')
            return;
        showError(ev.code, ev.message);
        break;
    case 'seed':
        S.seed = ev;
        if (ev.refused)
            S.seedWant = false;
        renderSeed();
        break;
    case 'target':
        S.target = ev.name;
        if (S.view === 'rx')
            renderRxPlace();
        break;
    case 'export-progress':
        exportProgress(ev.done, ev.total);
        break;
    case 'exported':
        exported(ev);
        break;
    case 'discarded':
        if (S.restartAfterDiscard) {
            S.restartAfterDiscard = false;
            S.saved = null;
            startWorker();
        } else if (S.view === 'done') {
            closedDone();
        }
        break;
    default:
        break;
    }
}

let ticker = null;
function startTicking() {
    stopTicking();
    ticker = setInterval(() => {
        if (S.view === 'rx')
            renderRx();
    }, 1000);
}

function stopTicking() {
    clearInterval(ticker);
    ticker = null;
}

// Сцена: паром, буксир второй волны, буи, полосы на фарватере. До начала
// приёма — паром у берега отправителя.
function paintScene(snap) {
    const plan = S.vol.plan;
    const total = S.meta.total;
    const u = unitOf(total);
    let frac = 0;
    let two = false;
    let joinF = 0;
    let headF = 0;
    let tailF = 0;
    if (snap && snap.count) {
        frac = total ? snap.bytes / total : 1;
        two = snap.join > 0 && snap.join < snap.count;
        if (two) {
            joinF = snap.join / snap.count;
            headF = snap.w2 * joinF;
            tailF = snap.w1 * (1 - joinF);
        }
    } else if (S.saved) {
        frac = S.saved.complete ? 1 : S.savedHave / plan.count;
    }
    const shipX = two ? joinF + tailF : frac;
    setIf('two', two);
    setLeft('shipX', pct(shipX));
    setLeft('tugX', pct(headF));
    setSpan('s1', two ? pct(joinF) : '0%', pct(two ? tailF : frac));
    setSpan('s2', '0%', pct(two ? headF : 0));
    setText('pct', comma(frac * 100, 1));
    $('#rx-bar').setAttribute('aria-valuenow', String(Math.round(frac * 100)));
    setText('shipTag', two ? `волна 1 · ${comma(snap.w1 * 100, 0)} %` : `Ferry · ${comma(frac * 100, 1)} %`);
    if (two)
        setText('w2', comma(snap.w2 * 100, 0));

    const gotText = `${inUnit(frac * total, u)} из ${size(total)}`;
    if (!snap)
        setText('hudLine', S.saved ? `в браузере ${gotText}` : `к выдаче · ${size(total)}`);
    else
        setText('hudLine', `принято ${gotText} · осталось ${etaText(snap)}`);

    // Буи: восемь вех по тому, дошедшие горят.
    const list = $('[data-list="buoys"]');
    if (!list.dataset.ready) {
        const tpl = list.querySelector('template');
        for (let i = 0; i < 8; i++) {
            const node = tpl.content.firstElementChild.cloneNode(true);
            node.style.left = pct(i / 7);
            node.querySelector('[data-t="label"]').textContent = i === 0 ? '0'
                : inUnit((total * i) / 7, u) + (i === 7 ? NB + unitName(u) : '');
            list.append(node);
        }
        list.dataset.ready = '1';
    }
    const buoys = $$('[data-fill="color"]', list);
    buoys.forEach((b, i) => {
        const x = i / 7;
        const reached = two ? x <= headF + 0.001 || (x >= joinF && x <= shipX + 0.001) : x <= frac + 0.001;
        b.setAttribute('fill', reached ? '#ff6a1f' : '#5a7181');
    });
}

function etaText(s) {
    if (!s)
        return '—';
    if (s.received >= s.count || s.phase !== 'receiving')
        return '—';
    if (s.eta > 0)
        return dur(s.eta);
    return s.phase === 'receiving' ? 'считаю…' : '—';
}

function initRx() {
    const plan = S.vol.plan;
    const total = S.meta.total;
    const u = unitOf(total);
    setText('totalH', size(total));
    setText('countH', grp(plan.count));
    setText('rate', '0,0');
    setText('got', inUnit(0, u));
    setText('ver', '0');
    setText('eta', '—');

    const cells = Math.min(120, Math.max(plan.count, 1));
    const map = $('#rx-map');
    if (map.childElementCount !== cells)
        map.replaceChildren(...Array.from({ length: cells }, () => document.createElement('i')));
    const per = plan.count / cells;
    setText('mapNote', `${chunks(plan.count)} по ${size(plan.chunkSize)}`
        + (per > 1.05 ? ` · 1 ячейка ≈ ${chunks(Math.round(per))}` : ''));
    const ticks = $('#rx-ticks');
    // Подписи разносятся по краям и равномерно между ними, поэтому на узком
    // экране прячем каждую вторую — оставшиеся стоят на своих местах.
    ticks.replaceChildren(...Array.from({ length: 7 }, (_, i) => {
        const s = document.createElement('span');
        s.textContent = i === 0 ? '0' : inUnit((total * i) / 6, u) + NB + unitName(u);
        if (i % 2)
            s.className = 'hide-sm';
        return s;
    }));
    renderRxPlace();
    renderRx();
}

function renderRxPlace() {
    const m = S.vol.manifest;
    const tree = m.kind === 'tree';
    const dirName = S.dir ? S.dir.name : '';
    const store = S.store;
    kvs($('#rx-place'), [
        ['хранилище', store === 'opfs' ? 'хранилище браузера (OPFS)'
            : store === 'fsa' ? `прямо в папку «${dirName}»` : 'память вкладки'],
        store === 'fsa' && S.target ? ['имя в папке', S.target] : null,
        ['после приёма', store === 'fsa' ? 'уже на месте'
            : S.dest === 'dir' && S.dir ? `в папку «${dirName}»`
                : `в «Загрузки» · ${tree ? m.name + '.zip' : m.name}`],
        ['режим записи', store === 'fsa' ? 'по местам · без раздачи' : 'по местам · можно раздавать',
            store === 'fsa' ? '' : 'v--seed'],
        ['докачка после перезагрузки', store === 'opfs' ? 'да' : 'нет', store === 'opfs' ? 'v--ok' : 'v--warn'],
        ['шифрование', 'AES-256-GCM · ключ из ссылки'],
    ]);
    setText('placeNote', store === 'fsa'
        ? 'Браузер не даёт читать файлы, пока они пишутся, поэтому раздавать другим во время такого приёма не получится.'
        : 'Пока вкладка открыта, принятые чанки уходят тем, кто подключился позже, — так работает вторая волна.');
    setText('rxFoot', store === 'opfs'
        ? 'Не закрывайте вкладку: приём идёт в ней. Каждый чанк проверяется BLAKE3 сразу по приходу, а принятое лежит в хранилище браузера — после обрыва или перезагрузки продолжим с того же места.'
        : 'Не закрывайте вкладку: приём идёт в ней. Каждый чанк проверяется BLAKE3 сразу по приходу. После обрыва связи продолжим с того же места, но перезагрузка страницы начнёт приём заново.');
}

function renderRx() {
    const s = S.snap;
    const plan = S.vol.plan;
    const total = S.meta.total;
    const u = unitOf(total);
    paintScene(s);

    // Штамп и плашка обрыва.
    const stamp = $('#rx-stamp');
    const banner = $('#rx-banner');
    const bbtn = $('#rx-banner-btn');
    const dot = $('#rx-banner-dot');
    const phase = S.phase;
    const got = s && s.count ? s.received / s.count : 0;
    const gotPct = `${comma(got * 100, 0)} %`;
    const uses = s && typeof s.usesLeft === 'number' ? s.usesLeft : S.meta.usesLeft;
    const usesNote = uses >= 0
        ? `Каждое переподключение тратит одно использование ссылки: осталось ${grp(uses)}.`
        : 'Продолжим ровно с того места, где оборвалось: сервер получит список уже принятых чанков.';
    stamp.className = 'stamp';
    banner.className = 'banner';
    banner.hidden = true;
    $('#rx-stop').hidden = false;
    $('#rx-resume').hidden = true;

    if (phase === 'reconnecting' || (phase === 'connecting' && S.dropped)) {
        stamp.classList.add('stamp--wait');
        setText('rxStamp', 'пауза');
        setText('rxStampSub', 'связь потеряна');
        banner.hidden = false;
        dot.style.background = 'var(--wait)';
        if (phase === 'reconnecting' && s && s.reconnectAt) {
            const left = Math.max(0, Math.ceil((s.reconnectAt - Date.now()) / 1000));
            setText('bannerText', `Соединение потеряно. Принятое (${gotPct}) на месте — переподключаюсь через ${left}${NB}с.`);
            bbtn.hidden = false;
            bbtn.textContent = 'Переподключиться сейчас';
        } else {
            setText('bannerText', `Переподключаюсь… Принятое (${gotPct}) на месте.`);
            bbtn.hidden = true;
        }
        setText('bannerSub', (s && s.reconnectAttempt > 1 ? `Попытка ${s.reconnectAttempt}. ` : '') + usesNote);
    } else if (phase === 'stopped') {
        stamp.classList.add('stamp--wait');
        setText('rxStamp', 'остановлен');
        setText('rxStampSub', S.store === 'opfs' ? 'принятое сохранено' : 'по вашей команде');
        banner.hidden = false;
        banner.classList.add('banner--seed');
        dot.style.background = 'var(--seed)';
        setText('bannerText', S.store === 'opfs'
            ? `Приём остановлен. Принятые ${gotPct} сохранены в этом браузере.`
            : 'Приём остановлен.');
        setText('bannerSub', S.store === 'opfs'
            ? `Продолжим с того же места, пока раздача жива. ${uses >= 0 ? usesNote : ''}`
            : 'Принятое было только в этой вкладке — продолжить можно лишь с начала.');
        bbtn.hidden = true;
        $('#rx-stop').hidden = true;
        $('#rx-resume').hidden = false;
        $('#rx-resume').textContent = S.store === 'opfs' ? 'Продолжить приём' : 'Начать заново';
    } else if (phase === 'connecting' || phase === 'idle') {
        setText('rxStamp', 'на подходе');
        setText('rxStampSub', 'подключаюсь');
    } else {
        const two = s && s.join > 0 && s.join < s.count;
        setText('rxStamp', 'приём идёт');
        setText('rxStampSub', two ? 'две волны' : 'одна волна');
    }

    if (!s) {
        updateTitle();
        return;
    }

    // Числа.
    setText('rate', comma(s.phase === 'receiving' ? s.rate / 1e6 : 0, 1));
    setText('got', inUnit(s.bytes, u));
    setText('eta', etaText(s));
    setText('ver', grp(s.verified));

    const note = $('#rx-hashnote');
    if (S.meta.stream && !s.hashesComplete) {
        note.hidden = false;
        note.textContent = `Отправитель ещё досчитывает контрольные суммы: готово ${grp(s.hashed)} из ${grp(s.count)}. `
            + 'Чанк без суммы ждёт её на диске и проверяется, как только она приедет'
            + (s.pending > 0 ? ` — сейчас ждут ${grp(s.pending)}.` : '.');
    } else if (s.pending > 0) {
        note.hidden = false;
        note.textContent = `Перепроверяю принятое в прошлый раз: осталось ${chunks(s.pending)}.`;
    } else {
        note.hidden = true;
    }
    paintSources('#rx-src', s.src);

    // Волны.
    const two = s.join > 0 && s.join < s.count;
    const joinF = two ? s.join / s.count : 0;
    setText('w1', comma(s.w1 * 100, 0));
    setText('w2', comma(s.w2 * 100, 0));
    setText('w1label', two ? 'волна 1 · хвост, со всеми' : 'одна волна · со всеми');
    setText('waveNote', two ? `вы вошли на ${comma(joinF * 100, 0)} %` : 'вы успели к отплытию');
    setText('waveText', two
        ? `Вы подключились, когда раздача ушла на ${comma(joinF * 100, 0)} %. Хвост тома идёт со всеми из окна сервера, а начало приходит второй волной — от тех, кто уже скачал. Отправитель за него платит, только если больше не от кого.`
        : 'Вы подключились до того, как окно сервера ушло вперёд: весь том идёт одной волной, со всеми остальными.');
    wave(1, s.w1);
    wave(2, s.w2);

    // Карта тома.
    const cells = $('#rx-map').children;
    for (let i = 0; i < cells.length && i < s.cells.length; i++) {
        const cls = s.cells[i] ? `c${s.cells[i]}` : '';
        if (cells[i].className !== cls)
            cells[i].className = cls;
    }

    renderRxFiles(s);
    updateTitle();
}

function wave(n, w) {
    $(`#w${n}-clip`).setAttribute('width', (w * 1000).toFixed(1));
    const boat = $(`#w${n}-boat`);
    boat.style.left = pct(w);
    boat.style.transform = `translate(-80%, ${(-3 * Math.sin(2 * Math.PI * w * 10)).toFixed(2)}px)`;
}

function paintSources(sel, src) {
    const box = $(sel);
    if (!src) {
        box.hidden = true;
        return;
    }
    const sum = src.window + src.peers + src.sender;
    if (!(sum > 0)) {
        box.hidden = true;
        return;
    }
    box.hidden = false;
    for (const i of $$('[data-src]', box))
        i.style.width = pct(src[i.dataset.src] / sum);
    const share = (x) => `${Math.round((x / sum) * 100)} %`;
    setText('srcWindow', share(src.window));
    setText('srcPeers', share(src.peers));
    setText('srcSender', share(src.sender));
}

// Опись в ходе приёма: что едет сейчас, потом что ждёт, потом принятое.
const RX_ROWS = 6;

function renderRxFiles(s) {
    const spans = S.spans;
    const bits = s.bits;
    const active = [];
    const waiting = [];
    const done = [];
    for (let k = 0; k < spans.length; k++) {
        const f = spans[k];
        let have = 0;
        for (let i = f.c0; i <= f.c1; i++)
            if (hasBit(bits, i))
                have++;
        const share = have / (f.c1 - f.c0 + 1);
        const item = { k, share };
        if (share >= 1)
            done.push(item);
        else if (share > 0)
            active.push(item);
        else
            waiting.push(item);
    }
    const pick = active.concat(waiting, done).slice(0, RX_ROWS);
    const pad = Math.max(4, String(spans.length).length);
    $('#rx-files').replaceChildren(...pick.map(({ k, share }) => {
        const f = spans[k];
        const state = share >= 1 ? { cls: 'ok', text: 'принят' }
            : share > 0 ? { cls: 'go', text: `в пути ${Math.floor(share * 100)} %` }
                : { cls: '', text: 'ждёт' };
        return fileRow(String(k + 1).padStart(pad, '0'), f.path, size(f.size), state);
    }));
    setText('rxListNote', done.length === spans.length
        ? 'все приняты'
        : `${grp(pick.length)} из ${grp(spans.length)} · принято ${grp(done.length)}`);
}

// ---------------------------------------------------------------- доставлено

function enterDone() {
    stopTicking();
    const m = S.vol.manifest;
    const sm = S.summary;
    const tree = m.kind === 'tree';
    const d = new Date();
    const date = `${String(d.getDate()).padStart(2, '0')}.${String(d.getMonth() + 1).padStart(2, '0')}.${d.getFullYear()}`;
    setText('doneStampSub', `${grp(sm.count)} / ${grp(sm.count)} · BLAKE3 · ${date}`);
    setText('doneLead', tree ? 'Готово. Том у вашего причала.' : 'Готово. Файл у вашего причала.');
    setText('doneOk', `Проверено BLAKE3 — целостность подтверждена для каждого из ${grp(sm.count)} ${plural(sm.count, 'чанка', 'чанков', 'чанков')}.`);
    kvs($('#done-kv'), [
        ['том', m.name],
        ['состав', tree ? `${files(S.sum.files)}, ${dirs(S.sum.dirs)}` : 'один файл'],
        ['размер', `${grp(sm.total)} байт`],
        ['в пути', (sm.took < 1000 ? 'меньше секунды' : dur(sm.took)) + (sm.resumed ? ' · с докачкой' : '')],
        sm.resumed ? null : ['средняя скорость', `${comma(sm.rate / 1e6, 1)} МБ/с`],
        ['проверено чанков', `${grp(sm.count)} из ${grp(sm.count)}`, 'v--ok'],
    ]);
    paintSources('#done-src', sm.src || (S.snap && S.snap.src));
    setText('doneFoot', S.store === 'opfs'
        ? 'Сервер тома не хранит: чанки шли через его память и там не задержались, а ключа он не видел вовсе. Копия в хранилище браузера удалится, когда нажмёте «Закрыть».'
        : 'Сервер тома не хранит: чанки шли через его память и там не задержались, а ключа он не видел вовсе.');
    $('#done-close').disabled = false;
    $('#done-close').textContent = 'Закрыть';
    $('#done-list').hidden = !tree;
    $('#done-files').hidden = true;
    show('done');
    renderPlace();
    renderSeed();
    // Папку выбрали заранее — перекладываем сами, не дожидаясь кнопки.
    if (S.store !== 'fsa' && S.dest === 'dir' && S.dir && !S.exported)
        startExport('dir', S.dir);
}

function renderPlace() {
    const m = S.vol.manifest;
    const tree = m.kind === 'tree';
    const go = $('#exp-go');
    const alt = $('#exp-alt');
    $('#exp-bar').hidden = !S.exporting;
    go.hidden = true;
    alt.hidden = true;
    go.disabled = S.exporting;
    alt.disabled = S.exporting;

    if (S.closed) {
        setText('placeName', S.exported && S.exported.how === 'dir'
            ? `${S.dir ? S.dir.name + ' / ' : ''}${S.exported.name}` : 'копия в браузере удалена');
        setText('placeSub', S.exported ? 'выгружено; копия в браузере удалена' : 'том не сохранён');
        return;
    }
    if (S.store === 'fsa') {
        setText('placeName', `${S.dir ? S.dir.name : 'папка'} / ${S.target || m.name}`);
        setText('placeSub', tree ? 'дерево записано прямо в папку, с пустыми папками' : 'файл записан прямо в папку');
        return;
    }
    if (S.exported && S.exported.how === 'dir') {
        setText('placeName', `${S.dir ? S.dir.name : 'папка'} / ${S.exported.name}`);
        setText('placeSub', tree ? 'дерево воссоздано целиком, с пустыми папками' : 'файл лежит в выбранной папке');
        alt.hidden = false;
        alt.textContent = tree ? 'Ещё и ZIP в «Загрузки»' : 'Ещё и в «Загрузки»';
        return;
    }
    if (S.exporting) {
        setText('placeName', S.exporting === 'dir'
            ? `${S.dir ? S.dir.name : 'папка'} / ${m.name}`
            : 'Загрузки браузера');
        return;
    }
    if (S.dest === 'dir' && S.dir && !S.exported) {
        // Перекладка ещё не началась или упала: кнопка — повторить.
        setText('placeName', `${S.dir.name} / ${m.name}`);
        setText('placeSub', 'ещё в хранилище браузера');
        go.hidden = false;
        go.textContent = 'Сохранить в папку';
        alt.hidden = false;
        alt.textContent = tree ? 'Скачать ZIP' : 'Скачать файл';
        return;
    }
    const fileName = tree ? `${m.name}.zip` : m.name;
    setText('placeName', 'Загрузки браузера');
    setText('placeSub', S.exported
        ? `${fileName} — отдан браузеру, ищите в «Загрузках»`
        : tree ? `${fileName} · ZIP без сжатия, с пустыми папками` : fileName);
    go.hidden = false;
    go.textContent = S.exported ? 'Скачать ещё раз' : tree ? 'Скачать ZIP' : 'Скачать файл';
    if (S.fsa) {
        alt.hidden = false;
        alt.textContent = 'Сохранить в папку…';
    }
}

async function startExport(how, dir) {
    if (S.exporting)
        return;
    $('#exp-err').hidden = true;
    if (how === 'download' && S.exportBlob) {
        download(S.exportBlob, S.exportName);
        return;
    }
    S.exporting = how;
    exportProgress(0, 1);
    renderPlace();
    S.worker.postMessage({ cmd: 'export', how, dir: dir || null });
}

function exportProgress(done, total) {
    const bar = $('#exp-bar');
    bar.hidden = false;
    const p = total > 0 ? done / total : 0;
    bar.firstElementChild.style.width = pct(p);
    const m = S.vol.manifest;
    setText('placeSub', S.exporting === 'dir'
        ? `перекладываю из хранилища браузера · ${comma(p * 100, 0)} %`
        : m.kind === 'tree' ? `собираю ZIP · ${comma(p * 100, 0)} %` : 'готовлю файл…');
}

function exported(ev) {
    S.exporting = false;
    if (ev.how === 'download') {
        S.exportBlob = ev.blob;
        S.exportName = ev.name;
        download(ev.blob, ev.name);
        S.exported = { how: 'download', name: ev.name };
    } else {
        S.exported = { how: 'dir', name: ev.name };
    }
    renderPlace();
}

function exportFailed(ev) {
    S.exporting = false;
    renderPlace();
    $('#exp-bar').hidden = true;
    const box = $('#exp-err');
    box.hidden = false;
    setText('expErr', ev.code === 'no_space'
        ? 'Не хватило места, чтобы выгрузить том.'
        : 'Выгрузить том не получилось.');
    setText('expErrSub', `${ev.message || ev.code}. Том по-прежнему в хранилище браузера — можно попробовать ещё раз или выбрать другой путь.`);
}

function download(blob, name) {
    const url = URL.createObjectURL(blob);
    S.urls.push(url);
    const a = document.createElement('a');
    a.href = url;
    a.download = name;
    a.rel = 'noopener';
    a.hidden = true;
    document.body.append(a);
    a.click();
    a.remove();
}

const SEED_REFUSED = {
    uses_exhausted: 'использования ссылки кончились',
    transfer_not_found: 'раздачи на сервере уже нет',
    sender_gone: 'отправитель ушёл',
    expired: 'срок раздачи вышел',
    too_many_receivers: 'все места заняты',
    server_busy: 'сервер занят',
    network: 'сервер не отвечает',
};

function renderSeed() {
    if (S.view !== 'done')
        return;
    const sw = $('#seed-sw');
    const role = $('#seed-role');
    const canRead = S.seed.canRead !== false && S.store !== 'fsa';
    const on = !!S.seed.on && !S.closed;
    // Переключатель показывает желание человека, строка под ним — правду:
    // подключение для раздачи занимает пару секунд.
    const want = S.closed ? false : S.seedWant !== null ? S.seedWant : on;
    sw.disabled = !canRead || S.closed;
    sw.setAttribute('aria-checked', want ? 'true' : 'false');
    role.classList.toggle('role--seed', on);
    const served = S.snap ? S.snap.served : S.seed.served || 0;
    const refused = S.seed.refused
        ? `раздавать не вышло: ${SEED_REFUSED[S.seed.refused] || S.seed.refused}` : '';
    setText('seedRole', on
        ? `раздаёт другим · отдано ${chunks(served)}`
        : S.closed ? 'закрыто' : want ? 'подключаюсь к раздаче…' : refused || 'готово · не раздаёт');
    setText('seedText', canRead
        ? 'Те, кто подключится позже, возьмут начало тома у вас — отправителю не придётся отдавать его второй раз.'
        : 'При записи прямо в папку раздавать нечем: браузер не даёт читать файлы, пока они пишутся.');
}

function onClose() {
    const notSaved = S.store !== 'fsa' && !S.exported;
    if (notSaved && !confirm('Том ещё не выгружен из браузера — после «Закрыть» его не останется. Закрыть всё равно?'))
        return;
    if (S.exported && S.exported.how === 'download'
        && !confirm('Копия в браузере будет удалена. Убедитесь, что загрузка закончилась. Закрыть?'))
        return;
    $('#done-close').disabled = true;
    if (S.worker)
        S.worker.postMessage({ cmd: 'discard' });
    else
        closedDone();
}

function closedDone() {
    S.closed = true;
    S.seed = { on: false, served: S.seed.served, canRead: false };
    for (const u of S.urls.splice(0))
        setTimeout(() => URL.revokeObjectURL(u), 60000);
    if (S.worker) {
        S.worker.terminate();
        S.worker = null;
    }
    renderPlace();
    renderSeed();
    setText('doneFoot', 'Копия из хранилища браузера удалена. Вкладку можно закрыть.');
    $('#done-close').textContent = 'Закрыто';
}

function renderDoneFiles() {
    const box = $('#done-files');
    if (!box.hidden) {
        box.hidden = true;
        $('#done-list').textContent = 'Показать опись';
        return;
    }
    const entries = S.vol.manifest.entries;
    const n = Math.min(ALL_ROWS, entries.length);
    const pad = Math.max(4, String(entries.length).length);
    const rows = [];
    for (let i = 0; i < n; i++) {
        const e = entries[i];
        rows.push(fileRow(String(i + 1).padStart(pad, '0'), e.isDir ? e.path + '/' : e.path,
            e.isDir ? 'папка' : size(e.size), { cls: 'ok', text: 'принят' },
            'frow' + (e.isDir ? ' frow--dir' : '')));
    }
    box.replaceChildren(...rows);
    box.hidden = false;
    $('#done-list').textContent = 'Скрыть опись';
}

// ---------------------------------------------------------------- события

function wire() {
    for (const b of $$('[data-copy]'))
        b.addEventListener('click', () => copy($(`[data-t="${b.dataset.copy}"]`).textContent, b));

    $('#pv-more').addEventListener('click', () => {
        S.showAll = !S.showAll;
        renderPreviewList();
    });
    for (const r of $$('input[name="dest"]'))
        r.addEventListener('change', () => {
            S.dest = r.value;
            remember('ferry.dest', r.value);
            renderDest();
        });
    $('#pv-go').addEventListener('click', onGo);

    $('#rx-banner-btn').addEventListener('click', () => {
        if (S.worker)
            S.worker.postMessage({ cmd: 'reconnect' });
    });
    $('#rx-stop').addEventListener('click', () => {
        if (S.store !== 'opfs'
            && !confirm('Принятое лежит только в этой вкладке и пропадёт — продолжить можно будет лишь с начала. Остановить?'))
            return;
        if (S.worker)
            S.worker.postMessage({ cmd: 'stop' });
    });
    $('#rx-resume').addEventListener('click', () => startWorker());

    $('#exp-go').addEventListener('click', () => {
        if (S.dest === 'dir' && S.dir && !S.exported)
            startExport('dir', S.dir);
        else
            startExport('download');
    });
    $('#exp-alt').addEventListener('click', async () => {
        if (S.exported && S.exported.how === 'dir')
            return startExport('download');
        if (S.dest === 'dir' && S.dir && !S.exported)
            return startExport('download');
        try {
            S.dir = await window.showDirectoryPicker({ id: 'ferry', mode: 'readwrite', startIn: 'downloads' });
        } catch {
            return;
        }
        S.dest = 'dir';
        S.exported = null;
        startExport('dir', S.dir);
    });
    $('#done-list').addEventListener('click', renderDoneFiles);
    $('#seed-sw').addEventListener('click', () => {
        if (!S.worker || S.closed)
            return;
        const on = $('#seed-sw').getAttribute('aria-checked') !== 'true';
        S.seedWant = on;
        renderSeed();
        S.worker.postMessage({ cmd: 'seed', on });
    });
    $('#done-close').addEventListener('click', onClose);

    $('#err-link').addEventListener('submit', (e) => {
        e.preventDefault();
        const v = $('#err-link-in').value.trim();
        // Ключ могли прислать отдельно от ссылки — тогда вставляют только
        // его: 43 знака base64url, те самые, что стоят после #.
        if (S.link && /^#?[A-Za-z0-9_-]{43}$/.test(v)) {
            location.hash = v.replace(/^#/, '');
            location.reload();
            return;
        }
        const l = parseLink(v);
        if (!l || !l.key) {
            $('#err-link-in').setCustomValidity('Нужна ссылка Ferry целиком — с частью после # — или сам ключ');
            $('#err-link-in').reportValidity();
            return;
        }
        if (l.origin === location.origin && location.pathname === `/t/${l.id}`) {
            location.hash = new URL(v).hash;
            location.reload();
        } else {
            location.href = v;
        }
    });
    $('#err-link-in').addEventListener('input', (e) => e.target.setCustomValidity(''));

    window.addEventListener('online', () => {
        if (S.worker)
            S.worker.postMessage({ cmd: 'online' });
    });
    // Закрыть вкладку посреди приёма — потерять его (или хотя бы
    // соединение). Спрашиваем, как спрашивает любой загрузчик.
    window.addEventListener('beforeunload', (e) => {
        const receiving = S.view === 'rx' && !['stopped', 'failed', 'done'].includes(S.phase);
        const unsaved = S.view === 'done' && !S.closed && (S.exporting || (S.store === 'memory' && !S.exported));
        if (receiving || unsaved) {
            e.preventDefault();
            e.returnValue = '';
        }
    });
}

boot().catch((e) => showError('io', String(e && e.message || e)));
