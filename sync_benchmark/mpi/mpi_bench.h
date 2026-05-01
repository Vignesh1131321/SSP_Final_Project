/**
 * mpi/mpi_bench.h
 * ───────────────
 * RQ8 — How do synchronization mechanisms behave in distributed memory?
 *
 * MPI primitives evaluated (RPC-style, each rank is one "thread"):
 *   • MPI_Barrier            — global synchronisation point
 *   • MPI_Reduce             — collective reduction (like omp reduction)
 *   • MPI_Win_lock / unlock  — MPI-3 RMA passive-target locking
 *   • MPI_Compare_and_swap   — MPI-3 RMA atomic CAS on a shared window
 *   • MPI_Fetch_and_op       — MPI-3 RMA fetch-and-add
 *
 * This file is compiled ONLY when USE_MPI is defined.
 * Build: mpicxx -DUSE_MPI -std=c++17 -O3 mpi/mpi_bench_main.cpp -o mpi_bench
 *
 * Usage: mpirun -np <ranks> ./mpi_bench [--duration 2] [--reps 5]
 */
#pragma once

#ifdef USE_MPI
#include <mpi.h>
#endif

#include "../benchmark.h"
#include "../utils/csv_writer.h"
#include "../utils/timer.h"
#include "../utils/stats.h"
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>

#ifdef USE_MPI

/* ── helper: measure one MPI collective ─────────────────── */
struct MpiResult {
    double ops_per_sec  = 0;
    double mean_ns      = 0;
    double p95_ns       = 0;
    double p99_ns       = 0;
    double stddev_ns    = 0;
    uint64_t total_ops  = 0;
};

/* ─── MPI_Barrier ─────────────────────────────────────────── */
static MpiResult bench_mpi_barrier(int rank, double dur_sec) {
    std::vector<double> lats;
    lats.reserve(50000);

    MPI_Barrier(MPI_COMM_WORLD);  // warm-up
    uint64_t mend = timer::now_ns() + static_cast<uint64_t>(dur_sec * 1e9);
    uint64_t cnt = 0;

    while (timer::now_ns() < mend) {
        uint64_t t0 = timer::now_ns();
        MPI_Barrier(MPI_COMM_WORLD);
        uint64_t t1 = timer::now_ns();
        if (lats.size() < 50000)
            lats.push_back(static_cast<double>(t1 - t0));
        ++cnt;
    }

    MpiResult r;
    r.total_ops   = cnt;
    r.ops_per_sec = cnt / dur_sec;
    if (!lats.empty()) {
        stats::WelfordAccumulator acc;
        for (double x : lats) acc.push(x);
        r.mean_ns   = acc.mean();
        r.stddev_ns = acc.stddev();
        r.p95_ns    = stats::p95(lats);
        r.p99_ns    = stats::p99(lats);
    }
    return r;
}

/* ─── MPI_Reduce ──────────────────────────────────────────── */
static MpiResult bench_mpi_reduce(int rank, double dur_sec) {
    std::vector<double> lats;
    lats.reserve(50000);
    long long send_val = 1, recv_val = 0;

    MPI_Reduce(&send_val, &recv_val, 1, MPI_LONG_LONG, MPI_SUM, 0,
               MPI_COMM_WORLD);   // warm-up
    uint64_t mend = timer::now_ns() + static_cast<uint64_t>(dur_sec * 1e9);
    uint64_t cnt = 0;

    while (timer::now_ns() < mend) {
        uint64_t t0 = timer::now_ns();
        MPI_Reduce(&send_val, &recv_val, 1, MPI_LONG_LONG, MPI_SUM, 0,
                   MPI_COMM_WORLD);
        uint64_t t1 = timer::now_ns();
        if (lats.size() < 50000)
            lats.push_back(static_cast<double>(t1 - t0));
        ++cnt;
    }

    MpiResult r;
    r.total_ops   = cnt;
    r.ops_per_sec = cnt / dur_sec;
    if (!lats.empty()) {
        stats::WelfordAccumulator acc;
        for (double x : lats) acc.push(x);
        r.mean_ns   = acc.mean();
        r.stddev_ns = acc.stddev();
        r.p95_ns    = stats::p95(lats);
        r.p99_ns    = stats::p99(lats);
    }
    return r;
}

