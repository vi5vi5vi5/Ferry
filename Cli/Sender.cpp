#include "Cli/Sender.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Cli/ChunkPump.h"
#include "Cli/Hasher.h"
#include "Cli/Password.h"
#include "Cli/Tree.h"
#include "Cli/VolumeFile.h"
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
#include "core/VolumeLayout.h"

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

// Кто ждёт решения владельца (--approve).
struct Knock
{
    int id = 0;
    std::string name;
};

// Ответ на «пустить?» одной клавишей. Латиница и та же клавиша в русской
// раскладке; «д» — потому что «да». «н» нет нарочно: на русской
// раскладке это клавиша Y, а по-русски — первая буква «нет».
enum class Answer { None, Yes, No, All };
Answer answerFor(int key)
{
    switch (key) {
    case 'y': case 'Y': case 0x0434: case 0x0414:   // д Д
        return Answer::Yes;
    case 'n': case 'N': case 0x0442: case 0x0422:   // т Т — клавиша N
        return Answer::No;
    case 'a': case 'A': case 0x0444: case 0x0424:   // ф Ф — клавиша A
        return Answer::All;
    default:
        return Answer::None;
    }
}

// Ссылка владельца: открывает в браузере ту же раздачу с правами того,
// кто её завёл, — добавить использований, продлить срок, закрыть доступ.
// Всё после решётки, как и ключ: на сервер это не уходит, в журналы не
// попадает. Ключ добавляется, чтобы страница могла показать саму ссылку
// и QR; у раздачи по паролю ключа в ней нет.
std::string manageLink(const TransferLink &link, const std::string &ownerToken, bool withKey)
{
    std::string out = link.origin() + "/#manage=" + link.id + "." + ownerToken;
    if (withKey)
        out += "." + base64UrlEncode(link.key.data(), link.key.size());
    return out;
}

