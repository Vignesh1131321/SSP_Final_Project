/**
 * locks/spinlock.h  —  Test-and-Set spinlock
 *
 * Simple TAS lock: spin on an atomic flag.
 * Poor fairness, excellent low-contention latency.
 *
 * Portable: Linux (GCC/Clang), Windows (MSVC/MinGW), macOS.
 */
#pragma once
#include <atomic>

/* ── Portable CPU-pause hint ─────────────────────────────────
 * Reduces power consumption and memory-ordering pressure during
 * spin-wait loops. Falls back to a no-op on unknown architectures.
 */
#if defined(_MSC_VER)
#  include <intrin.h>
#  define CPU_PAUSE() _mm_pause()
#elif defined(__GNUC__) || defined(__clang__)
#  if defined(__x86_64__) || defined(__i386__)
#    define CPU_PAUSE() __asm__ __volatile__("pause" ::: "memory")
#  elif defined(__aarch64__) || defined(__arm__)
#    define CPU_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#  else
#    define CPU_PAUSE() __asm__ __volatile__("" ::: "memory")
#  endif
#else
#  define CPU_PAUSE() ((void)0)
#endif

class SpinLock {
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
public:
    void lock() noexcept {
        while (flag_.test_and_set(std::memory_order_acquire))
            CPU_PAUSE();
    }
    void unlock() noexcept {
        flag_.clear(std::memory_order_release);
    }
};

/* ── TTAS (test-and-test-and-set) variant — reduces bus traffic ── */
class TTASLock {
    std::atomic<bool> locked_{false};
public:
    void lock() noexcept {
        for (;;) {
            // Optimistic read before CAS to avoid write-invalidation
            while (locked_.load(std::memory_order_relaxed))
                CPU_PAUSE();
            if (!locked_.exchange(true, std::memory_order_acquire))
                return;
        }
    }
    void unlock() noexcept {
        locked_.store(false, std::memory_order_release);
    }
};
