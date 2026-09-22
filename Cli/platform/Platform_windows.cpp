#include "Cli/platform/Platform.h"
#include "Cli/platform/Threads.h"

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
#include <wincrypt.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>

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

    // ВВОД тоже в UTF-8. Отдельным вызовом, и это не дублирование: кодовые
    // страницы ввода и вывода в Windows независимы. Без этой строки на
    // вопрос «[д/н]» буква «д» приезжает в кодировке консоли (обычно
    // CP866) и не совпадает ни с чем — приглашение обещает «д», а
    // работает только «y».
    //
    // Полностью на неё, впрочем, не полагаемся: askYesNo принимает и
    // CP866, и CP1251 — терминалы по ssh бывают настроены как угодно.
    ::SetConsoleCP(CP_UTF8);

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

std::vector<std::string> systemRootCertificates()
{
    std::vector<std::string> out;

    // Два хранилища, а не одно. ROOT — это корни: то, чем в итоге
    // заканчивается любая цепочка. CA — промежуточные; они нужны потому,
    // что не всякий сервер присылает свою цепочку целиком, и без них
    // проверка спотыкается на «нет локально доверенного сертификата»,
    // хотя доверять на самом деле есть чему.
    //
    // CertOpenSystemStoreW, а не CertOpenStore с CERT_SYSTEM_STORE_*:
    // системное хранилище пользователя уже включает в себя машинное,
    // то есть и корпоративные корни, розданные политиками, тоже попадут.
    for (const wchar_t *name : {L"ROOT", L"CA"}) {
        const HCERTSTORE store = ::CertOpenSystemStoreW(0, name);
        if (!store)
            continue;
        PCCERT_CONTEXT ctx = nullptr;
        while ((ctx = ::CertEnumCertificatesInStore(store, ctx)) != nullptr) {
            if (ctx->pbCertEncoded && ctx->cbCertEncoded > 0)
                out.emplace_back(reinterpret_cast<const char *>(ctx->pbCertEncoded),
                                 size_t(ctx->cbCertEncoded));
        }
        // Второй аргумент 0: закрыть, даже если кто-то ещё держит ссылки.
        // Их не держит никто — контексты мы скопировали в строки.
        ::CertCloseStore(store, 0);
    }
    return out;
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

// FILETIME — это сотни наносекунд с 1601 года. Переводим в unix-секунды:
// манифест хранит время в одном виде на всех системах.
int64_t unixFromFileTime(const FILETIME &ft)
{
    const uint64_t ticks = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    if (ticks == 0)
        return 0;
    return int64_t(ticks / 10000000ull) - 11644473600ll;
}

// Точка переразбора на Windows — это не обязательно ссылка. Файл OneDrive,
// который ещё не скачан, — тоже точка переразбора, и таких у людей на рабочем
// столе может быть весь каталог. Считать их ссылками и молча не взять в том —
// худшее из возможного: человек отправит папку и получит пустоту. Читаем их
// как обычные файлы: первое чтение потянет содержимое с облака — ровно так
// же, как при обычном копировании в проводнике.
//
// tag — метка из WIN32_FIND_DATAW::dwReserved0; 0 означает «не знаем».
#ifndef FILE_ATTRIBUTE_RECALL_ON_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif
#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003
#endif
#ifndef IO_REPARSE_TAG_SYMLINK
#define IO_REPARSE_TAG_SYMLINK 0xA000000C
#endif
bool reparseIsLink(DWORD attrs, DWORD tag)
{
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
        return false;
    if (attrs & (FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS))
        return false;   // облачный файл, содержимое подтянется при чтении
    if (tag != 0)
        return tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT;
    return true;
}

bool fileStat(const std::string &path, FileInfo &out)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(toWidePath(path).c_str(), GetFileExInfoStandard, &data))
        return false;
    out.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    out.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out.isSymlink = reparseIsLink(data.dwFileAttributes, 0);
    out.isRegular = !out.isDirectory && !out.isSymlink;
    out.mtime = unixFromFileTime(data.ftLastWriteTime);
    return true;
}

