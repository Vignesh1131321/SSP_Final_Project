/**
 * locks/semaphore_lock.h  —  Semaphore lock (cross-platform)
 *
 * On Linux/macOS: uses POSIX sem_wait / sem_post (true kernel semaphore).
 * On Windows / non-POSIX: falls back to std::mutex + std::condition_variable
 * emulation, which still crosses into the kernel on every acquire/release
 * and serves the same benchmarking purpose.
 *
 * The #ifdef guard means you get the "real" semaphore on Linux (where the
 * paper's numbers should come from) and a compilable stub on Windows.
 */
#pragma once
#include <stdexcept>

#if defined(__linux__) || defined(__APPLE__)
/* ── POSIX semaphore ─────────────────────────────────────── */
#  include <semaphore.h>
#  include <cerrno>

class SemaphoreLock {
    sem_t sem_;
public:
    SemaphoreLock() {
        if (sem_init(&sem_, 0, 1) != 0)
            throw std::runtime_error("sem_init failed");
    }
    ~SemaphoreLock() { sem_destroy(&sem_); }
    void lock()   noexcept { sem_wait(&sem_); }
    void unlock() noexcept { sem_post(&sem_); }
};

class CountingSemaphore {
    sem_t sem_;
public:
    explicit CountingSemaphore(int initial = 0) {
        if (sem_init(&sem_, 0, static_cast<unsigned>(initial)) != 0)
            throw std::runtime_error("sem_init failed");
    }
    ~CountingSemaphore() { sem_destroy(&sem_); }
    void wait()  { sem_wait(&sem_); }
    bool try_wait() {
        if (sem_trywait(&sem_) == 0) return true;
        return false;
    }
    void post()  { sem_post(&sem_); }
    int  value() { int v = 0; sem_getvalue(&sem_, &v); return v; }
};

#else
/* ── Portable fallback (Windows / unknown POSIX) ──────────── */
#  include <mutex>
#  include <condition_variable>

class SemaphoreLock {
    std::mutex              mu_;
    std::condition_variable cv_;
    int                     count_{1};
public:
    SemaphoreLock() = default;
    void lock() {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this]{ return count_ > 0; });
        --count_;
    }
    void unlock() {
        std::lock_guard<std::mutex> lk(mu_);
        ++count_;
        cv_.notify_one();
    }
};

class CountingSemaphore {
    std::mutex              mu_;
    std::condition_variable cv_;
    int                     count_;
public:
    explicit CountingSemaphore(int initial = 0) : count_(initial) {}
    void wait() {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this]{ return count_ > 0; });
        --count_;
    }
    void post() {
        std::lock_guard<std::mutex> lk(mu_);
        ++count_;
        cv_.notify_one();
    }
    bool try_wait() {
        std::lock_guard<std::mutex> lk(mu_);
        if (count_ <= 0) return false;
        --count_;
        return true;
    }
    int value() {
        std::lock_guard<std::mutex> lk(mu_);
        return count_;
    }
};

#endif /* POSIX / Windows */
