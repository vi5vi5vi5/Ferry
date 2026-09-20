#include "Cli/Sender.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Cli/ChunkPump.h"
#include "Cli/Signals.h"
#include "Cli/net/WebSocketClient.h"
#include "Cli/platform/Platform.h"
#include "Cli/ui/LivePanel.h"
#include "Cli/ui/Qr.h"
#include "Cli/ui/Term.h"
#include "core/Base64Url.h"
#include "core/Chunker.h"
#include "core/ChunkSet.h"
#include "core/Crypto.h"
#include "core/HashList.h"
#include "core/Json.h"
#include "core/Link.h"
#include "core/Manifest.h"
#include "core/Protocol.h"

namespace ferry::cli {
namespace {

// Доля догона в аплоаде отправителя — три десятых (§6).
constexpr uint64_t kBackfillShare = 3;
constexpr uint64_t kLiveShare = 7;

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Сколько байт держим в очереди сокета. Меньше — на быстром канале
// появляются паузы между чанками; больше — мы читаем с диска и шифруем
// то, что уедет через полминуты, и зря держим это в памяти.
constexpr size_t kOutgoingHighWater = 8u * 1024u * 1024u;

std::string baseName(const std::string &path)
{
    size_t cut = 0;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/' || path[i] == '\\')
            cut = i + 1;
    }
    return path.substr(cut);
}

std::string b64(const Bytes &data)
{
    return base64UrlEncode(data.data(), data.size());
}

struct PeerRow
{
    int id = 0;
    std::string name;
    double progress = 0;
    std::string role;
};

// Скользящая средняя скорости. Мгновенная скорость на чанках прыгает так,
// что читать её невозможно; средняя за всё время, наоборот, не показывает,
// что канал просел прямо сейчас.
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

} // namespace

