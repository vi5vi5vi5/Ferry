#include "Cli/platform/Platform.h"

#ifdef _WIN32

// Порядок включений здесь не вкусовщина: winsock2.h обязан идти до
// windows.h, иначе последний втащит древний winsock.h версии 1 и сборка
// развалится на переопределениях.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <io.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>

namespace ferry::platform {
namespace {

// ------------------------------------------------------------------
//  UTF-8 <-> UTF-16
//
//  Граница кодировок проходит ровно здесь. Всё, что торчит из Platform.h,
//  говорит в UTF-8; всё, что уходит в Windows API, — в UTF-16. Ни одна
//  «A»-функция (CreateFileA, GetCommandLineA) в этом файле не зовётся, и
//  это осознанно: они работают в кодировке системы, и `съёмка.mov` через
//  них превращается в файл, которого потом не найти.
// ------------------------------------------------------------------

std::wstring toWide(const std::string &utf8)
{
    if (utf8.empty())
        return {};
    const int needed =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    if (needed <= 0)
        return {};
    std::wstring out(size_t(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), needed);
    return out;
}

std::string toUtf8(const std::wstring &wide)
{
    if (wide.empty())
        return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0,
                                             nullptr, nullptr);
    if (needed <= 0)
        return {};
    std::string out(size_t(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), out.data(), needed, nullptr,
                          nullptr);
    return out;
}

// Путь для Windows API. Прямые слэши система понимает и сама, но
// приводим к единому виду: иначе «D:/видео\файл.mov» из аргументов
// командной строки выглядел бы по-разному в сообщениях об ошибках.
std::wstring toWidePath(const std::string &utf8)
{
    std::wstring w = toWide(utf8);
    for (wchar_t &c : w) {
        if (c == L'/')
            c = L'\\';
    }
    return w;
}

bool g_ansiEnabled = false;

} // namespace

// ------------------------------------------------------------------
//  Запуск
// ------------------------------------------------------------------

void init()
{
    netInit();

    // Вывод в UTF-8: без этого рамки, карта тома и кириллица превращаются
    // в кашу даже в Windows Terminal.
    ::SetConsoleOutputCP(CP_UTF8);

    // Обработка ANSI-последовательностей. В Windows 10 и новее она есть,
    // но по умолчанию выключена, и включать её надо явно. На старом
    // conhost включить не выйдет — тогда клиент честно печатает простым
    // текстом, как при перенаправлении вывода в файл.
    const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (::GetConsoleMode(out, &mode)) {
            if (::SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
                g_ansiEnabled = true;
        }
    }
}

void shutdown()
{
    netShutdown();
}

std::vector<std::string> arguments(int argc, char **argv)
{
    // argv от рантайма приходит в кодировке системы (обычно CP1251), и имя
    // файла кириллицей в нём уже испорчено — восстановить его оттуда
    // нельзя. Настоящую командную строку отдаёт только GetCommandLineW.
    int wideCount = 0;
    LPWSTR *wideArgv = ::CommandLineToArgvW(::GetCommandLineW(), &wideCount);
    if (!wideArgv) {
        std::vector<std::string> fallback;
        fallback.reserve(size_t(argc));
        for (int i = 0; i < argc; ++i)
            fallback.emplace_back(argv[i]);
        return fallback;
    }

    std::vector<std::string> out;
    out.reserve(size_t(wideCount));
    for (int i = 0; i < wideCount; ++i)
        out.push_back(toUtf8(wideArgv[i]));
    ::LocalFree(wideArgv);
    return out;
}

// ------------------------------------------------------------------
//  Сокеты
// ------------------------------------------------------------------

void netInit()
{
    static bool done = false;
    if (done)
        return;
    WSADATA data{};
    ::WSAStartup(MAKEWORD(2, 2), &data);
    done = true;
}

void netShutdown()
{
    // WSACleanup намеренно не зовём: процесс всё равно заканчивается, а
    // вызов на фоне живых сокетов иногда подвешивает выход.
}

