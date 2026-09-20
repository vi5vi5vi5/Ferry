#include "Cli/ui/LivePanel.h"

#include <chrono>
#include <cstdio>

#include "Cli/ui/Term.h"

namespace {

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Как часто печатать строку состояния, когда вывод не в терминал.
constexpr int64_t kPlainIntervalMs = 15000;

// Как часто перерисовывать панель в терминале. Сто миллисекунд — это
// десять кадров в секунду: глазу достаточно, а главному циклу остаётся
// заниматься своим делом.
constexpr int64_t kDrawIntervalMs = 100;

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
    if (!m_cursorHidden) {
        std::printf("\033[?25l");   // курсор мешает читать перерисовку
        m_cursorHidden = true;
    }

    // Возвращаемся ровно на столько строк, сколько напечатали прошлый раз.
    // Каждую гасим целиком (\033[2K), а не затираем пробелами: строка
    // могла быть длиннее нынешней, и хвост остался бы висеть.
    if (m_printed > 0)
        std::printf("\033[%dA", m_printed);

    int drawn = 0;
    for (const std::string &line : lines) {
        std::printf("\033[2K%s\n", line.c_str());
        ++drawn;
    }
    // Панель стала короче — гасим то, что осталось от прошлой. Строки не
    // исчезают, а становятся пустыми: курсор обязан остаться ровно на
    // столько строк ниже верха панели, сколько мы помним в m_printed,
    // иначе следующий подъём уедет не туда.
    while (drawn < m_printed) {
        std::printf("\033[2K\n");
        ++drawn;
    }

    m_printed = drawn;
    m_dirty = false;
    std::fflush(stdout);
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
}
