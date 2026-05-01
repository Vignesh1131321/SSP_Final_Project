/**
 * workloads/concurrent_hash_table.h
 * Scenario-driven benchmark for a concurrent key-value store.
 *
 * The benchmark evaluates three implementations under six access patterns:
 *   1) balanced_uniform
 *   2) read_dominant
 *   3) write_dominant
 *   4) churn_delete_heavy
 *   5) bursty_rw
 *   6) hotspot_contention
 */
#pragma once

#include "../benchmark.h"
#include "../utils/cpu_affinity.h"
#include "../utils/csv_writer.h"
#include "../utils/stats.h"
#include "../utils/timer.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class CoarseHashTable {
    std::unordered_map<uint64_t, uint64_t> map_;
    std::mutex mu_;
public:
    void insert(uint64_t k, uint64_t v) {
        std::lock_guard<std::mutex> g(mu_);
        map_[k] = v;
    }

    bool lookup(uint64_t k, uint64_t& v) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = map_.find(k);
        if (it == map_.end()) return false;
        v = it->second;
        return true;
    }

    bool erase(uint64_t k) {
        std::lock_guard<std::mutex> g(mu_);
        return map_.erase(k) > 0;
    }
};

static constexpr int SHARD_COUNT = 64;

class FineHashTable {
    struct alignas(CACHE_LINE_SIZE) Shard {
        std::unordered_map<uint64_t, uint64_t> map;
        std::shared_mutex rw;
    };

    std::vector<Shard> shards_{SHARD_COUNT};

    Shard& shard_for(uint64_t k) {
        return shards_[k % SHARD_COUNT];
    }

public:
    void insert(uint64_t k, uint64_t v) {
        auto& s = shard_for(k);
        std::unique_lock<std::shared_mutex> g(s.rw);
        s.map[k] = v;
    }

    bool lookup(uint64_t k, uint64_t& v) {
        auto& s = shard_for(k);
        std::shared_lock<std::shared_mutex> g(s.rw);
        auto it = s.map.find(k);
        if (it == s.map.end()) return false;
        v = it->second;
        return true;
    }

    bool erase(uint64_t k) {
        auto& s = shard_for(k);
        std::unique_lock<std::shared_mutex> g(s.rw);
        return s.map.erase(k) > 0;
    }
};

class LockFreeHashTable {
    static constexpr uint64_t EMPTY = 0xFFFFFFFFFFFFFFFFULL;
    static constexpr uint64_t DELETED = 0xFFFFFFFFFFFFFFFEULL;
    static constexpr size_t CAPACITY = 1 << 20;

    struct Slot {
        std::atomic<uint64_t> key{EMPTY};
        std::atomic<uint64_t> val{0};
    };

    std::vector<Slot> table_{CAPACITY};

