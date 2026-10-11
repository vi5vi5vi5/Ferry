// Открыть том по метаданным раздачи и ключу из ссылки — то, что CLI
// делает в начале runGet: расшифровать манифест, проверить его против того,
// что сказал сервер, и, если хеши уже посчитаны, сверить список с корнем.
//
// Используется дважды: на странице — чтобы показать накладную до того,
// как человек нажмёт «Принять», и в воркере — перед самим приёмом.

import { blake3 } from './blake3.js';
import { deriveKeys, openChunk, LABEL_MANIFEST, LABEL_STREAM_MANIFEST, LABEL_HASH_LIST } from './crypto.js';
import { parseManifest, planWith, buildLayout, ManifestError } from './manifest.js';

export class VolumeError extends Error {
    constructor(code, message) {
        super(message);
        this.code = code;
    }
}

const dec = new TextDecoder('utf-8', { fatal: true });

export function equalBytes(a, b) {
    if (a.length !== b.length)
        return false;
    for (let i = 0; i < a.length; i++) {
        if (a[i] !== b[i])
            return false;
    }
    return true;
}

export async function openVolume(meta, masterKey) {
    const plan = planWith(meta.total, meta.chunkSize);
    if (!plan || plan.count !== meta.chunks)
        throw new VolumeError('bad_meta', 'Сервер описал том неправдоподобно — размеры не сходятся.');

    const keys = await deriveKeys(masterKey);

    // Раздача с хешами на лету: манифест промежуточный и лежит под своей
    // меткой, списка пока нет — он приедет сегментами по WebSocket.
    const label = meta.stream ? LABEL_STREAM_MANIFEST : LABEL_MANIFEST;
    const plain = await openChunk(keys.meta, meta.noncePrefix, label, plan.count, meta.manifest);
    if (!plain) {
        throw new VolumeError('bad_key',
            'Манифест не расшифровался: либо ключ в ссылке не тот, либо сервер описал том '
            + 'иначе, чем отправитель, — размер тома входит в проверяемые данные.');
    }

    let manifest;
    try {
        manifest = parseManifest(dec.decode(plain));
    } catch (e) {
        throw new VolumeError('bad_manifest', 'Манифест не разобрался: '
            + (e instanceof ManifestError ? e.message : 'не UTF-8'));
    }
    if (manifest.total !== meta.total)
        throw new VolumeError('bad_manifest', 'Размер в манифесте не сходится с размером раздачи.');
    // Сервер сказал «хеши на лету», а манифест говорит обратное (или
    // наоборот) — это чужой или подменённый манифест.
    if (manifest.stream !== meta.stream)
        throw new VolumeError('bad_manifest', 'Манифест не той формы, какую описал сервер, — том не принимаем.');

    // Список хешей. Пока корень не сошёлся, списку верить нельзя, а без
    // него нельзя проверить ни одного чанка.
    let hashes = null;
    if (!meta.stream) {
        const list = await openChunk(keys.meta, meta.noncePrefix, LABEL_HASH_LIST, plan.count, meta.hashList);
        if (!list)
            throw new VolumeError('bad_hashes', 'Список хешей не расшифровался.');
        if (list.length !== plan.count * 32)
            throw new VolumeError('bad_hashes', 'Список хешей не той длины.');
        if (!equalBytes(blake3(list), manifest.root))
            throw new VolumeError('bad_hashes', 'Корневой хеш не сошёлся со списком хешей — том не принимаем.');
        hashes = list;
    }

    return { keys, plan, manifest, layout: buildLayout(manifest), hashes };
}
