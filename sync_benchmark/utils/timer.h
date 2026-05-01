/**
 * utils/timer.h — High-resolution timing utilities
 *
 * Uses clock_gettime(CLOCK_MONOTONIC_RAW) where available,
 * falling back to std::chrono::steady_clock.
 */
#pragma once
#include <cstdint>
#include <ctime>
#include <chrono>

#ifndef CPU_PAUSE
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
#endif

namespace timer {

/* ── nanosecond timestamp ────────────────────────────────── */
inline uint64_t now_ns() {
#if defined(__linux__)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
#else
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            clock::now().time_since_epoch()).count());
#endif
}

/* ── RDTSC for low-overhead per-operation timing ─────────── */
inline uint64_t rdtsc() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#else
    return now_ns(); // fallback
#endif
}

/* Serialize RDTSC (prevents out-of-order execution) */
inline uint64_t rdtscp() {
#if defined(__x86_64__)
    unsigned lo, hi, aux;
    __asm__ __volatile__ ("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#else
    return rdtsc();
#endif
}

/* ── RAII scope timer ────────────────────────────────────── */
struct ScopeTimer {
    uint64_t start;
    uint64_t& out;
    explicit ScopeTimer(uint64_t& out_ref) : start(now_ns()), out(out_ref) {}
    ~ScopeTimer() { out = now_ns() - start; }
};

/* ── Busy-wait deadline ──────────────────────────────────── */
inline void sleep_ns(uint64_t ns) {
    uint64_t end = now_ns() + ns;
    while (now_ns() < end) CPU_PAUSE();
}

} // namespace timer
