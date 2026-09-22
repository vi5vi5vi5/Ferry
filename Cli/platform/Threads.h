#pragma once

#include <functional>

#ifndef _WIN32
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

// Потоки, мьютекс и условная переменная — ровно столько, сколько нужно
// фоновому подсчёту хешей.
//
// Почему не std::thread. Клиент под Windows собирается MinGW-w64 из
// Debian, а там GCC 12 с моделью потоков win32: в ней нет ни std::thread,
// ни std::mutex, ни std::condition_variable (они появились только в
// GCC 13). Перейти на вариант -posix значило бы сменить компилятор всей
// сборки и тащить статическую winpthread ради трёх классов. Здесь они
// обёрнуты так же, как файлы и сокеты в Platform.h: на Windows — родные
// SRW-блокировки и CreateThread, на остальных — std::.
//
// Заголовок намеренно не включает windows.h: SRWLOCK и CONDITION_VARIABLE
// — это ровно по одному указателю, и нулевой указатель — их законное
// начальное состояние (SRWLOCK_INIT, CONDITION_VARIABLE_INIT). Храним
// void* и приводим тип только в Platform_windows.cpp, где windows.h
// включён в правильном порядке с winsock2.h.
namespace ferry::platform {

// Сколько логических процессоров видит процесс. Не меньше единицы.
unsigned cpuCount();

void sleepMs(int ms);

class Mutex
{
public:
    Mutex() = default;
    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;

    void lock();
    void unlock();

private:
    friend class CondVar;
#ifdef _WIN32
    void *m_srw = nullptr;
#else
    std::mutex m_mutex;
#endif
};

class LockGuard
{
public:
    explicit LockGuard(Mutex &m) : m_m(m) { m_m.lock(); }
    ~LockGuard() { m_m.unlock(); }
    LockGuard(const LockGuard &) = delete;
    LockGuard &operator=(const LockGuard &) = delete;

private:
    Mutex &m_m;
};

class CondVar
{
public:
    CondVar() = default;
    CondVar(const CondVar &) = delete;
    CondVar &operator=(const CondVar &) = delete;

    // Мьютекс обязан быть захвачен. Ложные пробуждения возможны — ждать
    // надо в цикле по условию, как и у std::condition_variable.
    void wait(Mutex &m);
    void notifyOne();
    void notifyAll();

private:
#ifdef _WIN32
    void *m_cv = nullptr;
#else
    std::condition_variable m_cv;
#endif
};

class Thread
{
public:
    Thread() = default;
    ~Thread() { join(); }
    Thread(const Thread &) = delete;
    Thread &operator=(const Thread &) = delete;

    bool start(std::function<void()> fn);
    void join();

private:
    std::function<void()> m_fn;
#ifdef _WIN32
    void *m_handle = nullptr;
    static unsigned long __stdcall entry(void *self);
#else
    std::thread m_thread;
#endif
};

#ifndef _WIN32
inline void Mutex::lock() { m_mutex.lock(); }
inline void Mutex::unlock() { m_mutex.unlock(); }

inline void CondVar::wait(Mutex &m)
{
    // Захват мьютекса принадлежит вызывающему, а не этому unique_lock:
    // adopt_lock берёт его взаймы, release отдаёт обратно, не отпуская.
    std::unique_lock<std::mutex> lk(m.m_mutex, std::adopt_lock);
    m_cv.wait(lk);
    lk.release();
}
inline void CondVar::notifyOne() { m_cv.notify_one(); }
inline void CondVar::notifyAll() { m_cv.notify_all(); }

inline bool Thread::start(std::function<void()> fn)
{
    m_fn = std::move(fn);
    m_thread = std::thread([this] { m_fn(); });
    return true;
}
inline void Thread::join()
{
    if (m_thread.joinable())
        m_thread.join();
}
#endif

} // namespace ferry::platform
