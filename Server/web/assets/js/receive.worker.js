// Воркер приёма: сеть, расшифровка, BLAKE3 и запись — вне главного потока.
//
// Страница только рисует. Всё тяжёлое здесь: хеш 4 МиБ чанка в главном
// потоке подвешивал бы интерфейс, а синхронный доступ к OPFS браузер и
// вовсе даёт только воркеру.

import { Receiver } from './receiver.js';
import { OpfsStore, MemoryStore, DirectStore, StoreError, freeName, exportToDirectory } from './store.js';
import { buildZip } from './zip.js';
import { safeFileName } from './manifest.js';
import { openVolume } from './volume.js';

let rx = null;

const post = (msg, transfer) => self.postMessage(msg, transfer || []);

function baseName(manifest) {
    return safeFileName(manifest.name) || 'ferry';
}

async function makeStore(m) {
    if (m.store === 'opfs')
        return new OpfsStore(m.id);
    if (m.store === 'memory')
        return new MemoryStore();
    if (m.store === 'fsa') {
        // Прямая запись в папку. Имя тома в ней выбираем здесь, чтобы не
        // затереть то, что там уже лежит; для этого нужен манифест.
        const vol = await openVolume(m.meta, m.key);
        const name = await freeName(m.dir, baseName(vol.manifest), vol.manifest.kind === 'tree');
        post({ ev: 'target', name });
        return new DirectStore(m.dir, vol.manifest, vol.layout, name);
    }
    throw new StoreError('io', 'неизвестное хранилище');
}

async function start(m) {
    rx = new Receiver({
        origin: m.origin,
        id: m.id,
        key: m.key,
        meta: m.meta,
        store: await makeStore(m),
        name: m.name || '',
        onEvent: (ev) => {
            if (ev.ev === 'progress')
                post(ev, [ev.cells.buffer, ev.bits.buffer]);
            else
                post(ev);
        },
    });
    await rx.start();
}

async function doExport(m) {
    const blob = await rx.store.volumeBlob();
    if (!blob)
        throw new StoreError('io', 'том уже лежит в папке — перекладывать нечего');
    const manifest = rx.manifest;
    const name = baseName(manifest);
    const progress = (done, total) => post({ ev: 'export-progress', done, total });

    if (m.how === 'dir') {
        const target = await freeName(m.dir, name, manifest.kind === 'tree');
        await exportToDirectory(m.dir, manifest, rx.layout, blob, target, progress);
        post({ ev: 'exported', how: 'dir', name: target });
        return;
    }
    if (manifest.kind === 'tree') {
        const zip = await buildZip(manifest, rx.layout, blob, name, progress);
        post({ ev: 'exported', how: 'download', name: name + '.zip', blob: zip });
        return;
    }
    post({ ev: 'exported', how: 'download', name, blob });
}

self.onmessage = async (e) => {
    const m = e.data;
    try {
        switch (m.cmd) {
        case 'start':
            await start(m);
            break;
        case 'stop':
            if (rx)
                await rx.stop();
            break;
        case 'reconnect':
            if (rx)
                rx.reconnectNow();
            break;
        case 'online':
            if (rx)
                rx.networkBack();
            break;
        case 'seed':
            if (rx)
                rx.setSeeding(m.on);
            break;
        case 'export':
            await doExport(m);
            break;
        case 'discard':
            if (rx) {
                await rx.stop();
                await rx.store.discard();
            }
            post({ ev: 'discarded' });
            break;
        default:
            break;
        }
    } catch (err) {
        const code = err && err.code ? err.code : 'io';
        const where = m.cmd === 'export' ? 'export' : 'start';
        post({ ev: 'error', code, message: String(err && err.message || err), where,
            retry: where === 'export' });
    }
};
