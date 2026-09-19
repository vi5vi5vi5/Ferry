// ============================================================
//  ferry — клиент парома для файлов.
//
//    ferry send <файл>        отдать файл, получить ссылку
//    ferry get "<ссылка>"     забрать том
//
//  Один бинарь без зависимостей, кроме системных libc и libstdc++: всё
//  остальное — ядро Ferry, свой клиент WebSocket и статически влинкованный
//  OpenSSL. Это сделано ради установки: `curl … | sh` кладёт один файл, и
//  на машине, куда вы зашли по ssh, больше ничего ставить не нужно.
// ============================================================
#include <cstdio>
#include <cstring>

#include "Cli/Options.h"
#include "Cli/Receiver.h"
#include "Cli/Relay.h"
#include "Cli/Sender.h"
#include "Cli/Signals.h"
#include "Cli/ui/Term.h"
#include "core/Protocol.h"

#ifndef FERRY_COMMIT
#define FERRY_COMMIT "unknown"
#endif
#ifndef FERRY_BUILD_TIME
#define FERRY_BUILD_TIME "unknown"
#endif

namespace {

void printVersion()
{
    std::printf("ferry, протокол v%d, сборка %s (%s)\n", int(ferry::kProtocolVersion),
                FERRY_COMMIT, FERRY_BUILD_TIME);
}

void printConfig(const ferry::cli::Options &options)
{
    using namespace ferry::ui;
    const std::string path = ferry::cli::configPath();
    std::printf("%s\n", field("файл настроек", path.empty() ? "не найден" : path).c_str());
    std::printf("%s\n",
                field("релей", options.relay.empty() ? "не задан" : options.relay).c_str());
    std::printf("%s\n", field("сертификат", options.insecure ? "НЕ проверяется (--insecure)"
                                                             : "проверяется").c_str());
    std::printf("%s\n", field("имя", options.name.empty() ? "не задано" : options.name).c_str());
    if (options.relay.empty()) {
        std::printf("\n%sЧтобы не писать --relay каждый раз, пропишите адрес:%s\n", dim(), reset());
        std::printf("  mkdir -p ~/.config/ferry && echo 'relay = ferry.example.ru' > "
                    "~/.config/ferry/config\n");
    }
}

// Предупреждение про --insecure печатается в обоих режимах и одинаково:
// это ослабление, и молчать о нём нельзя, даже когда человек сам его
// попросил.
void warnInsecure()
{
    ferry::ui::printWarningBox(
        "сертификат сервера не проверяется",
        {"Запущено с --insecure. Посредник в сети может выдать себя за релей.",
         "Ключ шифрования при этом не утекает — он никогда не покидает это",
         "устройство, — но подменённый сервер может подсунуть не тот том.",
         "",
         "Когда у релея появится домен и настоящий сертификат, флаг станет",
         "не нужен: ./tools/update.sh --domain ваш-домен --email вы@почта"});
    std::printf("\n");
}

} // namespace

int main(int argc, char **argv)
{
    using namespace ferry::cli;

    installSignalHandlers();

    const Options options = Options::parse(argc, argv);
    if (!options.error.empty()) {
        std::fprintf(stderr, "%s\n\nЗапустите `ferry help`.\n", options.error.c_str());
        return 2;
    }

    switch (options.command) {
    case Options::Command::Help:
    case Options::Command::None:
        printHelp();
        return 0;

    case Options::Command::Version:
        printVersion();
        return 0;

    case Options::Command::Config:
        printConfig(options);
        return 0;

    case Options::Command::Send: {
        Relay relay;
        std::string err;
        if (!Relay::parse(options.relay, options.insecure, relay, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 2;
        }
        if (relay.insecure)
            warnInsecure();
        return runSend(options, relay);
    }

    case Options::Command::Get:
        // Получателю адрес релея не нужен: он весь в ссылке.
        return runGet(options);
    }

    return 0;
}
