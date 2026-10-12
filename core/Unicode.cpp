#include "core/Unicode.h"

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace ferry {
namespace {

struct Decomposition {
    uint32_t cp;
    uint32_t first;
    uint32_t second;   // 0 — синглтон, раскладывается в один знак
    bool primary;      // собирается обратно (не синглтон и не исключение)
};

struct CombiningRange {
    uint32_t lo;
    uint32_t hi;
    int ccc;
};

#include "core/UnicodeTables.inc"

// Хангыль: слоги складываются из чамо по формуле (UAX #15, §3.12), таблица
// на одиннадцать тысяч слогов не нужна.
constexpr uint32_t kSBase = 0xAC00, kLBase = 0x1100, kVBase = 0x1161, kTBase = 0x11A7;
constexpr uint32_t kLCount = 19, kVCount = 21, kTCount = 28;
constexpr uint32_t kNCount = kVCount * kTCount, kSCount = kLCount * kNCount;
constexpr uint32_t kNone = 0xFFFFFFFF;

int combiningClass(uint32_t cp)
{
    if (cp < 0x300)
        return 0;
    const auto it = std::lower_bound(std::begin(kCombining), std::end(kCombining), cp,
                                     [](const CombiningRange &r, uint32_t v) { return r.hi < v; });
    return it != std::end(kCombining) && it->lo <= cp ? it->ccc : 0;
}

const Decomposition *findDecomposition(uint32_t cp)
{
    const auto it = std::lower_bound(std::begin(kDecompositions), std::end(kDecompositions), cp,
                                     [](const Decomposition &d, uint32_t v) { return d.cp < v; });
    return it != std::end(kDecompositions) && it->cp == cp ? it : nullptr;
}

// Первичные составные по паре (первый, второй) — для сборки. Строится
// один раз.
const std::vector<const Decomposition *> &byPair()
{
    static const std::vector<const Decomposition *> index = [] {
        std::vector<const Decomposition *> v;
        for (const Decomposition &d : kDecompositions)
            if (d.primary)
                v.push_back(&d);
        std::sort(v.begin(), v.end(), [](const Decomposition *a, const Decomposition *b) {
            return a->first != b->first ? a->first < b->first : a->second < b->second;
        });
        return v;
    }();
    return index;
}

uint32_t compose(uint32_t a, uint32_t b)
{
    if (a >= kLBase && a < kLBase + kLCount && b >= kVBase && b < kVBase + kVCount)
        return kSBase + ((a - kLBase) * kVCount + (b - kVBase)) * kTCount;
    if (a >= kSBase && a < kSBase + kSCount && (a - kSBase) % kTCount == 0 && b > kTBase
        && b < kTBase + kTCount)
        return a + (b - kTBase);

    const std::vector<const Decomposition *> &index = byPair();
    const auto it = std::lower_bound(index.begin(), index.end(), std::make_pair(a, b),
                                     [](const Decomposition *d, const std::pair<uint32_t, uint32_t> &v) {
                                         return d->first != v.first ? d->first < v.first
                                                                    : d->second < v.second;
                                     });
    return it != index.end() && (*it)->first == a && (*it)->second == b ? (*it)->cp : kNone;
}

void decompose(uint32_t cp, std::vector<uint32_t> &out)
{
    if (cp >= kSBase && cp < kSBase + kSCount) {
        const uint32_t s = cp - kSBase;
        out.push_back(kLBase + s / kNCount);
        out.push_back(kVBase + (s % kNCount) / kTCount);
        if (s % kTCount != 0)
            out.push_back(kTBase + s % kTCount);
        return;
    }
    if (const Decomposition *d = findDecomposition(cp)) {
        decompose(d->first, out);
        if (d->second != 0)
            decompose(d->second, out);
        return;
    }
    out.push_back(cp);
}

} // namespace

void toNfc(std::vector<uint32_t> &codePoints)
{
    // Ниже U+0300 нет ни знаков сочетания, ни вторых половин пар, ни
    // синглтонов: такая строка уже в NFC. Это почти любой пароль латиницей.
    if (std::all_of(codePoints.begin(), codePoints.end(), [](uint32_t cp) { return cp < 0x300; }))
        return;

    // Разложить до конца…
    std::vector<uint32_t> buf;
    buf.reserve(codePoints.size() + 8);
    for (uint32_t cp : codePoints)
        decompose(cp, buf);

    // …расставить знаки сочетания по классу (устойчиво, внутри каждой
    // серии после базового знака)…
    for (size_t i = 1; i < buf.size(); ++i) {
        const int cls = combiningClass(buf[i]);
        if (cls == 0)
            continue;
        for (size_t j = i; j > 0 && combiningClass(buf[j - 1]) > cls; --j)
            std::swap(buf[j - 1], buf[j]);
    }

    // …и собрать обратно. Знак присоединяется к последнему базовому, если
    // между ними нет знака того же или большего класса (UAX #15, D115).
    size_t starter = 0;
    bool haveStarter = combiningClass(buf[0]) == 0;
    int lastClass = haveStarter ? 0 : 256;
    size_t out = 1;
    for (size_t i = 1; i < buf.size(); ++i) {
        const uint32_t ch = buf[i];
        const int cls = combiningClass(ch);
        if (haveStarter && (lastClass < cls || lastClass == 0)) {
            const uint32_t composite = compose(buf[starter], ch);
            if (composite != kNone) {
                buf[starter] = composite;
                continue;
            }
        }
        if (cls == 0) {
            starter = out;
            haveStarter = true;
        }
        lastClass = cls;
        buf[out++] = ch;
    }
    buf.resize(out);
    codePoints.swap(buf);
}

} // namespace ferry
