`argon2.umd.min.js` — Argon2id из hash-wasm 4.12.0 (MIT, Dani Biró), как
есть, без правок. WebAssembly лежит внутри файла строкой base64, поэтому
отдельного `.wasm` и своего MIME-типа релею не нужно. Подключается только
из `argon2.worker.js` через importScripts: это UMD, модулем его не
импортировать.