    size_t probe(uint64_t k) const {
        return (k * 11400714819323198485ULL) % CAPACITY;
    }

public:
    bool insert(uint64_t k, uint64_t v) {
        size_t h = probe(k);
        for (size_t i = 0; i < CAPACITY; ++i) {
            size_t idx = (h + i) % CAPACITY;
            uint64_t cur = table_[idx].key.load(std::memory_order_relaxed);
            if (cur == k) {
                table_[idx].val.store(v, std::memory_order_release);
                return true;
            }

            if (cur == EMPTY || cur == DELETED) {
                uint64_t expected = cur;
                if (table_[idx].key.compare_exchange_weak(
                        expected, k, std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    table_[idx].val.store(v, std::memory_order_release);
                    return true;
                }
            }
        }
        return false;
    }

    bool lookup(uint64_t k, uint64_t& v) const {
        size_t h = probe(k);
        for (size_t i = 0; i < CAPACITY; ++i) {
            size_t idx = (h + i) % CAPACITY;
            uint64_t ek = table_[idx].key.load(std::memory_order_acquire);
            if (ek == EMPTY) return false;
            if (ek == k) {
                v = table_[idx].val.load(std::memory_order_acquire);
                return true;
            }
        }
        return false;
    }

    bool erase(uint64_t k) {
        size_t h = probe(k);
        for (size_t i = 0; i < CAPACITY; ++i) {
            size_t idx = (h + i) % CAPACITY;
            uint64_t ek = table_[idx].key.load(std::memory_order_acquire);
            if (ek == EMPTY) return false;
            if (ek == k) {
                table_[idx].key.store(DELETED, std::memory_order_release);
                return true;
            }
        }
        return false;
    }
};

struct HashScenario {
    const char* name;
    int lookup_pct;
    int insert_pct;
    int erase_pct;
    uint64_t keyspace;
    uint64_t hotset;
    bool bursty;
};

static const HashScenario HASH_SCENARIOS[] = {
    {"balanced_uniform", 50, 35, 15, 1u << 17, 0, false},
    {"read_dominant", 85, 10, 5, 1u << 17, 0, false},
    {"write_dominant", 20, 65, 15, 1u << 17, 0, false},
    {"churn_delete_heavy", 30, 20, 50, 1u << 17, 0, false},
    {"bursty_rw", 50, 35, 15, 1u << 17, 0, true},
    {"hotspot_contention", 50, 35, 15, 1u << 17, 256, false},
};

template <typename HT>
static void seed_hashtable(HT& ht, uint64_t keyspace) {
    const uint64_t init = keyspace / 4;
    for (uint64_t i = 0; i < init; ++i) {
        ht.insert(i, i ^ 0x9e3779b97f4a7c15ULL);
    }
}

template <typename HT>
static double bench_hashtable_scenario(
    HT& ht,
    const HashScenario& scenario,
    int n_threads,
    double dur_sec,
    bool pin_cpus,
    std::vector<double>& lats_out
) {
    std::atomic<uint64_t> total_ops{0};
    auto affinity = cpu::make_affinity_map(n_threads);
    std::vector<std::thread> threads;
    std::vector<std::vector<double>> per_thread(n_threads);

    const uint64_t deadline = timer::now_ns() +
                              static_cast<uint64_t>(dur_sec * 1e9);

    for (int tid = 0; tid < n_threads; ++tid) {
        threads.emplace_back([&, tid] {
            if (pin_cpus && tid < static_cast<int>(affinity.size())) {
                cpu::pin_to_core(affinity[tid]);
            }

            std::mt19937_64 rng(0x9e3779b97f4a7c15ULL ^ (uint64_t(tid) << 32));
            auto& lat = per_thread[tid];
            lat.reserve(20000);
            uint64_t local_ops = 0;

            while (timer::now_ns() < deadline) {
                const uint64_t op_index = local_ops;
                int lookup_cut = scenario.lookup_pct;
                int insert_cut = scenario.lookup_pct + scenario.insert_pct;

                if (scenario.bursty) {
                    const bool write_phase = ((op_index / 4096) % 2) == 1;
                    if (write_phase) {
                        lookup_cut = 20;
                        insert_cut = 85;
                    } else {
                        lookup_cut = 85;
                        insert_cut = 95;
                    }
                }

                uint64_t key = 0;
                if (scenario.hotset > 0 && (rng() % 100) < 90) {
                    key = rng() % scenario.hotset;
                } else {
                    key = rng() % scenario.keyspace;
                }

                int op = static_cast<int>(rng() % 100);
                uint64_t t0 = timer::now_ns();
                if (op < lookup_cut) {
                    uint64_t out = 0;
                    (void)ht.lookup(key, out);
                } else if (op < insert_cut) {
                    ht.insert(key, key ^ rng());
                } else {
                    (void)ht.erase(key);
                }
                uint64_t t1 = timer::now_ns();

                if (lat.size() < 20000) {
                    lat.push_back(static_cast<double>(t1 - t0));
                }
                ++local_ops;
            }

            total_ops.fetch_add(local_ops, std::memory_order_relaxed);
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    for (auto& sample : per_thread) {
        lats_out.insert(lats_out.end(), sample.begin(), sample.end());
    }

    return static_cast<double>(total_ops.load(std::memory_order_relaxed)) / dur_sec;
}

static void write_hash_result(
    CsvWriter& csv,
    const char* primitive,
    const HashScenario& scenario,
    int threads,
    int rep,
    double ops,
    const std::vector<double>& lats
) {
    stats::WelfordAccumulator acc;
    for (double x : lats) {
        acc.push(x);
    }

    std::vector<double> p95_src = lats;
    std::vector<double> p99_src = lats;

    TrialResult tr;
    tr.suite = "workload";
    tr.experiment = "hashtable_scenarios";
    tr.primitive = primitive;
    tr.threads = threads;
    tr.trial = rep;
    tr.ops_per_sec = ops;
    tr.mean_latency_ns = acc.mean();
    tr.stddev_ns = acc.stddev();
    tr.p95_ns = stats::p95(p95_src);
    tr.p99_ns = stats::p99(p99_src);
    tr.notes = std::string("scenario=") + scenario.name;
    csv.write(tr);
}

template <typename TableFactory>
static void run_hash_impl(
    const BenchmarkConfig& cfg,
    CsvWriter& csv,
    const char* primitive,
    TableFactory factory
) {
    for (const auto& scenario : HASH_SCENARIOS) {
        printf("  Scenario: %-20s\n", scenario.name);
        for (int n : cfg.thread_counts) {
            for (int rep = 0; rep < cfg.repetitions; ++rep) {
                auto table = factory();
                seed_hashtable(*table, scenario.keyspace);

                std::vector<double> lats;
                const double ops = bench_hashtable_scenario(
                    *table,
                    scenario,
                    n,
                    cfg.duration_sec,
                    cfg.pin_cpus,
                    lats
                );

                write_hash_result(csv, primitive, scenario, n, rep, ops, lats);

                if (rep == 0) {
                    stats::WelfordAccumulator acc;
                    for (double x : lats) {
                        acc.push(x);
                    }
                    std::vector<double> p99_src = lats;
                    printf(
                        "    %-16s  Thr=%2d  Ops/s=%12.0f  Mean=%9.1fns  P99=%9.1fns\n",
                        primitive,
                        n,
                        ops,
                        acc.mean(),
                        stats::p99(p99_src)
                    );
                }
            }
        }
        printf("\n");
    }
}

inline void run_hashtable_experiment(const BenchmarkConfig& cfg, CsvWriter& csv) {
    printf("  %-18s  %-7s  %-12s  %-14s  %-12s\n",
           "Implementation", "Thr", "Ops/sec", "Mean(ns)", "P99(ns)");
    printf("  %s\n", std::string(72, '-').c_str());

    run_hash_impl(
        cfg,
        csv,
        "ht_coarse_mutex",
        [] { return std::make_unique<CoarseHashTable>(); }
    );

    run_hash_impl(
        cfg,
        csv,
        "ht_fine_rwlock",
        [] { return std::make_unique<FineHashTable>(); }
    );

    run_hash_impl(
        cfg,
        csv,
        "ht_lockfree",
        [] { return std::make_unique<LockFreeHashTable>(); }
    );
}
