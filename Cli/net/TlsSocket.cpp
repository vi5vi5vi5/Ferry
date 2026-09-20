#include "Cli/net/TlsSocket.h"

#include <cstring>
#include <vector>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
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

// Откуда контекст берёт доверие.
//
// Linux отдаёт пустой список, и тогда работает штатный путь OpenSSL: он
// сам знает, где у системы лежат корни, и пересобирать за него этот список
// значило бы разойтись с системным доверием.
//
// Windows отдаёт список настоящий, потому что взять его больше негде.
// Статический OpenSSL носит в себе путь, по которому его собирали, — это
// каталог внутри докер-образа, и на машине пользователя его нет. Без этой
// ветки клиент под Windows отвергает ЛЮБОЙ сертификат, в том числе
// безупречный: домен открывается в браузере, а ferry говорит про
// «unable to get local issuer certificate», и человек идёт ставить
// --insecure на ровном месте.
bool loadTrustAnchors(SSL_CTX *ctx, std::string *err)
{
    const std::vector<std::string> roots = platform::systemRootCertificates();

    if (roots.empty()) {
        if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
            if (err)
                *err = "не найдено хранилище корневых сертификатов системы";
            return false;
        }
        return true;
    }

    X509_STORE *store = SSL_CTX_get_cert_store(ctx);
    size_t added = 0;
    for (const std::string &der : roots) {
        const auto *p = reinterpret_cast<const unsigned char *>(der.data());
        X509 *cert = d2i_X509(nullptr, &p, long(der.size()));
        if (!cert)
            continue;   // системное хранилище хранит всякое; молча мимо
        if (X509_STORE_add_cert(store, cert) == 1)
            ++added;
        X509_free(cert);
    }
    // Дубликаты и негодные записи складывают ошибки в очередь OpenSSL.
    // Очередь общая на поток: не вычистив её, мы показали бы эти ошибки
    // человеку при следующем настоящем сбое, и они увели бы в сторону.
    ERR_clear_error();

    if (added == 0) {
        if (err)
            *err = "в системном хранилище не нашлось ни одного корневого сертификата";
        return false;
    }
    return true;
}

} // namespace

bool waitFor(platform::Socket s, bool forRead, bool forWrite, int timeoutMs, bool *readable,
             bool *writable)
{
    return platform::waitSocket(s, forRead, forWrite, timeoutMs, readable, writable);
}

TlsSocket::~TlsSocket()
{
    close();
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
        if (!loadTrustAnchors(ctx, err))
            return false;
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

    // SSL_set_fd принимает int и на Windows тоже: дескрипторы сокетов там
    // хоть и UINT_PTR, но помещаются в int — так делает и сам OpenSSL.
    SSL_set_fd(ssl, int(m_socket));

    for (;;) {
        const int rc = SSL_connect(ssl);
        if (rc == 1)
            return true;

        const int sslErr = SSL_get_error(ssl, rc);
        if (sslErr == SSL_ERROR_WANT_READ || sslErr == SSL_ERROR_WANT_WRITE) {
            bool ready = false;
            const bool forRead = (sslErr == SSL_ERROR_WANT_READ);
            if (!waitFor(m_socket, forRead, !forRead, timeoutMs, forRead ? &ready : nullptr,
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

    m_socket = platform::connectTcp(host, port, timeoutMs, err);
    if (m_socket == platform::kInvalidSocket)
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
    if (!isOpen() || len == 0)
        return -1;

    if (!m_tls) {
        const int n = platform::recvSocket(m_socket, buf, len);
        if (n > 0)
            return n;
        if (n == 0) {
            m_error = "соединение закрыто другой стороной";
            return -1;
        }
        const int e = platform::lastNetError();
        if (platform::wouldBlock(e) || platform::interrupted(e)) {
            m_wantRead = true;
            return 0;
        }
        m_error = platform::netErrorText(e);
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
    case SSL_ERROR_SYSCALL: {
        const int e = platform::lastNetError();
        if (platform::wouldBlock(e) || platform::interrupted(e)) {
            m_wantRead = true;
            return 0;
        }
        m_error = e ? platform::netErrorText(e) : std::string("соединение оборвалось");
        return -1;
    }
    default:
        m_error = opensslError();
        return -1;
    }
}

size_t TlsSocket::pending() const
{
    if (!m_tls || !m_ssl)
        return 0;
    const int n = SSL_pending(static_cast<SSL *>(m_ssl));
    return n > 0 ? size_t(n) : 0;
}

int TlsSocket::write(const void *buf, size_t len)
{
    m_wantRead = m_wantWrite = false;
    if (!isOpen())
        return -1;
    if (len == 0)
        return 0;

    if (!m_tls) {
        const int n = platform::sendSocket(m_socket, buf, len);
        if (n > 0)
            return n;
        const int e = platform::lastNetError();
        if (platform::wouldBlock(e) || platform::interrupted(e)) {
            m_wantWrite = true;
            return 0;
        }
        m_error = platform::netErrorText(e);
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
    case SSL_ERROR_SYSCALL: {
        const int e = platform::lastNetError();
        if (platform::wouldBlock(e) || platform::interrupted(e)) {
            m_wantWrite = true;
            return 0;
        }
        m_error = e ? platform::netErrorText(e) : std::string("соединение оборвалось");
        return -1;
    }
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
    if (m_socket != platform::kInvalidSocket) {
        platform::closeSocket(m_socket);
        m_socket = platform::kInvalidSocket;
    }
}

} // namespace ferry::net
