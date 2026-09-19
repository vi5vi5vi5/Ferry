#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Вывод в терминал.
//
// Тон задан проектным документом (§12): Ferry — инструмент, а не сервис.
// Ближе к судовой накладной, чем к лендингу: спокойно, плотно, много
// точных чисел и мало украшений. В терминале это значит рамки из линий,
// моноширинные числа с разделителями разрядов и ни одного эмодзи.
//
// Два правила, которые соблюдаются везде:
//   1) не tty или NO_COLOR — цветов нет вовсе, и вывод остаётся пригодным
//      для `| tee`, `| grep` и журнала systemd;
//   2) ни один статус не передаётся ТОЛЬКО цветом: рядом всегда слово.
namespace ferry::ui {

bool isTty();
int width();            // ширина терминала; 80, если узнать нельзя
bool colorEnabled();

// Управляющие последовательности. Пустые строки, когда цвет выключен, —
// поэтому их можно вставлять в вывод безусловно.
const char *reset();
const char *dim();
const char *bold();
const char *accent();   // то, что человек будет копировать: ссылка, имя
const char *ok();
const char *warn();
const char *bad();

// Числа. Разделитель разрядов — узкий пробел, десятичный — запятая:
// это русский текст, и «5.4 GB» в нём выглядит чужеродно.
std::string bytes(uint64_t value);
std::string rate(double bytesPerSecond);
std::string duration(int64_t ms);
std::string percent(double fraction);
std::string count(uint64_t value);

// Горизонтальная полоса прогресса шириной cells ячеек.
std::string bar(double fraction, int cells);

// Карта тома (§11). Состояния на сегмент — ровно те же пять, что в
// веб-макете, и они обязаны читаться в оттенках серого: цвет здесь
// вспомогательный, различает яркость символа.
enum class Seg : uint8_t {
    None = 0,      // нет
    Inflight,      // в пути
    Have,          // есть
    FromPeer,      // пришло от другого получателя (M2)
    FromSender,    // пришло от отправителя
};
std::string volumeMap(const std::vector<Seg> &segments, int cells);

// Рамка. Заголовок вписывается в верхнюю линию.
std::string ruleTop(const std::string &title, int cells);
std::string ruleBottom(int cells);

// Строка «ключ: значение» с выравниванием ключей по ширине labelWidth.
std::string field(const std::string &label, const std::string &value, int labelWidth = 14);

// Предупреждение в рамке. Используется для --insecure и подобного: то, о
// чём человек должен узнать, даже если читает вывод по диагонали.
void printWarningBox(const std::string &title, const std::vector<std::string> &lines);

} // namespace ferry::ui
