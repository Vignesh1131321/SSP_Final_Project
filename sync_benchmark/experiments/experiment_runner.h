/**
 * experiments/experiment_runner.h
 *
 * Generic thread-driver that:
 *   1. Spawns N threads (optionally pinned)
 *   2. Synchronises them with a RunBarrier
 *   3. Each thread loops until a stop-flag is set (duration_sec)
 *   4. Records per-operation latency samples (reservoir sampling)
 *   5. Aggregates via WelfordAccumulator
 *   6. Returns TrialResult
 */
#pragma once
#include "../benchmark.h"
#include "../utils/stats.h"
#include "../utils/timer.h"
#include "../utils/cpu_affinity.h"
#include "../utils/csv_writer.h"

#include <thread>
#include <atomic>
#include <vector>
#include <functional>
#include <cstring>
#include <random>
#include <algorithm>
#include <iostream>

/* Max latency samples per thread (reservoir) */
static constexpr size_t MAX_SAMPLES = 50000;

struct ThreadWork {
    /* in  */ std::function<void()> body;  // per-iteration work (lock + cs)
    /* out */ uint64_t              ops    = 0;
    /* out */ std::vector<double>   latencies_ns; // sampled
    /* out */ stats::WelfordAccumulator accum;
};

struct ExperimentResult {
    uint64_t              total_ops = 0;
    double                elapsed_sec = 0;
    double                ops_per_sec = 0;
    double                mean_ns   = 0;
    double                stddev_ns = 0;
    double                p50_ns    = 0;
    double                p95_ns    = 0;
    double                p99_ns    = 0;
    double                cv_pct    = 0;
    std::vector<double>   all_latencies;  // merged, max 100k
};

/**
 * run_experiment
 *
 * @param n_threads   thread count
 * @param duration    seconds to run
 * @param warmup      warm-up seconds (discarded)
 * @param make_body   factory: given thread-index, returns a callable
 *                    void() that performs ONE lock operation + CS
 * @param pin_cpus    whether to pin threads
 */
inline ExperimentResult run_experiment(
    int n_threads,
    double duration_sec,
    double warmup_sec,
    std::function<std::function<void()>(int /*tid*/)> make_body,
    bool pin_cpus = false)
{
    std::vector<ThreadWork> workers(n_threads);
    std::atomic<bool> stop_flag{false};

    // ── warm-up barrier ──────────────────────────────────
    RunBarrier warmup_bar(n_threads);

    // ── launch threads ───────────────────────────────────
    auto affinity_map = cpu::make_affinity_map(n_threads);
    std::vector<std::thread> threads;
    threads.reserve(n_threads);

    for (int tid = 0; tid < n_threads; ++tid) {
        workers[tid].body = make_body(tid);
        workers[tid].latencies_ns.reserve(MAX_SAMPLES);

        threads.emplace_back([&, tid] {
            if (pin_cpus) cpu::pin_to_core(affinity_map[tid]);

            std::mt19937 rng(tid * 1234567891ULL);

            // ── warm-up phase ────────────────────────────
            workers[tid].body(); // ensure JIT, TLB, etc.
            warmup_bar.arrive_and_wait();

            uint64_t wend = timer::now_ns() +
                static_cast<uint64_t>(warmup_sec * 1e9);
            while (timer::now_ns() < wend)
                workers[tid].body();

            // ── measurement phase ────────────────────────
            uint64_t ops = 0;
            uint64_t mend = timer::now_ns() +
                static_cast<uint64_t>(duration_sec * 1e9);

            while (!stop_flag.load(std::memory_order_relaxed) &&
                   timer::now_ns() < mend)
            {
                uint64_t t0 = timer::now_ns();
                workers[tid].body();
                uint64_t t1 = timer::now_ns();
                double   lat = static_cast<double>(t1 - t0);

                workers[tid].accum.push(lat);
                ++ops;

                // Reservoir sampling (keep MAX_SAMPLES random samples)
                if (workers[tid].latencies_ns.size() < MAX_SAMPLES) {
                    workers[tid].latencies_ns.push_back(lat);
                } else {
                    size_t j = rng() % ops;
                    if (j < MAX_SAMPLES)
                        workers[tid].latencies_ns[j] = lat;
                }
            }
            workers[tid].ops = ops;
        });
    }

    // Wait for threads to finish
    for (auto& t : threads) t.join();

    // ── aggregate ────────────────────────────────────────
    ExperimentResult res;
    std::vector<double> all_lat;
    all_lat.reserve(n_threads * std::min((size_t)5000, MAX_SAMPLES));

    for (auto& w : workers) {
        res.total_ops += w.ops;
        for (double l : w.latencies_ns) all_lat.push_back(l);
    }

    // Merge Welford stats
    stats::WelfordAccumulator merged;
    for (auto& l : all_lat) merged.push(l);

    res.elapsed_sec = duration_sec;
    res.ops_per_sec = res.total_ops / duration_sec;
    res.mean_ns     = merged.mean();
    res.stddev_ns   = merged.stddev();
    res.cv_pct      = merged.cv();
    res.p50_ns      = stats::p50(all_lat);
    res.p95_ns      = stats::p95(all_lat);
    res.p99_ns      = stats::p99(all_lat);
    res.all_latencies = std::move(all_lat);

    return res;
}
