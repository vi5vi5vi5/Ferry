#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Всё, чем Linux отличается от Windows, собрано здесь и реализовано ровно
// в двух файлах: Platform_posix.cpp и Platform_windows.cpp.
//
// Почему так, а не #ifdef по месту. Ifdef, рассыпанный по Sender.cpp и
// TlsSocket.cpp, означает, что каждую правку надо держать в голове дважды,
// и одна из двух веток рано или поздно перестаёт собираться незаметно.
// Здесь же граница видна глазами: всё, что ниже, имеет две реализации,
// всё остальное — одну.
//
// Правило для того, кто будет добавлять сюда функции: наружу торчат только
// UTF-8-строки. Windows внутри работает с UTF-16, и конверсия живёт по ту
// сторону этой границы — иначе кодировки расползлись бы по всему клиенту.
namespace ferry::platform {

// ------------------------------------------------------------------
//  Запуск
// ------------------------------------------------------------------

// Зовётся первой строкой main. На Windows поднимает Winsock, переводит
// консоль в UTF-8 и включает обработку ANSI-последовательностей.
void init();
void shutdown();

// Аргументы командной строки в UTF-8.
//
// На Windows argv приходит в кодировке системы (обычно CP1251), и
// `ferry send съёмка.mov` дошло бы до нас мусором. Настоящие аргументы
// берутся из GetCommandLineW и конвертируются здесь.
std::vector<std::string> arguments(int argc, char **argv);

// ------------------------------------------------------------------
//  Сокеты
// ------------------------------------------------------------------

// На Windows SOCKET — это UINT_PTR, на POSIX дескриптор int. intptr_t
// вмещает оба, а -1 одинаково означает «недействительный».
using Socket = std::intptr_t;
inline constexpr Socket kInvalidSocket = -1;

void netInit();
void netShutdown();

// Открыть TCP-соединение: разрешение имени, подключение с таймаутом,
// перевод в неблокирующий режим и выключение алгоритма Нэйгла.
//
// Целиком здесь, а не в TlsSocket, по одной причине: getaddrinfo на
// Windows живёт в ws2tcpip.h, который тянет за собой весь winsock, и
// пустить его в общий код значило бы завести #ifdef в файле, который про
// операционные системы знать не должен.
//
// kInvalidSocket — не соединились, причина в err.
Socket connectTcp(const std::string &host, uint16_t port, int timeoutMs, std::string *err);

void setNonBlocking(Socket s);
void closeSocket(Socket s);

int lastNetError();
bool wouldBlock(int err);     // операция не готова прямо сейчас
bool inProgress(int err);     // connect пошёл, ждём готовности к записи
bool interrupted(int err);    // прервано сигналом (на Windows не бывает)
std::string netErrorText(int err);

// Ошибка, накопленная сокетом после неблокирующего connect (SO_ERROR).
int socketPendingError(Socket s);

int recvSocket(Socket s, void *buf, size_t len);
int sendSocket(Socket s, const void *buf, size_t len);

// Подождать готовности ОДНОГО сокета. timeoutMs < 0 — ждать сколько
// угодно. Возвращает false только при настоящей ошибке ожидания; вышедший
// впустую таймаут — это true с обоими флагами в false.
//
// На Windows внутри select, а не WSAPoll, и это важно: WSAPoll не
// сигналит об ошибке неудачного connect (известный и незакрытый баг), из-за
// чего клиент повисал бы на мёртвом адресе до самого таймаута вместо
// внятного «соединиться не удалось».
bool waitSocket(Socket s, bool forRead, bool forWrite, int timeoutMs, bool *readable,
                bool *writable);

// ------------------------------------------------------------------
//  Файлы
// ------------------------------------------------------------------

// HANDLE на Windows, дескриптор на POSIX.
using File = std::intptr_t;
inline constexpr File kInvalidFile = -1;

// Все пути — UTF-8. На Windows внутри они превращаются в UTF-16 и
// открываются через CreateFileW: иначе файл с кириллицей в имени просто
// не находится.
File fileOpenRead(const std::string &path);
File fileOpenReadWrite(const std::string &path);   // создаёт, если нет
void fileClose(File f);

// Чтение и запись ПО СМЕЩЕНИЮ, без общего курсора. Это не прихоть: чанки
// приходят вразнобой (две волны, докачка), и seek+read означал бы гонку,
// как только рядом появится второй поток.
int64_t fileReadAt(File f, void *buf, size_t len, uint64_t offset);
int64_t fileWriteAt(File f, const void *buf, size_t len, uint64_t offset);

bool fileTruncate(File f, uint64_t size);
bool fileSync(File f);

struct FileInfo
{
    uint64_t size = 0;
    bool isDirectory = false;
};
bool fileStat(const std::string &path, FileInfo &out);
bool fileExists(const std::string &path);
bool fileRemove(const std::string &path);

// Переименование С ЗАМЕНОЙ. На Windows обычный rename падает, если цель
// существует, — и недокачка осталась бы лежать рядом с целью навсегда.
bool fileRename(const std::string &from, const std::string &to);

bool makeDirectories(const std::string &path);

// ------------------------------------------------------------------
//  Консоль
// ------------------------------------------------------------------

bool consoleIsTty();
int consoleWidth();

// Умеет ли вывод ANSI-последовательности. На Windows это надо не только
// спросить, но и включить (см. init), и на старом conhost включить не
// получится — тогда клиент честно печатает простым текстом.
bool consoleSupportsAnsi();

// ------------------------------------------------------------------
//  Пути
// ------------------------------------------------------------------

// ~/.config/ferry/config или %APPDATA%\ferry\config. Пусто — не удалось
// понять, где домашний каталог.
std::string configFilePath();

} // namespace ferry::platform
