#include "Cli/Uninstall.h"

#include <cstdio>
#include <string>
#include <vector>

#include "Cli/platform/Platform.h"
#include "Cli/ui/Term.h"

namespace ferry::cli {
namespace {

std::string parentOf(const std::string &path)
{
    size_t cut = std::string::npos;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/' || path[i] == '\\')
            cut = i;
    }
    return cut == std::string::npos ? std::string() : path.substr(0, cut);
}

// Подтверждение. Отдельно от askYesNo получателя: там спрашивают про
// приём тома, здесь — про удаление, и сливать эти два вопроса в одну
// функцию значит однажды спутать умолчания.
bool confirm(const std::string &question)
{
    std::printf("%s [д/н] ", question.c_str());
    std::fflush(stdout);

    char buf[16] = {};
    if (!std::fgets(buf, sizeof(buf), stdin))
        return false;

    const auto b0 = static_cast<unsigned char>(buf[0]);
    const auto b1 = static_cast<unsigned char>(buf[1]);
    if (b0 == 'y' || b0 == 'Y' || b0 == 'd' || b0 == 'D')
        return true;
    if (b0 == 0xD0 && (b1 == 0xB4 || b1 == 0x94))   // UTF-8: д, Д
        return true;
    if (b0 == 0xE4 || b0 == 0xC4 || b0 == 0xA4 || b0 == 0x84)   // CP1251, CP866
        return true;
    return false;
}

} // namespace

int runUninstall(const Options &options)
{
    using namespace ferry::ui;

    const int cells = std::min(width(), 78);

    const std::string exe = platform::executablePath();
    const std::string binDir = parentOf(exe);
    const std::string configDir = platform::configDirPath();

    // Сначала показываем, что именно исчезнет, и только потом спрашиваем.
    // Человек должен видеть список до ответа, а не после.
    std::printf("%s%s%s\n", dim(), ruleTop("будет удалено", cells).c_str(), reset());

    bool anything = false;
    if (!exe.empty() && platform::fileExists(exe)) {
        std::printf("%s│%s %s\n", dim(), reset(), field("программа", exe).c_str());
        anything = true;
    }
    if (!configDir.empty() && platform::fileExists(configDir)) {
        std::printf("%s│%s %s\n", dim(), reset(),
                    field("настройки", configDir + "  (адрес релея и имя)").c_str());
        anything = true;
    }
    if (!binDir.empty())
        std::printf("%s│%s %s\n", dim(), reset(),
                    field("из PATH", binDir + "  (если установщик его туда добавлял)").c_str());

    std::printf("%s%s%s\n", dim(), ruleBottom(cells).c_str(), reset());

    if (!anything) {
        std::printf("\n%sСледов не нашлось — удалять нечего.%s\n", dim(), reset());
        return 0;
    }

    // Про недокачки говорим ЧЕСТНО и до вопроса: человек должен знать,
    // что останется, а не обнаружить это потом.
    std::printf("\n%sНедокачки (*.ferry-part и *.ferry-map) останутся там, куда вы их\n"
                "скачивали: это ваши файлы в ваших каталогах, и искать их по всему\n"
                "диску Ferry не станет.%s\n\n",
                dim(), reset());

    if (!options.yes) {
        if (!platform::consoleStdinIsTty()) {
            std::fprintf(stderr, "Подтверждать некому: ввод не с терминала. Добавьте -y.\n");
            return 1;
        }
        if (!confirm("Удалить Ferry с этой машины?")) {
            std::printf("%sОтменено. Ничего не тронуто.%s\n", dim(), reset());
            return 1;
        }
        std::printf("\n");
    }

    int failures = 0;

    // ---- 1. Настройки ----
    if (!configDir.empty() && platform::fileExists(configDir)) {
        if (platform::removeTree(configDir)) {
            std::printf("%s✓%s настройки удалены: %s\n", ok(), reset(), configDir.c_str());
        } else {
            std::fprintf(stderr, "%s✗%s не удалось удалить %s\n", bad(), reset(),
                         configDir.c_str());
            ++failures;
        }
    }

    // ---- 2. PATH ----
    if (!binDir.empty()) {
        if (platform::removeFromUserPath(binDir))
            std::printf("%s✓%s каталог убран из PATH пользователя\n", ok(), reset());
    }

    // ---- 3. Сам бинарь ----
    //
    // Последним, и не случайно: если что-то пойдёт не так раньше, у
    // человека останется программа, которой можно попробовать снова.
    bool deferred = false;
    if (!exe.empty() && platform::fileExists(exe)) {
        if (platform::removeSelf(exe, &deferred)) {
            if (deferred) {
                std::printf("%s✓%s программа удалена (файл исчезнет через пару секунд —\n"
                            "  Windows не отдаёт образ запущенного процесса)\n",
                            ok(), reset());
            } else {
                std::printf("%s✓%s программа удалена: %s\n", ok(), reset(), exe.c_str());
            }
        } else {
            std::fprintf(stderr, "%s✗%s не удалось удалить %s\n", bad(), reset(), exe.c_str());
            ++failures;
        }
    }

    std::printf("\n");
    if (failures == 0) {
        std::printf("%s%s%s\n", ok(), ruleTop("готово", cells).c_str(), reset());
        std::printf("%s│%s Ferry больше нет на этой машине.\n", ok(), reset());
#ifdef _WIN32
        std::printf("%s│%s Вернуть: irm https://<релей>/install.ps1 | iex\n", ok(), reset());
#else
        std::printf("%s│%s Вернуть: curl -fsSL https://<релей>/install.sh | sh\n", ok(), reset());
#endif
        std::printf("%s%s%s\n", ok(), ruleBottom(cells).c_str(), reset());
        return 0;
    }

    std::fprintf(stderr, "%sУбрано не всё: %d пункт(ов) не поддались.%s\n", warn(), failures,
                 reset());
    return 1;
}

} // namespace ferry::cli
