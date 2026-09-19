#include "Cli/net/TlsSocket.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

namespace ferry::net {
namespace {

std::string opensslError()
{
    std::string out;
    unsigned long code;
    char buf[256];
    while ((code = ERR_get_error()) != 0) {
        ERR_error_string_n(code, buf, sizeof(buf));
        if (!out.empty())
            out += "; ";
        out += buf;
    }
    return out.empty() ? std::string("неизвестная ошибка TLS") : out;
}

// Инициализация OpenSSL один раз на процесс. В 1.1.0+ она ленивая и сама,
// но явный вызов ничего не стоит и снимает вопрос.
void initOpenSsl()
{
    static bool done = false;
    if (!done) {
        SSL_library_init();
        SSL_load_error_strings();
        done = true;
    }
}

} // namespace

bool waitFor(int fd, bool forRead, bool forWrite, int timeoutMs, bool *readable, bool *writable)
{
    if (readable)
        *readable = false;
    if (writable)
        *writable = false;
    if (fd < 0)
        return false;

    pollfd p{};
    p.fd = fd;
    p.events = short((forRead ? POLLIN : 0) | (forWrite ? POLLOUT : 0));

    int rc;
    do {
        rc = ::poll(&p, 1, timeoutMs);
    } while (rc < 0 && errno == EINTR);   // Ctrl-C и прочие сигналы — не ошибка

    if (rc < 0)
        return false;
    if (rc == 0)
        return true;   // просто таймаут

    if (readable)
        *readable = (p.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
    if (writable)
        *writable = (p.revents & (POLLOUT | POLLERR)) != 0;
    return true;
}

TlsSocket::~TlsSocket()
{
    close();
}

void TlsSocket::setNonBlocking()
{
    const int flags = ::fcntl(m_fd, F_GETFL, 0);
    if (flags >= 0)
        ::fcntl(m_fd, F_SETFL, flags | O_NONBLOCK);
}

bool TlsSocket::doConnect(const std::string &host, uint16_t port, int timeoutMs, std::string *err)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;      // и IPv4, и IPv6
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *res = nullptr;
    const std::string portText = std::to_string(port);
    const int rc = ::getaddrinfo(host.c_str(), portText.c_str(), &hints, &res);
    if (rc != 0 || !res) {
        if (err)
            *err = "не удалось разрешить имя " + host + ": " + gai_strerror(rc);
        return false;
    }

    std::string lastError = "нет подходящих адресов";
    for (addrinfo *a = res; a; a = a->ai_next) {
        m_fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (m_fd < 0)
            continue;

        setNonBlocking();
        int rcConnect = ::connect(m_fd, a->ai_addr, a->ai_addrlen);
        if (rcConnect < 0 && errno == EINPROGRESS) {
            bool writable = false;
            if (waitFor(m_fd, false, true, timeoutMs, nullptr, &writable) && writable) {
                int soErr = 0;
                socklen_t len = sizeof(soErr);
                if (::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &soErr, &len) == 0 && soErr == 0)
                    rcConnect = 0;
                else
                    lastError = std::strerror(soErr ? soErr : ETIMEDOUT);
            } else {
                lastError = "таймаут соединения";
            }
        } else if (rcConnect < 0) {
            lastError = std::strerror(errno);
        }

        if (rcConnect == 0) {
            ::freeaddrinfo(res);
            // Наши сообщения мелкие и частые (need, ack), а чанки крупные.
            // Алгоритм Нэйгла склеивал бы мелкие в пакеты по 200 мс, и
            // управление ползло бы вслед за данными.
            int one = 1;
            ::setsockopt(m_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            return true;
        }

        ::close(m_fd);
        m_fd = -1;
    }

    ::freeaddrinfo(res);
    if (err)
        *err = "не удалось соединиться с " + host + ": " + lastError;
    return false;
}

bool TlsSocket::doHandshake(const std::string &host, bool insecure, int timeoutMs, std::string *err)
{
    initOpenSsl();

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        if (err)
            *err = "не создался контекст TLS: " + opensslError();
        return false;
    }
    m_ctx = ctx;

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    if (insecure) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
            if (err)
                *err = "не найдено хранилище корневых сертификатов системы";
            return false;
        }
    }

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        if (err)
            *err = "не создалось соединение TLS: " + opensslError();
        return false;
    }
    m_ssl = ssl;

    // SNI: без него сервер за общим адресом отдаст сертификат не того сайта.
    SSL_set_tlsext_host_name(ssl, host.c_str());

    if (!insecure) {
        // Проверка имени в сертификате. Отдельно от SSL_VERIFY_PEER: без
        // неё подошёл бы любой сертификат, подписанный любым доверенным
        // центром, — то есть почти любой.
        X509_VERIFY_PARAM *param = SSL_get0_param(ssl);
        X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        if (X509_VERIFY_PARAM_set1_host(param, host.c_str(), 0) != 1) {
            if (err)
                *err = "не удалось задать проверку имени хоста";
            return false;
        }
    }

    SSL_set_fd(ssl, m_fd);

    for (;;) {
        const int rc = SSL_connect(ssl);
        if (rc == 1)
            return true;

        const int sslErr = SSL_get_error(ssl, rc);
        if (sslErr == SSL_ERROR_WANT_READ || sslErr == SSL_ERROR_WANT_WRITE) {
            bool ready = false;
            const bool forRead = (sslErr == SSL_ERROR_WANT_READ);
            if (!waitFor(m_fd, forRead, !forRead, timeoutMs, forRead ? &ready : nullptr,
                         forRead ? nullptr : &ready)
                || !ready) {
                if (err)
                    *err = "таймаут рукопожатия TLS";
                return false;
            }
            continue;
        }

        if (err) {
            const long verify = SSL_get_verify_result(ssl);
            if (verify != X509_V_OK) {
                *err = std::string("сертификат сервера не принят: ")
                       + X509_verify_cert_error_string(verify)
                       + "\nЕсли у релея пока нет домена и сертификат самоподписанный —"
                         " запустите с --insecure.";
            } else {
                *err = "рукопожатие TLS не удалось: " + opensslError();
            }
        }
        return false;
    }
}

