#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Cli/net/HttpClient.h"
#include "Cli/net/WebSocketClient.h"
#include "core/Types.h"

// Адрес релея и четыре запроса к его API. Всё, что клиенту нужно от HTTP;
// дальше разговор идёт по WebSocket.
namespace ferry::cli {

struct Relay
{
    std::string host;
    uint16_t port = 443;
    bool tls = true;
    bool insecure = false;

    std::string origin() const;
    net::HttpTarget target() const;

    // Принимает "ferry.example.ru", "https://ferry.example.ru",
    // "http://localhost:8080" и "1.2.3.4:8443". Схема по умолчанию https:
    // без TLS ключ шифрования тома всё ещё не утекает (он никогда не
    // покидает устройство), но всё остальное — да.
    static bool parse(const std::string &text, bool insecure, Relay &out, std::string *err);
};

struct CreatedTransfer
{
    std::string id;
    std::string ownerToken;   // base64url, как отдал сервер
    std::string publicUrl;    // каким адресом сервер представляется снаружи

    // Что умеет релей. Старый релей этого поля не шлёт — список пуст, и
    // отправитель считает хеши заранее, как раньше.
    std::vector<std::string> features;
    bool hasFeature(const std::string &name) const
    {
        for (const std::string &f : features)
            if (f == name)
                return true;
        return false;
    }
};

bool apiCreateTransfer(const Relay &relay, CreatedTransfer &out, std::string *err);

struct TransferMeta
{
    uint64_t total = 0;
    uint32_t chunkSize = 0;
    uint64_t chunks = 0;
    Bytes manifest;     // шифротекст
    Bytes hashList;     // шифротекст; пуст, пока хеши едут на лету

    // Отправитель ещё считает хеши: манифест промежуточный, без корня,
    // а список приедет сегментами по WebSocket. hashed — сколько чанков
    // подряд с начала уже посчитано.
    bool streamHashes = false;
    uint64_t hashed = 0;
    Bytes noncePrefix;
    std::string mode;
    std::string state;
    int receivers = 0;
    int usesLeft = -1;
    int64_t expiresInMs = 0;
};

bool apiFetchMeta(const Relay &relay, const std::string &id, TransferMeta &out, std::string *err);
bool apiFetchChallenge(const Relay &relay, const std::string &id, Bytes &out, std::string *err);

// Вытащить причину из очереди уже закрывшегося соединения.
//
// Нужно потому, что сервер отвечает ошибкой и тут же закрывает сокет:
// pump() возвращает false, хотя сообщение с причиной уже прочитано и лежит
// в очереди. Без этого человек вместо «ссылкой уже воспользовались»
// получал бы пустое «соединение оборвалось» — то есть ровно ничего.
//
// Пустая строка — сервер действительно не сказал ничего.
std::string drainErrorReason(net::WebSocketClient &ws);

// Понятная человеку расшифровка кода ошибки из §8.
std::string explainError(const std::string &code);

} // namespace ferry::cli
