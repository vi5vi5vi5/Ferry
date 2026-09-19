#include "Cli/Signals.h"

#include <csignal>

namespace ferry::cli {
namespace {

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int)
{
    g_stop = 1;
}

} // namespace

void installSignalHandlers()
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    // Труба закрылась (`ferry send x | head`) — это не повод падать по
    // SIGPIPE посреди раздачи: запись вернёт EPIPE, и мы разберёмся сами.
    std::signal(SIGPIPE, SIG_IGN);
}

bool stopRequested()
{
    return g_stop != 0;
}

} // namespace ferry::cli