int runSend(const Options &options, const Relay &relay)
{
    using namespace ferry::ui;

    // ---- 1. Открываем файл ----
    platform::FileInfo info;
    if (!platform::fileStat(options.path, info)) {
        std::fprintf(stderr, "Не нашёл файл: %s\n", options.path.c_str());
        return 1;
    }
    if (info.isDirectory) {
        std::fprintf(stderr,
                     "%s — это папка. Папки Ferry научится возить в следующей версии;\n"
                     "пока упакуйте её, например: tar -C %s -cf - . | zstd -o папка.tar.zst\n",
                     options.path.c_str(), options.path.c_str());
        return 1;
    }

    const platform::File fd = platform::fileOpenRead(options.path);
    if (fd == platform::kInvalidFile) {
        std::fprintf(stderr, "Не смог открыть %s\n", options.path.c_str());
        return 1;
    }

    const uint64_t total = info.size;
    const ChunkPlan plan = planFor(total);
    if (!plan.valid()) {
        std::fprintf(stderr, "Не понял размер файла.\n");
        platform::fileClose(fd);
        return 1;
    }

    const std::string name = safeFileName(baseName(options.path));
    if (name.empty()) {
        std::fprintf(stderr, "Из имени файла не получилось ничего пригодного.\n");
        platform::fileClose(fd);
        return 1;
    }

    const int cells = std::min(width(), 78);
    std::printf("%s%s%s\n", dim(), ruleTop("том", cells).c_str(), reset());
    std::printf("%s│%s %s\n", dim(), reset(),
                field("имя", std::string(accent()) + name + reset()).c_str());
    std::printf("%s│%s %s\n", dim(), reset(), field("размер", bytes(total)).c_str());
    std::printf("%s│%s %s\n", dim(), reset(),
                field("чанки", count(plan.chunkCount) + " по " + bytes(plan.chunkSize)).c_str());
    std::printf("%s%s%s\n", dim(), ruleBottom(cells).c_str(), reset());

    // ---- 2. Хеши ----
    // Полный проход по файлу до начала раздачи. На пяти гигабайтах это
    // секунды, но молчать нельзя: человек не должен гадать, почему ничего
    // не происходит.
    HashList hashes;
    hashes.reserve(size_t(plan.chunkCount));
    {
        std::vector<uint8_t> buffer(plan.chunkSize);
        LivePanel panel;
        RateMeter meter;
        const int64_t startedMs = nowMs();
        for (uint64_t i = 0; i < plan.chunkCount; ++i) {
            const uint32_t len = plan.sizeOf(i);
            const int64_t got = platform::fileReadAt(fd, buffer.data(), len, plan.offsetOf(i));
            if (got != int64_t(len)) {
                panel.finish();
                std::fprintf(stderr, "\nФайл читается не целиком — он изменился прямо сейчас?\n");
                platform::fileClose(fd);
                return 1;
            }
            hashes.append(blake3(buffer.data(), size_t(len)));
            meter.add(len);

            if ((i % 16) == 0 || i + 1 == plan.chunkCount) {
                const double frac = double(i + 1) / double(plan.chunkCount);
                panel.update({std::string("считаю хеши  ") + bar(frac, cells - 30) + "  "
                              + percent(frac) + "  " + rate(meter.value())});
            }
            if (stopRequested()) {
                panel.finish();
                platform::fileClose(fd);
                return 130;
            }
        }
        panel.finish();
        std::printf("%sхеши готовы за %s — BLAKE3, %s хешей%s\n", dim(),
                    duration(nowMs() - startedMs).c_str(), count(hashes.size()).c_str(), reset());
    }

    // ---- 3. Ключи и метаданные ----
    bool ok = false;
    const Key32 master = randomKey32(&ok);
    if (!ok) {
        std::fprintf(stderr, "Системный генератор случайных чисел недоступен — "
                             "продолжать нельзя.\n");
        platform::fileClose(fd);
        return 1;
    }
    const TransferKeys keys = TransferKeys::derive(master);

    Bytes noncePrefix;
    if (!randomBytes(noncePrefix, kNoncePrefixSize)) {
        std::fprintf(stderr, "Системный генератор случайных чисел недоступен.\n");
        platform::fileClose(fd);
        return 1;
    }

    Manifest manifest;
    manifest.kind = "file";
    manifest.name = name;
    manifest.total = total;
    manifest.root = hashes.root();
    const std::string manifestJson = manifest.toJson();

    Bytes encManifest, encHashes;
    const Bytes rawHashes = hashes.serialize();
    if (!sealChunk(keys.meta, noncePrefix.data(), kMetaLabelManifest, plan.chunkCount,
                   reinterpret_cast<const uint8_t *>(manifestJson.data()), manifestJson.size(),
                   encManifest)
        || !sealChunk(keys.meta, noncePrefix.data(), kMetaLabelHashList, plan.chunkCount,
                      rawHashes.data(), rawHashes.size(), encHashes)) {
        std::fprintf(stderr, "Не удалось зашифровать метаданные.\n");
        platform::fileClose(fd);
        return 1;
    }

    // ---- 4. Заводим раздачу ----
    CreatedTransfer created;
    std::string err;
    if (!apiCreateTransfer(relay, created, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        platform::fileClose(fd);
        return 1;
    }

    TransferLink link;
    {
        // Ссылку собираем от того адреса, каким сервер представляется
        // снаружи. Если владелец сервера его не задал — от того, по
        // которому мы сами пришли: за прокси сервер своего имени не знает.
        TransferLink fromPublic;
        if (!created.publicUrl.empty()
            && TransferLink::parse(created.publicUrl + "/t/" + created.id, fromPublic, nullptr)) {
            link = fromPublic;
        } else {
            link.scheme = relay.tls ? "https" : "http";
            const bool defaultPort = (relay.tls && relay.port == 443)
                                     || (!relay.tls && relay.port == 80);
            link.host = relay.host;
            if (!defaultPort) {
                link.host += ':';
                link.host += std::to_string(relay.port);
            }
            link.id = created.id;
        }
        link.key = master;
        link.hasKey = true;
    }

    // ---- 5. Подключаемся и делаем offer ----
    net::WebSocketClient ws;
    if (!ws.connectTo(relay.host, relay.port, relay.tls, relay.insecure, std::string(kWsPath),
                      20000, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        platform::fileClose(fd);
        return 1;
    }

    {
        json::Value offer = json::Value::object();
        offer.set("type", json::Value::make("offer"));
        offer.set("id", json::Value::make(created.id));
        offer.set("owner_token", json::Value::make(created.ownerToken));
        offer.set("total", json::Value::make(int64_t(total)));
        offer.set("chunk_size", json::Value::make(int64_t(plan.chunkSize)));
        offer.set("chunks", json::Value::make(int64_t(plan.chunkCount)));
        offer.set("nonce_prefix", json::Value::make(b64(noncePrefix)));
        offer.set("verifier",
                  json::Value::make(base64UrlEncode(keys.verifier.data(), keys.verifier.size())));
        offer.set("manifest", json::Value::make(b64(encManifest)));
        offer.set("hash_list", json::Value::make(b64(encHashes)));
        offer.set("mode", json::Value::make("key"));

        json::Value policy = json::Value::object();
        if (options.uses > 0)
            policy.set("uses", json::Value::make(int64_t(options.uses)));
        if (options.ttlMs > 0)
            policy.set("ttl_ms", json::Value::make(options.ttlMs));
        if (options.maxConcurrent > 0)
            policy.set("max_concurrent", json::Value::make(int64_t(options.maxConcurrent)));
        offer.set("policy", std::move(policy));

        ws.sendText(offer.dump());
    }

    // Что сервер попросил и что мы ещё не отдали — двумя очередями.
    //
    // Здесь было одно число — самая правая граница просьбы, — и этого
    // хватало, пока сервер умел просить только «дальше». Как только
    // появляется опоздавший получатель, сервер просит НАЗАД, и монотонный
    // курсор на такую просьбу не отвечал вообще — молча и навсегда.
    //
    // Очередей две, и порядок между ними строгий: сначала живая волна,
    // потом догон. Иначе один опоздавший тянул бы начало тома впереди
    // всех, кто качает вовремя, — ровно то, чего должен не допускать
    // drop-behind.
    //
    // Объявлено ДО ожидания offer_ok, и это не мелочь: сервер шлёт первый
    // need сразу за подтверждением, оба сообщения приезжают одной пачкой, и
    // цикл ожидания обязан его сохранить, а не выбросить. Потерянный need
    // означает раздачу, которая вежливо стоит на нуле и ничего не говорит.
    ChunkSet pendingLive(plan.chunkCount);
    ChunkSet pendingBackfill(plan.chunkCount);

    // Откуда искать следующий чанк в каждой очереди.
    //
    // Без подсказки пришлось бы каждый раз просматривать карту с
    // нуля: у тома в терабайт это 32 КиБ на каждый отправленный чанк,
    // причём почти всё — пустота позади курсора. Очередь
    // разбирается по возрастанию, поэтому подсказка только растёт —
    // кроме случая, когда сервер попросил назад и её надо откатить.
    uint64_t liveHint = 0;
    uint64_t backfillHint = 0;

    const auto rememberNeed = [&](const json::Value &v) {
        // Полоса необязательная: релей M1 о ней не знает, а всё, что он
        // просит, — это живая волна по определению.
        const bool backfill = v["lane"].toString("live") == "backfill";
        ChunkSet &queue = backfill ? pendingBackfill : pendingLive;

        const json::Value &ranges = v["ranges"];
        for (size_t i = 0; i < ranges.size(); ++i) {
            const json::Value &r = ranges.at(i);
            if (r.size() < 2)
                continue;
            const int64_t from = r.at(0).toInt(-1);
            const int64_t to = r.at(1).toInt(-1);
            if (from < 0 || to < from)
                continue;
            if (!queue.setRange({uint64_t(from), uint64_t(to)}))
                continue;   // диапазон за пределами тома — мы такого не отдадим
            uint64_t &hint = backfill ? backfillHint : liveHint;
            hint = std::min(hint, uint64_t(from));
        }
    };

    // Ждём offer_ok: пока сервер не принял раздачу, печатать ссылку нельзя.
    bool accepted = false;
    const int64_t offerDeadline = nowMs() + 20000;
    while (!accepted && nowMs() < offerDeadline && !stopRequested()) {
        if (!ws.pump(200)) {
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
            json::Value v;
            if (msg.binary || !json::Value::parse(msg.data, v) || !v.isObject())
                continue;
            const std::string type = v["type"].toString();
            if (type == "offer_ok") {
                accepted = true;
            } else if (type == "need") {
                rememberNeed(v);
            } else if (type == "error") {
                std::fprintf(stderr, "%s\n", explainError(v["reason"].toString()).c_str());
                platform::fileClose(fd);
                return 1;
            }
        }
    }
    if (!accepted) {
        std::fprintf(stderr, "Сервер не подтвердил раздачу.\n");
        platform::fileClose(fd);
        return 1;
    }

    // ---- 6. Ссылка ----
    const std::string linkText = link.toString();
    std::printf("\n%s%s%s\n", dim(), ruleTop("ссылка и ключ", cells).c_str(), reset());
    std::printf("%s│%s %s%s%s\n", dim(), reset(), accent(), linkText.c_str(), reset());
    std::printf("%s│%s\n", dim(), reset());
    std::printf("%s│%s %sЧасть после решётки — это ключ. На сервер она не уходит.%s\n", dim(),
                reset(), dim(), reset());
    std::printf("%s│%s %sКто получил ссылку целиком — получил том.%s\n", dim(), reset(), dim(),
                reset());
    std::printf("%s%s%s\n", dim(), ruleBottom(cells).c_str(), reset());

    std::printf("\nЗабрать:  %sferry get \"%s\"%s\n", bold(), linkText.c_str(), reset());
    if (!options.noQr) {
        std::printf("\n");
        if (!printQr(linkText))
            std::printf("%s  (QR не поместился или вывод не в терминал)%s\n", dim(), reset());
    }

    if (options.uses > 0 || options.ttlMs > 0) {
        std::printf("\n%sполитика:%s ", dim(), reset());
        if (options.uses > 0)
            std::printf("использований %d  ", options.uses);
        if (options.ttlMs > 0)
            std::printf("живёт %s", duration(options.ttlMs).c_str());
        std::printf("\n");
    }

    std::printf("\n%sРаздача идёт, пока запущена эта команда. На сервере, куда вы зашли по ssh,\n"
                "её стоит запускать в tmux:  tmux new -s ferry%s\n\n", dim(), reset());

    // ---- 7. Качаем ----
    ChunkPump pump;
    pump.init(fd, plan, keys.data, noncePrefix.data());
    ChunkSet sent(plan.chunkCount);
    uint64_t sentBytes = 0;
    uint64_t sentDistinctBytes = 0;
    uint64_t sentLiveChunks = 0;
    uint64_t sentBackfillChunks = 0;
    std::vector<PeerRow> peers;
    RateMeter meter;
    LivePanel panel;
    const int64_t startedMs = nowMs();
    bool failed = false;
    std::string failure;

    while (!stopRequested() && ws.isOpen()) {
        if (!ws.pump(100)) {
            if (!stopRequested()) {
                failed = true;
                const std::string reason = drainErrorReason(ws);
                if (!reason.empty())
                    failure = explainError(reason);
                else
                    failure = ws.error().empty() ? std::string("соединение закрылось") : ws.error();
            }
            break;
        }

        net::WsMessage msg;
        while (ws.next(msg)) {
            if (msg.binary)
                continue;
            json::Value v;
            if (!json::Value::parse(msg.data, v) || !v.isObject())
                continue;
            const std::string type = v["type"].toString();

            if (type == "need") {
                rememberNeed(v);
            } else if (type == "peers") {
                peers.clear();
                const json::Value &arr = v["receivers"];
                for (size_t i = 0; i < arr.size(); ++i) {
                    const json::Value &p = arr.at(i);
                    PeerRow row;
                    row.id = int(p["id"].toInt(0));
                    row.name = p["name"].toString();
                    row.progress = p["progress"].toDouble(0);
                    row.role = p["role"].toString();
                    peers.push_back(std::move(row));
                }
            } else if (type == "error") {
                failed = true;
                failure = explainError(v["reason"].toString());
            }
        }
        if (failed)
            break;

        // Шлём ровно то, что попросили, и ровно столько, сколько влезает в
        // очередь сокета. Читаем с диска лениво: файл целиком в память не
        // попадает никогда, каким бы большим он ни был.
        //
        // Живая волна идёт первой, но не вытесняет догон насовсем.
        //
        // Первая редакция была строгой: сначала вся живая волна, потом
        // остатки. В обычном случае это даже лучше доли — догон едет в
        // простое время отправителя, пока окно на сервере полно, — но есть
        // случай, когда простоя нет вовсе: получатели быстрее нашего
        // аплоада, окно всегда пустое, живая очередь никогда не пустеет —
        // и опоздавший не получает ничего вообще. Поэтому доля, как в §6:
        // догону до 30 %, группа проседает на треть, а не встаёт.
        while (ws.pendingBytes() < kOutgoingHighWater) {
            // Когда одна очередь пуста, вторая берёт всё.
            bool live = !pendingLive.empty();
            if (live && !pendingBackfill.empty()
                && sentBackfillChunks * kLiveShare < sentLiveChunks * kBackfillShare) {
                live = false;
            }
            ChunkSet &queue = live ? pendingLive : pendingBackfill;
            if (queue.empty())
                break;
            uint64_t &hint = live ? liveHint : backfillHint;

            // Меньший индекс вперёд — внутри каждой очереди отдельно. Для
            // живой волны это естественный порядок тома, для догона —
            // порядок, в котором опоздавший сможет писать подряд.
            uint64_t index = queue.firstPresent(hint);
            if (index >= plan.chunkCount) {
                // Подсказка ушла дальше всего, что осталось в очереди. Очередь
                // при этом НЕ пуста — значит искать надо с начала. Без этого
                // шага раздача встала бы насмерть, держа неотданные чанки и
                // не видя их.
                index = queue.firstPresent(0);
                if (index >= plan.chunkCount)
                    break;
            }
            hint = index;

            if (!pump.send(ws, index)) {
                failed = true;
                failure = pump.error();
                break;
            }
            const uint32_t len = pump.lastPlainSize();
            meter.add(len);

            // Два счётчика, и разница между ними — главное число всего
            // проекта. sent — сколько тома мы отдали хоть раз; sentBytes —
            // сколько байт ушло в сеть вообще. У парома второе обязано
            // равняться первому, сколько бы ни было получателей: этим он и
            // отличается от обычного релея.
            sentBytes += len;
            if (sent.set(index))
                sentDistinctBytes += len;
            if (live)
                ++sentLiveChunks;
            else
                ++sentBackfillChunks;

            queue.clear(index);
        }
        if (failed)
            break;

        // ---- панель ----
        std::vector<std::string> lines;
        // Прогресс — по РАЗЛИЧНЫМ чанкам, а не по отправленным байтам:
        // повторная отдача не приближает к концу и не должна двигать полоску.
        const double frac =
            plan.chunkCount ? double(sent.cardinality()) / double(plan.chunkCount) : 1.0;
        lines.push_back(std::string("отдано   ") + bar(frac, std::max(10, cells - 40)) + "  "
                        + percent(frac) + "  " + bytes(sentDistinctBytes) + " из " + bytes(total));

        // Повторы видны отдельной строкой и только тогда, когда они есть.
        // Это то самое число, ради которого всё затевалось: пока оно ноль,
        // паром везёт байт один раз, сколько бы ни было получателей.
        std::string second = std::string("скорость ") + rate(meter.value()) + "   в очереди "
                             + bytes(ws.pendingBytes()) + "   идёт "
                             + duration(nowMs() - startedMs);
        if (sentBytes > sentDistinctBytes)
            second += std::string("   ") + warn() + "повторно "
                      + bytes(sentBytes - sentDistinctBytes) + reset();
        lines.push_back(std::move(second));

        if (peers.empty()) {
            lines.push_back(std::string(dim()) + "получателей пока нет — ссылка ждёт" + reset());
        } else {
            lines.push_back(std::string(dim()) + "получатели:" + reset());
            for (const PeerRow &p : peers) {
                const std::string who = p.name.empty() ? ("гость " + std::to_string(p.id)) : p.name;
                // «Догоняет» стоит отдельного слова: такой получатель по процентам
                // выглядит отстающим, хотя на самом деле просто пришёл позже и
                // тянет начало второй волной.
                std::string role = "качает";
                if (p.role == "seed")
                    role = "готово";
                else if (p.role == "catching")
                    role = "догоняет";
                lines.push_back("  " + field(who, "", 18) + bar(p.progress, 20) + "  "
                                + percent(p.progress) + "  " + role);
            }
        }
        panel.update(lines);
    }

    panel.finish();
    platform::fileClose(fd);
    ws.closeGracefully();

    // Итог раздачи — двумя числами, и второе важнее первого.
    //
    // «Отдано» — сколько тома ушло хоть раз. «Повторно» — сколько байт
    // пришлось отдать сверх того. У парома второе обязано оставаться
    // нулём, сколько бы ни было получателей: они берут начало тома друг у
    // друга, а не у нас. Печатается всегда, в том числе при обрыве:
    // именно по этой строке видно, работает обещание или нет.
    std::printf("\n%s%s%s\n", dim(), ruleTop("итог", cells).c_str(), reset());
    std::printf("%s│%s %s\n", dim(), reset(),
                field("отдано", bytes(sentDistinctBytes) + " из " + bytes(total)).c_str());
    std::printf("%s│%s %s\n", dim(), reset(),
                field("повторно", sentBytes > sentDistinctBytes
                                      ? bytes(sentBytes - sentDistinctBytes)
                                      : std::string("ничего")).c_str());
    std::printf("%s│%s %s\n", dim(), reset(),
                field("всего в сеть", bytes(sentBytes)).c_str());
    std::printf("%s%s%s\n", dim(), ruleBottom(cells).c_str(), reset());

    if (failed) {
        std::fprintf(stderr, "\n%sРаздача прервана:%s %s\n", bad(), reset(), failure.c_str());
        return 1;
    }
    if (stopRequested())
        std::printf("\n%sРаздача остановлена. Ссылка больше не работает.%s\n", dim(), reset());
    return 0;
}

} // namespace ferry::cli
