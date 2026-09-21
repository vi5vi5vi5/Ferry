#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Разбор аргументов и файла настроек ~/.config/ferry/config.
//
// Порядок старшинства обычный: флаг сильнее переменной окружения, она
// сильнее файла. Файл пишет install.sh при установке — именно поэтому
// после `curl … | sh` команда `ferry send file` работает без единого
// флага: адрес релея уже прописан.
namespace ferry::cli {

struct Options
{
    enum class Command { None, Send, Get, Version, Help, Config, Uninstall };

    Command command = Command::None;

    // Общее
    std::string relay;          // "ferry.example.ru" или "https://ferry.example.ru:8443"
    bool insecure = false;      // не проверять сертификат (см. предупреждение в выводе)
    bool noQr = false;
    std::string name;           // как представиться: видно только отправителю

    // send
    std::string path;           // что отдаём
    int uses = -1;              // -1 — без лимита
    int64_t ttlMs = 0;          // 0 — умолчание сервера
    int maxConcurrent = 0;      // 0 — умолчание сервера

    // get
    std::string link;           // полная ссылка
    std::string id;             // либо id и ключ по отдельности
    std::string key;
    std::string outPath;        // куда класть; пусто — рядом, по имени из манифеста
    bool yes = false;           // не спрашивать подтверждения
    bool seed = false;          // после приёма остаться источником для остальных

    std::string error;          // непусто — разбор не удался

    // Аргументы приходят уже в UTF-8: на Windows их достаёт
    // platform::arguments из GetCommandLineW, потому что штатный argv
    // отдан в кодировке системы и кириллица в нём уже испорчена.
    static Options parse(const std::vector<std::string> &args);
};

// Разбор строки вида "1h", "24h", "7d", "90m", "3600" (секунды).
// Возвращает 0, если не разобралось.
int64_t parseDurationMs(const std::string &text);

void printHelp();

} // namespace ferry::cli
