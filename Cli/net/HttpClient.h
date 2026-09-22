#pragma once

#include <cstdint>
#include <string>

// Минимальный HTTP/1.1-клиент поверх TlsSocket. Ровно столько, сколько
// нужно четырём ручкам API Ferry: один запрос — одно соединение, ответ
// читается по Content-Length, chunked не поддерживается (и не встречается:
// на том конце наш же сервер).
//
// Тел здесь почти нет, кроме одного исключения: зашифрованный список хешей
// для терабайтного тома доходит до мегабайтов. Поэтому потолок ответа
// щедрый, но он есть — верить незнакомому серверу на слово нельзя.
namespace ferry::net {

struct HttpResponse
{
    int status = 0;
    std::string body;
};

struct HttpTarget
{
    std::string host;
    uint16_t port = 443;
    bool tls = true;
    bool insecure = false;
};

bool httpRequest(const HttpTarget &target, const std::string &method, const std::string &path,
                 const std::string &requestBody, HttpResponse &out, std::string *err,
                 int timeoutMs = 20000, const std::string &extraHeaders = std::string());

} // namespace ferry::net
