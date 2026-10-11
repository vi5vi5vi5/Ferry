// Чтение тома отправителя из объектов File — общее для хешера и отправки.
//
// Том — конкатенация файлов описи (manifest.js: buildLayout), поэтому
// чанк папки может лежать в нескольких файлах сразу. File.slice() ничего
// не читает до arrayBuffer(), и в память попадает ровно запрошенный
// диапазон, каким бы большим ни был файл.

import { slice } from './manifest.js';

export async function readRange(files, layout, from, length) {
    const pieces = slice(layout, from, length);
    const out = pieces.length === 1 ? null : new Uint8Array(length);
    let at = 0;
    for (const p of pieces) {
        const part = new Uint8Array(await files[p.file].slice(p.offset, p.offset + p.length).arrayBuffer());
        if (part.length !== p.length)
            throw new Error(`файл ${layout.files[p.file].path} стал короче, чем был при выборе`);
        if (!out)
            return part;
        out.set(part, at);
        at += part.length;
    }
    if (at !== length)
        throw new Error('том стал короче, чем был при выборе');
    return out;
}

// Человеческая причина. Chrome бросает NotReadableError, когда файл
// изменили или удалили после выбора: объект File помнит размер и время
// изменения и сверяет их при каждом чтении.
export function readProblem(e) {
    const name = e && e.name;
    if (name === 'NotReadableError' || name === 'NotFoundError')
        return 'файл изменился или пропал после выбора — выберите его заново';
    return (e && e.message) || String(e);
}
