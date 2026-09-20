#include "Cli/platform/Platform.h"

#ifndef _WIN32

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace ferry::platform {

// ---------- запуск ----------

void init()
{
    // На POSIX готовить нечего: консоль и так в UTF-8, ANSI и так работает.
}

void shutdown() {}

std::vector<std::string> arguments(int argc, char **argv)
{
    std::vector<std::string> out;
    out.reserve(size_t(argc));
    for (int i = 0; i < argc; ++i)
        out.emplace_back(argv[i]);
    return out;
}

// ---------- сокеты ----------

void netInit() {}
void netShutdown() {}

Socket connectTcp(const std::string &host, uint16_t port, int timeoutMs, std::string *err)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;   // и IPv4, и IPv6
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *res = nullptr;
    const std::string portText = std::to_string(port);
    const int rc = ::getaddrinfo(host.c_str(), portText.c_str(), &hints, &res);
    if (rc != 0 || !res) {
        if (err)
            *err = "не удалось разрешить имя " + host + ": " + ::gai_strerror(rc);
        return kInvalidSocket;
    }

    std::string lastError = "нет подходящих адресов";
    for (addrinfo *a = res; a; a = a->ai_next) {
        const int fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0)
            continue;

        setNonBlocking(Socket(fd));
        int rcConnect = ::connect(fd, a->ai_addr, a->ai_addrlen);
        if (rcConnect < 0 && inProgress(lastNetError())) {
            bool writable = false;
            if (waitSocket(Socket(fd), false, true, timeoutMs, nullptr, &writable) && writable) {
                const int soErr = socketPendingError(Socket(fd));
                if (soErr == 0)
                    rcConnect = 0;
                else
                    lastError = netErrorText(soErr);
            } else {
                lastError = "таймаут соединения";
            }
        } else if (rcConnect < 0) {
            lastError = netErrorText(lastNetError());
        }

        if (rcConnect == 0) {
            ::freeaddrinfo(res);
            // Наши управляющие сообщения мелкие и частые (need, ack), а
            // чанки крупные. Алгоритм Нэйгла склеивал бы мелкие в пакеты
            // по 200 мс, и управление ползло бы вслед за данными.
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            return Socket(fd);
        }

        ::close(fd);
    }

    ::freeaddrinfo(res);
    if (err)
        *err = "не удалось соединиться с " + host + ": " + lastError;
    return kInvalidSocket;
}

void setNonBlocking(Socket s)
{
    const int flags = ::fcntl(int(s), F_GETFL, 0);
    if (flags >= 0)
        ::fcntl(int(s), F_SETFL, flags | O_NONBLOCK);
}

void closeSocket(Socket s)
{
    if (s >= 0)
        ::close(int(s));
}

int lastNetError()
{
    return errno;
}

bool wouldBlock(int err)
{
    return err == EAGAIN || err == EWOULDBLOCK;
}

bool inProgress(int err)
{
    return err == EINPROGRESS;
}

bool interrupted(int err)
{
    return err == EINTR;
}

std::string netErrorText(int err)
{
    return std::strerror(err);
}

int socketPendingError(Socket s)
{
    int soErr = 0;
    socklen_t len = sizeof(soErr);
    if (::getsockopt(int(s), SOL_SOCKET, SO_ERROR, &soErr, &len) != 0)
        return errno;
    return soErr;
}

int recvSocket(Socket s, void *buf, size_t len)
{
    return int(::recv(int(s), buf, len, 0));
}

int sendSocket(Socket s, const void *buf, size_t len)
{
    // MSG_NOSIGNAL: без него закрытая труба убивала бы процесс сигналом
    // прямо посреди раздачи. Ignore на SIGPIPE стоит и в Signals.cpp —
    // здесь подстраховка на случай, если обработчик не поставили.
    return int(::send(int(s), buf, len, MSG_NOSIGNAL));
}

bool waitSocket(Socket s, bool forRead, bool forWrite, int timeoutMs, bool *readable,
                bool *writable)
{
    if (readable)
        *readable = false;
    if (writable)
        *writable = false;
    if (s < 0)
        return false;

    pollfd p{};
    p.fd = int(s);
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

// ---------- файлы ----------

File fileOpenRead(const std::string &path)
{
    const int fd = ::open(path.c_str(), O_RDONLY);
    return fd < 0 ? kInvalidFile : File(fd);
}

File fileOpenReadWrite(const std::string &path)
{
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    return fd < 0 ? kInvalidFile : File(fd);
}

void fileClose(File f)
{
    if (f != kInvalidFile)
        ::close(int(f));
}

int64_t fileReadAt(File f, void *buf, size_t len, uint64_t offset)
{
    return ::pread(int(f), buf, len, off_t(offset));
}

int64_t fileWriteAt(File f, const void *buf, size_t len, uint64_t offset)
{
    return ::pwrite(int(f), buf, len, off_t(offset));
}

bool fileTruncate(File f, uint64_t size)
{
    return ::ftruncate(int(f), off_t(size)) == 0;
}

bool fileSync(File f)
{
    return ::fsync(int(f)) == 0;
}

bool fileStat(const std::string &path, FileInfo &out)
{
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0)
        return false;
    out.size = uint64_t(st.st_size);
    out.isDirectory = S_ISDIR(st.st_mode);
    return true;
}

bool fileExists(const std::string &path)
{
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

bool fileRemove(const std::string &path)
{
    return ::unlink(path.c_str()) == 0;
}

bool fileRename(const std::string &from, const std::string &to)
{
    // POSIX rename заменяет цель молча — ровно то поведение, которое на
    // Windows приходится просить отдельным флагом.
    return ::rename(from.c_str(), to.c_str()) == 0;
}

bool makeDirectories(const std::string &path)
{
    std::string current;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!current.empty() && ::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST)
                return false;
        }
        if (i < path.size())
            current += path[i];
    }
    return true;
}

// ---------- консоль ----------

bool consoleIsTty()
{
    return ::isatty(STDOUT_FILENO) == 1;
}

bool consoleStdinIsTty()
{
    return ::isatty(STDIN_FILENO) == 1;
}

int consoleWidth()
{
    winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20)
        return int(ws.ws_col);
    return 80;
}

bool consoleSupportsAnsi()
{
    return consoleIsTty();
}

// ---------- пути ----------

std::string configFilePath()
{
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"))
        if (*xdg)
            return std::string(xdg) + "/ferry/config";
    if (const char *home = std::getenv("HOME"))
        if (*home)
            return std::string(home) + "/.config/ferry/config";
    return {};
}

} // namespace ferry::platform

#endif // !_WIN32
