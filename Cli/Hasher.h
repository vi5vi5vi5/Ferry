#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "Cli/platform/Threads.h"
#include "core/Chunker.h"
#include "core/HashList.h"
#include "core/Types.h"
#include "core/VolumeLayout.h"

// Хеши тома в фоне.
//
// Раньше отправитель читал том целиком ДО ссылки: прочитать чанк,
// дождаться, посчитать хеш, прочитать следующий. Диск и процессор
// работали по очереди, хеш считало одно ядро, а человек с архивом на сто
// гигабайт смотрел на полосу «считаю хеши» по десять минут, прежде чем
// получить хоть что-то, что можно кому-то отправить.
//
// Здесь то же самое разнесено по потокам. Один читает том подряд — ровно
// так, как любит любой диск, включая HDD, — а несколько считают BLAKE3 по
// уже прочитанным чанкам. Пока хеш одного чанка считается, следующий уже
// читается, и скорость упирается в диск, а не в сумму диска и процессора.
//
// Наружу видно одно число — сколько чанков подряд С НАЧАЛА уже посчитано.
// Именно подряд: хеши уходят получателям сегментами встык, и дырка в
// середине значила бы, что сегмент отправлять ещё нельзя.
namespace ferry::cli {

class Hasher
{
public:
    struct Source
    {
        std::string path;                     // файл или корень дерева
        bool tree = false;
        const VolumeLayout *layout = nullptr; // для дерева; живёт дольше хешера
        uint64_t total = 0;                   // для одиночного файла
    };

    Hasher() = default;
    ~Hasher() { stop(); }
    Hasher(const Hasher &) = delete;
    Hasher &operator=(const Hasher &) = delete;

    // Сколько потоков считают хеши. 0 — выбрать по числу ядер.
    bool start(const Source &src, const ChunkPlan &plan, std::string *err, unsigned workers = 0);

    // Остановить и дождаться потоков. Безопасно звать повторно.
    void stop();

    // Посчитано подряд с начала. at(i) законен только для i < done().
    uint64_t done() const { return m_done.load(std::memory_order_acquire); }
    bool finished() const { return m_started && done() == m_plan.chunkCount; }
    const Hash32 &at(uint64_t i) const { return m_hashes[size_t(i)]; }

    // Сколько байт уже прочитано и посчитано — для скорости на экране.
    uint64_t bytesDone() const { return m_bytes.load(std::memory_order_relaxed); }

    bool failed() const { return m_failed.load(std::memory_order_acquire); }
    std::string error();

    // Весь список. Только после finished().
    HashList list() const;

private:
    struct Job
    {
        uint64_t index = 0;
        size_t slot = 0;
    };

    void readerLoop();
    void workerLoop();
    void fail(const std::string &why);

    Source m_src;
    ChunkPlan m_plan;
    bool m_started = false;

    std::vector<Hash32> m_hashes;
    std::vector<uint8_t> m_ready;              // под m_mu: посчитан ли чанк
    std::atomic<uint64_t> m_done{0};
    std::atomic<uint64_t> m_bytes{0};

    // Буферы по размеру чанка. Их немного больше, чем считающих потоков:
    // пока одни считаются, в другие уже читается.
    std::vector<std::vector<uint8_t>> m_buffers;
    std::vector<size_t> m_free;                // под m_mu
    std::deque<Job> m_queue;                   // под m_mu, по возрастанию индекса
    bool m_readerDone = false;                 // под m_mu

    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_failed{false};
    std::string m_error;                       // под m_mu

    platform::Mutex m_mu;
    platform::CondVar m_haveFree;              // освободился буфер
    platform::CondVar m_haveJob;               // появилась работа или всё кончилось

    platform::Thread m_reader;
    std::vector<std::unique_ptr<platform::Thread>> m_workers;
};

} // namespace ferry::cli
