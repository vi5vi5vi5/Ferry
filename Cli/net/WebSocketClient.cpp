#include "Cli/net/WebSocketClient.h"

#include <cstring>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace ferry::net {
namespace {

constexpr uint8_t kOpContinuation = 0x0;
constexpr uint8_t kOpText = 0x1;
constexpr uint8_t kOpBinary = 0x2;
constexpr uint8_t kOpClose = 0x8;
constexpr uint8_t kOpPing = 0x9;
constexpr uint8_t kOpPong = 0xA;

// Обычный base64 (не url): именно его требует рукопожатие WebSocket.
std::string base64(const uint8_t *data, size_t len)
{
    static const char *alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
        i += 3;
    }
    const size_t rest = len - i;
    if (rest == 1) {
        const uint32_t v = uint32_t(data[i]) << 16;
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += "==";
    } else if (rest == 2) {
        const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

std::string sha1Base64(const std::string &input)
{
    unsigned char digest[20];
    unsigned int len = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
    EVP_DigestUpdate(ctx, input.data(), input.size());
    EVP_DigestFinal_ex(ctx, digest, &len);
    EVP_MD_CTX_free(ctx);
    return base64(digest, len);
}

std::string toLower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

bool WebSocketClient::connectTo(const std::string &host, uint16_t port, bool tls, bool insecure,
                                const std::string &path, int timeoutMs, std::string *err)
{
    if (!m_socket.connectTo(host, port, tls, insecure, timeoutMs, err))
        return false;

    uint8_t keyRaw[16];
    if (RAND_bytes(keyRaw, sizeof(keyRaw)) != 1) {
        if (err)
            *err = "генератор случайных чисел недоступен";
        return false;
    }
    const std::string key = base64(keyRaw, sizeof(keyRaw));

    std::string request;
    request += "GET " + path + " HTTP/1.1\r\n";
    // Порт в Host нужен только нестандартный: иначе nginx с server_name
    // не узнает свой же виртуальный хост.
    const bool defaultPort = (tls && port == 443) || (!tls && port == 80);
    request += "Host: " + host + (defaultPort ? "" : ":" + std::to_string(port)) + "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: " + key + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n";
    request += "User-Agent: ferry-cli\r\n";
    request += "\r\n";

    // Рукопожатие пишем и читаем синхронно: без него дальше всё равно
    // делать нечего, а логика проще на целый порядок.
    size_t sent = 0;
    while (sent < request.size()) {
        const int n = m_socket.write(request.data() + sent, request.size() - sent);
        if (n < 0) {
            if (err)
                *err = "не удалось отправить запрос на смену протокола: " + m_socket.error();
            return false;
        }
        if (n == 0) {
            bool ready = false;
            waitFor(m_socket.fd(), m_socket.wantRead(), !m_socket.wantRead(), timeoutMs, &ready,
                    &ready);
            continue;
        }
        sent += size_t(n);
    }

    std::string response;
    for (;;) {
        char buf[4096];
        const int n = m_socket.read(buf, sizeof(buf));
        if (n < 0) {
            if (err)
                *err = "соединение оборвалось на рукопожатии: " + m_socket.error();
            return false;
        }
        if (n == 0) {
            bool ready = false;
            if (!waitFor(m_socket.fd(), true, false, timeoutMs, &ready, nullptr) || !ready) {
                if (err)
                    *err = "сервер не ответил на смену протокола";
                return false;
            }
            continue;
        }
        response.append(buf, size_t(n));

        const size_t headEnd = response.find("\r\n\r\n");
        if (headEnd == std::string::npos) {
            if (response.size() > 16384) {
                if (err)
                    *err = "сервер ответил чем-то, не похожим на HTTP";
                return false;
            }
            continue;
        }

        const std::string head = response.substr(0, headEnd);
        // Хвост после заголовков — это уже первые фреймы: сервер вправе
        // начать говорить сразу, и потерять их нельзя.
        m_in = response.substr(headEnd + 4);

        if (head.compare(0, 9, "HTTP/1.1 ") != 0 || head.compare(9, 3, "101") != 0) {
            if (err) {
                const size_t lineEnd = head.find("\r\n");
                *err = "сервер отказал в смене протокола: " + head.substr(0, lineEnd);
            }
            return false;
        }

        const std::string lower = toLower(head);
        const size_t acceptPos = lower.find("sec-websocket-accept:");
        if (acceptPos == std::string::npos) {
            if (err)
                *err = "в ответе нет Sec-WebSocket-Accept";
            return false;
        }
        size_t valueStart = head.find(':', acceptPos) + 1;
        while (valueStart < head.size() && (head[valueStart] == ' ' || head[valueStart] == '\t'))
            ++valueStart;
        size_t valueEnd = head.find("\r\n", valueStart);
        if (valueEnd == std::string::npos)
            valueEnd = head.size();
        const std::string got = head.substr(valueStart, valueEnd - valueStart);

        // Магическая строка из RFC 6455. Проверка не косметическая: без неё
        // мы бы приняли за WebSocket любой прокси, ответивший «101».
        const std::string expected = sha1Base64(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        if (got != expected) {
            if (err)
                *err = "Sec-WebSocket-Accept не сошёлся — на том конце не WebSocket";
            return false;
        }
        break;
    }

    m_closed = false;
    // В m_in мог остаться хвост от рукопожатия — разбираем сразу.
    return parseFrames();
}

void WebSocketClient::enqueueFrame(uint8_t opcode, const uint8_t *data, size_t len)
{
    if (m_closed)
        return;

    std::string frame;
    frame.reserve(len + 14);
    frame += char(0x80 | opcode);   // FIN + опкод; фрагментировать нам незачем

    // Маска обязательна для клиента (RFC 6455 §5.3). Сервер, получивший
    // немаскированный фрейм, обязан разорвать соединение.
    uint8_t mask[4];
    if (RAND_bytes(mask, sizeof(mask)) != 1)
        std::memset(mask, 0x5A, sizeof(mask));   // хуже, но не тишина

    if (len < 126) {
        frame += char(0x80 | uint8_t(len));
    } else if (len <= 0xFFFF) {
        frame += char(0x80 | 126);
        frame += char((len >> 8) & 0xFF);
        frame += char(len & 0xFF);
    } else {
        // Чанк в 4 МиБ живёт именно здесь — это не редкая ветка.
        frame += char(0x80 | 127);
        for (int i = 7; i >= 0; --i)
            frame += char((uint64_t(len) >> (8 * i)) & 0xFF);
    }
    frame.append(reinterpret_cast<const char *>(mask), 4);

    const size_t payloadStart = frame.size();
    frame.resize(payloadStart + len);
    for (size_t i = 0; i < len; ++i)
        frame[payloadStart + i] = char(data[i] ^ mask[i & 3]);

    m_pending += frame.size();
    m_out.push_back(std::move(frame));
}

void WebSocketClient::sendText(const std::string &text)
{
    enqueueFrame(kOpText, reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

void WebSocketClient::sendBinary(const uint8_t *data, size_t len)
{
    enqueueFrame(kOpBinary, data, len);
}

bool WebSocketClient::flushOutgoing()
{
    while (!m_out.empty()) {
        const std::string &front = m_out.front();
        const int n = m_socket.write(front.data() + m_outOffset, front.size() - m_outOffset);
        if (n < 0) {
            m_error = m_socket.error();
            return false;
        }
        if (n == 0)
            return true;   // сокет полон — вернёмся на следующем обороте

        m_outOffset += size_t(n);
        m_pending -= size_t(n);
        if (m_outOffset >= front.size()) {
            m_out.pop_front();
            m_outOffset = 0;
        }
    }
    return true;
}

bool WebSocketClient::parseFrames()
{
    for (;;) {
        if (m_in.size() < 2)
            return true;

        const auto *p = reinterpret_cast<const uint8_t *>(m_in.data());
        const bool fin = (p[0] & 0x80) != 0;
        const uint8_t opcode = p[0] & 0x0F;
        const bool masked = (p[1] & 0x80) != 0;
        uint64_t len = p[1] & 0x7F;
        size_t header = 2;

        if (len == 126) {
            if (m_in.size() < 4)
                return true;
            len = (uint64_t(p[2]) << 8) | p[3];
            header = 4;
        } else if (len == 127) {
            if (m_in.size() < 10)
                return true;
            len = 0;
            for (int i = 0; i < 8; ++i)
                len = (len << 8) | p[2 + i];
            header = 10;
        }

        if (masked) {
            // Сервер маскировать не имеет права. Если маскирует — это не
            // наш сервер, и продолжать разговор нельзя.
            m_error = "сервер прислал маскированный фрейм — так не бывает";
            return false;
        }
        if (len > kMaxFrame) {
            m_error = "сервер прислал фрейм неправдоподобного размера";
            return false;
        }
        if (m_in.size() < header + len)
            return true;   // тело ещё не всё пришло

        const std::string payload = m_in.substr(header, size_t(len));
        m_in.erase(0, header + size_t(len));

        switch (opcode) {
        case kOpPing:
            enqueueFrame(kOpPong, reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
            break;
        case kOpPong:
            break;
        case kOpClose:
            // Отвечаем тем же и закрываемся: держать сокет после close —
            // верный способ получить полуоткрытое соединение навсегда.
            if (!m_closed) {
                enqueueFrame(kOpClose, reinterpret_cast<const uint8_t *>(payload.data()),
                             payload.size());
                m_closed = true;
            }
            return true;
        case kOpText:
        case kOpBinary:
            if (!fin) {
                m_fragActive = true;
                m_fragBinary = (opcode == kOpBinary);
                m_frag = payload;
            } else {
                m_ready.push_back({opcode == kOpBinary, payload});
            }
            break;
        case kOpContinuation:
            if (!m_fragActive) {
                m_error = "продолжение без начала сообщения";
                return false;
            }
            if (m_frag.size() + payload.size() > kMaxFrame) {
                m_error = "склеенное сообщение неправдоподобного размера";
                return false;
            }
            m_frag += payload;
            if (fin) {
                m_ready.push_back({m_fragBinary, std::move(m_frag)});
                m_frag.clear();
                m_fragActive = false;
            }
            break;
        default:
            m_error = "неизвестный тип фрейма";
            return false;
        }
    }
}

bool WebSocketClient::pump(int timeoutMs)
{
    if (!m_socket.isOpen())
        return false;

    // Ждём только того, что нам действительно нужно: если очередь пуста,
    // просыпаться по готовности к записи незачем — так poll не крутился бы
    // вхолостую на полной скорости.
    const bool wantWrite = !m_out.empty();
    bool readable = false, writable = false;
    if (!waitFor(m_socket.fd(), true, wantWrite, timeoutMs, &readable, &writable)) {
        m_error = "ошибка ожидания на сокете";
        return false;
    }

    if (readable) {
        for (;;) {
            char buf[65536];
            const int n = m_socket.read(buf, sizeof(buf));
            if (n < 0) {
                m_error = m_socket.error();
                return false;
            }
            if (n == 0)
                break;
            m_in.append(buf, size_t(n));
            if (m_in.size() > kMaxFrame * 2) {
                m_error = "входящий буфер переполнен";
                return false;
            }
            // Разбираем по ходу: иначе на быстром канале буфер рос бы
            // быстрее, чем мы его читаем.
            if (!parseFrames())
                return false;
            if (size_t(n) < sizeof(buf))
                break;
        }
    }

    if (!parseFrames())
        return false;

    if (!flushOutgoing())
        return false;

    if (m_closed && m_out.empty())
        return false;

    return true;
}

bool WebSocketClient::next(WsMessage &out)
{
    if (m_ready.empty())
        return false;
    out = std::move(m_ready.front());
    m_ready.pop_front();
    return true;
}

void WebSocketClient::closeGracefully()
{
    if (m_closed || !m_socket.isOpen())
        return;
    const uint8_t normal[2] = {0x03, 0xE8};   // код 1000
    enqueueFrame(kOpClose, normal, sizeof(normal));
    m_closed = true;
    // Дать последнему фрейму уйти. Долго не ждём: если сокет не берёт
    // два байта за полсекунды, на том конце уже никого нет.
    for (int i = 0; i < 50 && !m_out.empty(); ++i) {
        bool writable = false;
        waitFor(m_socket.fd(), false, true, 10, nullptr, &writable);
        if (!flushOutgoing())
            break;
    }
    m_socket.close();
}

} // namespace ferry::net
