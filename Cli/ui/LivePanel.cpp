#include "Cli/ui/LivePanel.h"

#include <chrono>
#include <cstdio>
#include <string>

#include "Cli/ui/Term.h"

namespace {

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Как часто печатать строку состояния, когда вывод не в терминал.
constexpr int64_t kPlainIntervalMs = 15000;

// Как часто перерисовывать панель в терминале. Полсекунды — ровно так
// часто обновляется скорость. Десять кадров в секунду были плавнее, но
// карта и счётчики на них рябили.
constexpr int64_t kDrawIntervalMs = 500;

} // namespace

void LivePanel::update(const std::vector<std::string> &lines)
{
    // Последнее состояние запоминаем в обоих режимах: в терминале его
    // дорисовывает finish(), а в журнал оно печатается целиком в конце.
    m_last = lines;

    if (!ferry::ui::isTty()) {
        if (lines.empty())
            return;
        const int64_t t = nowMs();
        if (m_lastPlainMs != 0 && t - m_lastPlainMs < kPlainIntervalMs)
            return;
        m_lastPlainMs = t;
        std::printf("%s\n", lines.front().c_str());
        std::fflush(stdout);
        return;
    }

    const int64_t t = nowMs();
    if (m_lastDrawMs != 0 && t - m_lastDrawMs < kDrawIntervalMs) {
        // Кадр придержан. Если следующего вызова не будет (передача
        // кончилась ровно сейчас), его дорисует finish.
        m_dirty = true;
        return;
    }
    m_lastDrawMs = t;
    draw(lines);
}

void LivePanel::draw(const std::vector<std::string> &lines)
{
    // Кадр собирается целиком и уходит одной записью. По printf на строку
    // терминал успевал показать стёртую строку до того, как приходила
    // новая, — отсюда и мигание.
    std::string out;

    // Синхронный вывод (DEC 2026): терминал, который его знает, покажет
    // кадр целиком, а не на середине. Кто не знает — молча пропускает.
    out += "\033[?2026h";
    if (!m_cursorHidden) {
        out += "\033[?25l";   // курсор мешает читать перерисовку
        m_cursorHidden = true;
    }

    // Возвращаемся ровно на столько строк, сколько напечатали прошлый раз.
    if (m_printed > 0)
        out += "\033[" + std::to_string(m_printed) + "A";

    // Строку, которая не изменилась, не трогаем вовсе — перевод строки
    // просто опускает курсор. Изменившуюся гасим целиком (\033[2K), а не
    // затираем пробелами: она могла быть длиннее нынешней, и хвост остался
    // бы висеть.
    int drawn = 0;
    for (const std::string &line : lines) {
        if (size_t(drawn) < m_shown.size() && m_shown[size_t(drawn)] == line)
            out += "\n";
        else
            out += "\r\033[2K" + line + "\n";
        ++drawn;
    }
    // Панель стала короче — гасим то, что осталось от прошлой. Строки не
    // исчезают, а становятся пустыми: курсор обязан остаться ровно на
    // столько строк ниже верха панели, сколько мы помним в m_printed,
    // иначе следующий подъём уедет не туда.
    while (drawn < m_printed) {
        out += "\r\033[2K\n";
        ++drawn;
    }
    out += "\033[?2026l";

    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);

    m_shown = lines;
    m_shown.resize(size_t(drawn));
    m_printed = drawn;
    m_dirty = false;
}

void LivePanel::finish()
{
    // В журнал — итоговое состояние целиком, один раз.
    //
    // По ходу дела туда печатается одна строка раз в пятнадцать секунд:
    // журнал, забитый миллионом строк «49,9 %», никому не нужен. А вот
    // последний кадр нужен очень — в нём и карта тома, и разбивка по
    // источникам, то есть ровно то, ради чего в журнал и заглядывают.
    if (!ferry::ui::isTty()) {
        for (const std::string &line : m_last)
            std::printf("%s\n", line.c_str());
        std::fflush(stdout);
        m_last.clear();
        return;
    }

    if (m_dirty)
        draw(m_last);

    if (m_cursorHidden) {
        std::printf("\033[?25h");
        m_cursorHidden = false;
    }
    std::fflush(stdout);
    m_printed = 0;
    m_shown.clear();
}
