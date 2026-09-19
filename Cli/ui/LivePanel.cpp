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

} // namespace

void LivePanel::update(const std::vector<std::string> &lines)
{
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
    std::fflush(stdout);
}

void LivePanel::finish()
{
    if (m_cursorHidden) {
        std::printf("\033[?25h");
        m_cursorHidden = false;
    }
    std::fflush(stdout);
    m_printed = 0;
}
