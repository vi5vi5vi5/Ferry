#include "Cli/Tree.h"

#include <algorithm>

#include "Cli/platform/Platform.h"

namespace ferry::cli {
namespace {

// Потолки из §3. Не украшение: опись целиком едет внутри зашифрованного
// манифеста, и том из миллиона файлов раздул бы его до неприличия ещё до
// первого байта данных.
constexpr size_t kMaxEntries = 100000;
constexpr uint64_t kMaxDepth = 64;

// Последний сегмент пути, без завершающих разделителей.
std::string lastSegment(const std::string &path)
{
    size_t end = path.size();
    while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\'))
        --end;
    size_t start = 0;
    for (size_t i = 0; i < end; ++i) {
        if (path[i] == '/' || path[i] == '\\')
            start = i + 1;
    }
    return path.substr(start, end - start);
}

struct Walker
{
    TreeScan *out = nullptr;
    std::string root;
    std::string *err = nullptr;

    bool fail(const std::string &why)
    {
        if (err)
            *err = why;
        return false;
    }

    // rel — путь от корня тома, пустой для самого корня.
    bool walk(const std::string &rel, uint64_t depth)
    {
        if (depth > kMaxDepth)
            return fail("дерево глубже " + std::to_string(kMaxDepth)
                        + " уровней — похоже на цикл по ссылкам");

        const std::string full = rel.empty() ? root : root + "/" + rel;
        std::vector<platform::DirEntry> entries;
        if (!platform::listDirectory(full, entries))
            return fail("не удалось прочитать каталог: " + (rel.empty() ? root : rel));

        // Порядок описи — лексикографический по пути (§3), и это не
        // вкусовщина: при нём любой подкаталог оказывается непрерывным
        // диапазоном байт тома. Сортируем здесь, на каждом уровне, потому
        // что файловая система отдаёт записи в произвольном порядке, а
        // раскладка тома обязана быть одинаковой на любой машине.
        std::sort(entries.begin(), entries.end(),
                  [](const platform::DirEntry &a, const platform::DirEntry &b) {
                      return a.name < b.name;
                  });

        bool hadChildren = false;
        for (const platform::DirEntry &e : entries) {
            const std::string childRel = rel.empty() ? e.name : rel + "/" + e.name;

            // Симлинки не возим (§3). Разыменовать — значит утащить
            // полдиска или уйти в цикл; перенести как есть некуда, на той
            // стороне цели может не быть.
            if (e.isSymlink) {
                out->skipped.push_back(childRel + " (ссылка)");
                continue;
            }

            // Всё остальное, что не каталог и не обычный файл: FIFO, сокет,
            // устройство — или запись, о которой система не стала рассказывать.
            // Взять такое в том нельзя: размер у FIFO нулевой, а чтение из него
            // встанет насмерть и увезёт за собой всю раздачу.
            if (e.isOther) {
                out->skipped.push_back(childRel + " (не обычный файл)");
                continue;
            }

            if (e.isDirectory) {
                hadChildren = true;
                if (!walk(childRel, depth + 1))
                    return false;
                continue;
            }

            std::string pathErr;
            if (!sanitizeRelPath(childRel, &pathErr)) {
                return fail("имя не пройдёт на другой стороне: " + childRel + " — " + pathErr);
            }
            if (out->manifest.entries.size() >= kMaxEntries)
                return fail("в каталоге больше " + std::to_string(kMaxEntries) + " файлов");

            ManifestEntry me;
            me.path = childRel;
            me.size = e.size;
            me.mtime = e.mtime;
            me.isDir = false;
            out->manifest.entries.push_back(std::move(me));
            out->manifest.total += e.size;
            ++out->fileCount;
            hadChildren = true;
        }

        // Пустой каталог — отдельная запись: иначе он не доедет вовсе, а
        // пустая папка бывает осмысленной частью дерева.
        if (!hadChildren && !rel.empty()) {
            std::string pathErr;
            if (!sanitizeRelPath(rel, &pathErr))
                return fail("имя каталога не пройдёт на другой стороне: " + rel);
            ManifestEntry me;
            me.path = rel;
            me.isDir = true;
            out->manifest.entries.push_back(std::move(me));
            ++out->dirCount;
        }
        return true;
    }
};

} // namespace

bool scanTree(const std::string &root, TreeScan &out, std::string *err)
{
    out = TreeScan{};
    out.manifest.kind = "tree";
    out.manifest.name = safeFileName(lastSegment(root));
    if (out.manifest.name.empty()) {
        if (err)
            *err = "из имени каталога не получилось ничего пригодного";
        return false;
    }

    Walker w;
    w.out = &out;
    w.root = root;
    w.err = err;
    if (!w.walk({}, 0))
        return false;

    // Сортируем опись целиком ещё раз. Обход шёл по уровням, а порядок
    // тома задаётся сравнением ПОЛНЫХ путей: «a/b» и «a.txt» на разных
    // уровнях встретились бы в другом порядке.
    std::sort(out.manifest.entries.begin(), out.manifest.entries.end(),
              [](const ManifestEntry &a, const ManifestEntry &b) { return a.path < b.path; });

    if (out.manifest.entries.empty()) {
        if (err)
            *err = "каталог пуст — возить нечего";
        return false;
    }
    return true;
}

} // namespace ferry::cli
