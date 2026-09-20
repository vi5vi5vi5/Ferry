#include "Cli/net/HttpClient.h"

#include <cctype>
#include <cstdlib>

#include "Cli/net/TlsSocket.h"

namespace ferry::net {
namespace {

// 64 МиБ: список хешей терабайтного тома при чанке 4 МиБ — это 8 МБ, так
// что запас десятикратный. Но потолок нужен: иначе сервер, объявивший
// Content-Length в гигабайты, съел бы память клиента одним ответом.
constexpr size_t kMaxResponseBody = 64u * 1024u * 1024u;

std::string toLower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool writeAll(TlsSocket &socket, const std::string &data, int timeoutMs, std::string *err)
{
    size_t sent = 0;
    while (sent < data.size()) {
        const int n = socket.write(data.data() + sent, data.size() - sent);
        if (n < 0) {
            if (err)
                *err = socket.error();
            return false;
        }
        if (n == 0) {
            bool ready = false;
            const bool forRead = socket.wantRead();
            if (!waitFor(socket.handle(), forRead, !forRead, timeoutMs, forRead ? &ready : nullptr,
                         forRead ? nullptr : &ready)
                || !ready) {
                if (err)
                    *err = "таймаут отправки запроса";
                return false;
            }
            continue;
        }
        sent += size_t(n);
    }
    return true;
}

} // namespace

bool httpRequest(const HttpTarget &target, const std::string &method, const std::string &path,
                 const std::string &requestBody, HttpResponse &out, std::string *err,
                 int timeoutMs)
{
    TlsSocket socket;
    if (!socket.connectTo(target.host, target.port, target.tls, target.insecure, timeoutMs, err))
        return false;

    const bool defaultPort = (target.tls && target.port == 443) || (!target.tls && target.port == 80);

    std::string request;
    request += method + " " + path + " HTTP/1.1\r\n";
    request += "Host: " + target.host + (defaultPort ? "" : ":" + std::to_string(target.port)) + "\r\n";
    request += "User-Agent: ferry-cli\r\n";
    request += "Accept: application/json\r\n";
    // Соединение на один запрос: keep-alive сэкономил бы рукопожатие, но
    // запросов у нас три штуки за раздачу, а состояния он добавляет много.
    request += "Connection: close\r\n";
    request += "Content-Length: " + std::to_string(requestBody.size()) + "\r\n";
    request += "\r\n";
    request += requestBody;

    if (!writeAll(socket, request, timeoutMs, err))
        return false;

    std::string response;
    size_t headEnd = std::string::npos;
    long long contentLength = -1;
    bool headParsed = false;

    for (;;) {
        char buf[65536];
        const int n = socket.read(buf, sizeof(buf));
        if (n < 0) {
            // Закрытие после полного тела — штатный конец, а не ошибка.
            if (headParsed && contentLength < 0)
                break;
            if (err)
                *err = "ответ оборвался: " + socket.error();
            return false;
        }
        if (n == 0) {
            bool ready = false;
            const bool forWrite = socket.wantWrite();
            if (!waitFor(socket.handle(), !forWrite, forWrite, timeoutMs, forWrite ? nullptr : &ready,
                         forWrite ? &ready : nullptr)
                || !ready) {
                if (err)
                    *err = "сервер молчит дольше отведённого времени";
                return false;
            }
            continue;
        }
        response.append(buf, size_t(n));

        if (!headParsed) {
            headEnd = response.find("\r\n\r\n");
            if (headEnd == std::string::npos) {
                if (response.size() > 64 * 1024) {
                    if (err)
                        *err = "заголовки ответа неправдоподобно длинные";
                    return false;
                }
                continue;
            }

            const std::string head = response.substr(0, headEnd);
            if (head.compare(0, 5, "HTTP/") != 0) {
                if (err)
                    *err = "это не HTTP-ответ";
                return false;
            }
            const size_t sp = head.find(' ');
            if (sp == std::string::npos) {
                if (err)
                    *err = "в ответе нет кода состояния";
                return false;
            }
            out.status = std::atoi(head.c_str() + sp + 1);

            const std::string lower = toLower(head);
            const size_t clPos = lower.find("content-length:");
            if (clPos != std::string::npos)
                contentLength = std::atoll(head.c_str() + clPos + 15);
            if (contentLength > (long long)kMaxResponseBody) {
                if (err)
                    *err = "ответ сервера неправдоподобно велик";
                return false;
            }
            headParsed = true;
        }

        const size_t haveBody = response.size() - (headEnd + 4);
        if (contentLength >= 0 && haveBody >= size_t(contentLength))
            break;
        if (response.size() > kMaxResponseBody + 64 * 1024) {
            if (err)
                *err = "ответ сервера неправдоподобно велик";
            return false;
        }
    }

    if (!headParsed) {
        if (err)
            *err = "сервер закрыл соединение, не ответив";
        return false;
    }

    out.body = response.substr(headEnd + 4);
    if (contentLength >= 0 && out.body.size() > size_t(contentLength))
        out.body.resize(size_t(contentLength));
    return true;
}

} // namespace ferry::net
