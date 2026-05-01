/**
 * experiments/contention.h + contention.cpp
 *
 * Experiment 2 — Critical-Section Contention
 * ──────────────────────────────────────────
 * Varies the critical-section workload size from 0 to 10000 cycles.
 * Shows the crossover point where spinlocks beat mutexes.
 *
 * Research question answered:
 *   Q2 — Contention sensitivity: what happens when CS size changes?
 *   Q6 — Kernel vs user-space overhead
 */
#pragma once
#include "../benchmark.h"
#include "../utils/csv_writer.h"
#include "../locks/lock_dispatch.h"
#include "experiment_runner.h"
#include <iostream>
#include <iomanip>
#include <cstdio>

inline void run_contention_experiment(const BenchmarkConfig& cfg, CsvWriter& csv) {
    const int fixed_threads = std::min(8, (int)cfg.thread_counts.back());

    printf("  %-14s  %8s  %14s  %10s  %10s\n",
           "Primitive", "CS-cyc", "Ops/sec", "Mean(ns)", "P99(ns)");
    printf("  %s\n", std::string(56, '-').c_str());

    for (auto kind : all_primitives()) {
        const char* pname = primitive_name(kind);

        for (uint64_t cs : cfg.cs_cycles) {
            auto lock = make_lock(kind);
            std::atomic<uint64_t> dummy{0};

            auto make_body = [&](int) -> std::function<void()> {
                return [&] {
                    // Pre-contention delay: allows other threads to form queue
                    // This makes lock contention realistic and visible
                    busy_work(50);
                    
                    lock->acquire();
                    busy_work(cs);  // variable critical-section work
                    dummy.fetch_add(1, std::memory_order_relaxed);
                    lock->release();
                };
            };

            double sum_ops = 0, sum_mean = 0, sum_p99 = 0;

            for (int rep = 0; rep < cfg.repetitions; ++rep) {
                auto res = run_experiment(fixed_threads,
                                         cfg.duration_sec,
                                         cfg.warmup_sec,
                                         make_body,
                                         cfg.pin_cpus);

                TrialResult tr;
                tr.suite           = "contention";
                tr.experiment      = "cs_size";
                tr.primitive       = pname;
                tr.threads         = fixed_threads;
                tr.cs_cycles       = cs;
                tr.trial           = rep;
                tr.ops_per_sec     = res.ops_per_sec;
                tr.mean_latency_ns = res.mean_ns;
                tr.p50_ns          = res.p50_ns;
                tr.p95_ns          = res.p95_ns;
                tr.p99_ns          = res.p99_ns;
                tr.stddev_ns       = res.stddev_ns;
                tr.cv_percent      = res.cv_pct;
                tr.throughput_ops  = res.total_ops;
                csv.write(tr);

                sum_ops  += res.ops_per_sec;
                sum_mean += res.mean_ns;
                sum_p99  += res.p99_ns;
            }
            int R = cfg.repetitions;
            printf("  %-14s  %8lu  %14.0f  %10.1f  %10.1f\n",
                   pname, (unsigned long)cs,
                   sum_ops/R, sum_mean/R, sum_p99/R);
        }
        printf("\n");
    }
}
