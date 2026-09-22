#include "Cli/Relay.h"

#include <algorithm>
#include <cstdlib>

#include "core/Base64Url.h"
#include "core/Json.h"
#include "core/Protocol.h"

namespace ferry::cli {
namespace {

bool decodeField(const json::Value &o, const char *key, Bytes &out, std::string *err)
{
    if (!base64UrlDecode(o[key].toString(), out)) {
        if (err)
            *err = std::string("поле ") + key + " в ответе сервера не разбирается";
        return false;
    }
    return true;
}

// Тело ошибки от сервера — это {"error":"код"}. Вытаскиваем код, чтобы
// показать человеку не «503», а «на сервере кончилась память под окна».
std::string errorCodeFrom(const std::string &body)
{
    json::Value v;
    if (json::Value::parse(body, v) && v.isObject())
        return v["error"].toString();
    return {};
}

} // namespace

std::string Relay::origin() const
{
    const bool defaultPort = (tls && port == 443) || (!tls && port == 80);
    // Собираем по шагам, а не одним выражением: GCC 12 даёт на
    // operator+(const char*, std::string&&) ложное -Wrestrict, а держать
    // в сборке предупреждение, которое все привыкают пролистывать, —
    // верный способ однажды не заметить настоящее.
    std::string s = tls ? "https://" : "http://";
    s += host;
    if (!defaultPort) {
        s += ':';
        s += std::to_string(port);
    }
    return s;
}

net::HttpTarget Relay::target() const
{
    net::HttpTarget t;
    t.host = host;
    t.port = port;
    t.tls = tls;
    t.insecure = insecure;
    return t;
}

bool Relay::parse(const std::string &text, bool insecure, Relay &out, std::string *err)
{
    if (text.empty()) {
        if (err)
            *err = "не задан адрес релея: укажите --relay или пропишите его в "
                   "~/.config/ferry/config";
        return false;
    }

    Relay r;
    r.insecure = insecure;
    std::string rest = text;

    const size_t schemeEnd = rest.find("://");
    if (schemeEnd != std::string::npos) {
        const std::string scheme = rest.substr(0, schemeEnd);
        if (scheme == "http") {
            r.tls = false;
            r.port = 80;
        } else if (scheme == "https") {
            r.tls = true;
            r.port = 443;
        } else {
            if (err)
                *err = "незнакомая схема в адресе релея: " + scheme;
            return false;
        }
        rest.erase(0, schemeEnd + 3);
    }

    // Путь и всё, что за ним, отбрасываем: релей — это хост, а не страница.
    const size_t slash = rest.find('/');
    if (slash != std::string::npos)
        rest.resize(slash);

    // IPv6 в скобках: [::1]:8443.
    if (!rest.empty() && rest.front() == '[') {
        const size_t close = rest.find(']');
        if (close == std::string::npos) {
            if (err)
                *err = "незакрытая скобка в адресе IPv6";
            return false;
        }
        r.host = rest.substr(1, close - 1);
        if (close + 1 < rest.size() && rest[close + 1] == ':')
            r.port = uint16_t(std::atoi(rest.c_str() + close + 2));
    } else {
        const size_t colon = rest.rfind(':');
        if (colon != std::string::npos) {
            r.host = rest.substr(0, colon);
            r.port = uint16_t(std::atoi(rest.c_str() + colon + 1));
        } else {
            r.host = rest;
        }
    }

    if (r.host.empty() || r.port == 0) {
        if (err)
            *err = "не удалось разобрать адрес релея: " + text;
        return false;
    }
    out = r;
    return true;
}

bool apiCreateTransfer(const Relay &relay, CreatedTransfer &out, std::string *err)
{
    net::HttpResponse resp;
    if (!net::httpRequest(relay.target(), "POST", std::string(kApiTransfers), std::string(), resp,
                          err))
        return false;

    if (resp.status != 201 && resp.status != 200) {
        const std::string code = errorCodeFrom(resp.body);
        if (err)
            *err = code.empty() ? "сервер отказался заводить раздачу (код " + std::to_string(resp.status) + ")"
                                : explainError(code);
        return false;
    }

    json::Value v;
    std::string parseErr;
    if (!json::Value::parse(resp.body, v, &parseErr) || !v.isObject()) {
        if (err)
            *err = "ответ сервера не разбирается: " + parseErr;
        return false;
    }

    out.id = v["id"].toString();
    out.ownerToken = v["owner_token"].toString();
    out.publicUrl = v["public_url"].toString();
    out.features.clear();
    const json::Value &features = v["features"];
    for (size_t i = 0; i < features.size(); ++i)
        out.features.push_back(features.at(i).toString());
    if (out.id.empty() || out.ownerToken.empty()) {
        if (err)
            *err = "сервер не вернул идентификатор раздачи";
        return false;
    }
    return true;
}

bool apiFetchMeta(const Relay &relay, const std::string &id, TransferMeta &out, std::string *err)
{
    net::HttpResponse resp;
    // Говорим, что умеем хеши на лету. Без этого релей, у которого
    // отправитель ещё считает хеши, ответил бы preparing — так он
    // отвечает старым клиентам.
    const std::string headers = std::string(kFeaturesHeader) + ": " + kFeatureStreamHashes + "\r\n";
    if (!net::httpRequest(relay.target(), "GET", std::string(kApiTransfers) + "/" + id,
                          std::string(), resp, err, 20000, headers))
        return false;

    if (resp.status != 200) {
        const std::string code = errorCodeFrom(resp.body);
        if (err)
            *err = code.empty() ? "сервер не отдал раздачу (код " + std::to_string(resp.status) + ")"
                                : explainError(code);
        return false;
    }

    json::Value v;
    std::string parseErr;
    if (!json::Value::parse(resp.body, v, &parseErr) || !v.isObject()) {
        if (err)
            *err = "ответ сервера не разбирается: " + parseErr;
        return false;
    }

    out.total = uint64_t(v["total"].toInt(0));
    out.chunkSize = uint32_t(v["chunk_size"].toInt(0));
    out.chunks = uint64_t(v["chunks"].toInt(0));
    out.mode = v["mode"].toString();
    out.state = v["state"].toString();
    out.receivers = int(v["receivers"].toInt(0));
    out.usesLeft = int(v["uses_left"].toInt(-1));
    out.expiresInMs = v["expires_in_ms"].toInt(0);

    out.streamHashes = v["hash_mode"].toString() == "stream";
    out.hashed = uint64_t(std::max<int64_t>(0, v["hashed"].toInt(0)));
    if (!decodeField(v, "manifest", out.manifest, err))
        return false;
    if (!out.streamHashes && !decodeField(v, "hash_list", out.hashList, err))
        return false;
    if (!decodeField(v, "nonce_prefix", out.noncePrefix, err))
        return false;

    if (out.noncePrefix.size() != kNoncePrefixSize) {
        if (err)
            *err = "сервер прислал префикс nonce не той длины";
        return false;
    }
    return true;
}

bool apiFetchChallenge(const Relay &relay, const std::string &id, Bytes &out, std::string *err)
{
    net::HttpResponse resp;
    if (!net::httpRequest(relay.target(), "GET",
                          std::string(kApiTransfers) + "/" + id + "/challenge", std::string(), resp,
                          err))
        return false;

    if (resp.status != 200) {
        const std::string code = errorCodeFrom(resp.body);
        if (err)
            *err = code.empty() ? "сервер не выдал challenge (код " + std::to_string(resp.status) + ")"
                                : explainError(code);
        return false;
    }

    json::Value v;
    if (!json::Value::parse(resp.body, v) || !v.isObject()) {
        if (err)
            *err = "ответ сервера не разбирается";
        return false;
    }
    if (!base64UrlDecode(v["challenge"].toString(), out) || out.size() != kChallengeSize) {
        if (err)
            *err = "сервер прислал challenge не той длины";
        return false;
    }
    return true;
}

std::string drainErrorReason(net::WebSocketClient &ws)
{
    net::WsMessage msg;
    while (ws.next(msg)) {
        if (msg.binary)
            continue;
        json::Value v;
        if (json::Value::parse(msg.data, v) && v.isObject()
            && v["type"].toString() == "error")
            return v["reason"].toString();
    }
    return {};
}

std::string explainError(const std::string &code)
{
    // Каждая строка отвечает на два вопроса: что произошло и что делать.
    // И ни одна не намекает, что человек сделал что-то не так, — потому
    // что в большинстве случаев он не делал.
    if (code == err::kNeedKey)
        return "ссылка без ключа. Ключ — это часть после решётки; попросите "
               "прислать ссылку целиком.";
    if (code == err::kNotFound)
        return "такой раздачи на сервере нет. Либо отправитель её закрыл, либо "
               "сервер перезапускали: Ferry ничего не хранит на диске, и "
               "перезапуск гасит все раздачи.";
    if (code == err::kUsesExhausted)
        return "ссылкой уже воспользовались столько раз, сколько было разрешено.";
    if (code == err::kExpired)
        return "срок жизни раздачи вышел.";
    if (code == err::kTooManyReceivers)
        return "сейчас качают столько человек, сколько разрешил отправитель. "
               "Попробуйте через несколько минут.";
    if (code == err::kApprovalDenied)
        return "отправитель не пустил.";
    if (code == err::kSenderGone)
        return "отправитель отключился. В этой версии раздача живёт, только "
               "пока запущен ferry send на той стороне.";
    if (code == err::kNoSource)
        return "раздача уже идёт, и начало тома сервер отдать неоткуда. "
               "Подключаться к идущей раздаче научимся в следующей версии; "
               "сейчас нужно, чтобы получатели стартовали вместе.";
    if (code == err::kChunkMismatch)
        return "пришедший кусок не сошёлся с хешем. Это либо повреждение по "
               "дороге, либо подмена; том не принят.";
    if (code == err::kServerBusy)
        return "релей занят: кончилась память под окна или упёрлись в лимит "
               "раздач. Попробуйте позже.";
    if (code == err::kOwnerConflict)
        return "эта раздача уже занята другим отправителем.";
    if (code == err::kBadMessage)
        return "сервер не понял сообщение клиента. Похоже, версии клиента и "
               "сервера разошлись: обновите клиент командой из install.sh.";
    if (code == err::kPreparing)
        return "отправитель ещё считает хеши тома. Подождите минуту и попробуйте "
               "снова — как только он досчитает, раздача откроется.";
    return "сервер ответил кодом «" + code + "».";
}

} // namespace ferry::cli
