// Service Worker загрузок: приём потоком прямо в «Загрузки» (путь C).
//
// Браузер умеет сохранить файл только из ответа на запрос. Этот SW и есть
// такой ответ: страница регистрирует у него загрузку (имя, точный размер и
// MessagePort к воркеру приёма) и открывает в невидимом iframe адрес
// /assets/js/dl/<rid>/<имя>. SW отвечает потоком, тело которого тянет из
// порта — по куску на каждую просьбу браузера, так что быстрее, чем пишет
// диск, данные не идут.
//
// Content-Length — точный: по нему менеджер загрузок показывает процент
// и по нему же отличает целый файл от оборванного. Оборванная загрузка
// помечается неудавшейся, а не ложится в «Загрузки» обрезанным «готовым»
// файлом.
//
// Регистрация одноразовая: адрес загрузки работает ровно один раз.

const PREFIX = '/assets/js/dl/';

const pending = new Map();       // rid → { name, size, port }: ещё не открыта
const live = new Map();          // rid → оборвать идущую загрузку

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (e) => e.waitUntil(self.clients.claim()));

self.addEventListener('message', (e) => {
    const m = e.data || {};
    if (m.type === 'register' && typeof m.rid === 'string' && m.port) {
        pending.set(m.rid, { name: String(m.name || 'ferry'), size: Number(m.size), port: m.port });
        if (m.ack)
            m.ack.postMessage({ type: 'registered', rid: m.rid });
    } else if (m.type === 'abort' && typeof m.rid === 'string') {
        // Страница уходит или начинает заново. Воркера приёма, который
        // оборвал бы загрузку через свой порт, может уже не быть — и
        // загрузка висела бы «идущей», пока браузер не остановит этот SW.
        const r = pending.get(m.rid);
        if (r) {
            abortPort(r.port);
            pending.delete(m.rid);
        }
        const stop = live.get(m.rid);
        if (stop)
            stop();
    }
    // ping — ничего не делает: само сообщение продлевает жизнь SW, пока
    // идёт загрузка, и браузер не останавливает его за «простой».
});

function abortPort(port) {
    try {
        port.postMessage({ type: 'cancel' });
        port.close();
    } catch {
        // уже закрыт
    }
}

// RFC 6266: имя в UTF-8 плюс ASCII-запаска для старых клиентов.
function disposition(name) {
    const ascii = name.replace(/[^\x20-\x7e]/g, '_').replace(/["\\]/g, '_');
    const utf = encodeURIComponent(name).replace(/['()*]/g, (c) => '%' + c.charCodeAt(0).toString(16).toUpperCase());
    return `attachment; filename="${ascii}"; filename*=UTF-8''${utf}`;
}

self.addEventListener('fetch', (e) => {
    const url = new URL(e.request.url);
    if (url.origin !== self.location.origin || !url.pathname.startsWith(PREFIX))
        return;
    const rid = url.pathname.slice(PREFIX.length).split('/')[0];
    const r = pending.get(rid);
    pending.delete(rid);
    if (!r) {
        e.respondWith(new Response('Эта загрузка уже взята или не существует.',
            { status: 404, headers: { 'Content-Type': 'text/plain; charset=utf-8' } }));
        return;
    }
    e.respondWith(stream(rid, r));
});

function stream(rid, r) {
    const port = r.port;
    let waiting = null;      // ответ на текущую просьбу
    let controller = null;
    let finished = false;

    const finish = () => {
        finished = true;
        live.delete(rid);
        port.onmessage = null;
        try {
            port.close();
        } catch {
            // уже закрыт
        }
    };

    port.onmessage = (ev) => {
        const d = ev.data || {};
        if (d.type === 'abort') {
            // Обрыв приходит и без просьбы: браузер мог ещё не попросить.
            if (!finished) {
                controller.error(new Error('ferry: ' + (d.reason || 'aborted')));
                finish();
            }
            if (waiting) {
                waiting();
                waiting = null;
            }
            return;
        }
        if (!waiting)
            return;
        const done = waiting;
        waiting = null;
        if (d.type === 'data') {
            controller.enqueue(new Uint8Array(d.buf));
        } else if (d.type === 'end') {
            controller.close();
            port.postMessage({ type: 'closed' });
            finish();
        }
        done();
    };

    live.set(rid, () => {
        if (!finished) {
            controller.error(new Error('ferry: aborted'));
            port.postMessage({ type: 'cancel' });
            finish();
        }
        if (waiting) {
            waiting();
            waiting = null;
        }
    });

    const body = new ReadableStream({
        start(c) {
            controller = c;
        },
        pull() {
            if (finished)
                return undefined;
            return new Promise((resolve) => {
                waiting = resolve;
                port.postMessage({ type: 'pull' });
            });
        },
        cancel() {
            // Загрузку отменили в браузере — приём должен об этом узнать.
            if (!finished) {
                port.postMessage({ type: 'cancel' });
                finish();
            }
            if (waiting) {
                waiting();
                waiting = null;
            }
        },
    }, new CountQueuingStrategy({ highWaterMark: 4 }));

    const headers = new Headers({
        'Content-Type': 'application/octet-stream',
        'Content-Disposition': disposition(r.name),
        'X-Content-Type-Options': 'nosniff',
        'Cache-Control': 'no-store',
    });
    if (Number.isSafeInteger(r.size) && r.size >= 0)
        headers.set('Content-Length', String(r.size));
    return new Response(body, { headers });
}
