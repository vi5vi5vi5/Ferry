#include "Cli/Hasher.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

#include "Cli/VolumeFile.h"

namespace ferry::cli {
namespace {

int64_t steadyMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Потолок скорости чтения — ТОЛЬКО для сценарных проверок.
//
// На быстром диске том в полгигабайта считается за доли секунды, и
// проверить главное свойство хешей на лету — «ссылка раньше, чем посчитан
// том» — было бы нечем: подсчёт кончался бы раньше, чем тест успевал
// посмотреть. Переменная окружения, а не флаг командной строки: людям
// она не нужна и в справке ей не место.
double testThrottleBytesPerSec()
{
    const char *v = std::getenv("FERRY_TEST_HASH_MBPS");
    if (!v || !*v)
        return 0;
    const double mbps = std::atof(v);
    return mbps > 0 ? mbps * 1024.0 * 1024.0 : 0;
}

} // namespace

bool Hasher::start(const Source &src, const ChunkPlan &plan, std::string *err, unsigned workers)
{
    m_src = src;
    m_plan = plan;
    const size_t count = size_t(plan.chunkCount);
    m_hashes.assign(count, Hash32{});
    m_ready.assign(count, 0);
    m_done.store(0);
    m_bytes.store(0);
    m_stop.store(false);
    m_failed.store(false);
    m_error.clear();
    m_queue.clear();
    m_readerDone = false;

    // Одно ядро оставляем тому, кто этим хешером пользуется: отправитель в
    // это же время шифрует и шлёт чанки. Больше четырёх считающих потоков
    // не нужно никогда — BLAKE3 на одном современном ядре уже быстрее
    // любого диска, и лишние потоки только съели бы память на буферы.
    if (workers == 0) {
        const unsigned cpus = platform::cpuCount();
        workers = cpus > 1 ? std::min(cpus - 1, 4u) : 1u;
    }

    const size_t slots = size_t(workers) * 2 + 2;
    m_buffers.assign(slots, std::vector<uint8_t>(plan.chunkSize));
    m_free.clear();
    for (size_t i = 0; i < slots; ++i)
        m_free.push_back(i);

    m_started = true;
    if (count == 0)
        return true;

    if (!m_reader.start([this] { readerLoop(); })) {
        if (err)
            *err = "не удалось запустить поток чтения";
        m_started = false;
        return false;
    }
    for (unsigned i = 0; i < workers; ++i) {
        auto t = std::make_unique<platform::Thread>();
        if (!t->start([this] { workerLoop(); })) {
            // Хотя бы один считающий поток уже есть — работать можно, просто
            // медленнее. Нет ни одного — чтение встанет, и это надо сказать.
            if (m_workers.empty()) {
                stop();
                if (err)
                    *err = "не удалось запустить поток подсчёта хешей";
                m_started = false;
                return false;
            }
            break;
        }
        m_workers.push_back(std::move(t));
    }
    return true;
}

void Hasher::stop()
{
    // Флаг ставится ПОД мьютексом, и это не перестраховка. Потоки проверяют
    // его под тем же мьютексом прямо перед wait. Поставь его без блокировки
    // — и поток мог бы проверить «не остановлен», а уведомление прилетело
    // бы раньше, чем он уснёт: после этого он ждал бы вечно, а join вместе
    // с ним.
    {
        platform::LockGuard g(m_mu);
        m_stop.store(true);
    }
    m_haveFree.notifyAll();
    m_haveJob.notifyAll();
    m_reader.join();
    for (auto &w : m_workers)
        w->join();
    m_workers.clear();
}

std::string Hasher::error()
{
    platform::LockGuard g(m_mu);
    return m_error;
}

HashList Hasher::list() const
{
    HashList out;
    out.reserve(m_hashes.size());
    for (const Hash32 &h : m_hashes)
        out.append(h);
    return out;
}

void Hasher::fail(const std::string &why)
{
    {
        platform::LockGuard g(m_mu);
        if (m_error.empty())
            m_error = why;
        m_stop.store(true);
    }
    m_failed.store(true, std::memory_order_release);
    m_haveFree.notifyAll();
    m_haveJob.notifyAll();
}

void Hasher::readerLoop()
{
    // Свой том со своими дескрипторами: отправитель в это же время читает
    // тот же файл для раздачи, и делить с ним курсоры и кэш открытых файлов
    // значило бы делить гонки.
    VolumeFile volume;
    const bool opened = m_src.tree ? volume.openTree(m_src.path, *m_src.layout, false)
                                   : volume.openSingle(m_src.path, false, m_src.total);
    if (!opened) {
        fail(volume.error().empty() ? std::string("не удалось открыть том для подсчёта хешей")
                                    : volume.error());
    } else {
        const double throttle = testThrottleBytesPerSec();
        const int64_t startedMs = steadyMs();
        uint64_t readTotal = 0;
        for (uint64_t i = 0; i < m_plan.chunkCount; ++i) {
            size_t slot = 0;
            {
                platform::LockGuard g(m_mu);
                while (m_free.empty() && !m_stop.load())
                    m_haveFree.wait(m_mu);
                if (m_stop.load())
                    break;
                slot = m_free.back();
                m_free.pop_back();
            }

            const uint32_t len = m_plan.sizeOf(i);
            const int64_t got = volume.readAt(m_buffers[slot].data(), len, m_plan.offsetOf(i));
            if (got != int64_t(len)) {
                fail(volume.error().empty()
                         ? std::string("Файл читается не целиком — его изменили прямо сейчас?")
                         : volume.error());
                break;
            }

            {
                platform::LockGuard g(m_mu);
                m_queue.push_back({i, slot});
            }
            m_haveJob.notifyOne();

            if (throttle > 0) {
                readTotal += len;
                const int64_t due = startedMs + int64_t(double(readTotal) * 1000.0 / throttle);
                const int64_t now = steadyMs();
                if (due > now && !m_stop.load())
                    platform::sleepMs(int(due - now));
            }
        }
    }

    {
        platform::LockGuard g(m_mu);
        m_readerDone = true;
    }
    m_haveJob.notifyAll();
}

void Hasher::workerLoop()
{
    for (;;) {
        Job job;
        {
            platform::LockGuard g(m_mu);
            while (m_queue.empty() && !m_readerDone && !m_stop.load())
                m_haveJob.wait(m_mu);
            if (m_stop.load() || m_queue.empty())
                return;
            job = m_queue.front();
            m_queue.pop_front();
        }

        const uint32_t len = m_plan.sizeOf(job.index);
        const Hash32 h = blake3(m_buffers[job.slot].data(), len);

        {
            platform::LockGuard g(m_mu);
            m_hashes[size_t(job.index)] = h;
            m_ready[size_t(job.index)] = 1;
            m_free.push_back(job.slot);

            // Потоки заканчивают вразнобой, а наружу отдаётся только
            // сплошное начало. Сдвигаем его, пока впереди готовые чанки;
            // каждый чанк проходится здесь один раз, так что в сумме это
            // линейно, а не квадратично.
            uint64_t p = m_done.load(std::memory_order_relaxed);
            while (p < m_plan.chunkCount && m_ready[size_t(p)])
                ++p;
            // release: хеши до p записаны раньше — под этим же мьютексом,
            // кем бы из потоков ни были посчитаны, — и тот, кто прочитает
            // done() с acquire, их увидит.
            m_done.store(p, std::memory_order_release);
        }
        m_bytes.fetch_add(len, std::memory_order_relaxed);
        m_haveFree.notifyOne();
    }
}

} // namespace ferry::cli
