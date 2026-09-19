#include "Cli/ui/Term.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/ioctl.h>
#include <unistd.h>

namespace ferry::ui {
namespace {

// Узкий неразрывный пробел (U+202F) между разрядами и перед единицей.
// Обычный пробел ломал бы копирование числа двойным щелчком, а отсутствие
// разделителя превращает 5368709120 в нечитаемую кашу.
constexpr const char *kThin = " ";

bool envDisablesColor()
{
    // NO_COLOR — договорённость, которую уважают все приличные утилиты:
    // переменная задана (пусть даже пустая) — цветов нет.
    return std::getenv("NO_COLOR") != nullptr;
}

std::string groupDigits(uint64_t value)
{
    std::string digits = std::to_string(value);
    std::string out;
    int since = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (since == 3) {
            out += " ";   // задом наперёд соберётся правильно
            since = 0;
        }
        out += *it;
        ++since;
    }
    // Разворачиваем, но байты многобайтового разделителя переворачивать
    // нельзя — поэтому собираем заново по кодовым точкам.
    std::string reversed;
    reversed.reserve(out.size());
    for (size_t i = out.size(); i > 0;) {
        size_t start = i - 1;
        while (start > 0 && (static_cast<unsigned char>(out[start]) & 0xC0) == 0x80)
            --start;
        reversed.append(out, start, i - start);
        i = start;
    }
    return reversed;
}

std::string fixed1(double v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    // Десятичная запятая: текст русский.
    std::string s = buf;
    const size_t dot = s.find('.');
    if (dot != std::string::npos)
        s[dot] = ',';
    return s;
}

} // namespace

bool isTty()
{
    return ::isatty(STDOUT_FILENO) == 1;
}

int width()
{
    winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20)
        return int(ws.ws_col);
    return 80;
}

bool colorEnabled()
{
    static const bool enabled = isTty() && !envDisablesColor();
    return enabled;
}

const char *reset()  { return colorEnabled() ? "\033[0m" : ""; }
const char *dim()    { return colorEnabled() ? "\033[2m" : ""; }
const char *bold()   { return colorEnabled() ? "\033[1m" : ""; }
const char *accent() { return colorEnabled() ? "\033[36m" : ""; }
const char *ok()     { return colorEnabled() ? "\033[32m" : ""; }
const char *warn()   { return colorEnabled() ? "\033[33m" : ""; }
const char *bad()    { return colorEnabled() ? "\033[31m" : ""; }

std::string bytes(uint64_t value)
{
    // Двоичные приставки, но подписанные привычно: человек, увидевший
    // «5,0 ГБ» у файла в 5 368 709 120 байт, не удивится, а увидевший
    // «5,4 ГБ» — удивится, потому что так показывает проводник.
    static const char *units[] = {"Б", "КБ", "МБ", "ГБ", "ТБ", "ПБ"};
    if (value < 1024)
        return groupDigits(value) + kThin + units[0];

    double v = double(value);
    int unit = 0;
    while (v >= 1024.0 && unit < 5) {
        v /= 1024.0;
        ++unit;
    }
    return fixed1(v) + kThin + units[unit];
}

std::string rate(double bytesPerSecond)
{
    if (bytesPerSecond <= 0 || !std::isfinite(bytesPerSecond))
        return std::string("—");
    static const char *units[] = {"Б/с", "КБ/с", "МБ/с", "ГБ/с"};
    double v = bytesPerSecond;
    int unit = 0;
    while (v >= 1024.0 && unit < 3) {
        v /= 1024.0;
        ++unit;
    }
    return fixed1(v) + kThin + units[unit];
}

std::string duration(int64_t ms)
{
    if (ms < 0)
        return "—";
    const int64_t total = ms / 1000;
    const int64_t h = total / 3600;
    const int64_t m = (total % 3600) / 60;
    const int64_t s = total % 60;

    char buf[64];
    if (h > 0)
        std::snprintf(buf, sizeof(buf), "%lld ч %02lld мин", (long long)h, (long long)m);
    else if (m > 0)
        std::snprintf(buf, sizeof(buf), "%lld мин %02lld с", (long long)m, (long long)s);
    else
        std::snprintf(buf, sizeof(buf), "%lld с", (long long)s);
    return buf;
}