bool listDirectory(const std::string &path, std::vector<DirEntry> &out)
{
    out.clear();
    const std::wstring pattern = toWidePath(path) + L"\\*";

    WIN32_FIND_DATAW fd{};
    const HANDLE h = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                        FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    do {
        const std::wstring wname = fd.cFileName;
        if (wname == L"." || wname == L"..")
            continue;
        DirEntry e;
        e.name = toUtf8(wname);
        e.isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.isSymlink = reparseIsLink(fd.dwFileAttributes, fd.dwReserved0);
        e.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        e.mtime = unixFromFileTime(fd.ftLastWriteTime);
        out.push_back(std::move(e));
    } while (::FindNextFileW(h, &fd));

    ::FindClose(h);
    return true;
}

bool removeTree(const std::string &path)
{
    FileInfo info;
    if (!fileStat(path, info))
        return false;

    // Точку переразбора сносим как есть, внутрь не заходим: иначе
    // уборка одного каталога могла бы унести совсем другой.
    if (info.isDirectory && !info.isSymlink) {
        std::vector<DirEntry> entries;
        if (!listDirectory(path, entries))
            return false;
        bool ok = true;
        for (const DirEntry &e : entries)
            ok = removeTree(path + "\\" + e.name) && ok;
        return ::RemoveDirectoryW(toWidePath(path).c_str()) != 0 && ok;
    }
    if (info.isDirectory)
        return ::RemoveDirectoryW(toWidePath(path).c_str()) != 0;

    // С файла снимаем атрибут «только чтение»: иначе DeleteFile
    // откажет, и удаление встанет на пустом месте.
    const std::wstring w = toWidePath(path);
    const DWORD attrs = ::GetFileAttributesW(w.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
        ::SetFileAttributesW(w.c_str(), attrs & ~DWORD(FILE_ATTRIBUTE_READONLY));
    return ::DeleteFileW(w.c_str()) != 0;
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

bool consoleStdinIsTty()
{
    return ::_isatty(::_fileno(stdin)) != 0;
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

std::string configDirPath()
{
    const std::string file = configFilePath();
    size_t cut = std::string::npos;
    for (size_t i = 0; i < file.size(); ++i) {
        if (file[i] == '/' || file[i] == '\\')
            cut = i;
    }
    return cut == std::string::npos ? std::string() : file.substr(0, cut);
}

std::string executablePath()
{
    std::wstring buf(MAX_PATH * 4, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    if (n == 0 || n >= buf.size())
        return {};
    buf.resize(n);
    return toUtf8(buf);
}

bool removeFromUserPath(const std::string &dir)
{
    // PATH пользователя живёт в HKCU\\Environment. install.ps1 дописывает
    // туда свой каталог — значит убрать его наша обязанность, иначе
    // после деинсталляции осталась бы запись на несуществующий путь.
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ | KEY_WRITE, &key)
        != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD bytes = 0;
    if (::RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS
        || bytes == 0) {
        ::RegCloseKey(key);
        return false;
    }
    std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
    if (::RegQueryValueExW(key, L"Path", nullptr, &type,
                           reinterpret_cast<LPBYTE>(value.data()), &bytes)
        != ERROR_SUCCESS) {
        ::RegCloseKey(key);
        return false;
    }
    value.resize(::wcslen(value.c_str()));

    // Разбираем по точке с запятой и собираем обратно без нашего.
    // Сравнение без учёта регистра и без хвостового слэша: в PATH путь
    // мог оказаться записан иначе, чем мы его сейчас видим.
    const std::wstring needle = toWidePath(dir);
    const auto norm = [](std::wstring t) {
        while (!t.empty() && (t.back() == L'\\' || t.back() == L'/'))
            t.pop_back();
        for (wchar_t &c : t)
            c = wchar_t(::towlower(c));
        return t;
    };
    const std::wstring want = norm(needle);

    std::wstring rebuilt;
    bool removed = false;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find(L';', start);
        if (end == std::wstring::npos)
            end = value.size();
        std::wstring part = value.substr(start, end - start);
        if (!part.empty() && norm(part) == want) {
            removed = true;
        } else if (!part.empty()) {
            if (!rebuilt.empty())
                rebuilt += L';';
            rebuilt += part;
        }
        start = end + 1;
    }

    if (removed) {
        ::RegSetValueExW(key, L"Path", 0, REG_EXPAND_SZ,
                         reinterpret_cast<const BYTE *>(rebuilt.c_str()),
                         DWORD((rebuilt.size() + 1) * sizeof(wchar_t)));
        // Без этого уже открытые окна будут видеть старый PATH до
        // перезахода в систему.
        ::SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                              reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 3000,
                              nullptr);
    }
    ::RegCloseKey(key);
    return removed;
}

// Аргумент для cmd.exe в кавычках. Путь может содержать пробелы, а
// пропущенные кавычки превратили бы «Program Files» в два аргумента и
// удалили бы не то.
std::wstring quoteArg(const std::wstring &text)
{
    return L"\"" + text + L"\"";
}

bool removeSelf(const std::string &exePath, bool *deferred)
{
    // Windows держит образ запущенного процесса и удалить его не даст.
    // Переименовать, впрочем, даёт — этим и пользуемся: сначала
    // убираем файл с его имени (с этого мгновения `ferry` в PATH уже
    // нет), а потом оставляем поручение добить остаток после
    // нашего выхода.
    if (deferred)
        *deferred = true;

    const std::wstring w = toWidePath(exePath);
    std::wstring tmp = w + L".uninstall";
    ::DeleteFileW(tmp.c_str());
    if (!::MoveFileExW(w.c_str(), tmp.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        // Переименовать не вышло — будем удалять по исходному имени.
        tmp = w;
    }

    // Поручение: подождать, пока мы закончим, и снести файл вместе с
    // каталогом, если тот опустеет. rmdir без /s — нарочно: если
    // человек положил туда своё, это его собственность.
    std::wstring dir = w;
    while (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/')
        dir.pop_back();
    if (!dir.empty())
        dir.pop_back();

    std::wstring cmd = L"/c timeout /t 2 /nobreak >nul & del /f /q " + quoteArg(tmp);
    if (!dir.empty())
        cmd += L" & rmdir " + quoteArg(dir);

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"open";
    info.lpFile = L"cmd.exe";
    info.lpParameters = cmd.c_str();
    info.nShow = SW_HIDE;
    if (!::ShellExecuteExW(&info))
        return false;
    if (info.hProcess)
        ::CloseHandle(info.hProcess);
    return true;
}

// ------------------------------------------------------------------
//  Потоки (Threads.h)
//
//  SRWLOCK и CONDITION_VARIABLE — по одному указателю, и нулевой — это их
//  начальное состояние. Поэтому в заголовке лежит void*, а здесь —
//  приведение к настоящему типу, того же размера.
// ------------------------------------------------------------------

static_assert(sizeof(SRWLOCK) == sizeof(void *), "SRWLOCK — один указатель");
static_assert(sizeof(CONDITION_VARIABLE) == sizeof(void *), "CONDITION_VARIABLE — один указатель");

unsigned cpuCount()
{
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? unsigned(si.dwNumberOfProcessors) : 1u;
}

void sleepMs(int ms)
{
    ::Sleep(DWORD(ms > 0 ? ms : 0));
}

void Mutex::lock()
{
    ::AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&m_srw));
}

void Mutex::unlock()
{
    ::ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&m_srw));
}

void CondVar::wait(Mutex &m)
{
    ::SleepConditionVariableSRW(reinterpret_cast<PCONDITION_VARIABLE>(&m_cv),
                                reinterpret_cast<PSRWLOCK>(&m.m_srw), INFINITE, 0);
}

void CondVar::notifyOne()
{
    ::WakeConditionVariable(reinterpret_cast<PCONDITION_VARIABLE>(&m_cv));
}

void CondVar::notifyAll()
{
    ::WakeAllConditionVariable(reinterpret_cast<PCONDITION_VARIABLE>(&m_cv));
}

unsigned long __stdcall Thread::entry(void *self)
{
    static_cast<Thread *>(self)->m_fn();
    return 0;
}

bool Thread::start(std::function<void()> fn)
{
    m_fn = std::move(fn);
    m_handle = ::CreateThread(nullptr, 0, &Thread::entry, this, 0, nullptr);
    return m_handle != nullptr;
}

void Thread::join()
{
    if (!m_handle)
        return;
    ::WaitForSingleObject(HANDLE(m_handle), INFINITE);
    ::CloseHandle(HANDLE(m_handle));
    m_handle = nullptr;
}

} // namespace ferry::platform

#endif // _WIN32
