#pragma once

#include <string>

#include "Types.h"

// Ссылка на раздачу (§4):
//
//   https://ferry.example/t/<id>#<key>
//                          │       └── 32 байта base64url — НИКОГДА не
//                          │           уходит на сервер: фрагмент не
//                          │           попадает ни в запрос, ни в логи,
//                          │           ни в Referer
//                          └────────── 16 байт base64url, публичный id
//
// Здесь только разбор и сборка: случайные байты берутся в Crypto, чтобы
// ядро оставалось без зависимостей.
namespace ferry {

struct TransferLink
{
    std::string scheme = "https";   // http допускаем: локальная отладка
    std::string host;               // с портом, если он нестандартный
    std::string id;                 // base64url, 16 байт
    Key32 key{};                    // K
    bool hasKey = false;            // парольный режим приходит без фрагмента

    std::string origin() const { return scheme + "://" + host; }

    // Полная ссылка. Если ключа нет — без фрагмента.
    std::string toString() const;

    // Разбор. Принимает и полную ссылку, и то, что от неё осталось, если
    // человек потерял фрагмент по дороге (тогда hasKey == false, и
    // вызывающий скажет об этом вслух: без ключа расшифровать нечего).
    static bool parse(const std::string &text, TransferLink &out, std::string *err = nullptr);
};

// id раздачи в ссылке — это base64url от 16 байт, то есть ровно 22 символа
// без выравнивания. Проверка нужна и серверу, и клиенту: id приходит из
// URL, то есть снаружи.
bool isValidTransferId(const std::string &id);

} // namespace ferry
