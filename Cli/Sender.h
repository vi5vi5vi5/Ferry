#pragma once

#include "Cli/Options.h"
#include "Cli/Relay.h"

namespace ferry::cli {

// `ferry send <файл>`: посчитать хеши, завести раздачу, напечатать ссылку
// и дальше жить источником, пока человек не остановит.
//
// Отправитель отдаёт байт один раз (§1) — за это отвечает сервер. Наша
// сторона сделки: читать с диска лениво, срезами, и слать ровно то, что
// сервер попросил в need, ни чанком больше.
int runSend(const Options &options, const Relay &relay);

} // namespace ferry::cli
