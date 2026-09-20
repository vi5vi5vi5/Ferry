#include "Cli/Receiver.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <cstring>
#include <string>
#include <vector>

#include "Cli/Signals.h"
#include "Cli/net/WebSocketClient.h"
#include "Cli/platform/Platform.h"
#include "Cli/ui/LivePanel.h"
#include "Cli/ui/Term.h"
#include "core/Base64Url.h"
#include "core/Chunker.h"
#include "core/Crypto.h"
#include "core/HashList.h"
#include "core/Json.h"
#include "core/Link.h"
#include "core/Manifest.h"
#include "core/Protocol.h"

namespace ferry::cli {
namespace {

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint64_t readBe64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | p[i];
    return v;
}

// Карта принятых чанков рядом с недокачанным файлом.
//
// Пишется уже сейчас, хотя в M1 чанки приходят по порядку и хватило бы
// одного числа. Причина простая: в M2 приходить они будут вразнобой, и
// формат, который придётся менять, — это формат, из-за которого чужая
// недокачка станет несовместимой. Лучше заложить его сразу.
class ChunkMap
{
public:
    bool init(const std::string &path, uint64_t chunkCount, uint32_t chunkSize, uint64_t total,
              const Hash32 &root)
    {
        m_path = path;
        m_count = chunkCount;
        m_bits.assign(size_t((chunkCount + 7) / 8), 0);
        m_chunkSize = chunkSize;
        m_total = total;
        m_root = root;
        return true;
    }

    // Пытается поднять карту с диска. false — карты нет или она от другого
    // тома (тогда вызывающий начинает с нуля).
    bool load()
    {
        const platform::File fd = platform::fileOpenRead(m_path);
        if (fd == platform::kInvalidFile)
            return false;

        uint8_t head[8 + 4 + 4 + 8 + 8 + 32];
        const int64_t got = platform::fileReadAt(fd, head, sizeof(head), 0);
        if (got != int64_t(sizeof(head))) {
            platform::fileClose(fd);
            return false;
        }
        if (std::memcmp(head, "FERRYMAP", 8) != 0) {
            platform::fileClose(fd);
            return false;
        }
        const uint32_t version = (uint32_t(head[8]) << 24) | (uint32_t(head[9]) << 16)
                                 | (uint32_t(head[10]) << 8) | head[11];
        const uint32_t chunkSize = (uint32_t(head[12]) << 24) | (uint32_t(head[13]) << 16)
                                   | (uint32_t(head[14]) << 8) | head[15];
        const uint64_t count = readBe64(head + 16);
        const uint64_t total = readBe64(head + 24);

        // Корневой хеш — то, что делает карту принадлежащей именно этому
        // тому. Совпало имя файла, но не совпал корень — значит рядом
        // лежит недокачка чего-то другого, и мешать их нельзя.
        if (version != 1 || chunkSize != m_chunkSize || count != m_count || total != m_total
            || std::memcmp(head + 32, m_root.data(), 32) != 0) {
            platform::fileClose(fd);
            return false;
        }

        const int64_t bits = platform::fileReadAt(fd, m_bits.data(), m_bits.size(), sizeof(head));
        platform::fileClose(fd);
        return bits == int64_t(m_bits.size());
    }

    bool has(uint64_t index) const
    {
        return index < m_count && (m_bits[size_t(index / 8)] & (1u << (index % 8))) != 0;
    }

    void set(uint64_t index)
    {
        if (index < m_count)
            m_bits[size_t(index / 8)] |= uint8_t(1u << (index % 8));
    }

    uint64_t haveCount() const
    {
        uint64_t n = 0;
        for (uint64_t i = 0; i < m_count; ++i)
            if (has(i))
                ++n;
        return n;
    }

    // Сколько чанков подряд есть с начала. В M1 этого хватает и серверу:
    // он просто ставит курсор получателя на это место.
    uint64_t havePrefix() const
    {
        uint64_t n = 0;
        while (n < m_count && has(n))
            ++n;
        return n;
    }

