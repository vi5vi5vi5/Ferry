// Ссылка и HTTP-ручки релея — зеркало core/Link.cpp и Cli/Relay.cpp.

import { b64urlDecode } from './b64.js';

export const TRANSFER_ID_LEN = 22;   // 16 байт в base64url
export const MASTER_KEY_SIZE = 32;
export const NONCE_PREFIX_SIZE = 4;
export const CHALLENGE_SIZE = 16;

export class RelayError extends Error {
    constructor(code, message) {
        super(message || code);
        this.code = code;
    }
}

export function isValidTransferId(id) {
    if (typeof id !== 'string' || id.length !== TRANSFER_ID_LEN)
        return false;
    const raw = b64urlDecode(id);
    return !!raw && raw.length === 16;
}

// Разбор ссылки. Принимает и полную ссылку, и то, что от неё осталось без
// фрагмента: тогда key === null, и вызывающий говорит об этом вслух.
// null — это вообще не ссылка Ferry.
export function parseLink(text) {
    let s = String(text || '').trim();
    if (!s)
        return null;
    let url;
    try {
        url = new URL(s);
    } catch {
        return null;
    }
    if (url.protocol !== 'https:' && url.protocol !== 'http:')
        return null;
    const path = url.pathname.replace(/\/+$/, '');
    if (!path.startsWith('/t/'))
        return null;
    const id = path.slice(3);
    if (!isValidTransferId(id))
        return null;
    let key = null;
    const frag = url.hash.slice(1);
    if (frag) {
        const raw = b64urlDecode(frag);
        if (!raw || raw.length !== MASTER_KEY_SIZE)
            return { origin: url.origin, id, key: null, badKey: true };
        key = raw;
    }
    return { origin: url.origin, id, key, badKey: false };
}

async function readError(resp) {
    try {
        const body = await resp.json();
        if (body && typeof body.error === 'string')
            return body.error;
    } catch {
        // тело не JSON — значит отвечал не релей, а что-то перед ним
    }
    return '';
}

function decodeField(o, key, want) {
    const raw = b64urlDecode(o[key]);
    if (!raw || (want && raw.length !== want))
        throw new RelayError('bad_reply', `поле ${key} в ответе сервера не разбирается`);
    return raw;
}

// Метаданные раздачи. Ничего не сжигает — ни использования, ни challenge.
// Заголовок говорит, что мы умеем хеши на лету: иначе раздача, у которой
// отправитель ещё считает, ответила бы preparing.
export async function fetchMeta(origin, id) {
    let resp;
    try {
        resp = await fetch(`${origin}/api/transfers/${encodeURIComponent(id)}`, {
            headers: { 'X-Ferry-Features': 'stream_hashes' },
            cache: 'no-store',
            credentials: 'omit',
        });
    } catch (e) {
        throw new RelayError('network', 'сервер не отвечает');
    }
    if (!resp.ok) {
        const code = await readError(resp);
        throw new RelayError(code || `http_${resp.status}`);
    }
    const v = await resp.json();
    const meta = {
        id: v.id,
        total: v.total,
        chunkSize: v.chunk_size,
        chunks: v.chunks,
        mode: v.mode,
        state: v.state,
        receivers: v.receivers | 0,
        usesLeft: typeof v.uses_left === 'number' ? v.uses_left : -1,
        expiresInMs: typeof v.expires_in_ms === 'number' ? v.expires_in_ms : 0,
        requireApproval: !!v.require_approval,
        stream: v.hash_mode === 'stream',
        hashed: Math.max(0, v.hashed | 0),
        manifest: decodeField(v, 'manifest'),
        hashList: v.hash_mode === 'stream' ? null : decodeField(v, 'hash_list'),
        noncePrefix: decodeField(v, 'nonce_prefix', NONCE_PREFIX_SIZE),
    };
    return meta;
}

// Завести раздачу. Черновик живёт минуту: за это время отправитель обязан
// дойти до WebSocket и прислать offer.
export async function createTransfer(origin) {
    let resp;
    try {
        resp = await fetch(`${origin}/api/transfers`, {
            method: 'POST',
            cache: 'no-store',
            credentials: 'omit',
        });
    } catch {
        throw new RelayError('network', 'сервер не отвечает');
    }
    if (!resp.ok) {
        const code = await readError(resp);
        throw new RelayError(code || `http_${resp.status}`);
    }
    const v = await resp.json();
    if (!isValidTransferId(v.id) || typeof v.owner_token !== 'string')
        throw new RelayError('bad_reply', 'сервер ответил не тем');
    return {
        id: v.id,
        ownerToken: v.owner_token,
        publicUrl: typeof v.public_url === 'string' ? v.public_url.replace(/\/+$/, '') : '',
        features: Array.isArray(v.features) ? v.features : [],
    };
}

// Challenge одноразовый и живёт две минуты, поэтому берётся прямо перед
// подключением, а не заранее.
export async function fetchChallenge(origin, id) {
    let resp;
    try {
        resp = await fetch(`${origin}/api/transfers/${encodeURIComponent(id)}/challenge`, {
            cache: 'no-store',
            credentials: 'omit',
        });
    } catch {
        throw new RelayError('network', 'сервер не отвечает');
    }
    if (!resp.ok) {
        const code = await readError(resp);
        throw new RelayError(code || `http_${resp.status}`);
    }
    const v = await resp.json();
    const raw = b64urlDecode(v.challenge);
    if (!raw || raw.length !== CHALLENGE_SIZE)
        throw new RelayError('bad_reply', 'сервер прислал challenge не той длины');
    return raw;
}

export function wsUrl(origin) {
    return origin.replace(/^http/, 'ws') + '/wsf';
}
