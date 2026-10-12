// Argon2id для режима «пароль вместо ключа» — в отдельном потоке.
//
// 64 МиБ памяти и три прохода — это заметная доля секунды даже в
// WebAssembly, а на телефоне и несколько секунд. В основном потоке
// страница бы замерла, поэтому здесь. Воркер классический, а не модульный:
// hash-wasm собран как UMD и подключается только через importScripts.
//
// Параметры зашиты в протокол версии 1 и совпадают с CLI (Cli/Password.h).
importScripts('vendor/argon2.umd.min.js');

self.onmessage = async (e) => {
    const { id, password, salt } = e.data || {};
    try {
        const key = await self.hashwasm.argon2id({
            password,
            salt,
            parallelism: 1,
            iterations: 3,
            memorySize: 64 * 1024,   // КиБ
            hashLength: 32,
            outputType: 'binary',
        });
        self.postMessage({ id, key }, [key.buffer]);
    } catch (err) {
        self.postMessage({ id, error: String(err && err.message || err) });
    }
};