    bool flush() const
    {
        const platform::File fd = platform::fileOpenReadWrite(m_path);
        if (fd == platform::kInvalidFile)
            return false;

        uint8_t head[8 + 4 + 4 + 8 + 8 + 32];
        std::memcpy(head, "FERRYMAP", 8);
        const uint32_t version = 1;
        for (int i = 0; i < 4; ++i)
            head[8 + i] = uint8_t(version >> (24 - 8 * i));
        for (int i = 0; i < 4; ++i)
            head[12 + i] = uint8_t(m_chunkSize >> (24 - 8 * i));
        for (int i = 0; i < 8; ++i)
            head[16 + i] = uint8_t(m_count >> (56 - 8 * i));
        for (int i = 0; i < 8; ++i)
            head[24 + i] = uint8_t(m_total >> (56 - 8 * i));
        std::memcpy(head + 32, m_root.data(), 32);

        bool okWrite = platform::fileWriteAt(fd, head, sizeof(head), 0) == int64_t(sizeof(head));
        okWrite = okWrite
                  && platform::fileWriteAt(fd, m_bits.data(), m_bits.size(), sizeof(head))
                         == int64_t(m_bits.size());
        // Длина карты фиксирована и известна заранее, поэтому обрезать
        // хвост не нужно: файл либо новый, либо ровно такой же.
        platform::fileClose(fd);
        return okWrite;
    }

    void remove() const { platform::fileRemove(m_path); }

private:
    std::string m_path;
    std::vector<uint8_t> m_bits;
    uint64_t m_count = 0;
    uint32_t m_chunkSize = 0;
    uint64_t m_total = 0;
    Hash32 m_root{};
};

class RateMeter
{
public:
    void add(uint64_t bytes)
    {
        const int64_t t = nowMs();
        if (m_lastMs == 0) {
            m_lastMs = t;
            m_bytes = bytes;
            return;
        }
        m_bytes += bytes;
        const int64_t dt = t - m_lastMs;
        if (dt >= 500) {
            const double instant = double(m_bytes) * 1000.0 / double(dt);
            m_value = m_value <= 0 ? instant : m_value * 0.7 + instant * 0.3;
            m_bytes = 0;
            m_lastMs = t;
        }
    }
    double value() const { return m_value; }

private:
    int64_t m_lastMs = 0;
    uint64_t m_bytes = 0;
    double m_value = 0;
};

bool askYesNo(const std::string &question)
{
    std::printf("%s [д/н] ", question.c_str());
    std::fflush(stdout);
    char buf[16] = {};
    if (!std::fgets(buf, sizeof(buf), stdin))
        return false;
    const char c = buf[0];
    return c == 'y' || c == 'Y' || c == 'd' || c == 'D'
           || (static_cast<unsigned char>(buf[0]) == 0xD0
               && (static_cast<unsigned char>(buf[1]) == 0xB4       // д
                   || static_cast<unsigned char>(buf[1]) == 0x94)); // Д
}

} // namespace