/* ─── MPI_Win_lock / unlock (passive-target RMA locking) ──── */
static MpiResult bench_mpi_win_lock(int rank, int nranks, double dur_sec) {
    std::vector<double> lats;
    lats.reserve(50000);

    long long shared_mem = 0;
    MPI_Win win;
    MPI_Win_create(&shared_mem, sizeof(long long), sizeof(long long),
                   MPI_INFO_NULL, MPI_COMM_WORLD, &win);

    // All ranks take exclusive lock on rank-0's window
    for (int i = 0; i < 100; ++i) {
        MPI_Win_lock(MPI_LOCK_EXCLUSIVE, 0, 0, win);
        MPI_Win_unlock(0, win);
    }

    uint64_t mend = timer::now_ns() + static_cast<uint64_t>(dur_sec * 1e9);
    uint64_t cnt = 0;

    while (timer::now_ns() < mend) {
        uint64_t t0 = timer::now_ns();
        MPI_Win_lock(MPI_LOCK_EXCLUSIVE, 0, 0, win);
        long long one = 1;
        MPI_Accumulate(&one, 1, MPI_LONG_LONG, 0, 0, 1, MPI_LONG_LONG,
                       MPI_SUM, win);
        MPI_Win_unlock(0, win);
        uint64_t t1 = timer::now_ns();
        if (lats.size() < 50000)
            lats.push_back(static_cast<double>(t1 - t0));
        ++cnt;
    }

    MPI_Win_free(&win);

    MpiResult r;
    r.total_ops   = cnt;
    r.ops_per_sec = cnt / dur_sec;
    if (!lats.empty()) {
        stats::WelfordAccumulator acc;
        for (double x : lats) acc.push(x);
        r.mean_ns   = acc.mean();
        r.stddev_ns = acc.stddev();
        r.p95_ns    = stats::p95(lats);
        r.p99_ns    = stats::p99(lats);
    }
    return r;
}

/* ─── MPI_Compare_and_swap ────────────────────────────────── */
static MpiResult bench_mpi_cas(int rank, int nranks, double dur_sec) {
    std::vector<double> lats;
    lats.reserve(50000);

    long long window_val = 0, result = 0;
    MPI_Win win;
    MPI_Win_create(&window_val, sizeof(long long), sizeof(long long),
                   MPI_INFO_NULL, MPI_COMM_WORLD, &win);

    uint64_t mend = timer::now_ns() + static_cast<uint64_t>(dur_sec * 1e9);
    uint64_t cnt = 0;
    long long compare_val = 0, replace_val = 1;

    while (timer::now_ns() < mend) {
        MPI_Win_lock(MPI_LOCK_EXCLUSIVE, 0, 0, win);
        uint64_t t0 = timer::now_ns();
        MPI_Compare_and_swap(&replace_val, &compare_val, &result,
                             MPI_LONG_LONG, 0, 0, win);
        MPI_Win_flush(0, win);
        uint64_t t1 = timer::now_ns();
        MPI_Win_unlock(0, win);

        if (lats.size() < 50000)
            lats.push_back(static_cast<double>(t1 - t0));
        ++cnt;
    }

    MPI_Win_free(&win);

    MpiResult r;
    r.total_ops   = cnt;
    r.ops_per_sec = cnt / dur_sec;
    if (!lats.empty()) {
        stats::WelfordAccumulator acc;
        for (double x : lats) acc.push(x);
        r.mean_ns   = acc.mean();
        r.stddev_ns = acc.stddev();
        r.p95_ns    = stats::p95(lats);
        r.p99_ns    = stats::p99(lats);
    }
    return r;
}

