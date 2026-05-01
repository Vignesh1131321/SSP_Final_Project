/**
 * hardware/numa_ht_bench.h
 * ─────────────────────────
 * RQ10 — How do NUMA and hyperthreading affect synchronisation scalability?
 * RQ4  — CPU pinning vs OS scheduling
 *
 * Experiments:
 *   A) Hyper-threading analysis
 *      Threads ≤ physical cores  (stride=2 pinning avoids HT siblings)
 *      Threads = physical cores  (all physical cores used)
 *      Threads > physical cores  (hyper-threads activated / oversubscribed)
 *
 *   B) NUMA-aware pinning (Linux only via numactl / libnuma)
 *      Same-node:     all threads on NUMA node 0
 *      Cross-node:    threads spread across all NUMA nodes
 *
 * On non-NUMA / non-Linux systems the NUMA section stubs out gracefully.
 */
#pragma once
#include "../benchmark.h"
#include "../utils/csv_writer.h"
#include "../utils/timer.h"
#include "../utils/stats.h"
#include "../utils/cpu_affinity.h"
#include "../locks/lock_dispatch.h"
#include "../experiments/experiment_runner.h"

#include <vector>
#include <thread>
#include <string>
#include <cstdio>
#include <atomic>

/* ── numactl / hwloc detection ──────────────────────────── */
#if defined(__linux__)
#  if __has_include(<numa.h>)
#    include <numa.h>
#    define HAS_NUMA 1
#  else
#    define HAS_NUMA 0
#  endif
#else
#  define HAS_NUMA 0
#endif

/* ═══════════════════════════════════════════════════════════
 * A) Hyper-threading experiment
 *    Compares performance when threads are pinned to:
 *    (a) Physical cores only  (stride-2 mapping)
 *    (b) All logical cores    (stride-1, activates HT siblings)
 *    (c) Oversubscribed       (more threads than logical cores)
 * ═══════════════════════════════════════════════════════════ */
