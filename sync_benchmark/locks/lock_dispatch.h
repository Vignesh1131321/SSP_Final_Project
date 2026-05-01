/**
 * locks/lock_dispatch.h
 *
 * Unified runtime-dispatch wrapper. Compatible with C++14 and C++17.
 *
 * Portability:
 *   - thread_local static members defined in lock_dispatch.cpp (not inline)
 *     so it compiles under -std=c++14 as well as -std=c++17.
 *   - std::shared_mutex guarded by __cplusplus >= 201703L.
 *   - std::make_unique replaced with new-expression for C++14 compat.
 *   - CPU_PAUSE pulled from spinlock.h (cross-platform x86/ARM/MSVC).
 */
#pragma once
#include "../benchmark.h"
#include "spinlock.h"        // also defines CPU_PAUSE
#include "ticket_lock.h"
#include "mcs_lock.h"
#include "clh_lock.h"
#include "semaphore_lock.h"

#include <mutex>
#include <atomic>
#include <memory>
#include <functional>

#if __cplusplus >= 201703L
#  include <shared_mutex>
#endif

/* ── abstract interface ──────────────────────────────────── */
struct ILock {
    virtual ~ILock() = default;
    virtual void acquire()  = 0;
    virtual void release()  = 0;
    void lock_section(const std::function<void()>& fn) {
        acquire(); fn(); release();
    }
};

/* ── Atomic CAS lock ─────────────────────────────────────── */
struct AtomicCASLock : ILock {
    std::atomic<bool> flag_{false};
    void acquire() override {
        bool exp = false;
        while (!flag_.compare_exchange_weak(exp, true,
               std::memory_order_acquire, std::memory_order_relaxed)) {
            exp = false;
            CPU_PAUSE();
        }
    }
    void release() override { flag_.store(false, std::memory_order_release); }
};

/* ── std::mutex ──────────────────────────────────────────── */
struct MutexLock : ILock {
    std::mutex m_;
    void acquire() override { m_.lock(); }
    void release() override { m_.unlock(); }
};

/* ── SpinLock ────────────────────────────────────────────── */
struct SpinLockWrapper : ILock {
    SpinLock sl_;
    void acquire() override { sl_.lock(); }
    void release() override { sl_.unlock(); }
};

/* ── TicketLock ──────────────────────────────────────────── */
struct TicketLockWrapper : ILock {
    TicketLock tl_;
    void acquire() override { tl_.lock(); }
    void release() override { tl_.unlock(); }
};

/* ── MCS lock ────────────────────────────────────────────── */
struct MCSLockWrapper : ILock {
    MCSLock ml_;
    static thread_local MCSNode tl_node;   // defined in lock_dispatch.cpp
    void acquire() override { ml_.lock(&tl_node); }
    void release() override { ml_.unlock(&tl_node); }
};

/* ── CLH lock ────────────────────────────────────────────── */
struct CLHLockWrapper : ILock {
    CLHLock cl_;
    static thread_local CLHNode* tl_node;  // defined in lock_dispatch.cpp
    static thread_local CLHNode* tl_pred;  // defined in lock_dispatch.cpp
    void acquire() override { cl_.lock(tl_node, &tl_pred); }
    void release() override {
        cl_.unlock(tl_node);
        tl_node = tl_pred;
    }
};

/* ── Semaphore ───────────────────────────────────────────── */
struct SemaphoreWrapper : ILock {
    SemaphoreLock sl_;
    void acquire() override { sl_.lock(); }
    void release() override { sl_.unlock(); }
};

/* ── RW Lock  (shared_mutex = C++17; plain mutex fallback on C++14) ── */
#if __cplusplus >= 201703L
struct RWLockWrapper : ILock {
    std::shared_mutex rw_;
    void acquire() override { rw_.lock(); }
    void release() override { rw_.unlock(); }
};
#else
struct RWLockWrapper : ILock {
    std::mutex rw_;
    void acquire() override { rw_.lock(); }
    void release() override { rw_.unlock(); }
};
#endif

/* ── factory ─────────────────────────────────────────────── */
inline std::unique_ptr<ILock> make_lock(PrimitiveKind k) {
    switch (k) {
        case PrimitiveKind::Mutex:      return std::unique_ptr<ILock>(new MutexLock());
        case PrimitiveKind::SpinLock:   return std::unique_ptr<ILock>(new SpinLockWrapper());
        case PrimitiveKind::MCSLock:    return std::unique_ptr<ILock>(new MCSLockWrapper());
        case PrimitiveKind::Semaphore:  return std::unique_ptr<ILock>(new SemaphoreWrapper());
        default: return nullptr;
    }
}
