#include "Link.h"

#include <cstring>

#include "Base64Url.h"
#include "Protocol.h"

namespace ferry {
namespace {

// Хост должен выглядеть как хост: буквы, цифры, точки, дефисы, двоеточие
// перед портом и квадратные скобки для IPv6. Всё остальное означает, что
// ссылку куда-то не туда склеили, а мы по ней сейчас пойдём соединяться.
bool hostLooksSane(const std::string &host)
{
    if (host.empty() || host.size() > 255)
        return false;
    for (unsigned char c : host) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                        || (c >= '0' && c <= '9') || c == '.' || c == '-'
                        || c == ':' || c == '[' || c == ']';
        if (!ok)
            return false;
    }
    return true;
}

} // namespace

bool isValidTransferId(const std::string &id)
{
    // 16 байт в base64url без выравнивания — это 22 символа.
    if (id.size() != 22)
        return false;
    Bytes raw;
    if (!base64UrlDecode(id, raw))
        return false;
    return raw.size() == kTransferIdSize;
}

std::string TransferLink::toString() const
{
    std::string s = origin();
    s += "/t/";
    s += id;
    if (hasKey) {
        s += '#';
        s += base64UrlEncode(key.data(), key.size());
    }
    return s;
}

bool TransferLink::parse(const std::string &text, TransferLink &out, std::string *err)
{
    const auto bad = [&](const char *why) {
        if (err)
            *err = why;
        return false;
    };

    std::string s = text;
    // Пробелы по краям — след копирования из мессенджера.
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    if (s.empty())
        return bad("пустая ссылка");

    TransferLink link;

    const size_t schemeEnd = s.find("://");
    if (schemeEnd == std::string::npos)
        return bad("в ссылке нет схемы (ожидается https://…)");
    link.scheme = s.substr(0, schemeEnd);
    if (link.scheme != "https" && link.scheme != "http")
        return bad("незнакомая схема ссылки");
    s.erase(0, schemeEnd + 3);

    // Фрагмент отрезаем первым: в нём лежит ключ, и он не часть пути.
    const size_t hash = s.find('#');
    std::string fragment;
    if (hash != std::string::npos) {
        fragment = s.substr(hash + 1);
        s.resize(hash);
    }

    const size_t slash = s.find('/');
    if (slash == std::string::npos)
        return bad("в ссылке нет пути /t/<id>");
    link.host = s.substr(0, slash);
    if (!hostLooksSane(link.host))
        return bad("непохожий на адрес хост в ссылке");

    std::string path = s.substr(slash);
    // Хвостовой слэш иногда дорисовывают почтовые клиенты.
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    if (path.compare(0, 3, "/t/") != 0)
        return bad("путь в ссылке не /t/<id>");
    link.id = path.substr(3);
    if (!isValidTransferId(link.id))
        return bad("идентификатор раздачи не той формы");

    if (!fragment.empty()) {
        Bytes raw;
        if (!base64UrlDecode(fragment, raw) || raw.size() != kMasterKeySize)
            return bad("ключ после решётки не той длины");
        std::memcpy(link.key.data(), raw.data(), kMasterKeySize);
        link.hasKey = true;
    }

    out = std::move(link);
    return true;
}

} // namespace ferry
