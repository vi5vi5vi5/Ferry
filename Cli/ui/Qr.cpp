#include "Cli/ui/Qr.h"

#include <cstdio>
#include <vector>

#include "Cli/ui/Term.h"

extern "C" {
#include "qrcodegen.h"
}

namespace ferry::ui {
namespace {

// Модуль QR рисуется половиной символьной ячейки: символ «верхняя половина
// блока» показывает ДВА модуля по вертикали — цветом переднего плана
// верхний, цветом фона нижний.
//
// Это не трюк ради красоты, а единственный способ уместить квадрат в
// терминал: ячейка вдвое выше своей ширины, поэтому один модуль в ширину и
// два в высоту дают ровно квадратный модуль. Рисуя модуль целой ячейкой,
// мы получили бы вытянутый вдвое код — и часть сканеров его не возьмёт.
constexpr const char *kUpperHalf = "▀";

// QR обязан быть тёмным на светлом, и фон задаётся явно — иначе в тёмной
// теме терминала код инвертируется и не читается (§12 проектного документа
// требует этого же от веб-клиента).
constexpr const char *kFgDark = "\033[30m";
constexpr const char *kFgLight = "\033[97m";
constexpr const char *kBgDark = "\033[40m";
constexpr const char *kBgLight = "\033[107m";
constexpr const char *kOff = "\033[0m";

} // namespace

bool printQr(const std::string &text)
{
    if (!colorEnabled())
        return false;

    std::vector<uint8_t> qr(qrcodegen_BUFFER_LEN_FOR_VERSION(qrcodegen_VERSION_MAX));
    std::vector<uint8_t> temp(qrcodegen_BUFFER_LEN_FOR_VERSION(qrcodegen_VERSION_MAX));

    // Уровень коррекции низкий: код сканируют с экрана вблизи, а не с
    // мятого листа, зато версия получается меньше и квадрат уже.
    if (!qrcodegen_encodeText(text.c_str(), temp.data(), qr.data(), qrcodegen_Ecc_LOW,
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true))
        return false;

    const int size = qrcodegen_getSize(qr.data());
    const int quiet = 2;   // тихая зона: без неё сканеры часто не находят код
    const int cells = size + quiet * 2;
    if (cells > width())
        return false;

    const auto module = [&](int x, int y) {
        const int mx = x - quiet;
        const int my = y - quiet;
        if (mx < 0 || my < 0 || mx >= size || my >= size)
            return false;   // тихая зона — светлая
        return qrcodegen_getModule(qr.data(), mx, my);
    };

    for (int y = 0; y < cells; y += 2) {
        std::string line;
        for (int x = 0; x < cells; ++x) {
            const bool top = module(x, y);
            const bool bottom = (y + 1 < cells) && module(x, y + 1);
            line += top ? kFgDark : kFgLight;
            line += bottom ? kBgDark : kBgLight;
            line += kUpperHalf;
        }
        line += kOff;
        std::printf("  %s\n", line.c_str());
    }
    return true;
}

} // namespace ferry::ui
