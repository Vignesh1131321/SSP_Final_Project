/**
 * experiments/scalability.cpp
 */
#include "scalability.h"
#include <cstdio>

void run_scalability_experiment(const BenchmarkConfig& cfg, CsvWriter& csv) {

    // Table header
    printf("  %-14s  %6s  %14s  %10s  %10s  %10s\n",
           "Primitive", "Thr", "Ops/sec", "Mean(ns)", "P95(ns)", "CV%");
    printf("  %s\n", std::string(68, '-').c_str());

    int hw_concurrency = static_cast<int>(std::thread::hardware_concurrency());
    fflush(stdout);

    for (auto kind : all_primitives()) {
        const char* pname = primitive_name(kind);
        if (!cfg.primitive_filter.empty() && cfg.primitive_filter != pname) {
            continue;
        }

        fprintf(stderr, "[DEBUG] Starting primitive: %s\n", pname);
        fflush(stderr);

        for (int n_threads : cfg.thread_counts) {
            fprintf(stderr, "[DEBUG]   Testing threads=%d\n", n_threads);
            fflush(stderr);

            // MCS and CLH locks require CPU pinning to avoid deadlock
            // when thread count exceeds hardware concurrency.
            // Skip if oversubscribed without pinning.
            if (kind == PrimitiveKind::MCSLock &&
                n_threads > hw_concurrency && !cfg.pin_cpus) {
                printf("  %-14s  %6d  (skipped: oversubscribed without pinning)\n",
                       pname, n_threads);
                fprintf(stderr, "[DEBUG]     SKIPPED (oversubscribed without pinning)\n");
                fflush(stderr);
                continue;
            }

            fprintf(stderr, "[DEBUG]     Creating lock...\n");
            fflush(stderr);
            // Each thread has its own lock (or shared — we test shared)
            auto lock = make_lock(kind);
            fprintf(stderr, "[DEBUG]     Lock created\n");
            fflush(stderr);

            // Shared counter to ensure work actually happens
            std::atomic<uint64_t> shared_counter{0};

            uint64_t cs_cycles = cfg.scalability_cs_cycles;
            auto make_body = [&](int /*tid*/) -> std::function<void()> {
                return [&] {
                    lock->acquire();
                    // Configurable critical-section work; defaults to 100 cycles.
                    // Increase via --cs-cycles to expose spinlock CPU burn at low
                    // thread counts (spinlock holds the core while sleeping-locks park).
                    busy_work(cs_cycles);
                    shared_counter.fetch_add(1, std::memory_order_relaxed);
                    lock->release();
                };
            };

            double total_ops_sec = 0;
            double total_mean    = 0;
            double total_p95     = 0;
            double total_cv      = 0;

            for (int rep = 0; rep < cfg.repetitions; ++rep) {
                fprintf(stderr, "[DEBUG]       Rep %d/%d: Starting experiment...\n", 
                        rep + 1, cfg.repetitions);
                fflush(stderr);
                
                auto res = run_experiment(n_threads,
                                         cfg.duration_sec,
                                         cfg.warmup_sec,
                                         make_body,
                                         cfg.pin_cpus);
                
                fprintf(stderr, "[DEBUG]       Rep %d/%d: Experiment done, ops=%.0f\n",
                        rep + 1, cfg.repetitions, res.ops_per_sec);
                fflush(stderr);

                TrialResult tr;
                tr.suite          = "scalability";
                tr.experiment     = "scalability";
                tr.primitive      = pname;
                tr.threads        = n_threads;
                tr.cs_cycles      = cs_cycles;
                tr.trial          = rep;
                tr.ops_per_sec    = res.ops_per_sec;
                tr.mean_latency_ns = res.mean_ns;
                tr.p50_ns         = res.p50_ns;
                tr.p95_ns         = res.p95_ns;
                tr.p99_ns         = res.p99_ns;
                tr.stddev_ns      = res.stddev_ns;
                tr.cv_percent     = res.cv_pct;
                tr.throughput_ops = res.total_ops;
                tr.notes          = cfg.pin_cpus ? "pinned" : "os-sched";
                csv.write(tr);

                total_ops_sec += res.ops_per_sec;
                total_mean    += res.mean_ns;
                total_p95     += res.p95_ns;
                total_cv      += res.cv_pct;
            }

            // Print row averages
            int R = cfg.repetitions;
            printf("  %-14s  %6d  %14.0f  %10.1f  %10.1f  %10.1f\n",
                   pname, n_threads,
                   total_ops_sec / R,
                   total_mean    / R,
                   total_p95     / R,
                   total_cv      / R);
            fprintf(stderr, "[DEBUG]   Threads=%d COMPLETE\n", n_threads);
            fflush(stderr);
        }
        fprintf(stderr, "[DEBUG] Primitive %s COMPLETE\n", pname);
        fflush(stderr);
        printf("\n");
    }
}
