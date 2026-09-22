#include "Manifest.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "Base64Url.h"
#include "Json.h"

namespace ferry {
namespace {

// Ограничения v1 из §3: дальше начинает страдать память клиента и UI.
constexpr size_t kMaxEntries = 100000;
constexpr size_t kMaxNameBytes = 1024;
constexpr size_t kMaxPathBytes = 4096;

bool isReservedWindowsName(const std::string &segment)
{
    // Сравниваем только основу до первой точки: "NUL.txt" на Windows — это
    // всё тот же NUL, и файл с таким именем там не создать.
    std::string base = segment.substr(0, segment.find('.'));
    for (char &c : base)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    static const char *kNames[] = {"con", "prn", "aux", "nul"};
    for (const char *n : kNames) {
        if (base == n)
            return true;
    }
    if (base.size() == 4 && (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0)
        && base[3] >= '1' && base[3] <= '9')
        return true;
    return false;
}

bool segmentIsSane(const std::string &segment, std::string *err)
{
    if (segment.empty()) {
        if (err)
            *err = "пустой сегмент пути";
        return false;
    }
    if (segment == "." || segment == "..") {
        if (err)
            *err = "сегмент пути выходит за корень";
        return false;
    }
    for (unsigned char c : segment) {
        if (c < 0x20 || c == 0x7F) {
            if (err)
                *err = "управляющий символ в пути";
            return false;
        }
        if (std::strchr("<>:\"|?*\\", c) != nullptr) {
            if (err)
                *err = "недопустимый символ в пути";
            return false;
        }
    }
    const char last = segment.back();
    if (last == '.' || last == ' ') {
        // Windows молча срезает хвостовую точку и пробел, и тогда путь на
        // диске перестаёт совпадать с манифестом.
        if (err)
            *err = "сегмент пути кончается точкой или пробелом";
        return false;
    }
    if (isReservedWindowsName(segment)) {
        if (err)
            *err = "зарезервированное имя Windows в пути";
        return false;
    }
    return true;
}

} // namespace

bool sanitizeRelPath(const std::string &path, std::string *err)
{
    if (path.empty() || path.size() > kMaxPathBytes) {
        if (err)
            *err = "путь пуст или слишком длинный";
        return false;
    }
    if (path.front() == '/') {
        if (err)
            *err = "абсолютный путь";
        return false;
    }
    // Буква диска: "C:/…" или "C:…".
    if (path.size() >= 2 && path[1] == ':') {
        if (err)
            *err = "путь с буквой диска";
        return false;
    }

    // Завершающий слэш допустим только у пустой директории; вызывающий
    // отрезает его заранее, здесь он уже недопустим.
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string segment =
            path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!segmentIsSane(segment, err))
            return false;
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return true;
}

std::string safeFileName(const std::string &name)
{
    // Отрезаем всё, что похоже на путь: отправитель мог прислать
    // "/home/user/dump.sql" или "C:\\temp\\dump.sql".
    size_t cut = 0;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '/' || name[i] == '\\' || name[i] == ':')
            cut = i + 1;
    }
    std::string out = name.substr(cut);

    // Заменяем непригодные символы, а не выбрасываем: так длина и форма
    // имени остаются узнаваемыми для человека.
    std::string cleaned;
    cleaned.reserve(out.size());
    for (unsigned char c : out) {
        if (c < 0x20 || c == 0x7F)
            continue;
        if (std::strchr("<>:\"|?*\\/", c) != nullptr)
            cleaned += '_';
        else
            cleaned += static_cast<char>(c);
    }
    while (!cleaned.empty() && (cleaned.back() == '.' || cleaned.back() == ' '))
        cleaned.pop_back();
    while (!cleaned.empty() && cleaned.front() == ' ')
        cleaned.erase(cleaned.begin());

    if (cleaned.empty() || cleaned == "." || cleaned == "..")
        return {};
    if (isReservedWindowsName(cleaned))
        cleaned = "_" + cleaned;
    if (cleaned.size() > kMaxNameBytes)
        cleaned.resize(kMaxNameBytes);
    return cleaned;
}