Socket connectTcp(const std::string &host, uint16_t port, int timeoutMs, std::string *err)
{
    netInit();

    addrinfoW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    // Широкий вариант getaddrinfo: имя хоста может быть каким угодно, а
    // «A»-функции работают в кодировке системы.
    PADDRINFOW res = nullptr;
    const std::wstring wideHost = toWide(host);
    const std::wstring widePort = toWide(std::to_string(port));
    const int rc = ::GetAddrInfoW(wideHost.c_str(), widePort.c_str(), &hints, &res);
    if (rc != 0 || !res) {
        if (err)
            *err = "не удалось разрешить имя " + host + ": " + netErrorText(rc);
        return kInvalidSocket;
    }

    std::string lastError = "нет подходящих адресов";
    for (PADDRINFOW a = res; a; a = a->ai_next) {
        const SOCKET fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd == INVALID_SOCKET)
            continue;

        setNonBlocking(Socket(fd));
        int rcConnect = ::connect(fd, a->ai_addr, int(a->ai_addrlen));
        if (rcConnect == SOCKET_ERROR && inProgress(lastNetError())) {
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
        } else if (rcConnect == SOCKET_ERROR) {
            lastError = netErrorText(lastNetError());
        }

        if (rcConnect == 0) {
            ::FreeAddrInfoW(res);
            // Мелкие управляющие сообщения не должны ждать, пока Нэйгл
            // наберёт из них пакет.
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one),
                         sizeof(one));
            return Socket(fd);
        }

        ::closesocket(fd);
    }

    ::FreeAddrInfoW(res);
    if (err)
        *err = "не удалось соединиться с " + host + ": " + lastError;
    return kInvalidSocket;
}

void setNonBlocking(Socket s)
{
    u_long mode = 1;
    ::ioctlsocket(SOCKET(s), FIONBIO, &mode);
}

void closeSocket(Socket s)
{
    if (s != kInvalidSocket)
        ::closesocket(SOCKET(s));
}

int lastNetError()
{
    return ::WSAGetLastError();
}

bool wouldBlock(int err)
{
    return err == WSAEWOULDBLOCK;
}

bool inProgress(int err)
{
    // Неблокирующий connect на Windows сообщает о себе тем же кодом, что и
    // «данных пока нет», — отдельного EINPROGRESS здесь нет.
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
}

bool interrupted(int)
{
    // Прерывания сигналом в Winsock не бывает.
    return false;
}

std::string netErrorText(int err)
{
    LPWSTR buffer = nullptr;
    const DWORD len = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, DWORD(err), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (!len || !buffer)
        return "ошибка сети " + std::to_string(err);

    std::wstring text(buffer, len);
    ::LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
        text.pop_back();
    return toUtf8(text);
}

int socketPendingError(Socket s)
{
    int soErr = 0;
    int len = sizeof(soErr);
    if (::getsockopt(SOCKET(s), SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soErr), &len) != 0)
        return ::WSAGetLastError();
    return soErr;
}

int recvSocket(Socket s, void *buf, size_t len)
{
    return ::recv(SOCKET(s), static_cast<char *>(buf), int(len), 0);
}

int sendSocket(Socket s, const void *buf, size_t len)
{
    return ::send(SOCKET(s), static_cast<const char *>(buf), int(len), 0);
}

bool waitSocket(Socket s, bool forRead, bool forWrite, int timeoutMs, bool *readable,
                bool *writable)
{
    if (readable)
        *readable = false;
    if (writable)
        *writable = false;
    if (s == kInvalidSocket)
        return false;

    // select, а не WSAPoll. WSAPoll не сообщает об ошибке неудачного
    // connect (документированный и до сих пор не закрытый баг): вместо
    // «соединиться не удалось» клиент просто висел бы до таймаута. У
    // select для этого есть exceptfds, и он честно срабатывает.
    //
    // Ограничение select на FD_SETSIZE нам не мешает: мы всегда ждём
    // ровно один сокет.
    fd_set readSet, writeSet, exceptSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    FD_ZERO(&exceptSet);

    const SOCKET native = SOCKET(s);
    if (forRead)
        FD_SET(native, &readSet);
    if (forWrite)
        FD_SET(native, &writeSet);
    FD_SET(native, &exceptSet);

    timeval tv{};
    timeval *tvp = nullptr;
    if (timeoutMs >= 0) {
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        tvp = &tv;
    }

    const int rc = ::select(0, forRead ? &readSet : nullptr, forWrite ? &writeSet : nullptr,
                            &exceptSet, tvp);
    if (rc == SOCKET_ERROR)
        return false;
    if (rc == 0)
        return true;   // просто таймаут

    const bool failed = FD_ISSET(native, &exceptSet) != 0;
    if (readable)
        *readable = (forRead && FD_ISSET(native, &readSet)) || failed;
    if (writable)
        *writable = (forWrite && FD_ISSET(native, &writeSet)) || failed;
    return true;
}

// ------------------------------------------------------------------
//  Файлы
// ------------------------------------------------------------------

