/**
 * workloads/graph_workload.h
 * ──────────────────────────
 * RQ — Real-world workload: Parallel Graph Processing
 *
 * Algorithms:
 *   1. Parallel BFS (Breadth-First Search)
 *      — frontier expansion protected by mutex / atomic CAS
 *   2. Parallel PageRank (iterative)
 *      — rank accumulation with mutex / atomic / reduction strategies
 *
 * Graph model: random Erdős–Rényi G(n, p) generated in-process.
 * N=100,000 nodes, avg degree ≈ 10.
 */
#pragma once
#include "../benchmark.h"
#include "../utils/csv_writer.h"
#include "../utils/timer.h"
#include "../utils/stats.h"

#include <vector>
#include <queue>
#include <mutex>
#include <atomic>
#include <thread>
#include <random>
#include <functional>
#include <cstdio>
#include <string>
#include <cmath>
#include <algorithm>
#include <numeric>

/* ── Graph generation ──────────────────────────────────────── */
struct Graph {
    int n;
    std::vector<std::vector<int>> adj;

    static Graph random_graph(int n, int avg_degree, uint64_t seed = 42) {
        Graph g;
        g.n = n;
        g.adj.resize(n);
        std::mt19937_64 rng(seed);
        double p = static_cast<double>(avg_degree) / n;
        for (int u = 0; u < n; ++u) {
            for (int v = u + 1; v < n; ++v) {
                if (static_cast<double>(rng()) / rng.max() < p) {
                    g.adj[u].push_back(v);
                    g.adj[v].push_back(u);
                }
            }
        }
        return g;
    }
};

/* ══════════════════════════════════════════════════════════
 * BFS — mutex-protected frontier
 * ══════════════════════════════════════════════════════════ */
static double bfs_mutex(const Graph& g, int n_threads) {
    std::vector<int>    dist(g.n, -1);
    std::vector<int>    frontier, next_frontier;
    std::mutex          mutex_frontier;

    dist[0] = 0;
    frontier.push_back(0);

    uint64_t t0 = timer::now_ns();
    while (!frontier.empty()) {
        next_frontier.clear();
        std::mutex mut;

        std::vector<std::thread> threads;
        int chunk = std::max(1, static_cast<int>(frontier.size()) / n_threads);

        for (int t = 0; t < n_threads; ++t) {
            int lo = t * chunk;
            int hi = (t == n_threads - 1) ?
                     static_cast<int>(frontier.size()) :
                     std::min(lo + chunk, static_cast<int>(frontier.size()));
            if (lo >= hi) continue;

            threads.emplace_back([&, lo, hi]{
                std::vector<int> local_next;
                for (int i = lo; i < hi; ++i) {
                    int u = frontier[i];
                    for (int v : g.adj[u]) {
                        if (dist[v] == -1) {
                            // CAS-like check before lock
                            if (dist[v] == -1) {
                                local_next.push_back(v);
                            }
                        }
                    }
                }
                std::lock_guard<std::mutex> lk(mut);
                for (int v : local_next) {
                    if (dist[v] == -1) {
                        dist[v] = dist[frontier[lo]] + 1;
                        next_frontier.push_back(v);
                    }
                }
            });
        }
        for (auto& t : threads) t.join();
        frontier.swap(next_frontier);
    }
    uint64_t t1 = timer::now_ns();
    return static_cast<double>(t1 - t0) / 1e6;  // ms
}

/* ── BFS with atomic visited array ────────────────────────── */
static double bfs_atomic(const Graph& g, int n_threads) {
    std::vector<std::atomic<int>> dist(g.n);
    for (auto& d : dist) d.store(-1, std::memory_order_relaxed);
    dist[0].store(0, std::memory_order_relaxed);

    std::vector<int> frontier = {0};
    std::vector<int> next_frontier;

    uint64_t t0 = timer::now_ns();
    while (!frontier.empty()) {
        next_frontier.clear();
        std::mutex merge_mu;

        int chunk = std::max(1, static_cast<int>(frontier.size()) / n_threads);
        std::vector<std::thread> threads;

        for (int t = 0; t < n_threads; ++t) {
            int lo = t * chunk;
            int hi = (t == n_threads - 1) ?
                     static_cast<int>(frontier.size()) :
                     std::min(lo + chunk, static_cast<int>(frontier.size()));
            if (lo >= hi) continue;

            threads.emplace_back([&, lo, hi]{
                std::vector<int> local_next;
                int cur_dist = dist[frontier[lo]].load(std::memory_order_relaxed);
                for (int i = lo; i < hi; ++i) {
                    int u = frontier[i];
                    for (int v : g.adj[u]) {
                        int expected = -1;
                        if (dist[v].compare_exchange_strong(
                                expected, cur_dist + 1,
                                std::memory_order_relaxed))
                            local_next.push_back(v);
                    }
                }
                std::lock_guard<std::mutex> lk(merge_mu);
                next_frontier.insert(next_frontier.end(),
                                     local_next.begin(), local_next.end());
            });
        }
        for (auto& t : threads) t.join();
        frontier.swap(next_frontier);
    }
    uint64_t t1 = timer::now_ns();
    return static_cast<double>(t1 - t0) / 1e6;  // ms
}

/* ══════════════════════════════════════════════════════════
 * PageRank — parallel iterative
 * ══════════════════════════════════════════════════════════ */