// Пароль для --password: из FERRY_PASSWORD или с клавиатуры, дважды.
bool askNewPassword(std::string &out)
{
    if (const char *env = std::getenv("FERRY_PASSWORD")) {
        out = normalizePassword(env);
        const std::string problem = passwordProblem(out);
        if (!problem.empty()) {
            std::fprintf(stderr, "FERRY_PASSWORD не подходит: %s.\n", problem.c_str());
            return false;
        }
        return true;
    }
    if (!platform::consoleStdinIsTty()) {
        std::fprintf(stderr, "--password: пароль спрашивается с клавиатуры, а её нет. "
                             "Для скриптов есть переменная FERRY_PASSWORD.\n");
        return false;
    }
    std::fprintf(stderr, "Парольная фраза — пять слов и больше. Её передают голосом или\n"
                         "другим мессенджером, отдельно от ссылки. Регистр, лишние пробелы\n"
                         "и «ё» значения не имеют.\n");
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::string first, second;
        if (!platform::consoleReadSecret("Фраза: ", first))
            return false;
        out = normalizePassword(first);
        const std::string problem = passwordProblem(out);
        if (!problem.empty()) {
            std::fprintf(stderr, "Не подходит: %s.\n", problem.c_str());
            continue;
        }
        if (!platform::consoleReadSecret("Ещё раз: ", second))
            return false;
        if (normalizePassword(second) != out) {
            std::fprintf(stderr, "Фразы не совпали.\n");
            continue;
        }
        return true;
    }
    return false;
}

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

    // ---- 1. Что отдаём: файл или каталог ----
    platform::FileInfo info;
    if (!platform::fileStat(options.path, info)) {
        std::fprintf(stderr, "Не нашёл: %s\n", options.path.c_str());
        return 1;
    }

    Manifest manifest;
    TreeScan scan;
    if (info.isDirectory) {
        std::string err;
        if (!scanTree(options.path, scan, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        manifest = scan.manifest;
    } else {
        manifest.kind = "file";
        manifest.name = safeFileName(baseName(options.path));
        manifest.total = info.size;
        if (manifest.name.empty()) {
            std::fprintf(stderr, "Из имени файла не получилось ничего пригодного.\n");
            return 1;
        }
    }

    const uint64_t total = manifest.total;
    const ChunkPlan plan = planFor(total);
    if (!plan.valid()) {
        std::fprintf(stderr, "Не понял размер тома.\n");
        return 1;
    }
    if (total == 0) {
        std::fprintf(stderr, "Том пустой — возить нечего.\n");
        return 1;
    }

    // Раскладка тома: какой байт какому файлу принадлежит. Для одиночного
    // файла получается один файл во весь том — дальше обе ветки ходят
    // одной дорогой, и ни отправитель, ни получатель больше не знают,
    // сколько там файлов.
    VolumeLayout layout;
    layout.build(manifest);

    VolumeFile volume;
    const bool opened = info.isDirectory
                            ? volume.openTree(options.path, layout, false)
                            : volume.openSingle(options.path, false, total);
    if (!opened) {
        std::fprintf(stderr, "%s\n", volume.error().c_str());
        return 1;
    }

    const std::string name = manifest.name;

    const int cells = std::min(width(), 78);
    std::printf("%s%s%s\n", dim(), ruleTop("том", cells).c_str(), reset());
    std::printf("%s│%s %s\n", dim(), reset(),
                field("имя", std::string(accent()) + name + reset()).c_str());
    if (info.isDirectory) {
        std::printf("%s│%s %s\n", dim(), reset(),
                    field("состав",
                          countOf(scan.fileCount, "файл", "файла", "файлов")
                              + (scan.dirCount
                                     ? ", " + countOf(scan.dirCount, "пустая папка",
                                                      "пустые папки", "пустых папок")
                                     : std::string())).c_str());
    }
    std::printf("%s│%s %s\n", dim(), reset(), field("размер", bytes(total)).c_str());
    std::printf("%s│%s %s\n", dim(), reset(),
                field("чанки", count(plan.chunkCount) + " по " + bytes(plan.chunkSize)).c_str());
    std::printf("%s%s%s\n", dim(), ruleBottom(cells).c_str(), reset());


    // Пропущенное говорим вслух и до ссылки. Человек ещё может отменить
    // отправку и решить по-другому; узнать об этом от получателя, что
    // «чего-то не хватает», сильно хуже.
    if (!scan.skipped.empty()) {
        std::printf("%s│%s %s\n", warn(), reset(),
                    field("пропущено",
                          countOf(scan.skipped.size(), "запись", "записи", "записей")
                              + " — их в томе не будет").c_str());
        const size_t show = std::min<size_t>(scan.skipped.size(), 8);
        for (size_t i = 0; i < show; ++i)
            std::printf("%s│%s   %s\n", warn(), reset(), scan.skipped[i].c_str());
        if (scan.skipped.size() > show)
            std::printf("%s│%s   …и ещё %s\n", warn(), reset(),
                        count(scan.skipped.size() - show).c_str());
        std::printf("\n");
    }

    // Отвечать «пустить или нет» можно только с клавиатуры. Без неё
    // каждый получатель стоял бы у двери до конца срока.
    if (options.approve && !platform::consoleStdinIsTty()) {
        std::fprintf(stderr, "--approve: решать, кого пускать, можно только из терминала, "
                             "а ввод сюда перенаправлен.\n");
        return 1;
    }

    // ---- 2. Ключи ----
    // Пароль спрашиваем ДО того, как заводить раздачу: черновик на релее
    // живёт минуту, а человек может думать над фразой и дольше.
    bool ok = false;
    Key32 master = randomKey32(&ok);
    if (!ok) {
        std::fprintf(stderr, "Системный генератор случайных чисел недоступен — "
                             "продолжать нельзя.\n");
        return 1;
    }
    Bytes passwordSalt;
    if (options.password) {
        std::string phrase;
        if (!askNewPassword(phrase))
            return 1;
        if (!randomBytes(passwordSalt, kPasswordSaltSize)) {
            std::fprintf(stderr, "Системный генератор случайных чисел недоступен.\n");
            return 1;
        }
        std::printf("%sвывожу ключ из пароля (Argon2id, 64 МиБ)…%s\n", dim(), reset());
        if (!passwordKey(phrase, passwordSalt.data(), passwordSalt.size(), master)) {
            std::fprintf(stderr, "Не удалось вывести ключ из пароля: не хватило памяти?\n");
            return 1;
        }
    }
    const TransferKeys keys = TransferKeys::derive(master);

    Bytes noncePrefix;
    if (!randomBytes(noncePrefix, kNoncePrefixSize)) {
        std::fprintf(stderr, "Системный генератор случайных чисел недоступен.\n");
        return 1;
    }

    // ---- 3. Заводим раздачу и узнаём, что умеет релей ----
    // Раньше это шло после хешей. Теперь — до: от ответа зависит, считать
    // ли том целиком заранее или отдавать хеши на лету.
    CreatedTransfer created;
    std::string err;
    if (!apiCreateTransfer(relay, created, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    const int64_t createdAtMs = nowMs();
    const bool streamHashes = created.hasFeature(kFeatureStreamHashes);

    // Ручки M4 требуют релея, который их понимает. Старый релей молча
    // пропустил бы незнакомые поля политики — и человек думал бы, что
    // его спросят, хотя пускать будут всех.
    if ((options.approve || options.leaveOpen) && !created.hasFeature(kFeaturePolicy)) {
        std::fprintf(stderr, "Этот релей не умеет %s. Обновите релей или отправьте без этого "
                             "ключа.\n",
                     options.approve ? "спрашивать отправителя (--approve)"
                                     : "оставлять раздачу без отправителя (--leave-open)");
        return 1;
    }
    if (options.password && !created.hasFeature(kFeaturePassword)) {
        std::fprintf(stderr, "Этот релей не умеет раздачи по паролю. Обновите релей или "
                             "отправьте без --password.\n");
        return 1;
    }

    // ---- 4. Хеши ----
    // На лету (релей умеет): считаются в фоне, ссылка появляется сразу, а
    // раздача начинается с первого посчитанного чанка. Архив на сто
    // гигабайт больше не держит человека у полосы «считаю хеши».
    //
    // Заранее (релей старый): как раньше, весь том до ссылки — но уже
    // тем же фоновым хешером, где диск и процессор работают одновременно.
    Hasher hasher;
    {
        Hasher::Source src;
        src.path = options.path;
        src.tree = info.isDirectory;
        src.layout = &layout;
        src.total = total;
        std::string herr;
        if (!hasher.start(src, plan, &herr)) {
            std::fprintf(stderr, "%s\n", herr.c_str());
            return 1;
        }
    }
    const int64_t hashStartedMs = nowMs();

    Bytes encManifest, encHashes;
    if (!streamHashes) {
        LivePanel panel;
        RateMeter meter;
        uint64_t lastBytes = 0;
        while (!hasher.finished()) {
            if (hasher.failed()) {
                panel.finish();
                std::fprintf(stderr, "\n%s\n", hasher.error().c_str());
                return 1;
            }
            if (stopRequested()) {
                panel.finish();
                hasher.stop();
                return 130;
            }
            platform::sleepMs(50);
            const uint64_t b = hasher.bytesDone();
            meter.add(b - lastBytes);
            lastBytes = b;
            const double frac = double(hasher.done()) / double(plan.chunkCount);
            panel.update({std::string("считаю хеши  ") + bar(frac, cells - 30) + "  "
                          + percent(frac) + "  " + rate(meter.value())});
        }
        panel.finish();
        const HashList hashes = hasher.list();
        std::printf("%sхеши готовы за %s — BLAKE3, %s%s\n", dim(),
                    duration(nowMs() - hashStartedMs).c_str(),
                    countOf(hashes.size(), "хеш", "хеша", "хешей").c_str(), reset());

        // Манифест уже собран выше — здесь остаётся вписать корневой хеш,
        // который стал известен только после подсчёта.
        manifest.root = hashes.root();
        const std::string manifestJson = manifest.toJson();
        const Bytes rawHashes = hashes.serialize();
        if (!sealChunk(keys.meta, noncePrefix.data(), kMetaLabelManifest, plan.chunkCount,
                       reinterpret_cast<const uint8_t *>(manifestJson.data()),
                       manifestJson.size(), encManifest)
            || !sealChunk(keys.meta, noncePrefix.data(), kMetaLabelHashList, plan.chunkCount,
                          rawHashes.data(), rawHashes.size(), encHashes)) {
            std::fprintf(stderr, "Не удалось зашифровать метаданные.\n");
            return 1;
        }

        // Черновик раздачи на релее живёт минуту с момента создания, а
        // подсчёт на медленном диске идёт дольше: тогда первый черновик уже
        // убран, и нужен свежий. Но только тогда — заводить второй каждый
        // раз значило бы вдвое быстрее выбирать квоту релея на создание
        // раздач, а она у него одна на всех. 45 секунд — с запасом на
        // шифрование, подключение и offer.
        if (nowMs() - createdAtMs > 45000 && !apiCreateTransfer(relay, created, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
    } else {
        // Промежуточный манифест: всё то же, но без корня — корня ещё нет.
        // И под своей меткой: итоговый уйдёт позже под обычной, а один nonce
        // под двумя разными текстами GCM не прощает.
        manifest.streamHashes = true;
        const std::string manifestJson = manifest.toJson();
        if (!sealChunk(keys.meta, noncePrefix.data(), kMetaLabelStreamManifest, plan.chunkCount,
                       reinterpret_cast<const uint8_t *>(manifestJson.data()),
                       manifestJson.size(), encManifest)) {
            std::fprintf(stderr, "Не удалось зашифровать метаданные.\n");
            return 1;
        }
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
        // По паролю ключа в ссылке нет: его выводят из пароля на той стороне.
        link.key = master;
        link.hasKey = !options.password;
    }

    // ---- 5. Подключаемся и делаем offer ----
    net::WebSocketClient ws;
    if (!ws.connectTo(relay.host, relay.port, relay.tls, relay.insecure, std::string(kWsPath),
                      20000, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
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
        if (streamHashes)
            offer.set("hash_mode", json::Value::make("stream"));
        else
            offer.set("hash_list", json::Value::make(b64(encHashes)));
        if (options.password) {
            offer.set("mode", json::Value::make("password"));
            offer.set("salt", json::Value::make(b64(passwordSalt)));
        } else {
            offer.set("mode", json::Value::make("key"));
        }

        json::Value policy = json::Value::object();
        if (options.approve)
            policy.set("require_approval", json::Value::make(true));
        if (options.leaveOpen) {
            policy.set("leave_open", json::Value::make(true));
            // Сказал «живи без меня» и «спрашивай меня» одновременно —
            // после ухода спрашивать некого, и раздача пускает всех с
            // ключом. Закрываться ему бы противоречило первому.
            if (options.approve)
                policy.set("on_owner_leave", json::Value::make("freeze"));
        }
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
                return 1;
            }
        }
    }
    if (!accepted) {
        std::fprintf(stderr, "Сервер не подтвердил раздачу.\n");
        return 1;
    }

    // ---- 6. Ссылка ----
    const std::string linkText = link.toString();
    if (options.password) {
        std::printf("\n%s%s%s\n", dim(), ruleTop("ссылка без ключа", cells).c_str(), reset());
        std::printf("%s│%s %s%s%s\n", dim(), reset(), accent(), linkText.c_str(), reset());
        std::printf("%s│%s\n", dim(), reset());
        std::printf("%s│%s %sКлюч выводится из пароля. Пароль передайте отдельно — голосом%s\n",
                    dim(), reset(), dim(), reset());
        std::printf("%s│%s %sили другим мессенджером.%s\n", dim(), reset(), dim(), reset());
        std::printf("%s│%s %sЭтот режим слабее ключа: у кого ссылка, тот может перебирать%s\n",
                    warn(), reset(), dim(), reset());
        std::printf("%s│%s %sпароли сколько угодно, не спрашивая сервер.%s\n", warn(), reset(),
                    dim(), reset());
        std::printf("%s%s%s\n", dim(), ruleBottom(cells).c_str(), reset());
    } else {
        std::printf("\n%s%s%s\n", dim(), ruleTop("ссылка и ключ", cells).c_str(), reset());
        std::printf("%s│%s %s%s%s\n", dim(), reset(), accent(), linkText.c_str(), reset());
        std::printf("%s│%s\n", dim(), reset());
        std::printf("%s│%s %sЧасть после решётки — это ключ. На сервер она не уходит.%s\n", dim(),
                    reset(), dim(), reset());
        std::printf("%s│%s %sКто получил ссылку целиком — получил том.%s\n", dim(), reset(),
                    dim(), reset());
        std::printf("%s%s%s\n", dim(), ruleBottom(cells).c_str(), reset());
    }

    std::printf("\nЗабрать:  %sferry get \"%s\"%s\n", bold(), linkText.c_str(), reset());
    // В буфер — только когда рядом человек. Скрипту, который перенаправил
    // вывод, чужой буфер обмена трогать незачем.
    if (isTty() && platform::copyToClipboard(linkText))
        std::printf("%sСсылка уже в буфере обмена.%s\n", dim(), reset());
    if (options.qr) {
        std::printf("\n");
        if (!printQr(linkText))
            std::printf("%s  (QR не поместился или вывод не в терминал)%s\n", dim(), reset());
    }

    if (options.uses > 0 || options.ttlMs > 0 || options.approve || options.leaveOpen) {
        std::printf("\n%sполитика:%s ", dim(), reset());
        if (options.uses > 0)
            std::printf("использований %d  ", options.uses);
        if (options.ttlMs > 0)
            std::printf("живёт %s  ", duration(options.ttlMs).c_str());
        if (options.approve)
            std::printf("пускаю сам  ");
        if (options.leaveOpen)
            std::printf("без меня живёт на сидах");
        std::printf("\n");
    }
    if (options.leaveOpen)
        std::printf("%sCtrl-C — уйти. Раздача продолжится, пока на связи кто-то, скачавший "
                    "том целиком;\nусловия после ухода уже не поменять.%s\n",
                    dim(), reset());

    // Раздача живёт, пока жива команда, а ssh-сессия уносит её с собой.
    // Подсказка нужна ровно там — зашли по ssh и ещё не в tmux или screen;
    // на своём компьютере она только шум. И сразу целиком, с клавишами:
    // «tmux new "ferry send …"» закрывает окно вместе с ошибкой, и кроме
    // [exited] человек ничего не видит.
    if (std::getenv("SSH_CONNECTION") && !std::getenv("TMUX") && !std::getenv("STY"))
        std::printf("\n%sРаздача идёт, пока запущена эта команда, и закроется вместе с ssh.\n"
                    "Чтобы не зависеть от сессии: tmux new -s ferry, там ferry send,\n"
                    "потом Ctrl-B и D. Вернуться — tmux attach -t ferry.%s\n",
                    dim(), reset());
    std::printf("\n");

    // ---- 7. Качаем ----
    ChunkPump pump;
    pump.init(volume, plan, keys.data, noncePrefix.data());
    ChunkSet sent(plan.chunkCount);
    uint64_t sentBytes = 0;
    uint64_t sentDistinctBytes = 0;
    uint64_t sentLiveChunks = 0;
    uint64_t sentBackfillChunks = 0;
    std::vector<PeerRow> peers;
    RateMeter meter;
    LivePanel panel;

    // Хеши на лету. hashesSent — сколько чанков подряд с начала уже
    // объявлено получателям: дальше этой границы чанки не уходят.
    // При хешах, посчитанных заранее, граница сразу в конце тома.
    uint64_t hashesSent = streamHashes ? 0 : plan.chunkCount;
    bool hashesDoneSent = !streamHashes;
    int64_t lastSegmentMs = 0;
    int64_t hashesTookMs = streamHashes ? -1 : nowMs() - hashStartedMs;
    RateMeter hashMeter;
    uint64_t hashBytesSeen = 0;
    const int64_t startedMs = nowMs();
    bool failed = false;
    std::string failure;

    // Очередь у двери (--approve). Спрашиваем по одному: клавиша отвечает
    // за того, кто стоит первым.
    std::vector<Knock> knocks;
    bool keysOn = false;
    const auto answer = [&](int id, bool yes, bool all) {
        json::Value a = json::Value::object();
        a.set("type", json::Value::make("approve"));
        if (all)
            a.set("all", json::Value::make(true));
        else
            a.set("receiver", json::Value::make(int64_t(id)));
        a.set("ok", json::Value::make(yes));
        ws.sendText(a.dump());
    };

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
            } else if (type == "approval_request") {
                Knock k;
                k.id = int(v["receiver"].toInt(0));
                k.name = v["name"].toString();
                knocks.push_back(std::move(k));
            } else if (type == "approval_cancel") {
                const int id = int(v["receiver"].toInt(0));
                knocks.erase(std::remove_if(knocks.begin(), knocks.end(),
                                            [id](const Knock &k) { return k.id == id; }),
                             knocks.end());
            } else if (type == "error") {
                failed = true;
                failure = explainError(v["reason"].toString());
            }
        }
        if (failed)
            break;

        // ---- кто у двери ----
        // Клавиши читаются, только пока есть вопрос: всё остальное время
        // терминал ведёт себя как обычно.
        if (!knocks.empty() && !keysOn) {
            platform::consoleKeys(true);
            keysOn = true;
        } else if (knocks.empty() && keysOn) {
            platform::consoleKeys(false);
            keysOn = false;
        }
        for (int key = platform::consolePollKey(); key >= 0 && !knocks.empty();
             key = platform::consolePollKey()) {
            const Answer a = answerFor(key);
            if (a == Answer::None)
                continue;
            if (a == Answer::All) {
                answer(0, true, true);
                knocks.clear();
            } else {
                answer(knocks.front().id, a == Answer::Yes, false);
                knocks.erase(knocks.begin());
            }
        }

        // ---- хеши на лету ----
        // Сегменты уходят пачками: по 256 хешей или раз в 300 мс, чтобы не
        // слать по одному на каждый чанк. Но если живая волна упёрлась в
        // чанк, чей хеш ещё не объявлен, сегмент уходит сразу — иначе данные
        // стояли бы ради красоты пачки.
        if (!hashesDoneSent) {
            if (hasher.failed()) {
                failed = true;
                failure = hasher.error();
                break;
            }
            const uint64_t ready = hasher.done();
            const uint64_t hb = hasher.bytesDone();
            hashMeter.add(hb - hashBytesSeen);
            hashBytesSeen = hb;

            const uint64_t nextLive =
                pendingLive.empty() ? plan.chunkCount : pendingLive.firstPresent(0);
            const bool starving = nextLive >= hashesSent && nextLive < ready;
            const bool batch = ready - hashesSent >= 256;
            const bool stale = ready > hashesSent && nowMs() - lastSegmentMs >= 300;
            if (ready > hashesSent
                && (starving || batch || stale || ready == plan.chunkCount)) {
                while (hashesSent < ready) {
                    const uint64_t n = std::min<uint64_t>(ready - hashesSent, kHashSegmentMax);
                    Bytes raw(size_t(n) * 32);
                    for (uint64_t k = 0; k < n; ++k)
                        std::memcpy(raw.data() + size_t(k) * 32, hasher.at(hashesSent + k).data(),
                                    32);
                    Bytes sealed;
                    if (!sealChunk(keys.meta, noncePrefix.data(), hashSegmentLabel(hashesSent),
                                   plan.chunkCount, raw.data(), raw.size(), sealed)) {
                        failed = true;
                        failure = "не удалось зашифровать сегмент хешей";
                        break;
                    }
                    json::Value seg = json::Value::object();
                    seg.set("type", json::Value::make("hashes"));
                    seg.set("from", json::Value::make(int64_t(hashesSent)));
                    seg.set("count", json::Value::make(int64_t(n)));
                    seg.set("data", json::Value::make(b64(sealed)));
                    ws.sendText(seg.dump());
                    hashesSent += n;
                }
                lastSegmentMs = nowMs();
            }
            if (failed)
                break;

            // Всё посчитано — итоговые манифест с корнем и список целиком.
            // Ровно в том виде, в каком их ждёт любой клиент: с этой минуты
            // раздача открыта и тем, кто хеши на лету принимать не умеет.
            if (hashesSent == plan.chunkCount) {
                const HashList full = hasher.list();
                Manifest finalManifest = manifest;
                finalManifest.streamHashes = false;
                finalManifest.root = full.root();
                const std::string finalJson = finalManifest.toJson();
                const Bytes rawHashes = full.serialize();
                Bytes finalManifestEnc, finalHashesEnc;
                if (!sealChunk(keys.meta, noncePrefix.data(), kMetaLabelManifest, plan.chunkCount,
                               reinterpret_cast<const uint8_t *>(finalJson.data()),
                               finalJson.size(), finalManifestEnc)
                    || !sealChunk(keys.meta, noncePrefix.data(), kMetaLabelHashList,
                                  plan.chunkCount, rawHashes.data(), rawHashes.size(),
                                  finalHashesEnc)) {
                    failed = true;
                    failure = "не удалось зашифровать итоговые метаданные";
                    break;
                }
                json::Value done = json::Value::object();
                done.set("type", json::Value::make("hashes_done"));
                done.set("manifest", json::Value::make(b64(finalManifestEnc)));
                done.set("hash_list", json::Value::make(b64(finalHashesEnc)));
                ws.sendText(done.dump());
                hashesDoneSent = true;
                hashesTookMs = nowMs() - hashStartedMs;
            }
        }

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

            // Чанк не уходит раньше своего хеша — на этом правиле держатся
            // хеши на лету, и релей его проверяет. Подождём следующего
            // сегмента: хешер почти всегда далеко впереди сети.
            if (index >= hashesSent)
                break;

            if (!pump.send(ws, index)) {
                failed = true;
                failure = pump.error();
                break;
            }
            // Сразу в сокет, пока читается и шифруется следующий.
            if (!ws.flush()) {
                failed = true;
                failure = ws.error().empty() ? std::string("соединение закрылось") : ws.error();
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

        if (!hashesDoneSent) {
            const double hfrac = double(hasher.done()) / double(plan.chunkCount);
            lines.push_back(std::string("хеши     ") + bar(hfrac, std::max(10, cells - 40)) + "  "
                            + percent(hfrac) + "  " + rate(hashMeter.value()));
        } else if (streamHashes && hashesTookMs >= 0) {
            lines.push_back(std::string(dim()) + "хеши посчитаны на лету за "
                            + duration(hashesTookMs) + reset());
        }

        if (!knocks.empty()) {
            const Knock &k = knocks.front();
            const std::string who = k.name.empty() ? ("гость " + std::to_string(k.id)) : k.name;
            std::string q = std::string(accent()) + "Пустить «" + who + "»?" + reset() + "  "
                            + dim() + "y — да · n — нет · a — пускать всех дальше" + reset();
            if (knocks.size() > 1)
                q += std::string(dim()) + "  (ждут ещё " + std::to_string(knocks.size() - 1) + ")"
                     + reset();
            lines.push_back(std::move(q));
        }

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
    if (keysOn)
        platform::consoleKeys(false);
    ws.closeGracefully();

    // Ушёл сам, разрешив раздаче жить без него. Жива ли она — решает
    // сервер: нужен кто-то со всем томом. Последняя сводка это и говорит.
    bool delegated = false;
    if (!failed && stopRequested() && options.leaveOpen) {
        for (const PeerRow &p : peers)
            if (p.role == "seed")
                delegated = true;
    }

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
    if (delegated) {
        std::printf("\n%sВы ушли, раздача продолжается без вас.%s Её отдают те, кто скачал том\n"
                    "целиком, пока хоть один из них на связи. Продлить срок, поменять условия\n"
                    "или закрыть доступ можно из браузера:\n  %s%s%s\n"
                    "%sЭта ссылка — права владельца. Никому её не пересылайте.%s\n",
                    bold(), reset(), accent(),
                    manageLink(link, created.ownerToken, !options.password).c_str(), reset(),
                    dim(), reset());
    } else if (stopRequested() && options.leaveOpen) {
        std::printf("\n%sРаздача остановлена: никто не скачал том целиком, и отдавать его\n"
                    "без вас некому.%s\n", dim(), reset());
    } else if (stopRequested()) {
        std::printf("\n%sРаздача остановлена. Ссылка больше не работает.%s\n", dim(), reset());
    }
    return 0;
}

} // namespace ferry::cli