/* ─── MPI_Fetch_and_op ────────────────────────────────────── */
static MpiResult bench_mpi_fetch_and_op(int rank, int nranks,
                                         double dur_sec) {
    std::vector<double> lats;
    lats.reserve(50000);

    long long window_val = 0, result = 0, one = 1;
    MPI_Win win;
    MPI_Win_create(&window_val, sizeof(long long), sizeof(long long),
                   MPI_INFO_NULL, MPI_COMM_WORLD, &win);

    uint64_t mend = timer::now_ns() + static_cast<uint64_t>(dur_sec * 1e9);
    uint64_t cnt = 0;

    while (timer::now_ns() < mend) {
        MPI_Win_lock(MPI_LOCK_EXCLUSIVE, 0, 0, win);
        uint64_t t0 = timer::now_ns();
        MPI_Fetch_and_op(&one, &result, MPI_LONG_LONG, 0, 0, MPI_SUM, win);
        MPI_Win_flush(0, win);
        uint64_t t1 = timer::now_ns();
        MPI_Win_unlock(0, win);

        if (lats.size() < 50000)
            lats.push_back(static_cast<double>(t1 - t0));
        ++cnt;
    }

    MPI_Win_free(&win);

    MpiResult r;
    r.total_ops   = cnt;
    r.ops_per_sec = cnt / dur_sec;
    if (!lats.empty()) {
        stats::WelfordAccumulator acc;
        for (double x : lats) acc.push(x);
        r.mean_ns   = acc.mean();
        r.stddev_ns = acc.stddev();
        r.p95_ns    = stats::p95(lats);
        r.p99_ns    = stats::p99(lats);
    }
    return r;
}

/* ── Public entry point ─────────────────────────────────── */
inline void run_mpi_experiment(const BenchmarkConfig& cfg, CsvWriter& csv) {
    int rank = 0, nranks = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nranks);

    struct MpiTest {
        const char* name;
        std::function<MpiResult()> fn;
    };

    std::vector<MpiTest> tests = {
        {"mpi_barrier",
         [&]{ return bench_mpi_barrier(rank, cfg.duration_sec); }},
        {"mpi_reduce",
         [&]{ return bench_mpi_reduce(rank, cfg.duration_sec); }},
        {"mpi_win_lock",
         [&]{ return bench_mpi_win_lock(rank, nranks, cfg.duration_sec); }},
        {"mpi_cas",
         [&]{ return bench_mpi_cas(rank, nranks, cfg.duration_sec); }},
        {"mpi_fetch_and_op",
         [&]{ return bench_mpi_fetch_and_op(rank, nranks, cfg.duration_sec);}},
    };

    if (rank == 0) {
        printf("  %-20s  %6s  %14s  %10s  %10s\n",
               "MPI Primitive", "Ranks", "Ops/sec", "Mean(ns)", "P99(ns)");
        printf("  %s\n", std::string(64, '-').c_str());
    }

    for (auto& test : tests) {
        for (int rep = 0; rep < cfg.repetitions; ++rep) {
            MPI_Barrier(MPI_COMM_WORLD);
            auto r = test.fn();

            // Gather max latency across all ranks to root
            double global_mean = 0;
            MPI_Reduce(&r.mean_ns, &global_mean, 1, MPI_DOUBLE, MPI_MAX, 0,
                       MPI_COMM_WORLD);

            if (rank == 0) {
                TrialResult tr;
                tr.suite           = "mpi";
                tr.experiment      = "mpi_primitives";
                tr.primitive       = test.name;
                tr.threads         = nranks;
                tr.cs_cycles       = 0;
                tr.trial           = rep;
                tr.ops_per_sec     = r.ops_per_sec;
                tr.mean_latency_ns = global_mean;
                tr.p95_ns          = r.p95_ns;
                tr.p99_ns          = r.p99_ns;
                tr.stddev_ns       = r.stddev_ns;
                tr.throughput_ops  = r.total_ops;
                tr.notes           = "mpi_distributed";
                csv.write(tr);

                printf("  %-20s  %6d  %14.0f  %10.1f  %10.1f\n",
                       test.name, nranks,
                       r.ops_per_sec, global_mean, r.p99_ns);
            }
        }
        if (rank == 0) printf("\n");
    }
}

#else  /* USE_MPI not defined ─ stub so the header compiles cleanly */

inline void run_mpi_experiment(const BenchmarkConfig&, CsvWriter&) {
    printf("  [MPI] Not compiled with USE_MPI. "
           "Build with: mpicxx -DUSE_MPI ...\n");
}

#endif /* USE_MPI */
