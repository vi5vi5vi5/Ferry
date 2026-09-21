#include "Cli/Receiver.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <cstring>
#include <string>
#include <vector>

#include "Cli/ChunkPump.h"
#include "Cli/VolumeFile.h"
#include "Cli/Signals.h"
#include "Cli/net/WebSocketClient.h"
#include "Cli/platform/Platform.h"
#include "Cli/ui/LivePanel.h"
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
#include "core/VolumeLayout.h"

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

// Сколько чанков подряд готовы отвергнуть, прежде чем сдаться.
//
// Один испорченный чанк — не беда: сервер возьмёт его у другого
// источника. Сотня подряд означает, что чинить нечего — либо ключ
// не тот, либо сервер сломан, — и крутиться вечно хуже, чем сказать об
// этом вслух.
constexpr int kMaxBadChunks = 64;

// Сколько держим в очереди сокета, отдавая чужой догон. Меньше,
// чем у отправителя, и сознательно: сид — это прежде всего получатель,
// и его собственная загрузка важнее чужой.
constexpr int64_t kSeedHighWater = 2 * 1024 * 1024;

// Диапазоны в JSON: [[a,b],[c,d]], границы включительные с обеих сторон.
//
// Ровно то же самое сервер делает своим QJsonArray. Две реализации здесь
// неизбежны — у ядра нет ни json::Value, ни Qt одновременно, — а вот
// трактовка границ обязана быть одна, и она живёт в ferry::ChunkSet.
json::Value rangesToJson(const std::vector<ChunkRange> &ranges)
{
    json::Value arr = json::Value::array();
    for (const ChunkRange &r : ranges) {
        json::Value pair = json::Value::array();
        pair.push(json::Value::make(int64_t(r.from)));
        pair.push(json::Value::make(int64_t(r.to)));
        arr.push(std::move(pair));
    }
    return arr;
}

// Объявил ли собеседник возможность. Молчание — это «не умеет»: релей
// раздаёт свою версию клиента, но человек вполне мог принести старую, и
// заговорить с ней на языке, которого она не знает, значит получить от
// неё bad_message на ровном месте.
bool announces(const json::Value &msg, const char *feature)
{
    const json::Value &arr = msg["features"];
    for (size_t i = 0; i < arr.size(); ++i) {
        if (arr.at(i).toString() == feature)
            return true;
    }
    return false;
}

// Карта принятых чанков рядом с недокачанным файлом.
//
// Пишется уже сейчас, хотя в M1 чанки приходят по порядку и хватило бы
// одного числа. Причина простая: в M2 приходить они будут вразнобой, и
// формат, который придётся менять, — это формат, из-за которого чужая
// недокачка станет несовместимой. Лучше заложить его сразу.
//
// Само множество бит живёт в ferry::ChunkSet — общем типе ядра. Здесь
// остаётся только формат файла: заголовок, привязка к тому и запись на
// диск. Разделение нужно потому, что этими же диапазонами сервер и
// клиент разговаривают по проводу, и трактовка границ обязана быть одна
// на всех.
class ChunkMap
{
public:
    bool init(const std::string &path, uint64_t chunkCount, uint32_t chunkSize, uint64_t total,
              const Hash32 &root)
    {
        m_path = path;
        m_count = chunkCount;
        m_set.reset(chunkCount);
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

        std::vector<uint8_t> raw(m_set.byteCount());
        const int64_t bits = platform::fileReadAt(fd, raw.data(), raw.size(), sizeof(head));
        platform::fileClose(fd);
        return bits == int64_t(raw.size()) && m_set.loadBits(raw.data(), raw.size());
    }

    bool has(uint64_t index) const { return m_set.has(index); }
    void set(uint64_t index) { m_set.set(index); }

    uint64_t haveCount() const { return m_set.cardinality(); }

    // Сколько чанков подряд есть с начала. В M1 этого хватает и серверу:
    // он просто ставит курсор получателя на это место.
    uint64_t havePrefix() const { return m_set.prefix(); }

