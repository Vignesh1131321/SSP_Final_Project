/**
 * experiments/throughput_saturation.h
 *
 * Experiment 6 — Throughput Saturation Curve
 * ──────────────────────────────────────────
 * Ramps thread count from 1 to N×hardware_concurrency and measures
 * when the system saturates (throughput plateaus or drops).
 * Also measures lock hold time vs contention.
 *
 * Research question answered:
 *   Q1 — Scalability cliff (where does adding threads hurt?)
 *   Q2 — CS size interaction with saturation point
 */
#pragma once
#include "../benchmark.h"
#include "../utils/csv_writer.h"
#include "../locks/lock_dispatch.h"
#include "experiment_runner.h"
#include <cstdio>
#include <algorithm>

inline void run_throughput_saturation_experiment(const BenchmarkConfig& cfg,
                                                  CsvWriter& csv) {
    // Ramp up to 2× hardware concurrency to show oversubscription
    int hw = std::thread::hardware_concurrency();
    std::vector<int> thread_sweep;
    for (int t = 1; t <= hw * 2; t = (t < 4) ? t + 1 : t * 2) {
        if (t > hw * 2) break;
        thread_sweep.push_back(t);
    }
    if (thread_sweep.empty()) thread_sweep = {1, 2};

    // Only test a representative subset of primitives to keep runtime sane
    std::vector<PrimitiveKind> test_primitives = {
        PrimitiveKind::Mutex,
        PrimitiveKind::SpinLock,
        PrimitiveKind::MCSLock,
        PrimitiveKind::Semaphore,
    };

    // Two CS sizes: zero and moderate
    std::vector<uint64_t> cs_sizes = {0, 500};

    printf("  %-14s  %8s  %6s  %14s  %10s\n",
           "Primitive", "CS-cyc", "Thr", "Ops/sec", "Efficiency%");
    printf("  %s\n", std::string(56, '-').c_str());

    for (auto kind : test_primitives) {
        const char* pname = primitive_name(kind);

        for (uint64_t cs : cs_sizes) {
            double single_thread_ops = 0;

            for (int n_threads : thread_sweep) {
                auto lock = make_lock(kind);
                std::atomic<uint64_t> dummy{0};

                auto make_body = [&](int) -> std::function<void()> {
                    return [&] {
                        lock->acquire();
                        busy_work(cs);
                        dummy.fetch_add(1, std::memory_order_relaxed);
                        lock->release();
                    };
                };

                double sum_ops = 0;
                for (int rep = 0; rep < cfg.repetitions; ++rep) {
                    auto res = run_experiment(n_threads,
                                             cfg.duration_sec,
                                             cfg.warmup_sec,
                                             make_body,
                                             cfg.pin_cpus);
                    sum_ops += res.ops_per_sec;

                    TrialResult tr;
                    tr.suite          = "throughput";
                    tr.experiment     = "throughput_saturation";
                    tr.primitive      = pname;
                    tr.threads        = n_threads;
                    tr.cs_cycles      = cs;
                    tr.trial          = rep;
                    tr.ops_per_sec    = res.ops_per_sec;
                    tr.mean_latency_ns = res.mean_ns;
                    tr.p95_ns         = res.p95_ns;
                    tr.p99_ns         = res.p99_ns;
                    tr.stddev_ns      = res.stddev_ns;
                    tr.cv_percent     = res.cv_pct;
                    tr.throughput_ops = res.total_ops;
                    csv.write(tr);
                }

                double avg_ops = sum_ops / cfg.repetitions;
                if (n_threads == 1) single_thread_ops = avg_ops;

                double efficiency = (single_thread_ops > 0 && n_threads > 1)
                    ? (avg_ops / (single_thread_ops * n_threads)) * 100.0
                    : 100.0;

                printf("  %-14s  %8lu  %6d  %14.0f  %10.1f\n",
                       pname, (unsigned long)cs, n_threads,
                       avg_ops, efficiency);
            }
            printf("\n");
        }
    }
}
