/**
 * openmp/openmp_bench.h
 * ─────────────────────
 * RQ7 — How do compiler-managed parallel frameworks (OpenMP) compare
 *        with manual locking mechanisms?
 *
 * Constructs evaluated:
 *   • #pragma omp critical       — implicit global mutex
 *   • #pragma omp atomic         — single-instruction atomic update
 *   • #pragma omp reduction      — compiler-generated tree reduction
 *   • #pragma omp barrier        — all-thread synchronisation point
 *
 * Each is run across the same thread-count sweep used for manual locks
 * so results are directly comparable on the same CSV.
 *
 * Build requirement:
 *   -fopenmp  (GCC/Clang)   or   /openmp  (MSVC)
 */
#pragma once
#include "../benchmark.h"
#include "../utils/csv_writer.h"
#include "../utils/timer.h"
#include "../utils/stats.h"

#include <omp.h>
#include <vector>
#include <cstdio>
#include <cmath>
#include <string>
#include <algorithm>
#include <numeric>

/* ── internal helper: run one OpenMP trial ───────────────── */
struct OmpResult {
    double ops_per_sec    = 0;
    double mean_ns        = 0;
    double p95_ns         = 0;
    double p99_ns         = 0;
    double stddev_ns      = 0;
    uint64_t total_ops    = 0;
};

/* --- omp critical ----------------------------------------- */
static OmpResult bench_omp_critical(int n_threads, double dur_sec,
                                     double warmup_sec) {
    omp_set_num_threads(n_threads);
    volatile long long counter = 0;

    std::vector<double> lats;
    lats.reserve(static_cast<size_t>(n_threads) * 20000);
    std::vector<std::vector<double>> per_thread(n_threads);

    // warm-up
    #pragma omp parallel
    {
        for (int i = 0; i < 10000; ++i) {
            #pragma omp critical
            { ++counter; }
        }
    }
    counter = 0;

    uint64_t mend = timer::now_ns() + static_cast<uint64_t>(dur_sec * 1e9);

    #pragma omp parallel shared(counter, per_thread, mend)
    {
        int tid = omp_get_thread_num();
        std::vector<double>& my_lats = per_thread[tid];
        my_lats.reserve(20000);

        while (timer::now_ns() < mend) {
            uint64_t t0 = timer::now_ns();
            #pragma omp critical
            { ++counter; }
            uint64_t t1 = timer::now_ns();
            if (my_lats.size() < 20000)
                my_lats.push_back(static_cast<double>(t1 - t0));
        }
    }

    OmpResult r;
    r.total_ops   = static_cast<uint64_t>(counter);
    r.ops_per_sec = r.total_ops / dur_sec;

    std::vector<double> all;
    for (auto& v : per_thread) for (double x : v) all.push_back(x);
    if (!all.empty()) {
        stats::WelfordAccumulator acc;
        for (double x : all) acc.push(x);
        r.mean_ns   = acc.mean();
        r.stddev_ns = acc.stddev();
        r.p95_ns    = stats::p95(all);
        r.p99_ns    = stats::p99(all);
    }
    return r;
}

/* --- omp atomic ------------------------------------------- */
static OmpResult bench_omp_atomic(int n_threads, double dur_sec,
                                   double warmup_sec) {
    omp_set_num_threads(n_threads);
    volatile long long counter = 0;
    long long atomic_counter = 0;

    std::vector<std::vector<double>> per_thread(n_threads);

    // warm-up
    #pragma omp parallel
    {
        for (int i = 0; i < 10000; ++i) {
            #pragma omp atomic
            atomic_counter++;
        }
    }
    atomic_counter = 0;

    uint64_t mend = timer::now_ns() + static_cast<uint64_t>(dur_sec * 1e9);

    #pragma omp parallel shared(atomic_counter, per_thread, mend)
    {
        int tid = omp_get_thread_num();
        std::vector<double>& my_lats = per_thread[tid];
        my_lats.reserve(20000);

        while (timer::now_ns() < mend) {
            uint64_t t0 = timer::now_ns();
            #pragma omp atomic
            atomic_counter++;
            uint64_t t1 = timer::now_ns();
            if (my_lats.size() < 20000)
                my_lats.push_back(static_cast<double>(t1 - t0));
        }
    }

    OmpResult r;
    r.total_ops   = static_cast<uint64_t>(atomic_counter);
    r.ops_per_sec = r.total_ops / dur_sec;

    std::vector<double> all;
    for (auto& v : per_thread) for (double x : v) all.push_back(x);
    if (!all.empty()) {
        stats::WelfordAccumulator acc;
        for (double x : all) acc.push(x);
        r.mean_ns   = acc.mean();
        r.stddev_ns = acc.stddev();
        r.p95_ns    = stats::p95(all);
        r.p99_ns    = stats::p99(all);
    }
    return r;
}