bool TlsSocket::connectTo(const std::string &host, uint16_t port, bool tls, bool insecure,
                          int timeoutMs, std::string *err)
{
    close();
    m_tls = tls;

    if (!doConnect(host, port, timeoutMs, err))
        return false;
    if (tls && !doHandshake(host, insecure, timeoutMs, err)) {
        close();
        return false;
    }
    return true;
}

int TlsSocket::read(void *buf, size_t len)
{
    m_wantRead = m_wantWrite = false;
    if (m_fd < 0 || len == 0)
        return -1;

    if (!m_tls) {
        const ssize_t n = ::recv(m_fd, buf, len, 0);
        if (n > 0)
            return int(n);
        if (n == 0) {
            m_error = "соединение закрыто другой стороной";
            return -1;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            m_wantRead = true;
            return 0;
        }
        m_error = std::strerror(errno);
        return -1;
    }

    auto *ssl = static_cast<SSL *>(m_ssl);
    const int n = SSL_read(ssl, buf, int(len));
    if (n > 0)
        return n;

    switch (SSL_get_error(ssl, n)) {
    case SSL_ERROR_WANT_READ:
        m_wantRead = true;
        return 0;
    case SSL_ERROR_WANT_WRITE:
        m_wantWrite = true;
        return 0;
    case SSL_ERROR_ZERO_RETURN:
        m_error = "соединение закрыто другой стороной";
        return -1;
    case SSL_ERROR_SYSCALL:
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            m_wantRead = true;
            return 0;
        }
        m_error = errno ? std::strerror(errno) : "соединение оборвалось";
        return -1;
    default:
        m_error = opensslError();
        return -1;
    }
}

int TlsSocket::write(const void *buf, size_t len)
{
    m_wantRead = m_wantWrite = false;
    if (m_fd < 0)
        return -1;
    if (len == 0)
        return 0;

    if (!m_tls) {
        const ssize_t n = ::send(m_fd, buf, len, MSG_NOSIGNAL);
        if (n > 0)
            return int(n);
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            m_wantWrite = true;
            return 0;
        }
        m_error = std::strerror(errno);
        return -1;
    }

    auto *ssl = static_cast<SSL *>(m_ssl);
    const int n = SSL_write(ssl, buf, int(len));
    if (n > 0)
        return n;

    switch (SSL_get_error(ssl, n)) {
    case SSL_ERROR_WANT_READ:
        // Да, запись может ждать ЧТЕНИЯ: так выглядит пересогласование.
        m_wantRead = true;
        return 0;
    case SSL_ERROR_WANT_WRITE:
        m_wantWrite = true;
        return 0;
    case SSL_ERROR_ZERO_RETURN:
        m_error = "соединение закрыто другой стороной";
        return -1;
    case SSL_ERROR_SYSCALL:
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            m_wantWrite = true;
            return 0;
        }
        m_error = errno ? std::strerror(errno) : "соединение оборвалось";
        return -1;
    default:
        m_error = opensslError();
        return -1;
    }
}

void TlsSocket::close()
{
    if (m_ssl) {
        // Без рукопожатия на закрытие: другая сторона и так увидит FIN, а
        // ждать её ответа на неблокирующем сокете значит зависнуть.
        SSL_free(static_cast<SSL *>(m_ssl));
        m_ssl = nullptr;
    }
    if (m_ctx) {
        SSL_CTX_free(static_cast<SSL_CTX *>(m_ctx));
        m_ctx = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

} // namespace ferry::net