static double pagerank_mutex(const Graph& g, int n_threads,
                              int max_iters = 10) {
    const double DAMP = 0.85;
    int n = g.n;
    std::vector<double> rank(n, 1.0 / n);
    std::vector<double> new_rank(n, 0.0);

    uint64_t t0 = timer::now_ns();
    for (int iter = 0; iter < max_iters; ++iter) {
        std::fill(new_rank.begin(), new_rank.end(), (1.0 - DAMP) / n);
        std::mutex mu;
        int chunk = std::max(1, n / n_threads);

        std::vector<std::thread> threads;
        for (int t = 0; t < n_threads; ++t) {
            int lo = t * chunk;
            int hi = (t == n_threads - 1) ? n :
                     std::min(lo + chunk, n);

            threads.emplace_back([&, lo, hi]{
                for (int u = lo; u < hi; ++u) {
                    if (g.adj[u].empty()) continue;
                    double contrib = DAMP * rank[u] / g.adj[u].size();
                    for (int v : g.adj[u]) {
                        std::lock_guard<std::mutex> lk(mu);
                        new_rank[v] += contrib;
                    }
                }
            });
        }
        for (auto& t : threads) t.join();
        rank.swap(new_rank);
    }
    uint64_t t1 = timer::now_ns();
    return static_cast<double>(t1 - t0) / 1e6;  // ms
}

static double pagerank_atomic(const Graph& g, int n_threads,
                               int max_iters = 10) {
    /* Use integer fixed-point (×10^6) to enable fetch_add */
    const double DAMP = 0.85;
    int n = g.n;
    static constexpr int64_t SCALE = 1000000LL;

    std::vector<std::atomic<int64_t>> new_rank(n);
    std::vector<int64_t> rank(n, SCALE / n);

    uint64_t t0 = timer::now_ns();
    for (int iter = 0; iter < max_iters; ++iter) {
        for (auto& r : new_rank)
            r.store(static_cast<int64_t>((1.0 - DAMP) / n * SCALE),
                    std::memory_order_relaxed);

        int chunk = std::max(1, n / n_threads);
        std::vector<std::thread> threads;
        for (int t = 0; t < n_threads; ++t) {
            int lo = t * chunk;
            int hi = (t == n_threads - 1) ? n : std::min(lo + chunk, n);
            threads.emplace_back([&, lo, hi]{
                for (int u = lo; u < hi; ++u) {
                    if (g.adj[u].empty()) continue;
                    int64_t contrib = static_cast<int64_t>(
                        DAMP * rank[u] / g.adj[u].size());
                    for (int v : g.adj[u])
                        new_rank[v].fetch_add(contrib, std::memory_order_relaxed);
                }
            });
        }
        for (auto& t : threads) t.join();
        for (int i = 0; i < n; ++i)
            rank[i] = new_rank[i].load(std::memory_order_relaxed);
    }
    uint64_t t1 = timer::now_ns();
    return static_cast<double>(t1 - t0) / 1e6;  // ms
}

/* ── Public entry point ─────────────────────────────────── */
inline void run_graph_experiment(const BenchmarkConfig& cfg, CsvWriter& csv) {
    const int N = 50000, AVG_DEG = 10;
    printf("  Generating random graph (n=%d, avg_deg=%d)...\n", N, AVG_DEG);
    auto g = Graph::random_graph(N, AVG_DEG);
    printf("  Graph ready.\n\n");

    printf("  %-24s  %6s  %12s\n", "Algorithm", "Thr", "Time(ms)");
    printf("  %s\n", std::string(46, '-').c_str());

    for (int n : cfg.thread_counts) {
        // BFS mutex
        for (int rep = 0; rep < cfg.repetitions; ++rep) {
            double ms = bfs_mutex(g, n);
            TrialResult tr;
            tr.suite = "workload"; tr.experiment = "graph_bfs";
            tr.primitive = "bfs_mutex"; tr.threads = n; tr.trial = rep;
            tr.mean_latency_ns = ms * 1e6;  // convert ms → ns
            tr.notes = "graph_bfs";
            csv.write(tr);
            if (rep == 0)
                printf("  %-24s  %6d  %12.2f\n", "bfs_mutex", n, ms);
        }
        // BFS atomic
        for (int rep = 0; rep < cfg.repetitions; ++rep) {
            double ms = bfs_atomic(g, n);
            TrialResult tr;
            tr.suite = "workload"; tr.experiment = "graph_bfs";
            tr.primitive = "bfs_atomic"; tr.threads = n; tr.trial = rep;
            tr.mean_latency_ns = ms * 1e6;
            tr.notes = "graph_bfs";
            csv.write(tr);
            if (rep == 0)
                printf("  %-24s  %6d  %12.2f\n", "bfs_atomic", n, ms);
        }
        // PageRank mutex
        for (int rep = 0; rep < cfg.repetitions; ++rep) {
            double ms = pagerank_mutex(g, n);
            TrialResult tr;
            tr.suite = "workload"; tr.experiment = "graph_pagerank";
            tr.primitive = "pagerank_mutex"; tr.threads = n; tr.trial = rep;
            tr.mean_latency_ns = ms * 1e6;
            tr.notes = "graph_pagerank";
            csv.write(tr);
            if (rep == 0)
                printf("  %-24s  %6d  %12.2f\n", "pagerank_mutex", n, ms);
        }
        // PageRank atomic fetch_add
        for (int rep = 0; rep < cfg.repetitions; ++rep) {
            double ms = pagerank_atomic(g, n);
            TrialResult tr;
            tr.suite = "workload"; tr.experiment = "graph_pagerank";
            tr.primitive = "pagerank_atomic"; tr.threads = n; tr.trial = rep;
            tr.mean_latency_ns = ms * 1e6;
            tr.notes = "graph_pagerank";
            csv.write(tr);
            if (rep == 0)
                printf("  %-24s  %6d  %12.2f\n", "pagerank_atomic", n, ms);
        }
        printf("\n");
    }
}