std::string percent(double fraction)
{
    const double p = std::clamp(fraction, 0.0, 1.0) * 100.0;
    // Без дробной части у 100 % и 0 %: «100,0 %» выглядит суетливо.
    if (p >= 99.95 || p <= 0.05) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0f %%", p);
        return buf;
    }
    return fixed1(p) + " %";
}

std::string count(uint64_t value)
{
    return groupDigits(value);
}

std::string bar(double fraction, int cells)
{
    cells = std::max(4, cells);
    const double f = std::clamp(fraction, 0.0, 1.0);
    // Восьмые доли символа: полоса двигается плавно, а не рывками по
    // целой ячейке — на медленном канале это единственный признак жизни.
    static const char *eighths[] = {"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
    const double exact = f * cells;
    const int full = int(exact);
    const int part = int((exact - full) * 8);

    std::string out;
    for (int i = 0; i < full && i < cells; ++i)
        out += "█";
    int used = std::min(full, cells);
    if (used < cells && part > 0) {
        out += eighths[part];
        ++used;
    }
    for (int i = used; i < cells; ++i)
        out += "·";
    return out;
}

std::string volumeMap(const std::vector<Seg> &segments, int cells)
{
    cells = std::max(8, cells);
    if (segments.empty())
        return std::string(size_t(cells), '.');

    // Агрегируем по пикселям, а не рисуем узел на чанк: у тома в 20 ГБ
    // чанков двадцать тысяч, и в строку они всё равно не влезут. В ячейке
    // побеждает самое «слабое» состояние — так дырка не теряется, а
    // именно её и важно увидеть.
    std::string out;
    for (int i = 0; i < cells; ++i) {
        const size_t from = segments.size() * size_t(i) / size_t(cells);
        size_t to = segments.size() * size_t(i + 1) / size_t(cells);
        if (to <= from)
            to = from + 1;
        if (to > segments.size())
            to = segments.size();

        Seg worst = Seg::FromSender;
        for (size_t k = from; k < to; ++k)
            worst = std::min(worst, segments[k]);

        switch (worst) {
        case Seg::None:       out += dim();    out += "·"; out += reset(); break;
        case Seg::Inflight:   out += warn();   out += "▒"; out += reset(); break;
        case Seg::Have:       out += "▓"; break;
        case Seg::FromPeer:   out += accent(); out += "█"; out += reset(); break;
        case Seg::FromSender: out += "█"; break;
        }
    }
    return out;
}

std::string ruleTop(const std::string &title, int cells)
{
    cells = std::max(20, cells);
    std::string out = "┌─ ";
    out += title;
    out += ' ';
    // Длина в кодовых точках, а не в байтах: заголовки у нас кириллические.
    int visible = 4;
    for (char c : title) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
            ++visible;
    }
    for (int i = visible; i < cells; ++i)
        out += "─";
    return out;
}

std::string ruleBottom(int cells)
{
    cells = std::max(20, cells);
    std::string out = "└";
    for (int i = 1; i < cells; ++i)
        out += "─";
    return out;
}

std::string field(const std::string &label, const std::string &value, int labelWidth)
{
    int visible = 0;
    for (char c : label) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
            ++visible;
    }
    std::string out = label;
    for (int i = visible; i < labelWidth; ++i)
        out += ' ';
    out += value;
    return out;
}

void printWarningBox(const std::string &title, const std::vector<std::string> &lines)
{
    const int cells = std::min(width(), 78);
    std::printf("%s%s%s\n", warn(), ruleTop(title, cells).c_str(), reset());
    for (const std::string &line : lines)
        std::printf("%s│%s %s\n", warn(), reset(), line.c_str());
    std::printf("%s%s%s\n", warn(), ruleBottom(cells).c_str(), reset());
}

} // namespace ferry::ui