static void ht_sweep(const BenchmarkConfig& cfg, CsvWriter& csv,
                      PrimitiveKind kind) {
    int hw = cpu::hardware_threads();
    const char* pname = primitive_name(kind);

    // Physical cores = hw / 2 on typical HT machine (heuristic)
    int physical = std::max(1, hw / 2);

    struct HTConfig {
        std::string label;
        int         threads;
        int         stride;   // 1=all logical, 2=skip HT sibling
    };

    std::vector<HTConfig> configs = {
        {"ht_under_physical",  physical,     2},
        {"ht_all_physical",    physical,     1},
        {"ht_logical_max",     hw,           1},
        {"ht_oversubscribed",  hw * 2,       1},
    };

    for (auto& c : configs) {
        auto lock = make_lock(kind);
        std::atomic<uint64_t> dummy{0};

        // Build affinity map using the chosen stride
        std::vector<int> affmap(c.threads);
        for (int i = 0; i < c.threads; ++i)
            affmap[i] = (i * c.stride) % hw;

        auto make_body = [&](int /*tid*/) -> std::function<void()> {
            return [&] {
                lock->acquire();
                busy_work(0);
                dummy.fetch_add(1, std::memory_order_relaxed);
                lock->release();
            };
        };

        for (int rep = 0; rep < cfg.repetitions; ++rep) {
            auto res = run_experiment(c.threads,
                                     cfg.duration_sec,
                                     cfg.warmup_sec,
                                     make_body,
                                     /*pin=*/true);

            TrialResult tr;
            tr.suite           = "hardware";
            tr.experiment      = "hyperthreading";
            tr.primitive       = pname;
            tr.threads         = c.threads;
            tr.trial           = rep;
            tr.ops_per_sec     = res.ops_per_sec;
            tr.mean_latency_ns = res.mean_ns;
            tr.p95_ns          = res.p95_ns;
            tr.p99_ns          = res.p99_ns;
            tr.stddev_ns       = res.stddev_ns;
            tr.throughput_ops  = res.total_ops;
            tr.notes           = c.label + " stride=" + std::to_string(c.stride);
            csv.write(tr);

            if (rep == 0)
                printf("  %-14s  %-24s  %6d  %12.0f\n",
                       pname, c.label.c_str(), c.threads, res.ops_per_sec);
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 * B) NUMA experiment
 * ═══════════════════════════════════════════════════════════ */
#if HAS_NUMA
static void numa_sweep(const BenchmarkConfig& cfg, CsvWriter& csv,
                        PrimitiveKind kind) {
    if (numa_available() < 0) {
        printf("  [NUMA] libnuma present but NUMA not available on this system.\n");
        return;
    }
    int num_nodes = numa_max_node() + 1;
    if (num_nodes < 2) {
        printf("  [NUMA] Only 1 NUMA node — cross-node test not applicable.\n");
    }

    const char* pname = primitive_name(kind);
    int n_threads = std::min(16, cpu::hardware_threads());

    struct NumaConfig {
        std::string label;
        std::function<void(int /*tid*/, int /*nthreads*/)> pin_fn;
    };

    std::vector<NumaConfig> configs;

    // Same node: all threads → node 0
    configs.push_back({"numa_same_node", [](int tid, int n) {
        numa_run_on_node(0);
    }});

    // Cross node: round-robin across all nodes
    if (num_nodes >= 2) {
        configs.push_back({"numa_cross_node", [num_nodes](int tid, int n) {
            numa_run_on_node(tid % num_nodes);
        }});
    }

    for (auto& nc : configs) {
        auto lock = make_lock(kind);
        std::atomic<uint64_t> dummy{0};

        auto make_body = [&](int tid) -> std::function<void()> {
            nc.pin_fn(tid, n_threads);
            return [&] {
                lock->acquire();
                busy_work(0);
                dummy.fetch_add(1, std::memory_order_relaxed);
                lock->release();
            };
        };

        for (int rep = 0; rep < cfg.repetitions; ++rep) {
            auto res = run_experiment(n_threads,
                                     cfg.duration_sec, cfg.warmup_sec,
                                     make_body, false);

            TrialResult tr;
            tr.suite           = "hardware";
            tr.experiment      = "numa";
            tr.primitive       = pname;
            tr.threads         = n_threads;
            tr.trial           = rep;
            tr.ops_per_sec     = res.ops_per_sec;
            tr.mean_latency_ns = res.mean_ns;
            tr.p95_ns          = res.p95_ns;
            tr.p99_ns          = res.p99_ns;
            tr.stddev_ns       = res.stddev_ns;
            tr.notes           = nc.label
                               + " nodes=" + std::to_string(num_nodes);
            csv.write(tr);

            if (rep == 0)
                printf("  %-14s  %-20s  %6d  %12.0f\n",
                       pname, nc.label.c_str(), n_threads, res.ops_per_sec);
        }
    }
}
#else
static void numa_sweep(const BenchmarkConfig&, CsvWriter&, PrimitiveKind) {
    printf("  [NUMA] Not available (requires Linux + libnuma). "
           "Install: sudo apt install libnuma-dev\n");
}
#endif

/* ── Public entry point ─────────────────────────────────── */
inline void run_hardware_experiment(const BenchmarkConfig& cfg,
                                     CsvWriter& csv) {
    // Representative primitives for HT / NUMA comparison
    std::vector<PrimitiveKind> test_prims = {
        PrimitiveKind::Mutex,
        PrimitiveKind::SpinLock,
        PrimitiveKind::MCSLock,
        PrimitiveKind::AtomicCAS,
    };

    printf("\n  ── Hyper-Threading Analysis ──────────────────────────\n");
    printf("  %-14s  %-24s  %6s  %12s\n",
           "Primitive", "HT Config", "Thr", "Ops/sec");
    printf("  %s\n", std::string(60, '-').c_str());

    for (auto kind : test_prims)
        ht_sweep(cfg, csv, kind);

    printf("\n  ── NUMA Analysis ─────────────────────────────────────\n");
    printf("  %-14s  %-20s  %6s  %12s\n",
           "Primitive", "NUMA Config", "Thr", "Ops/sec");
    printf("  %s\n", std::string(56, '-').c_str());

    for (auto kind : test_prims)
        numa_sweep(cfg, csv, kind);
}