int runGet(const Options &options)
{
    using namespace ferry::ui;

    // ---- 1. Ссылка ----
    TransferLink link;
    std::string err;
    if (!options.link.empty()) {
        if (!TransferLink::parse(options.link, link, &err)) {
            std::fprintf(stderr, "Ссылка не разобралась: %s\n", err.c_str());
            std::fprintf(stderr,
                         "%sСсылку в кавычках: шелл съедает всё после решётки, а там ключ.%s\n",
                         dim(), reset());
            return 1;
        }
    } else {
        // Разобранный вид на случай, если решётку по дороге потеряли.
        if (!options.relay.empty()) {
            Relay r;
            if (!Relay::parse(options.relay, options.insecure, r, &err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return 1;
            }
            link.scheme = r.tls ? "https" : "http";
            const bool defaultPort = (r.tls && r.port == 443) || (!r.tls && r.port == 80);
            link.host = r.host;
            if (!defaultPort) {
                link.host += ':';
                link.host += std::to_string(r.port);
            }
        } else {
            std::fprintf(stderr, "С --id нужен ещё и --relay: иначе непонятно, к какому серверу "
                                 "идти.\n");
            return 1;
        }
        link.id = options.id;
        if (!isValidTransferId(link.id)) {
            std::fprintf(stderr, "Идентификатор раздачи не той формы.\n");
            return 1;
        }
    }

    if (!options.key.empty()) {
        Bytes raw;
        if (!base64UrlDecode(options.key, raw) || raw.size() != kMasterKeySize) {
            std::fprintf(stderr, "Ключ не той длины.\n");
            return 1;
        }
        std::memcpy(link.key.data(), raw.data(), kMasterKeySize);
        link.hasKey = true;
    }

    if (!link.hasKey) {
        std::fprintf(stderr,
                     "В ссылке нет ключа — части после решётки.\n"
                     "%sБез неё расшифровать нечего: сервер ключа не знает и знать не может.\n"
                     "Попросите прислать ссылку целиком.%s\n", dim(), reset());
        return 1;
    }

    Relay relay;
    if (!Relay::parse(link.origin(), options.insecure, relay, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    if (relay.insecure) {
        printWarningBox("сертификат сервера не проверяется",
                        {"Запущено с --insecure. Посредник может подменить сервер и",
                         "подсунуть другой том. Ключ при этом не утекает — он никогда",
                         "не покидает это устройство, — но доверять тому, что приедет,",
                         "можно ровно настолько, насколько вы доверяете сети.",
                         "",
                         "Когда у релея появится домен, флаг станет не нужен."});
        std::printf("\n");
    }

    // ---- 2. Метаданные ----
    TransferMeta meta;
    if (!apiFetchMeta(relay, link.id, meta, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    const ChunkPlan plan = planWith(meta.total, meta.chunkSize);
    if (!plan.valid() || plan.chunkCount != meta.chunks) {
        std::fprintf(stderr, "Сервер описал том неправдоподобно — размеры не сходятся.\n");
        return 1;
    }

    const TransferKeys keys = TransferKeys::derive(link.key);

    Bytes manifestPlain;
    if (!openChunk(keys.meta, meta.noncePrefix.data(), kMetaLabelManifest, plan.chunkCount,
                   meta.manifest.data(), meta.manifest.size(), manifestPlain)) {
        std::fprintf(stderr,
                     "Манифест не расшифровался.\n"
                     "%sЛибо ключ в ссылке не тот, либо сервер описал том иначе, чем\n"
                     "отправитель: размер тома входит в проверяемые данные.%s\n", dim(), reset());
        return 1;
    }

    Manifest manifest;
    if (!Manifest::fromJson(std::string(manifestPlain.begin(), manifestPlain.end()), manifest,
                            &err)) {
        std::fprintf(stderr, "Манифест не разобрался: %s\n", err.c_str());
        return 1;
    }
    if (manifest.total != meta.total) {
        std::fprintf(stderr, "Размер в манифесте не сходится с размером раздачи.\n");
        return 1;
    }
    if (manifest.isTree()) {
        std::fprintf(stderr, "Это папка, а папки принимать эта версия ещё не умеет.\n");
        return 1;
    }

    Bytes hashesPlain;
    if (!openChunk(keys.meta, meta.noncePrefix.data(), kMetaLabelHashList, plan.chunkCount,
                   meta.hashList.data(), meta.hashList.size(), hashesPlain)) {
        std::fprintf(stderr, "Список хешей не расшифровался.\n");
        return 1;
    }

    HashList hashes;
    if (!HashList::parse(hashesPlain, plan.chunkCount, hashes)) {
        std::fprintf(stderr, "Список хешей не той длины.\n");
        return 1;
    }
    // Корень лежит в манифесте, а не в ссылке — иначе сервер видел бы
    // content-id и сопоставлял бы разные раздачи одного файла. Пока корень
    // не сошёлся, списку хешей верить нельзя, а без него нельзя проверить
    // ни одного чанка.
    if (hashes.root() != manifest.root) {
        std::fprintf(stderr, "Корневой хеш не сошёлся со списком хешей — том не принимаем.\n");
        return 1;
    }

    // ---- 3. Что это и куда класть ----
    const std::string outName = safeFileName(manifest.name);
    if (outName.empty()) {
        std::fprintf(stderr, "Имя из манифеста не годится для файла.\n");
        return 1;
    }
    const std::string outPath = options.outPath.empty() ? outName : options.outPath;
    const std::string partPath = outPath + ".ferry-part";
    const std::string mapPath = outPath + ".ferry-map";

    const int cells = std::min(width(), 78);
    std::printf("%s%s%s\n", dim(), ruleTop("том", cells).c_str(), reset());
    std::printf("%s│%s %s\n", dim(), reset(),
                field("имя", std::string(accent()) + manifest.name + reset()).c_str());
    std::printf("%s│%s %s\n", dim(), reset(), field("размер", bytes(manifest.total)).c_str());
    std::printf("%s│%s %s\n", dim(), reset(),
                field("чанки", count(plan.chunkCount) + " по " + bytes(plan.chunkSize)).c_str());
    std::printf("%s│%s %s\n", dim(), reset(),
                field("состояние", meta.state == "active" ? "отправитель на связи"
                                                          : "отправителя нет").c_str());
    if (meta.usesLeft >= 0)
        std::printf("%s│%s %s\n", dim(), reset(),
                    field("использований", std::to_string(meta.usesLeft)).c_str());
    std::printf("%s│%s %s\n", dim(), reset(), field("сохранить в", outPath).c_str());
    std::printf("%s%s%s\n", dim(), ruleBottom(cells).c_str(), reset());

    if (platform::fileExists(outPath)) {
        std::fprintf(stderr, "\n%s уже существует. Укажите другое имя через -o.\n",
                     outPath.c_str());
        return 1;
    }

    if (!options.yes) {
        if (!isTty()) {
            std::fprintf(stderr,
                         "\nВывод не в терминал, а подтверждения не было. Добавьте -y.\n");
            return 1;
        }
        std::printf("\n");
        if (!askYesNo("Забирать?")) {
            std::printf("Не забираем.\n");
            return 0;
        }
    }

    // ---- 4. Файл и карта принятого ----
    ChunkMap map;
    map.init(mapPath, plan.chunkCount, plan.chunkSize, plan.totalBytes, manifest.root);
    const bool resuming = map.load() && platform::fileExists(partPath);
    if (!resuming) {
        map.init(mapPath, plan.chunkCount, plan.chunkSize, plan.totalBytes, manifest.root);
        platform::fileRemove(partPath);
    }

    const platform::File fd = platform::fileOpenReadWrite(partPath);
    if (fd == platform::kInvalidFile) {
        std::fprintf(stderr, "Не смог создать %s\n", partPath.c_str());
        return 1;
    }
    // Растягиваем файл сразу на полный размер: дальше мы пишем по
    // смещениям, и место должно быть заранее — иначе первая же дырка
    // превратится в ошибку записи на середине тома.
    if (!platform::fileTruncate(fd, plan.totalBytes)) {
        std::fprintf(stderr, "Не хватает места под %s\n", bytes(plan.totalBytes).c_str());
        platform::fileClose(fd);
        return 1;
    }

    // Карту пишем сразу, ещё до первого чанка. Иначе обрыв в первые
    // секунды оставлял бы на диске недокачку БЕЗ карты — файл есть, а
    // понять, что в нём настоящее, нечем, и при следующем запуске всё
    // начиналось бы с нуля. Дальше она обновляется раз в пару секунд.
    map.flush();

    uint64_t have = map.haveCount();
    if (resuming && have > 0) {
        std::printf("%sпродолжаем: уже принято %s из %s (%s)%s\n", ok(), count(have).c_str(),
                    count(plan.chunkCount).c_str(),
                    percent(double(have) / double(plan.chunkCount)).c_str(), reset());
    }

    // ---- 5. Доказательство владения ключом ----
    Bytes challenge;
    if (!apiFetchChallenge(relay, link.id, challenge, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        platform::fileClose(fd);
        return 1;
    }
    const Bytes proof = proveKeyOwnership(keys.verifier, challenge.data(), challenge.size());
    if (proof.size() != 32) {
        std::fprintf(stderr, "Не удалось посчитать доказательство владения ключом.\n");
        platform::fileClose(fd);
        return 1;
    }

    // ---- 6. Подключаемся ----
    net::WebSocketClient ws;
    if (!ws.connectTo(relay.host, relay.port, relay.tls, relay.insecure, std::string(kWsPath),
                      20000, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        platform::fileClose(fd);
        return 1;
    }

    {
        json::Value hello = json::Value::object();
        hello.set("type", json::Value::make("hello"));
        hello.set("id", json::Value::make(link.id));
        hello.set("challenge", json::Value::make(base64UrlEncode(challenge)));
        hello.set("proof", json::Value::make(base64UrlEncode(proof)));
        if (!options.name.empty())
            hello.set("name", json::Value::make(options.name));
        // Докуда мы уже дошли. Сервер поставит курсор сюда и не будет
        // присылать то, что у нас есть.
        hello.set("have_upto", json::Value::make(int64_t(map.havePrefix())));
        ws.sendText(hello.dump());
    }

    // Чанки, приехавшие вместе с hello_ok. Сервер начинает лить сразу за
    // подтверждением, и обе вещи приходят одной пачкой; выбросить их
    // значит потерять начало тома и встать намертво, потому что второй раз
    // сервер их не пришлёт — по его учёту они уже у нас.
    std::deque<net::WsMessage> deferred;

    bool accepted = false;
    const int64_t helloDeadline = nowMs() + 20000;
    while (!accepted && nowMs() < helloDeadline && !stopRequested()) {
        if (!ws.pump(200)) {
            // Сервер отвечает ошибкой и сразу закрывает сокет: причина уже
            // лежит в очереди, надо только её оттуда достать.
            const std::string reason = drainErrorReason(ws);
            if (!reason.empty())
                std::fprintf(stderr, "%s\n", explainError(reason).c_str());
            else
                std::fprintf(stderr, "Соединение оборвалось: %s\n", ws.error().c_str());
            platform::fileClose(fd);
            return 1;
        }
        net::WsMessage msg;
        while (ws.next(msg)) {
            if (msg.binary) {
                deferred.push_back(std::move(msg));
                continue;
            }
            json::Value v;
            if (!json::Value::parse(msg.data, v) || !v.isObject())
                continue;
            const std::string type = v["type"].toString();
            if (type == "hello_ok") {
                accepted = true;
            } else if (type == "error") {
                std::fprintf(stderr, "%s\n", explainError(v["reason"].toString()).c_str());
                platform::fileClose(fd);
                return 1;
            }
        }
    }
    if (!accepted) {
        std::fprintf(stderr, "Сервер не пустил к раздаче.\n");
        platform::fileClose(fd);
        return 1;
    }

    std::printf("\n");

    // ---- 7. Приём ----
    RateMeter meter;
    LivePanel panel;
    std::vector<Seg> segments(size_t(plan.chunkCount), Seg::None);
    for (uint64_t i = 0; i < plan.chunkCount; ++i)
        if (map.has(i))
            segments[size_t(i)] = Seg::Have;

    uint64_t received = have;
    uint64_t ackedUpTo = map.havePrefix();
    int64_t lastFlushMs = nowMs();
    int64_t lastAckMs = nowMs();
    const int64_t startedMs = nowMs();
    bool failed = false;
    std::string failure;

    while (!stopRequested() && received < plan.chunkCount) {
        if (!ws.pump(100)) {
            failed = true;
            const std::string reason = drainErrorReason(ws);
            if (!reason.empty())
                failure = explainError(reason);
            else
                failure = ws.error().empty() ? std::string("соединение закрылось") : ws.error();
            break;
        }

        net::WsMessage msg;
        while (!deferred.empty() || ws.next(msg)) {
            if (!deferred.empty()) {
                msg = std::move(deferred.front());
                deferred.pop_front();
            }
            if (!msg.binary) {
                json::Value v;
                if (!json::Value::parse(msg.data, v) || !v.isObject())
                    continue;
                if (v["type"].toString() == "error") {
                    failed = true;
                    failure = explainError(v["reason"].toString());
                }
                continue;
            }

            if (msg.data.size() < kBinaryHeaderSize) {
                failed = true;
                failure = "сервер прислал обрезанный фрейм";
                break;
            }
            const auto *raw = reinterpret_cast<const uint8_t *>(msg.data.data());
            if (raw[0] != OpChunk)
                continue;
            const uint64_t index = readBe64(raw + 1);
            if (index >= plan.chunkCount) {
                failed = true;
                failure = "сервер прислал чанк с несуществующим номером";
                break;
            }
            if (map.has(index))
                continue;   // повтор — не беда

            Bytes plainChunk;
            if (!openChunk(keys.data, meta.noncePrefix.data(), index, plan.chunkCount,
                           raw + kBinaryHeaderSize, msg.data.size() - kBinaryHeaderSize,
                           plainChunk)) {
                failed = true;
                failure = "чанк не расшифровался — тег AEAD не сошёлся";
                break;
            }
            if (plainChunk.size() != plan.sizeOf(index)) {
                failed = true;
                failure = "чанк пришёл не той длины";
                break;
            }
            // Проверка против списка хешей — за O(1) и независимо от
            // порядка. Именно она делает безопасным приём чанков от кого
            // угодно: подсунуть мусор нельзя, подмена ловится на месте.
            if (!hashes.verify(index, plainChunk.data(), plainChunk.size())) {
                failed = true;
                failure = explainError(err::kChunkMismatch);
                break;
            }

            const int64_t written = platform::fileWriteAt(fd, plainChunk.data(),
                                                          plainChunk.size(),
                                                          plan.offsetOf(index));
            if (written != int64_t(plainChunk.size())) {
                failed = true;
                failure = "не удалось записать на диск — кончилось место?";
                break;
            }

            map.set(index);
            segments[size_t(index)] = Seg::FromSender;
            ++received;
            meter.add(plainChunk.size());
        }
        if (failed)
            break;

        const int64_t t = nowMs();

        // Карта на диск — раз в полсекунды, а не на каждый чанк: на
        // гигабитном канале это была бы тысяча записей в секунду.
        //
        // Полсекунды, а не две: карта весит один бит на чанк (у тома в
        // 5 ГиБ это 160 байт), и запись её стоит ровно ничего. Зато
        // интервал — это ровно столько работы, сколько теряется, если
        // клиента убьют не по-хорошему, а на быстром канале за две
        // секунды успевает приехать пара сотен мегабайт.
        if (t - lastFlushMs > 500) {
            map.flush();
            lastFlushMs = t;
        }

        const uint64_t prefix = map.havePrefix();
        if (prefix != ackedUpTo && t - lastAckMs > 300) {
            json::Value ack = json::Value::object();
            ack.set("type", json::Value::make("ack"));
            ack.set("upto", json::Value::make(int64_t(prefix)));
            ws.sendText(ack.dump());
            ackedUpTo = prefix;
            lastAckMs = t;
        }

        const double frac = plan.chunkCount ? double(received) / double(plan.chunkCount) : 1.0;
        const double remaining = double(plan.totalBytes) * (1.0 - frac);
        const int64_t eta = meter.value() > 1 ? int64_t(remaining / meter.value() * 1000) : -1;

        panel.update({
            std::string("принято  ") + bar(frac, std::max(10, cells - 40)) + "  " + percent(frac)
                + "  " + bytes(uint64_t(double(plan.totalBytes) * frac)) + " из "
                + bytes(plan.totalBytes),
            std::string("скорость ") + rate(meter.value()) + "   осталось " + duration(eta)
                + "   идёт " + duration(t - startedMs),
            std::string("карта    ") + volumeMap(segments, std::max(20, cells - 12)),
        });
    }

    panel.finish();
    map.flush();

    if (failed) {
        platform::fileSync(fd);
        platform::fileClose(fd);
        ws.closeGracefully();
        std::fprintf(stderr, "\n%sПриём прерван:%s %s\n", bad(), reset(), failure.c_str());
        std::fprintf(stderr, "%sПринятое сохранено в %s — при следующем запуске продолжим.%s\n",
                     dim(), partPath.c_str(), reset());
        return 1;
    }

    if (stopRequested()) {
        platform::fileSync(fd);
        platform::fileClose(fd);
        ws.closeGracefully();
        std::printf("\n%sОстановлено. Принятое сохранено в %s.%s\n", dim(), partPath.c_str(),
                    reset());
        return 130;
    }

    // ---- 8. Готово ----
    if (!platform::fileSync(fd)) {
        std::fprintf(stderr, "Не удалось дописать файл на диск.\n");
        platform::fileClose(fd);
        return 1;
    }
    platform::fileClose(fd);
    ws.closeGracefully();

    if (!platform::fileRename(partPath, outPath)) {
        std::fprintf(stderr, "Не удалось переименовать %s в %s\n", partPath.c_str(),
                     outPath.c_str());
        return 1;
    }
    map.remove();

    const int64_t tookMs = nowMs() - startedMs;
    std::printf("\n%s%s%s\n", ok(), ruleTop("готово", cells).c_str(), reset());
    std::printf("%s│%s %s\n", ok(), reset(), field("файл", outPath).c_str());
    std::printf("%s│%s %s\n", ok(), reset(), field("размер", bytes(plan.totalBytes)).c_str());
    std::printf("%s│%s %s\n", ok(), reset(),
                field("время", duration(tookMs) + "  ("
                                   + rate(double(plan.totalBytes) * 1000.0
                                          / double(std::max<int64_t>(tookMs, 1)))
                                   + ")").c_str());
    std::printf("%s│%s %s\n", ok(), reset(),
                field("целостность", "проверено BLAKE3 по каждому чанку").c_str());
    std::printf("%s%s%s\n", ok(), ruleBottom(cells).c_str(), reset());
    return 0;
}

} // namespace ferry::cli
