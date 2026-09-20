#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "Cli/net/TlsSocket.h"

// Минимальный клиент WebSocket (RFC 6455) поверх TlsSocket.
//
// Почему свой, а не Qt: клиент Ferry должен ставиться одним файлом, а
// QWebSocket тянет на машину двадцать мегабайт Qt. Всё, что нам нужно от
// протокола, — рукопожатие, фрейминг, маскирование и ping/pong; это
// пятьсот строк, написанных один раз.
//
// Три вещи, на которых спотыкается почти каждая самописная реализация, и
// которые здесь сделаны явно:
//   1) клиент ОБЯЗАН маскировать свои фреймы, сервер — обязан не маскировать;
//   2) полезная нагрузка от 64 КиБ требует 64-битной длины, а чанк у нас
//      бывает 4 МиБ — то есть эта ветка не экзотика, а основной путь;
//   3) сервер вправе фрагментировать сообщение, и собирать его должен
//      клиент (nginx этого не делает, но Qt однажды может).
namespace ferry::net {

struct WsMessage
{
    bool binary = false;
    std::string data;
};

class WebSocketClient
{
public:
    bool connectTo(const std::string &host, uint16_t port, bool tls, bool insecure,
                   const std::string &path, int timeoutMs, std::string *err);

    // Один оборот цикла: ждёт готовности до timeoutMs, читает всё, что
    // пришло, и дописывает очередь. false — соединение кончилось.
    bool pump(int timeoutMs);

    // Достать следующее собранное сообщение. false — пока нечего.
    bool next(WsMessage &out);

    void sendText(const std::string &text);
    void sendBinary(const uint8_t *data, size_t len);

    // Сколько байт стоит в очереди на отправку. Это мера того, поспеваем
    // ли мы за каналом: отправитель по ней решает, читать ли с диска
    // следующий чанк или подождать.
    size_t pendingBytes() const { return m_pending; }

    void closeGracefully();
    bool isOpen() const { return m_socket.isOpen() && !m_closed; }
    const std::string &error() const { return m_error; }

private:
    void enqueueFrame(uint8_t opcode, const uint8_t *data, size_t len);
    bool parseFrames();
    bool flushOutgoing();

    // Потолок на входящий фрейм. Больше 4 МиБ чанка нам не пришлют, но
    // верить в это нельзя: сервер мог оказаться не тем.
    static constexpr size_t kMaxFrame = 64u * 1024u * 1024u;

    // Сколько байт за один оборот pump() забираем из сокета, прежде чем
    // вернуть управление наверх. См. комментарий в pump().
    static constexpr size_t kDrainBudget = 8u * 1024u * 1024u;

    TlsSocket m_socket;
    std::string m_in;
    std::deque<std::string> m_out;
    size_t m_outOffset = 0;
    size_t m_pending = 0;

    // Сборка фрагментированного сообщения.
    bool m_fragActive = false;
    bool m_fragBinary = false;
    std::string m_frag;

    std::deque<WsMessage> m_ready;
    bool m_closed = false;
    std::string m_error;
};

} // namespace ferry::net
