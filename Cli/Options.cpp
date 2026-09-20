#include "Cli/Options.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "Cli/platform/Platform.h"
#include "Cli/ui/Term.h"

namespace ferry::cli {
namespace {

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
        --b;
    return s.substr(a, b - a);
}

bool truthy(const std::string &v)
{
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

// Файл настроек: простые "ключ = значение", комментарии с #.
void applyConfigFile(Options &o)
{
    const std::string path = platform::configFilePath();
    if (path.empty())
        return;
    std::ifstream in(path);
    if (!in)
        return;

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "relay" && o.relay.empty())
            o.relay = value;
        else if (key == "insecure" && truthy(value))
            o.insecure = true;
        else if (key == "name" && o.name.empty())
            o.name = value;
    }
}

void applyEnvironment(Options &o)
{
    if (const char *relay = std::getenv("FERRY_RELAY"))
        if (o.relay.empty())
            o.relay = relay;
    if (const char *insecure = std::getenv("FERRY_INSECURE"))
        if (truthy(insecure))
            o.insecure = true;
    if (const char *name = std::getenv("FERRY_NAME"))
        if (o.name.empty())
            o.name = name;
}

} // namespace

int64_t parseDurationMs(const std::string &text)
{
    if (text.empty())
        return 0;
    char *end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || value <= 0)
        return 0;

    const std::string suffix = trim(end);
    if (suffix.empty() || suffix == "s" || suffix == "с")
        return int64_t(value * 1000);
    if (suffix == "m" || suffix == "min" || suffix == "м")
        return int64_t(value * 60 * 1000);
    if (suffix == "h" || suffix == "ч")
        return int64_t(value * 60 * 60 * 1000);
    if (suffix == "d" || suffix == "д")
        return int64_t(value * 24 * 60 * 60 * 1000);
    return 0;
}

Options Options::parse(const std::vector<std::string> &args)
{
    const int argc = int(args.size());

    Options o;
    if (argc < 2) {
        o.command = Command::Help;
        return o;
    }

    const std::string cmd = args[1];
    if (cmd == "send")
        o.command = Command::Send;
    else if (cmd == "get")
        o.command = Command::Get;
    else if (cmd == "version" || cmd == "--version" || cmd == "-V")
        o.command = Command::Version;
    else if (cmd == "config")
        o.command = Command::Config;
    else if (cmd == "help" || cmd == "--help" || cmd == "-h")
        o.command = Command::Help;
    else {
        o.error = "неизвестная команда: " + cmd;
        return o;
    }

    std::vector<std::string> positional;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = args[size_t(i)];
        const auto need = [&](const char *what) -> std::string {
            if (i + 1 >= argc) {
                o.error = std::string("у ") + arg + " нет значения (" + what + ")";
                return {};
            }
            return args[size_t(++i)];
        };

        if (arg == "--relay")
            o.relay = need("адрес релея");
        else if (arg == "--insecure" || arg == "-k")
            o.insecure = true;
        else if (arg == "--no-qr")
            o.noQr = true;
        else if (arg == "--name")
            o.name = need("имя");
        else if (arg == "--uses")
            o.uses = std::atoi(need("число использований").c_str());
        else if (arg == "--ttl")
            o.ttlMs = parseDurationMs(need("срок жизни, например 24h"));
        else if (arg == "--max-concurrent")
            o.maxConcurrent = std::atoi(need("получателей одновременно").c_str());
        else if (arg == "-o" || arg == "--output")
            o.outPath = need("путь");
        else if (arg == "--id")
            o.id = need("идентификатор раздачи");
        else if (arg == "--key")
            o.key = need("ключ");
        else if (arg == "-y" || arg == "--yes")
            o.yes = true;
        else if (arg == "--seed")
            o.seed = true;
        else if (arg == "-h" || arg == "--help")
            o.command = Command::Help;
        else if (!arg.empty() && arg[0] == '-') {
            o.error = "неизвестный ключ: " + arg;
            return o;
        } else {
            positional.push_back(arg);
        }

        if (!o.error.empty())
            return o;
    }

    if (o.command == Command::Send) {
        if (positional.empty()) {
            o.error = "не сказано, что отправлять";
            return o;
        }
        o.path = positional.front();
        if (o.ttlMs < 0)
            o.ttlMs = 0;
    } else if (o.command == Command::Get) {
        if (!positional.empty())
            o.link = positional.front();
        if (o.link.empty() && o.id.empty()) {
            o.error = "не сказано, что забирать: нужна ссылка или --id с --key";
            return o;
        }
    }

    applyEnvironment(o);
    applyConfigFile(o);
    return o;
}

void printHelp()
{
    using namespace ferry::ui;
    std::printf("%sFerry%s — паром для файлов. Перевёз и ушёл.\n\n", bold(), reset());
    std::printf("  %sferry send <файл>%s          отдать файл, получить ссылку\n", bold(), reset());
    std::printf("  %sferry get \"<ссылка>\"%s       забрать том по ссылке\n\n", bold(), reset());
    std::printf("Ключи send:\n");
    std::printf("  --uses <N>            сколько раз ссылкой можно воспользоваться\n");
    std::printf("  --ttl <срок>          сколько живёт раздача: 90m, 24h, 7d\n");
    std::printf("  --max-concurrent <N>  сколько получателей одновременно\n");
    std::printf("  --no-qr               не печатать QR\n\n");
    std::printf("Ключи get:\n");
    std::printf("  -o, --output <путь>   куда положить\n");
    std::printf("  --id <id> --key <ключ>  если ссылка потеряла часть после решётки\n");
    std::printf("  -y, --yes             не спрашивать подтверждения\n");
    std::printf("  --seed                после приёма остаться источником до Ctrl-C\n\n");
    std::printf("Общие:\n");
    std::printf("  --relay <адрес>       адрес релея (иначе из файла настроек)\n");
    std::printf("  --name <имя>          как представиться отправителю\n");
    std::printf("  -k, --insecure        не проверять сертификат сервера\n");
    std::printf("  --version             версия\n\n");
    std::printf("%sСсылку печатайте и передавайте целиком, вместе с частью после\n"
                "решётки: это и есть ключ шифрования. Кто получил ссылку —\n"
                "получил том.%s\n", dim(), reset());
}

} // namespace ferry::cli
