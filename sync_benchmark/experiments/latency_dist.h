/**
 * experiments/latency_dist.h
 *
 * Experiment 5 — Latency Distribution
 * ──────────────────────────────────────
 * For each primitive, records a histogram of lock-acquisition latencies
 * at 8 threads. Writes percentile summary + histogram bins to CSV.
 * Focuses on tail latency (p99, p99.9) which matters for real-time
 * and latency-sensitive workloads.
 *
 * Research question answered:
 *   Q1 (latency dimension) — tail latency comparison
 */
#pragma once
#include "../benchmark.h"
#include "../utils/csv_writer.h"
#include "../utils/stats.h"
#include "../utils/perf_counters.h"
#include "../locks/lock_dispatch.h"
#include "experiment_runner.h"
#include <cstdio>
#include <algorithm>
#include <numeric>
#include <limits>

inline void run_latency_distribution_experiment(const BenchmarkConfig& cfg,
                                                 CsvWriter& csv) {

    const int hw_threads = std::max(1, (int)std::thread::hardware_concurrency());

    std::vector<int> latency_threads;
    latency_threads.reserve(cfg.thread_counts.size());
    for (int t : cfg.thread_counts) {
        if (t <= 0) continue;
        latency_threads.push_back(std::min(t, hw_threads));
    }
    if (latency_threads.empty()) {
        latency_threads.push_back(std::min(8, hw_threads));
    }

    printf("  %-14s  %10s  %10s  %10s  %10s  %10s  %10s\n",
           "Primitive", "Mean(ns)", "P50(ns)", "P95(ns)", "P99(ns)",
           "StdDev", "CV%");
    printf("  %s\n", std::string(74, '-').c_str());

    for (int n_threads : latency_threads) {
        printf("\n  [threads=%d]\n", n_threads);
        for (auto kind : all_primitives()) {
            const char* pname = primitive_name(kind);

            for (int rep = 0; rep < cfg.repetitions; ++rep) {
                auto lock = make_lock(kind);
                std::vector<uint64_t> per_thread_acqs(static_cast<size_t>(n_threads), 0);

                auto make_body_counted = [&](int tid) -> std::function<void()> {
                    return [&, tid] {
                        lock->acquire();
                        busy_work(0);
                        per_thread_acqs[static_cast<size_t>(tid)]++;
                        lock->release();
                    };
                };

                ExperimentResult res;
                perf::PerfGroup pg;
                const long ctx_before = perf::read_voluntary_ctxsw();
                perf::Counters os_ctr;
                if (pg.available) {
                    os_ctr = pg.read_delta([&] {
                        res = run_experiment(n_threads,
                                             cfg.duration_sec,
                                             cfg.warmup_sec,
                                             make_body_counted,
                                             cfg.pin_cpus);
                    });
                } else {
                    res = run_experiment(n_threads,
                                         cfg.duration_sec,
                                         cfg.warmup_sec,
                                         make_body_counted,
                                         cfg.pin_cpus);
                }
                const long ctx_after = perf::read_voluntary_ctxsw();
                if (os_ctr.ctx_switches < 0 && ctx_before >= 0 && ctx_after >= ctx_before) {
                    os_ctr.ctx_switches = ctx_after - ctx_before;
                }

                auto& lats = res.all_latencies;
                double p999 = 0;
                uint64_t clipped_overflow = 0;
                std::string hist_str;
                if (!lats.empty()) {
                    std::vector<double> tmp = lats;
                    p999 = stats::percentile(tmp, 0.999);

                    std::vector<double> clipped;
                    clipped.reserve(lats.size());
                    for (double v : lats) {
                        if (v <= p999) clipped.push_back(v);
                        else ++clipped_overflow;
                    }
                    auto hist = stats::Histogram::build(clipped.empty() ? lats : clipped, 80);
                    for (size_t i = 0; i < hist.counts.size(); ++i) {
                        hist_str += std::to_string((uint64_t)hist.edges[i]) + ":"
                                  + std::to_string(hist.counts[i]) + ";";
                    }
                }

                std::vector<double> fairness_vals;
                fairness_vals.reserve(per_thread_acqs.size());
                for (uint64_t c : per_thread_acqs) fairness_vals.push_back(static_cast<double>(c));
                const double gini = stats::gini_coefficient(fairness_vals);

                TrialResult tr;
                tr.suite            = "latency_dist";
                tr.experiment       = "latency_distribution";
                tr.primitive        = pname;
                tr.threads          = n_threads;
                tr.cs_cycles        = 0;
                tr.trial            = rep;
                tr.ops_per_sec      = res.ops_per_sec;
                tr.mean_latency_ns  = res.mean_ns;
                tr.p50_ns           = res.p50_ns;
                tr.p95_ns           = res.p95_ns;
                tr.p99_ns           = res.p99_ns;
                tr.stddev_ns        = res.stddev_ns;
                tr.cv_percent       = res.cv_pct;
                tr.throughput_ops   = res.total_ops;
                tr.fairness_gini    = gini;
                tr.fairness_jain    = std::numeric_limits<double>::quiet_NaN();
                tr.fairness_maxmin  = std::numeric_limits<double>::quiet_NaN();
                tr.fairness_cv      = std::numeric_limits<double>::quiet_NaN();
                tr.cpu_migrations   = os_ctr.cpu_migrations;
                tr.ctx_switches     = os_ctr.ctx_switches;
                tr.notes            = "p999=" + std::to_string((uint64_t)p999)
                                    + ";hist_clip=p999;hist_overflow=" + std::to_string(clipped_overflow)
                                    + ";gini=" + std::to_string(gini)
                                    + ";os_perf=" + std::string(pg.available ? "on" : "off")
                                    + ";hist=" + hist_str;
                csv.write(tr);

                printf("  %-14s  t=%2d  rep=%2d  %10.1f  %10.1f  %10.1f  %10.1f  %10.1f  %10.1f\n",
                       pname, n_threads, rep,
                       res.mean_ns, res.p50_ns, res.p95_ns, res.p99_ns,
                       res.stddev_ns, res.cv_pct);
            }
        }
    }
}
