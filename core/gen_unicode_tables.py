#!/usr/bin/env python3
# Генерирует core/UnicodeTables.inc — таблицы для NFC в core/Unicode.cpp.
#
#   python3 core/gen_unicode_tables.py > core/UnicodeTables.inc
#
# Данные берутся из unicodedata того Python, которым запущено; версия
# Unicode пишется в шапку файла. Пары композиции стабильны (Unicode
# Normalization Stability Policy), поэтому перегенерировать нужно только
# ради новых письменностей.
import sys
import unicodedata as u

pairs = []
for cp in range(0x110000):
    if 0xAC00 <= cp <= 0xD7A3:
        continue  # хангыль — алгоритмически, в Unicode.cpp
    d = u.decomposition(chr(cp))
    if not d or d.startswith('<'):
        continue
    parts = [int(x, 16) for x in d.split()] + [0]
    # Собирать обратно можно только первичные составные. Синглтоны (знак
    # ангстрема, CJK-совместимые иероглифы) и исключённые из композиции
    # (क़, ώ с оксией) NFC только разбирает.
    primary = parts[1] != 0 and u.normalize('NFC', chr(parts[0]) + chr(parts[1])) == chr(cp)
    pairs.append((cp, parts[0], parts[1], 'true' if primary else 'false'))

ranges = []
for cp in range(0x110000):
    c = u.combining(chr(cp))
    if not c:
        continue
    if ranges and ranges[-1][1] == cp - 1 and ranges[-1][2] == c:
        ranges[-1][1] = cp
    else:
        ranges.append([cp, cp, c])

out = sys.stdout
out.write('// Сгенерировано core/gen_unicode_tables.py из Unicode %s. Не править руками.\n\n'
          % u.unidata_version)
out.write('// Каноническое разложение: знак → (первый, второй или 0, собирается ли\n')
out.write('// обратно), по возрастанию знака.\n')
out.write('static const Decomposition kDecompositions[] = {\n')
for i in range(0, len(pairs), 3):
    out.write('    ' + ' '.join('{0x%04X, 0x%04X, 0x%04X, %s},' % p for p in pairs[i:i + 3]) + '\n')
out.write('};\n\n')
out.write('// Класс канонического сочетания (ccc) всех знаков, у которых он не 0.\n')
out.write('static const CombiningRange kCombining[] = {\n')
for i in range(0, len(ranges), 4):
    out.write('    ' + ' '.join('{0x%04X, 0x%04X, %d},' % tuple(r) for r in ranges[i:i + 4]) + '\n')
out.write('};\n')
