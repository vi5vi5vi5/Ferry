#pragma once

#include <cstdint>
#include <vector>

// Каноническая композиция Unicode (NFC) без ICU и без Qt: CLI ставится
// одним файлом и не тянет за собой библиотек.
//
// Нужна там, где байты строки должны совпасть с тем, что получит браузер
// через String.prototype.normalize('NFC') — в пароле. macOS охотно отдаёт
// «й» двумя знаками (и + U+0306), а ключ из пароля выводится по байтам.
//
// Полный алгоритм UAX #15: каноническое разложение по таблицам Unicode
// (core/UnicodeTables.inc, генерируется core/gen_unicode_tables.py),
// хангыль формулой, порядок знаков сочетания по классу и сборка обратно.
// Результат совпадает с браузером, пока совпадает версия Unicode.
namespace ferry {

void toNfc(std::vector<uint32_t> &codePoints);

} // namespace ferry
