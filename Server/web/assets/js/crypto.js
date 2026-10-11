// Криптография Ferry в браузере — зеркало core/Crypto.cpp поверх WebCrypto.
//
//   K_data = HKDF-SHA256(K, salt="", info="ferry/v1/data")      чанки
//   K_meta = HKDF-SHA256(K, salt="", info="ferry/v1/meta")      манифест и список
//   V      = HKDF-SHA256(K, salt="", info="ferry/v1/verifier")  серверу, в proof
//
//   nonce = [ nonce_prefix : 4 ][ index : 8 BE ]
//   AAD   = [ version : 1 ][ index : 8 BE ][ total_chunks : 8 BE ]
//
// Пустая соль в WebCrypto даёт то же, что блок из 32 нулей в C++: HMAC
// дополняет ключ нулями до размера блока, и пустой ключ с нулевым
// неотличимы. Это проверено тестом против CLI, а не только рассуждением.
//
// WebCrypto есть только в защищённом контексте: https или localhost. На
// голом http по адресу браузер его не даёт вовсе, и приём там невозможен.

export const PROTOCOL_VERSION = 1;
export const GCM_TAG = 16;

export const LABEL_MANIFEST = 0xFFFFFFFFFFFFFFFFn;
export const LABEL_HASH_LIST = 0xFFFFFFFFFFFFFFFEn;
export const LABEL_STREAM_MANIFEST = 0xFFFFFFFFFFFFFFFDn;
export const LABEL_HASH_SEGMENT_BASE = 0xF000000000000000n;
export const hashSegmentLabel = (from) => LABEL_HASH_SEGMENT_BASE + BigInt(from);

const enc = new TextEncoder();

export function cryptoAvailable() {
    return typeof crypto !== 'undefined' && !!crypto.subtle;
}

async function hkdf(master, info) {
    const base = await crypto.subtle.importKey('raw', master, 'HKDF', false, ['deriveBits']);
    const bits = await crypto.subtle.deriveBits(
        { name: 'HKDF', hash: 'SHA-256', salt: new Uint8Array(0), info: enc.encode(info) },
        base, 256);
    return new Uint8Array(bits);
}

// Три ключа из одного K. Сами байты наружу не отдаются: ключи импортируются
// неизвлекаемыми, и даже код этой страницы не может прочитать их обратно.
export async function deriveKeys(master) {
    const [data, meta, verifier] = await Promise.all([
        hkdf(master, 'ferry/v1/data'),
        hkdf(master, 'ferry/v1/meta'),
        hkdf(master, 'ferry/v1/verifier'),
    ]);
    const aes = (raw) => crypto.subtle.importKey('raw', raw, 'AES-GCM', false, ['encrypt', 'decrypt']);
    const keys = {
        data: await aes(data),
        meta: await aes(meta),
        verifier: await crypto.subtle.importKey('raw', verifier,
            { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']),
    };
    data.fill(0);
    meta.fill(0);
    verifier.fill(0);
    return keys;
}

// Ответ на challenge: HMAC-SHA256(V, challenge). Сервер знает V и может
// проверить, но вернуться от V к K не может.
export async function proveKeyOwnership(keys, challenge) {
    return new Uint8Array(await crypto.subtle.sign('HMAC', keys.verifier, challenge));
}

function putBe64(out, off, v) {
    for (let i = 7; i >= 0; i--) {
        out[off + i] = Number(v & 0xffn);
        v >>= 8n;
    }
}

function params(prefix, index, total) {
    const idx = BigInt(index);
    const iv = new Uint8Array(12);
    iv.set(prefix, 0);
    putBe64(iv, 4, idx);
    const aad = new Uint8Array(17);
    aad[0] = PROTOCOL_VERSION;
    putBe64(aad, 1, idx);
    putBe64(aad, 9, BigInt(total));
    return { name: 'AES-GCM', iv, additionalData: aad, tagLength: 128 };
}

// Расшифровать чанк или метаданные. null — тег не сошёлся: открытый текст
// в этом случае не отдаётся вовсе, как и в ядре.
export async function openChunk(key, prefix, index, total, cipher) {
    if (cipher.byteLength < GCM_TAG)
        return null;
    try {
        return new Uint8Array(await crypto.subtle.decrypt(params(prefix, index, total), key, cipher));
    } catch {
        return null;
    }
}

// Обратная операция — нужна сиду. AES-GCM детерминирован, поэтому
// шифротекст выходит байт в байт тем, что прислал отправитель.
export async function sealChunk(key, prefix, index, total, plain) {
    return new Uint8Array(await crypto.subtle.encrypt(params(prefix, index, total), key, plain));
}