File fileOpenRead(const std::string &path)
{
    const HANDLE h = ::CreateFileW(toWidePath(path).c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    return h == INVALID_HANDLE_VALUE ? kInvalidFile : File(h);
}

File fileOpenReadWrite(const std::string &path)
{
    const HANDLE h = ::CreateFileW(toWidePath(path).c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
    return h == INVALID_HANDLE_VALUE ? kInvalidFile : File(h);
}

void fileClose(File f)
{
    if (f != kInvalidFile)
        ::CloseHandle(HANDLE(f));
}

int64_t fileReadAt(File f, void *buf, size_t len, uint64_t offset)
{
    // OVERLAPPED на обычном (не асинхронном) дескрипторе — законный способ
    // прочитать по смещению, не трогая общий курсор файла. Вызов при этом
    // остаётся синхронным, и это ровно то, что нам нужно: аналог pread.
    OVERLAPPED ov{};
    ov.Offset = DWORD(offset & 0xFFFFFFFFull);
    ov.OffsetHigh = DWORD(offset >> 32);

    DWORD got = 0;
    if (!::ReadFile(HANDLE(f), buf, DWORD(len), &got, &ov)) {
        // Конец файла при чтении по смещению приходит именно так.
        if (::GetLastError() == ERROR_HANDLE_EOF)
            return 0;
        return -1;
    }
    return int64_t(got);
}

int64_t fileWriteAt(File f, const void *buf, size_t len, uint64_t offset)
{
    OVERLAPPED ov{};
    ov.Offset = DWORD(offset & 0xFFFFFFFFull);
    ov.OffsetHigh = DWORD(offset >> 32);

    DWORD written = 0;
    if (!::WriteFile(HANDLE(f), buf, DWORD(len), &written, &ov))
        return -1;
    return int64_t(written);
}

bool fileTruncate(File f, uint64_t size)
{
    LARGE_INTEGER pos{};
    pos.QuadPart = LONGLONG(size);
    if (!::SetFilePointerEx(HANDLE(f), pos, nullptr, FILE_BEGIN))
        return false;
    return ::SetEndOfFile(HANDLE(f)) != 0;
}

bool fileSync(File f)
{
    return ::FlushFileBuffers(HANDLE(f)) != 0;
}

bool fileStat(const std::string &path, FileInfo &out)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(toWidePath(path).c_str(), GetFileExInfoStandard, &data))
        return false;
    out.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    out.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return true;
}

bool fileExists(const std::string &path)
{
    return ::GetFileAttributesW(toWidePath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool fileRemove(const std::string &path)
{
    return ::DeleteFileW(toWidePath(path).c_str()) != 0;
}

bool fileRename(const std::string &from, const std::string &to)
{
    // MOVEFILE_REPLACE_EXISTING обязателен: без него MoveFile падает, если
    // цель существует, и докачка навсегда осталась бы лежать рядом с ней
    // под именем .ferry-part. На POSIX rename так себя ведёт по умолчанию.
    return ::MoveFileExW(toWidePath(from).c_str(), toWidePath(to).c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)
           != 0;
}

bool makeDirectories(const std::string &path)
{
    const std::wstring full = toWidePath(path);
    std::wstring current;
    for (size_t i = 0; i <= full.size(); ++i) {
        if (i == full.size() || full[i] == L'\\') {
            // «C:» само по себе не каталог — создавать его не пытаемся.
            const bool isDriveRoot = current.size() == 2 && current[1] == L':';
            if (!current.empty() && !isDriveRoot) {
                if (!::CreateDirectoryW(current.c_str(), nullptr)
                    && ::GetLastError() != ERROR_ALREADY_EXISTS)
                    return false;
            }
        }
        if (i < full.size())
            current += full[i];
    }
    return true;
}

// ------------------------------------------------------------------
//  Консоль
// ------------------------------------------------------------------

bool consoleIsTty()
{
    return ::_isatty(::_fileno(stdout)) != 0;
}

int consoleWidth()
{
    CONSOLE_SCREEN_BUFFER_INFO info{};
    const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE && ::GetConsoleScreenBufferInfo(out, &info)) {
        const int cols = info.srWindow.Right - info.srWindow.Left + 1;
        if (cols > 20)
            return cols;
    }
    return 80;
}

bool consoleSupportsAnsi()
{
    return consoleIsTty() && g_ansiEnabled;
}

// ------------------------------------------------------------------
//  Пути
// ------------------------------------------------------------------

std::string configFilePath()
{
    // %APPDATA% — то место, куда на Windows кладут настройки приложений.
    // Читаем его широкой функцией: путь содержит имя пользователя, а оно
    // вполне может быть кириллическим.
    wchar_t buffer[MAX_PATH * 2];
    DWORD len = ::GetEnvironmentVariableW(L"APPDATA", buffer, DWORD(std::size(buffer)));
    if (len > 0 && len < std::size(buffer))
        return toUtf8(std::wstring(buffer, len)) + "\\ferry\\config";

    len = ::GetEnvironmentVariableW(L"USERPROFILE", buffer, DWORD(std::size(buffer)));
    if (len > 0 && len < std::size(buffer))
        return toUtf8(std::wstring(buffer, len)) + "\\.ferry\\config";

    return {};
}

} // namespace ferry::platform

#endif // _WIN32