/* --- omp reduction ---------------------------------------- */
static OmpResult bench_omp_reduction(int n_threads, double dur_sec,
                                      double warmup_sec) {
    omp_set_num_threads(n_threads);

    /* Reduction is a bulk operation — we measure the time per
     * full reduction call (all threads contribute, then merge). */
    const int OPS_PER_ITER = 1024;  // work per thread per reduction
    std::vector<double> trial_lats;
    trial_lats.reserve(5000);

    uint64_t mend = timer::now_ns() + static_cast<uint64_t>(dur_sec * 1e9);
    uint64_t iters = 0;

    while (timer::now_ns() < mend) {
        long long sum = 0;
        uint64_t t0 = timer::now_ns();
        #pragma omp parallel for reduction(+:sum) schedule(static)
        for (int i = 0; i < n_threads * OPS_PER_ITER; ++i) {
            sum += (i & 1) ? 1 : -1;
        }
        uint64_t t1 = timer::now_ns();
        (void)sum;
        if (trial_lats.size() < 5000)
            trial_lats.push_back(static_cast<double>(t1 - t0));
        ++iters;
    }

    OmpResult r;
    r.total_ops   = iters * static_cast<uint64_t>(n_threads) * OPS_PER_ITER;
    r.ops_per_sec = r.total_ops / dur_sec;

    if (!trial_lats.empty()) {
        stats::WelfordAccumulator acc;
        for (double x : trial_lats) acc.push(x);
        r.mean_ns   = acc.mean();
        r.stddev_ns = acc.stddev();
        r.p95_ns    = stats::p95(trial_lats);
        r.p99_ns    = stats::p99(trial_lats);
    }
    return r;
}

/* --- omp barrier ------------------------------------------ */
static OmpResult bench_omp_barrier(int n_threads, double dur_sec,
                                    double warmup_sec) {
    omp_set_num_threads(n_threads);

    std::vector<std::vector<double>> per_thread(n_threads);
    std::atomic<bool> stop{false};

    uint64_t mend = timer::now_ns() + static_cast<uint64_t>(dur_sec * 1e9);

    #pragma omp parallel shared(per_thread, mend, stop)
    {
        int tid = omp_get_thread_num();
        std::vector<double>& my_lats = per_thread[tid];
        my_lats.reserve(10000);

        while (!stop.load(std::memory_order_relaxed) &&
               timer::now_ns() < mend) {
            uint64_t t0 = timer::now_ns();
            #pragma omp barrier
            uint64_t t1 = timer::now_ns();
            if (my_lats.size() < 10000)
                my_lats.push_back(static_cast<double>(t1 - t0));
        }
    }

    OmpResult r;
    uint64_t total = 0;
    std::vector<double> all;
    for (auto& v : per_thread) {
        total += v.size();
        for (double x : v) all.push_back(x);
    }
    r.total_ops   = total;
    r.ops_per_sec = total / dur_sec;

    if (!all.empty()) {
        stats::WelfordAccumulator acc;
        for (double x : all) acc.push(x);
        r.mean_ns   = acc.mean();
        r.stddev_ns = acc.stddev();
        r.p95_ns    = stats::p95(all);
        r.p99_ns    = stats::p99(all);
    }
    return r;
}

/* ── Public entry point ─────────────────────────────────── */
inline void run_openmp_experiment(const BenchmarkConfig& cfg, CsvWriter& csv) {

    struct OmpTest {
        const char* name;
        std::function<OmpResult(int,double,double)> fn;
    };

    std::vector<OmpTest> tests = {
        {"omp_critical",  bench_omp_critical },
        {"omp_atomic",    bench_omp_atomic   },
        {"omp_reduction", bench_omp_reduction},
        {"omp_barrier",   bench_omp_barrier  },
    };

    printf("  %-16s  %6s  %14s  %10s  %10s  %10s\n",
           "Construct", "Thr", "Ops/sec", "Mean(ns)", "P95(ns)", "P99(ns)");
    printf("  %s\n", std::string(66, '-').c_str());

    for (auto& test : tests) {
        for (int n : cfg.thread_counts) {
            if (n > omp_get_max_threads()) continue;

            double sum_ops = 0, sum_mean = 0, sum_p95 = 0, sum_p99 = 0;

            for (int rep = 0; rep < cfg.repetitions; ++rep) {
                auto r = test.fn(n, cfg.duration_sec, cfg.warmup_sec);

                TrialResult tr;
                tr.suite           = "openmp";
                tr.experiment      = "openmp_constructs";
                tr.primitive       = test.name;
                tr.threads         = n;
                tr.cs_cycles       = 0;
                tr.trial           = rep;
                tr.ops_per_sec     = r.ops_per_sec;
                tr.mean_latency_ns = r.mean_ns;
                tr.p95_ns          = r.p95_ns;
                tr.p99_ns          = r.p99_ns;
                tr.stddev_ns       = r.stddev_ns;
                tr.throughput_ops  = r.total_ops;
                tr.notes           = "openmp_framework";
                csv.write(tr);

                sum_ops  += r.ops_per_sec;
                sum_mean += r.mean_ns;
                sum_p95  += r.p95_ns;
                sum_p99  += r.p99_ns;
            }
            int R = cfg.repetitions;
            printf("  %-16s  %6d  %14.0f  %10.1f  %10.1f  %10.1f\n",
                   test.name, n,
                   sum_ops/R, sum_mean/R, sum_p95/R, sum_p99/R);
        }
        printf("\n");
    }
}
