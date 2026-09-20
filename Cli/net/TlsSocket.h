#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "Cli/platform/Platform.h"

// TCP + TLS на OpenSSL. Один сокет, неблокирующий после установки
// соединения.
//
// Почему неблокирующий, хотя клиент однопоточный: отправителю надо
// одновременно лить чанки и слушать команды сервера. На блокирующей записи
// мы бы залипли с полным буфером и не прочитали бы очередной need — а
// именно он говорит, что слать дальше. Поэтому наружу торчит дескриптор и
// явные флаги «жду чтения / жду записи», а цикл событий живёт этажом выше,
// в WebSocketClient.
//
// Всё, что отличается между Linux и Windows, вынесено в platform::.
// Здесь остаётся только OpenSSL — один и тот же на обеих системах, и это
// не экономия усилий, а требование: вторая реализация криптографии
// означала бы вторую возможность разойтись в форматах.
namespace ferry::net {

class TlsSocket
{
public:
    TlsSocket() = default;
    ~TlsSocket();
    TlsSocket(const TlsSocket &) = delete;
    TlsSocket &operator=(const TlsSocket &) = delete;

    // Соединение и рукопожатие целиком, с таймаутом. Здесь блокирующий
    // режим уместен: без соединения делать всё равно нечего.
    //
    // insecure отключает проверку сертификата. Это нужно, пока у релея нет
    // домена и сертификат самоподписанный, и это ослабление: с ним
    // посредник может подменить сервер. Ключ шифрования тома при этом не
    // утекает (он никогда не уходит с устройства), но подменённый сервер
    // может выдать чужой том — поэтому вызывающий обязан сказать об этом
    // человеку вслух.
    bool connectTo(const std::string &host, uint16_t port, bool tls, bool insecure, int timeoutMs,
                   std::string *err);

    // >0 — прочитано байт; 0 — сейчас данных нет (см. wantRead/wantWrite);
    // -1 — соединение кончилось или сломалось.
    int read(void *buf, size_t len);
    int write(const void *buf, size_t len);

    // После read/write == 0: чего именно ждёт TLS. Да, SSL_write может
    // захотеть ЧТЕНИЯ (пересогласование), и наоборот — это не опечатка,
    // а то место, на котором спотыкается почти каждый самописный клиент.
    bool wantRead() const { return m_wantRead; }
    bool wantWrite() const { return m_wantWrite; }

    platform::Socket handle() const { return m_socket; }
    bool isOpen() const { return m_socket != platform::kInvalidSocket; }

    void close();

    // Последняя ошибка человеческими словами.
    const std::string &error() const { return m_error; }

private:
    bool doHandshake(const std::string &host, bool insecure, int timeoutMs, std::string *err);

    platform::Socket m_socket = platform::kInvalidSocket;
    void *m_ssl = nullptr;   // SSL*
    void *m_ctx = nullptr;   // SSL_CTX*
    bool m_tls = false;
    bool m_wantRead = false;
    bool m_wantWrite = false;
    std::string m_error;
};

// Подождать готовности сокета. timeoutMs < 0 — ждать сколько угодно.
// Возвращает true, если что-то произошло (или таймаут вышел без ошибки).
bool waitFor(platform::Socket s, bool forRead, bool forWrite, int timeoutMs, bool *readable,
             bool *writable);

} // namespace ferry::net