std::string Manifest::toJson() const
{
    json::Value o = json::Value::object();
    o.set("v", json::Value::make(static_cast<int64_t>(version)));
    o.set("kind", json::Value::make(kind));
    o.set("name", json::Value::make(name));
    o.set("total", json::Value::make(static_cast<int64_t>(total)));
    // У промежуточного манифеста корня нет и быть не может: хеши ещё
    // считаются. Вписать туда нули значило бы соврать — получатель
    // сверил бы с ними список и отверг том.
    if (streamHashes)
        o.set("hashes", json::Value::make("stream"));
    else
        o.set("root", json::Value::make(base64UrlEncode(root.data(), root.size())));

    if (isTree()) {
        json::Value arr = json::Value::array();
        for (const ManifestEntry &e : entries) {
            json::Value item = json::Value::object();
            item.set("p", json::Value::make(e.path));
            item.set("s", json::Value::make(static_cast<int64_t>(e.size)));
            if (e.mtime != 0)
                item.set("m", json::Value::make(e.mtime));
            if (e.isDir)
                item.set("d", json::Value::make(true));
            arr.push(std::move(item));
        }
        o.set("entries", std::move(arr));
    }
    return o.dump();
}

bool Manifest::fromJson(const std::string &text, Manifest &out, std::string *err)
{
    const auto bad = [&](const char *why) {
        if (err)
            *err = why;
        return false;
    };

    json::Value root;
    std::string parseErr;
    if (!json::Value::parse(text, root, &parseErr)) {
        if (err)
            *err = "манифест не разбирается: " + parseErr;
        return false;
    }
    if (!root.isObject())
        return bad("манифест не объект");

    Manifest m;
    m.version = static_cast<int>(root["v"].toInt(0));
    if (m.version != 1)
        return bad("незнакомая версия манифеста");

    m.kind = root["kind"].toString();
    if (m.kind != "file" && m.kind != "tree")
        return bad("незнакомый вид тома");

    m.name = root["name"].toString();
    if (m.name.empty() || m.name.size() > kMaxNameBytes)
        return bad("пустое или слишком длинное имя тома");

    const int64_t total = root["total"].toInt(-1);
    if (total < 0)
        return bad("размер тома отсутствует или отрицателен");
    m.total = static_cast<uint64_t>(total);

    // Промежуточный манифест раздачи с хешами на лету: корня в нём нет, и
    // присутствие поля root здесь — признак чужого или сломанного
    // манифеста, а не «ну и ладно».
    const std::string hashesMode = root["hashes"].toString();
    if (!hashesMode.empty() && hashesMode != "stream")
        return bad("незнакомый способ доставки хешей");
    m.streamHashes = hashesMode == "stream";
    if (m.streamHashes) {
        if (!root["root"].isNull())
            return bad("у манифеста с хешами на лету не бывает корня");
    } else {
        std::vector<uint8_t> rootHash;
        if (!base64UrlDecode(root["root"].toString(), rootHash) || rootHash.size() != 32)
            return bad("корневой хеш отсутствует или не той длины");
        std::memcpy(m.root.data(), rootHash.data(), 32);
    }

    if (m.isTree()) {
        const json::Value &arr = root["entries"];
        if (!arr.isArray())
            return bad("у дерева нет описи");
        if (arr.size() > kMaxEntries)
            return bad("в описи слишком много записей");

        uint64_t sum = 0;
        m.entries.reserve(arr.size());
        for (size_t i = 0; i < arr.size(); ++i) {
            const json::Value &item = arr.at(i);
            ManifestEntry e;
            e.path = item["p"].toString();
            e.isDir = item["d"].toBool(false);
            const int64_t size = item["s"].toInt(-1);
            if (size < 0)
                return bad("у записи описи нет размера");
            e.size = static_cast<uint64_t>(size);
            e.mtime = item["m"].toInt(0);

            std::string pathErr;
            if (!sanitizeRelPath(e.path, &pathErr)) {
                if (err)
                    *err = "недопустимый путь в описи: " + pathErr;
                return false;
            }
            if (e.isDir && e.size != 0)
                return bad("у директории в описи ненулевой размер");

            sum += e.size;
            m.entries.push_back(std::move(e));
        }
        // Том — это конкатенация файлов описи, и ничего больше. Расхождение
        // означает, что раскладка тома не та, по которой считались хеши.
        if (sum != m.total)
            return bad("сумма размеров описи не сходится с размером тома");
    }

    out = std::move(m);
    return true;
}

} // namespace ferry