    // Само множество — для сообщений have и request: серверу нужны
    // диапазоны, а не одно число.
    const ferry::ChunkSet &set() const { return m_set; }

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

        const std::vector<uint8_t> &raw = m_set.bits();
        bool okWrite = platform::fileWriteAt(fd, head, sizeof(head), 0) == int64_t(sizeof(head));
        okWrite = okWrite
                  && platform::fileWriteAt(fd, raw.data(), raw.size(), sizeof(head))
                         == int64_t(raw.size());
        // Длина карты фиксирована и известна заранее, поэтому обрезать
        // хвост не нужно: файл либо новый, либо ровно такой же.
        platform::fileClose(fd);
        return okWrite;
    }

    void remove() const { platform::fileRemove(m_path); }

private:
    std::string m_path;
    ferry::ChunkSet m_set;
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

// Согласие человека на приём тома.
//
// Приглашение обещает «д», и «д» обязано работать — иначе это просто
// вранье в интерфейсе. Сложность в том, что на той стороне может быть
// какая угодно кодировка ввода: терминал по ssh обычно отдаёт UTF-8,
// консоль Windows — CP866, PuTTY с чужими настройками — CP1251. Одного
// варианта мало, поэтому принимаем все три, благо байты у них не
// пересекаются с «нет» ни в одной из кодировок.
//
// Всё, что не опознано как согласие, считается отказом: подтверждение
// стоит перед записью гигабайтов на чужой диск, и «наверное, да» здесь
// неуместно.
bool askYesNo(const std::string &question)
{
    std::printf("%s [д/н] ", question.c_str());
    std::fflush(stdout);

    char buf[16] = {};
    if (!std::fgets(buf, sizeof(buf), stdin))
        return false;

    const auto b0 = static_cast<unsigned char>(buf[0]);
    const auto b1 = static_cast<unsigned char>(buf[1]);

    // Латиница: y (yes) и d (да) в обоих регистрах.
    if (b0 == 'y' || b0 == 'Y' || b0 == 'd' || b0 == 'D')
        return true;

    // UTF-8: д = D0 B4, Д = D0 94.
    if (b0 == 0xD0 && (b1 == 0xB4 || b1 == 0x94))
        return true;

    // CP1251: д = E4, Д = C4.  CP866: д = A4, Д = 84.
    if (b0 == 0xE4 || b0 == 0xC4 || b0 == 0xA4 || b0 == 0x84)
        return true;

    return false;
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
        std::fprintf(stderr, "Имя из манифеста не годится.\n");
        return 1;
    }
    const std::string outPath = options.outPath.empty() ? outName : options.outPath;

    // Недокачка дерева — это каталог, а не файл: переименовать в конце
    // надо всё сразу, иначе на диске какое-то время лежит полутом под
    // настоящим именем, и человек решит, что всё готово.
    const std::string partPath = outPath + ".ferry-part";
    const std::string mapPath = outPath + ".ferry-map";

    // Раскладка тома: какой байт какому файлу принадлежит. Для одиночного
    // файла это один файл во весь том, и дальше вся запись идёт одной
    // дорогой — получатель не знает, сколько там файлов, ровно как и
    // сервер.
    VolumeLayout layout;
    layout.build(manifest);

    const int cells = std::min(width(), 78);
    std::printf("%s%s%s\n", dim(), ruleTop("том", cells).c_str(), reset());
    std::printf("%s│%s %s\n", dim(), reset(),
                field("имя", std::string(accent()) + manifest.name + reset()).c_str());
    std::printf("%s│%s %s\n", dim(), reset(), field("размер", bytes(manifest.total)).c_str());
    if (manifest.isTree()) {
        uint64_t files = 0, dirs = 0;
        for (const ManifestEntry &e : manifest.entries)
            (e.isDir ? dirs : files) += 1;
        std::printf("%s│%s %s\n", dim(), reset(),
                    field("состав",
                          countOf(files, "файл", "файла", "файлов")
                              + (dirs ? ", " + countOf(dirs, "пустая папка",
                                                      "пустые папки", "пустых папок")
                                      : std::string())).c_str());
    }
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
        // Смотрим на ВХОД, а не на вывод: ответ читается с клавиатуры, и
        // перенаправленный в файл вывод спрашивать не мешает. Раньше
        // условие стояло на выводе, и `ferry get … | tee log` отказывался
        // спрашивать, хотя человек сидел прямо перед клавиатурой.
        if (!platform::consoleStdinIsTty()) {
            std::fprintf(stderr,
                         "\nПодтверждать некому: ввод не с терминала. Добавьте -y.\n");
            return 1;
        }
        std::printf("\n");
        if (!askYesNo("Забирать?")) {
            std::printf("Не забираем.\n");
            return 0;
        }
    }

    // ---- 4. Том на диске и карта принятого ----
    ChunkMap map;
    map.init(mapPath, plan.chunkCount, plan.chunkSize, plan.totalBytes, manifest.root);
    const bool resuming = map.load() && platform::fileExists(partPath);
    if (!resuming) {
        map.init(mapPath, plan.chunkCount, plan.chunkSize, plan.totalBytes, manifest.root);
        // Недокачки прошлого раза может не быть, а может быть чужая — в
        // обоих случаях начинаем с чистого места. Дерево сносим целиком:
        // половина старого тома под новым именем хуже, чем ничего.
        if (platform::fileExists(partPath))
            platform::removeTree(partPath);
    }

    VolumeFile volume;
    if (manifest.isTree()) {
        if (!platform::makeDirectories(partPath)) {
            std::fprintf(stderr, "Не смог создать %s\n", partPath.c_str());
            return 1;
        }
        if (!volume.openTree(partPath, layout, true)
            || !volume.createEmpties(partPath, manifest)) {
            std::fprintf(stderr, "%s\n", volume.error().c_str());
            return 1;
        }
    } else if (!volume.openSingle(partPath, true, plan.totalBytes)) {
        std::fprintf(stderr, "%s\n", volume.error().c_str());
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
        volume.close();
        return 1;
    }
    const Bytes proof = proveKeyOwnership(keys.verifier, challenge.data(), challenge.size());
    if (proof.size() != 32) {
        std::fprintf(stderr, "Не удалось посчитать доказательство владения ключом.\n");
        volume.close();
        return 1;
    }

    // ---- 6. Подключаемся ----
    net::WebSocketClient ws;
    if (!ws.connectTo(relay.host, relay.port, relay.tls, relay.insecure, std::string(kWsPath),
                      20000, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        volume.close();
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
        // И то же самое диапазонами. Обе формы вместе, а не вместо:
        // have_upto понимает релей M1, диапазоны — новый, и недокачка,
        // начатая при одном, продолжается при другом.
        hello.set("have", rangesToJson(map.set().ranges()));
        // Что умеет этот клиент. "backfill" здесь появится, когда он
        // научится отвечать на serve; объявить раньше — значит позвать
        // сервер просить у нас то, чего мы не отдадим.
        json::Value features = json::Value::array();
        features.push(json::Value::make("ranges"));
        // Умеем отдавать чанки обратно. Сервер без этого слова не
        // пришлёт serve ни разу — и правильно сделает: ждать чанков
        // от того, кто их не пришлёт, значит повесить чужой догон.
        features.push(json::Value::make("backfill"));
        hello.set("features", std::move(features));
        ws.sendText(hello.dump());
    }

    // Чанки, приехавшие вместе с hello_ok. Сервер начинает лить сразу за
    // подтверждением, и обе вещи приходят одной пачкой; выбросить их
    // значит потерять начало тома и встать намертво, потому что второй раз
    // сервер их не пришлёт — по его учёту они уже у нас.
    std::deque<net::WsMessage> deferred;

    bool accepted = false;
    // Понимает ли этот релей диапазоны. Пока не ответил — молчим о них:
    // неизвестное сообщение для сервера это bad_message, а не «пропущу».
    bool serverSpeaksRanges = false;
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
            volume.close();
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
                serverSpeaksRanges = announces(v, "ranges");
            } else if (type == "error") {
                std::fprintf(stderr, "%s\n", explainError(v["reason"].toString()).c_str());
                volume.close();
                return 1;
            }
        }
    }
    if (!accepted) {
        std::fprintf(stderr, "Сервер не пустил к раздаче.\n");
        volume.close();
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

    // Что нам нужно. Сервер запоминает это и в M2.1 начнёт по нему
    // выбирать источник; сегодня сообщение ничего не меняет в поведении,
    // но формат провода фиксируется сейчас — переучивать разъехавшиеся
    // концы потом дороже, чем договориться заранее.
    if (serverSpeaksRanges) {
        const std::vector<ChunkRange> want = map.set().missing();
        if (!want.empty()) {
            json::Value req = json::Value::object();
            req.set("type", json::Value::make("request"));
            req.set("ranges", rangesToJson(want));
            req.set("budget_bytes", json::Value::make(int64_t(plan.chunkSize) * 8));
            ws.sendText(req.dump());
        }
    }

    // Насос для отдачи: читает из собственной недокачки и шифрует заново.
    // Шифротекст получается байт в байт тот же — см. ChunkPump.h.
    ChunkPump pump;
    pump.init(volume, plan, keys.data, meta.noncePrefix.data());
    ChunkSet serveQueue(plan.chunkCount);
    uint64_t servedChunks = 0;

    // Сколько чанков подряд не сошлось. Один испорченный чанк — это
    // не беда (сервер возьмёт его у другого источника), а вот сотня
    // подряд означает, что чинить нечего, и крутиться вечно нельзя.
    int badInARow = 0;

    // Самый правый индекс, который мы видели, — граница живой волны.
    uint64_t liveFront = 0;

    // Разбивка по источникам — со слов сервера: сам получатель её знать
    // не может. Пустая строка — сервер ещё не сказал или не умеет.
    std::string sourceLine;

    uint64_t received = have;
    uint64_t ackedUpTo = map.havePrefix();
    uint64_t sentHaveCount = map.set().cardinality();
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
                const std::string type = v["type"].toString();
                if (type == "error") {
                    failed = true;
                    failure = explainError(v["reason"].toString());
                } else if (type == "stats") {
                    const json::Value &src = v["src"];
                    const double w = src["window"].toDouble(0);
                    const double p = src["peers"].toDouble(0);
                    const double snd = src["sender"].toDouble(0);
                    if (w + p + snd > 0) {
                        sourceLine = std::string("источник окно ") + percent(w)
                                     + "   пиры " + percent(p) + "   отправитель "
                                     + percent(snd);
                    }
                } else if (type == "serve") {
                    // Сервер просит отдать чанки обратно — их ждёт
                    // кто-то, кто пришёл позже нас.
                    const json::Value &ranges = v["ranges"];
                    for (size_t i = 0; i < ranges.size(); ++i) {
                        const json::Value &r = ranges.at(i);
                        if (r.size() < 2)
                            continue;
                        const int64_t from = r.at(0).toInt(-1);
                        const int64_t to = r.at(1).toInt(-1);
                        if (from < 0 || to < from)
                            continue;
                        serveQueue.setRange({uint64_t(from), uint64_t(to)});
                    }
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

            // Проверка против списка хешей — за O(1) и независимо от
            // порядка. Именно она делает безопасным приём чанков от кого
            // угодно: подсунуть мусор нельзя, подмена ловится на месте.
            //
            // И именно поэтому несошедшийся чанк БОЛЬШЕ НЕ ВАЛИТ передачу.
            // Пока источник был один и доверенный, обрыв был честной
            // реакцией. Как только чанк может приехать от другого получателя,
            // такая реакция означает право любого участника убить чужую
            // загрузку одним испорченным байтом. Теперь мы выбрасываем чанк,
            // говорим серверу, что источник солгал, и ждём тот же индекс
            // откуда-нибудь ещё.
            const auto rejectChunk = [&](const char *why) {
                ++badInARow;
                if (serverSpeaksRanges) {
                    json::Value bad = json::Value::object();
                    bad.set("type", json::Value::make("bad_chunk"));
                    bad.set("index", json::Value::make(int64_t(index)));
                    bad.set("reason", json::Value::make(why));
                    ws.sendText(bad.dump());
                }
                if (badInARow >= kMaxBadChunks) {
                    failed = true;
                    failure = explainError(err::kChunkMismatch);
                }
            };

            Bytes plainChunk;
            if (!openChunk(keys.data, meta.noncePrefix.data(), index, plan.chunkCount,
                           raw + kBinaryHeaderSize, msg.data.size() - kBinaryHeaderSize,
                           plainChunk)) {
                rejectChunk("aead");
                if (failed)
                    break;
                continue;
            }
            if (plainChunk.size() != plan.sizeOf(index)) {
                rejectChunk("length");
                if (failed)
                    break;
                continue;
            }
            if (!hashes.verify(index, plainChunk.data(), plainChunk.size())) {
                rejectChunk("hash");
                if (failed)
                    break;
                continue;
            }
            badInARow = 0;

            const int64_t written =
                volume.writeAt(plainChunk.data(), plainChunk.size(), plan.offsetOf(index));
            if (written != int64_t(plainChunk.size())) {
                failed = true;
                failure = volume.error().empty()
                              ? std::string("не удалось записать на диск — кончилось место?")
                              : volume.error();
                break;
            }

            map.set(index);

            // Какой волной приехал чанк. Получателю этого никто не говорит и
            // говорить не должен: фрейм из окна и фрейм от пира одинаковы
            // до байта, и именно поэтому веер на сервере не копирует
            // полезную нагрузку. Зато волна видна по порядку: живой поток идёт
            // вперёд по тому, а всё, что пришло ПОЗАДИ уже виденного, —
            // это вторая волна.
            if (index >= liveFront) {
                liveFront = index;
                segments[size_t(index)] = Seg::Live;
            } else {
                segments[size_t(index)] = Seg::Backfill;
            }
            ++received;
            meter.add(plainChunk.size());
        }
        if (failed)
            break;

        // Отдаём то, что у нас попросили. По одному чанку за раз и с
        // оглядкой на свою же очередь отправки: мы здесь в первую очередь
        // получатель, и чужой догон не должен мешать собственному приёму.
        while (!serveQueue.empty() && ws.pendingBytes() < kSeedHighWater) {
            const uint64_t give = serveQueue.firstPresent(0);
            if (give >= plan.chunkCount)
                break;
            serveQueue.clear(give);
            if (!map.has(give))
                continue;   // у нас этого чанка нет — сервер найдёт другой источник
            if (!pump.send(ws, give)) {
                // Свою загрузку из-за чужой не роняем: перестаём сидировать,
                // и только.
                serveQueue.reset(plan.chunkCount);
                break;
            }
            ++servedChunks;
        }

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

        // Отчёт серверу о принятом: диапазонами, если он их понимает, и
        // префиксом, если нет.
        //
        // Именно диапазонами, а не обеими формами сразу: ack — это тот же
        // have, сжатый до одного числа, и слать оба значит говорить одно
        // и то же дважды. Со второй волной префикс к тому же перестанет
        // что-либо описывать: у получателя появятся дырки, и по ack он
        // будет выглядеть стоящим на месте, имея половину тома.
        const uint64_t prefix = map.havePrefix();
        const uint64_t haveNow = map.set().cardinality();
        if (t - lastAckMs > 300) {
            if (serverSpeaksRanges) {
                if (haveNow != sentHaveCount) {
                    json::Value have = json::Value::object();
                    have.set("type", json::Value::make("have"));
                    have.set("ranges", rangesToJson(map.set().ranges()));
                    ws.sendText(have.dump());
                    sentHaveCount = haveNow;
                    lastAckMs = t;
                }
            } else if (prefix != ackedUpTo) {
                json::Value ack = json::Value::object();
                ack.set("type", json::Value::make("ack"));
                ack.set("upto", json::Value::make(int64_t(prefix)));
                ws.sendText(ack.dump());
                ackedUpTo = prefix;
                lastAckMs = t;
            }
        }

        const double frac = plan.chunkCount ? double(received) / double(plan.chunkCount) : 1.0;
        const double remaining = double(plan.totalBytes) * (1.0 - frac);
        const int64_t eta = meter.value() > 1 ? int64_t(remaining / meter.value() * 1000) : -1;

        std::vector<std::string> panelLines = {
            std::string("принято  ") + bar(frac, std::max(10, cells - 40)) + "  " + percent(frac)
                + "  " + bytes(uint64_t(double(plan.totalBytes) * frac)) + " из "
                + bytes(plan.totalBytes),
            std::string("скорость ") + rate(meter.value()) + "   осталось " + duration(eta)
                + "   идёт " + duration(t - startedMs)
                + (servedChunks ? std::string("   ") + ok() + "отдано другим "
                                      + bytes(servedChunks * uint64_t(plan.chunkSize)) + reset()
                                : std::string()),
            std::string("карта    ") + volumeMap(segments, std::max(20, cells - 12)),
        };
        if (!sourceLine.empty())
            panelLines.push_back(std::string(dim()) + sourceLine + reset());
        panel.update(panelLines);
    }

    panel.finish();
    map.flush();

    // Последний have — обязательно, и не ради красоты.
    //
    // Отчёты идут не чаще раза в 300 мс, а небольшой том на быстром канале
    // уезжает быстрее: сервер тогда не услышал бы от нас ни одного have и
    // остался бы в убеждении, что у нас ничего нет. Для приёма это
    // безразлично, а вот сидировать нас после этого не позовут никогда —
    // источником выбирают того, про кого известно, что у него есть нужное.
    if (serverSpeaksRanges && ws.isOpen() && !failed) {
        json::Value have = json::Value::object();
        have.set("type", json::Value::make("have"));
        have.set("ranges", rangesToJson(map.set().ranges()));
        ws.sendText(have.dump());
        ws.pump(0);
    }

    if (failed) {
        volume.sync();
        volume.close();
        ws.closeGracefully();
        std::fprintf(stderr, "\n%sПриём прерван:%s %s\n", bad(), reset(), failure.c_str());
        std::fprintf(stderr, "%sПринятое сохранено в %s — при следующем запуске продолжим.%s\n",
                     dim(), partPath.c_str(), reset());
        return 1;
    }

    if (stopRequested()) {
        volume.sync();
        volume.close();
        ws.closeGracefully();
        std::printf("\n%sОстановлено. Принятое сохранено в %s.%s\n", dim(), partPath.c_str(),
                    reset());
        return 130;
    }

    // ---- 8. Готово ----
    if (!volume.sync()) {
        std::fprintf(stderr, "Не удалось дописать файл на диск.\n");
        volume.close();
        return 1;
    }
    volume.close();
    // Сокет НЕ закрываем здесь: с --seed мы остаёмся на связи и
    // отдаём чанки дальше. Закрытие — в конце, по обоим путям.
    if (!options.seed)
        ws.closeGracefully();

    // Переименование целиком — и для файла, и для дерева. До этой
    // строки под настоящим именем нет ничего: полутом, который
    // выглядит готовым, хуже отсутствия тома.
    if (!platform::fileRename(partPath, outPath)) {
        std::fprintf(stderr, "Не удалось переименовать %s в %s\n", partPath.c_str(),
                     outPath.c_str());
        return 1;
    }
    map.remove();

    const int64_t tookMs = nowMs() - startedMs;
    std::printf("\n%s%s%s\n", ok(), ruleTop("готово", cells).c_str(), reset());
    std::printf("%s│%s %s\n", ok(), reset(), field(manifest.isTree() ? "каталог" : "файл", outPath).c_str());
    std::printf("%s│%s %s\n", ok(), reset(), field("размер", bytes(plan.totalBytes)).c_str());
    std::printf("%s│%s %s\n", ok(), reset(),
                field("время", duration(tookMs) + "  ("
                                   + rate(double(plan.totalBytes) * 1000.0
                                          / double(std::max<int64_t>(tookMs, 1)))
                                   + ")").c_str());
    std::printf("%s│%s %s\n", ok(), reset(),
                field("целостность", "проверено BLAKE3 по каждому чанку").c_str());
    std::printf("%s%s%s\n", ok(), ruleBottom(cells).c_str(), reset());

    // ---- 9. Сидирование ----
    //
    // Файл уже переименован и лежит на месте: человеку не надо дожидаться
    // конца сидирования, чтобы его открыть. Отдаём теперь из готового
    // файла — смещения те же, содержимое то же.
    if (!options.seed)
        return 0;

    VolumeFile seed;
    const bool seedOpened = manifest.isTree() ? seed.openTree(outPath, layout, false)
                                              : seed.openSingle(outPath, false, plan.totalBytes);
    if (!seedOpened) {
        std::fprintf(stderr, "%sТом не открылся на чтение — сидировать не из чего.%s\n", dim(),
                     reset());
        return 0;
    }
    pump.init(seed, plan, keys.data, meta.noncePrefix.data());

    std::printf("\n%sОстаюсь источником для остальных. Ctrl-C — выйти.%s\n\n", dim(), reset());

    LivePanel seedPanel;
    const int64_t seedStartedMs = nowMs();
    while (!stopRequested() && ws.isOpen()) {
        if (!ws.pump(200))
            break;

        net::WsMessage m;
        while (ws.next(m)) {
            if (m.binary)
                continue;   // нам больше ничего не нужно
            json::Value v;
            if (!json::Value::parse(m.data, v) || !v.isObject())
                continue;
            if (v["type"].toString() != "serve")
                continue;
            const json::Value &ranges = v["ranges"];
            for (size_t i = 0; i < ranges.size(); ++i) {
                const json::Value &r = ranges.at(i);
                if (r.size() < 2)
                    continue;
                const int64_t from = r.at(0).toInt(-1);
                const int64_t to = r.at(1).toInt(-1);
                if (from < 0 || to < from)
                    continue;
                serveQueue.setRange({uint64_t(from), uint64_t(to)});
            }
        }

        while (!serveQueue.empty() && ws.pendingBytes() < kSeedHighWater) {
            const uint64_t give = serveQueue.firstPresent(0);
            if (give >= plan.chunkCount)
                break;
            serveQueue.clear(give);
            if (!pump.send(ws, give)) {
                std::fprintf(stderr, "\n%s\n", pump.error().c_str());
                serveQueue.reset(plan.chunkCount);
                break;
            }
            ++servedChunks;
        }

        seedPanel.update({
            std::string("источник  отдано ")
                + bytes(servedChunks * uint64_t(plan.chunkSize)) + "   идёт "
                + duration(nowMs() - seedStartedMs),
        });
    }
    seedPanel.finish();
    seed.close();
    ws.closeGracefully();
    std::printf("%sСидирование остановлено. Отдано %s.%s\n", dim(),
                bytes(servedChunks * uint64_t(plan.chunkSize)).c_str(), reset());
    return 0;
}

} // namespace ferry::cli
