#pragma once

#include <string>

#include "core/Crypto.h"

// Режим «ссылка отдельно, пароль отдельно» (§4 проектного документа).
//
// Ссылка уходит без фрагмента, а ключ выводится из пароля:
//
//   K = Argon2id(пароль, соль, m = 64 МиБ, t = 3, p = 1), 32 байта
//
// Соль — 16 случайных байт, сервер держит её в оперативке и отдаёт всем:
// это соль, она не секрет. Дальше всё как обычно: K_data, K_meta и V
// выводятся из K, и сервер по-прежнему видит только V.
//
// Параметры зашиты в протокол версии 1. Отдавать их по проводу значило
// бы дать серверу сказать получателю «посчитай с памятью в гигабайт».
namespace ferry::cli {

inline constexpr uint32_t kArgon2Iterations = 3;
inline constexpr uint32_t kArgon2MemoryKiB = 64 * 1024;
inline constexpr uint32_t kArgon2Lanes = 1;

// Сколько слов требуем от парольной фразы. Пароль здесь — единственное,
// что стоит между томом и тем, кто перебирает офлайн, поэтому короткие
// не принимаются вовсе.
inline constexpr int kPasswordMinWords = 5;
inline constexpr size_t kPasswordMinChars = 20;

// Пароль диктуют голосом и набирают на другом устройстве, поэтому
// неважное для смысла выравнивается одинаково на обеих сторонах:
// пробелы по краям срезаются, любые пробельные промежутки внутри
// сжимаются в один пробел, латиница и кириллица приводятся к строчным,
// «ё» становится «е». Веб-клиент делает ровно то же (password.js), и
// расходиться им нельзя.
std::string normalizePassword(const std::string &utf8);

// Подходит ли фраза: kPasswordMinWords слов и kPasswordMinChars знаков
// после выравнивания. Пустая причина — подходит.
std::string passwordProblem(const std::string &normalized);

// Ключ из пароля. false — не хватило памяти или соль не той длины.
bool passwordKey(const std::string &normalized, const uint8_t *salt, size_t saltLen, Key32 &out);

} // namespace ferry::cli
