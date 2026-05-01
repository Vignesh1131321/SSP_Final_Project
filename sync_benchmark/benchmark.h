/**
 * benchmark.h
 * Central configuration, types, and constants shared across
 * all experiments and utilities.
 */
#pragma once

#include <atomic>
#include <string>
#include <vector>
#include <thread>
#include <functional>
#include <cstdint>
#include <cstddef>

/* ── hardware constants ──────────────────────────────────── */
#ifndef CACHE_LINE_SIZE
#  define CACHE_LINE_SIZE 64
#endif

/* ── benchmark-wide configuration ───────────────────────── */
struct BenchmarkConfig {
    std::string suite        = "all";
    std::vector<int> thread_counts = {1, 2, 3, 4, 6, 8, 10, 12};
    double duration_sec      = 5.0;
    double warmup_sec        = 2.0;
    int    repetitions       = 10;
    std::string output_dir   = "output";
    std::string csv_file     = "results.csv";
    bool verbose             = false;
    bool pin_cpus            = false;
    std::string primitive_filter;

    // False-sharing publication settings.
    // placement_policy: all|none|compact|spread
    // smt_mode: both|on|off  ("off" uses an SMT-avoidance affinity map when possible)
    std::string placement_policy = "all";
    std::string smt_mode         = "both";

    /* critical-section workload sizes (in busy-loop cycles) */
    std::vector<uint64_t> cs_cycles = {0, 100, 500, 1000, 10000};

    /* scalability experiment CS size — tunable via --cs-cycles */
    uint64_t scalability_cs_cycles = 100;
};

/* ── per-trial result ────────────────────────────────────── */
struct TrialResult {
    std::string suite;
    std::string experiment;
    std::string primitive;
    int         threads         = 0;
    uint64_t    cs_cycles       = 0;
    int         trial           = 0;

    double ops_per_sec          = 0.0;
    double mean_latency_ns      = 0.0;
    double p50_ns               = 0.0;
    double p95_ns               = 0.0;
    double p99_ns               = 0.0;
    double stddev_ns            = 0.0;
    double cv_percent           = 0.0;   // coefficient of variation
    uint64_t throughput_ops     = 0;

    double false_sharing_factor = 0.0;  // slowdown = padded throughput / false-shared throughput
    double fairness_gini        = 0.0;  // 0 = perfect fairness
    double fairness_jain        = 0.0;  // 1 = perfect fairness
    double fairness_maxmin      = 0.0;  // min(per-thread) / max(per-thread)
    double fairness_cv          = 0.0;  // coefficient of variation over per-thread counts

    long   cpu_migrations       = -1;   // -1 = not measured
    long   ctx_switches         = -1;

    std::string notes;
};

/* ── primitive IDs ──────────────────────────────────────── */
enum class PrimitiveKind {
    Mutex,
    SpinLock,
    MCSLock,
    Semaphore,
};

inline const char* primitive_name(PrimitiveKind k) {
    switch (k) {
        case PrimitiveKind::Mutex:      return "mutex";
        case PrimitiveKind::SpinLock:   return "spinlock";
        case PrimitiveKind::MCSLock:    return "mcs_lock";
        case PrimitiveKind::Semaphore:  return "semaphore";
        default:                        return "unknown";
    }
}

/* ── all primitives we benchmark ────────────────────────── */
inline const std::vector<PrimitiveKind>& all_primitives() {
    static const std::vector<PrimitiveKind> v = {
        PrimitiveKind::Mutex,
        PrimitiveKind::SpinLock,
        PrimitiveKind::MCSLock,
        PrimitiveKind::Semaphore,
    };
    return v;
}

/* ── workload helper: busy-spin for N cycles ─────────────── */
inline void busy_work(uint64_t cycles) {
    if (cycles == 0) return;
    uint64_t end = 0;
    // Use volatile to prevent optimisation
    volatile uint64_t x = 1;
    for (uint64_t i = 0; i < cycles; ++i) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        end += x;
    }
    (void)end;
}

/* ── timing barrier ──────────────────────────────────────── */
struct RunBarrier {
    std::atomic<int>  waiting{0};
    std::atomic<bool> go{false};
    int               total;

    explicit RunBarrier(int n) : total(n) {}

    void arrive_and_wait() {
        int my_count = waiting.fetch_add(1, std::memory_order_acq_rel) + 1;
        // Wait until all threads have arrived
        if (my_count < total) {
            // Not all threads here yet, spin-wait
            while (waiting.load(std::memory_order_acquire) < total) {
                std::this_thread::yield();
            }
        }
        // All threads have arrived; signal others
        go.store(true, std::memory_order_release);
        // Final barrier: ensure all threads see the signal
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    bool should_run() const {
        return go.load(std::memory_order_acquire);
    }
};
