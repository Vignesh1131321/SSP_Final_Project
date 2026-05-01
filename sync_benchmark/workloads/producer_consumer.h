/**
 * workloads/producer_consumer.h
 * Scenario-driven benchmark for a bounded producer-consumer queue.
 *
 * Six queue-access scenarios are evaluated for each implementation:
 *   1) balanced_1to1
 *   2) read_dominant
 *   3) write_dominant
 *   4) bursty_backpressure
 *   5) hotspot_small_buffer
 *   6) fanin_fanout
 */
#pragma once

#include "../benchmark.h"
#include "../locks/semaphore_lock.h"
#include "../utils/cpu_affinity.h"
#include "../utils/csv_writer.h"
#include "../utils/stats.h"
#include "../utils/timer.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static constexpr size_t QUEUE_CAPACITY = 4096;

class MutexQueue {
    std::deque<uint64_t> q_;
    std::mutex mu_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    size_t cap_;

public:
    explicit MutexQueue(size_t cap = QUEUE_CAPACITY) : cap_(cap) {}

    bool enqueue(uint64_t val) {
        std::unique_lock<std::mutex> lk(mu_);
        if (q_.size() >= cap_) return false;
        q_.push_back(val);
        not_empty_.notify_one();
        return true;
    }

    bool dequeue(uint64_t& out) {
        std::unique_lock<std::mutex> lk(mu_);
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop_front();
        not_full_.notify_one();
        return true;
    }
};

class LockFreeQueue {
    struct alignas(CACHE_LINE_SIZE) Slot {
        std::atomic<uint64_t> seq;
        uint64_t val;
    };

    std::vector<Slot> slots_;
    size_t mask_;
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> head_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> tail_{0};

public:
    explicit LockFreeQueue(size_t cap = QUEUE_CAPACITY) : slots_(cap), mask_(cap - 1) {
        for (size_t i = 0; i < cap; ++i) {
            slots_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    bool enqueue(uint64_t v) {
        uint64_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = slots_[pos & mask_];
            uint64_t seq = slot.seq.load(std::memory_order_acquire);
            int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos);
            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    slot.val = v;
                    slot.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
    }

    bool dequeue(uint64_t& out) {
        uint64_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = slots_[pos & mask_];
            uint64_t seq = slot.seq.load(std::memory_order_acquire);
            int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos + 1);
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    out = slot.val;
                    slot.seq.store(pos + mask_ + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }
};

class SemaphoreQueue {
    std::deque<uint64_t> q_;
    std::mutex mu_;
    CountingSemaphore empty_slots_;
    CountingSemaphore full_slots_;

public:
    explicit SemaphoreQueue(int cap = static_cast<int>(QUEUE_CAPACITY))
        : empty_slots_(cap), full_slots_(0) {}

    bool enqueue(uint64_t val) {
        if (!empty_slots_.try_wait()) return false;
        {
            std::lock_guard<std::mutex> g(mu_);
            q_.push_back(val);
        }
        full_slots_.post();
        return true;
    }

    bool dequeue(uint64_t& out) {
        if (!full_slots_.try_wait()) return false;
        {
            std::lock_guard<std::mutex> g(mu_);
            out = q_.front();
            q_.pop_front();
        }
        empty_slots_.post();
        return true;
    }
};

struct QueueScenario {
    const char* name;
    int producer_share;
    size_t capacity;
    bool bursty;
};

static const QueueScenario QUEUE_SCENARIOS[] = {
    {"balanced_1to1", 50, 4096, false},
    {"read_dominant", 35, 4096, false},
    {"write_dominant", 70, 4096, false},
    {"bursty_backpressure", 50, 4096, true},
    {"hotspot_small_buffer", 50, 128, false},
    {"fanin_fanout", 25, 4096, false},
};

struct QueueRunResult {
    double ops_per_sec = 0.0;
    double mean_ns = 0.0;
    double p95_ns = 0.0;
    double p99_ns = 0.0;
    double stddev_ns = 0.0;
};

template <typename QueueFactory>
static QueueRunResult bench_queue_scenario(
    QueueFactory factory,
    const QueueScenario& scenario,
    int n_threads,
    double duration_sec,
    bool pin_cpus
) {
    int producers = std::max(1, (n_threads * scenario.producer_share) / 100);
    if (producers >= n_threads) producers = n_threads - 1;
    producers = std::max(1, producers);
    int consumers = std::max(1, n_threads - producers);

    auto queue = factory(scenario.capacity);
    std::atomic<uint64_t> success_ops{0};
    std::vector<std::thread> threads;
    auto affinity = cpu::make_affinity_map(n_threads);
    std::vector<std::vector<double>> producer_lats(producers);

    const uint64_t deadline = timer::now_ns() +
                              static_cast<uint64_t>(duration_sec * 1e9);

    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            if (pin_cpus && p < static_cast<int>(affinity.size())) {
                cpu::pin_to_core(affinity[p]);
            }

            auto& lats = producer_lats[p];
            lats.reserve(20000);
            uint64_t local = 0;
            uint64_t value = (uint64_t(p) << 48);

            while (timer::now_ns() < deadline) {
                if (scenario.bursty) {
                    uint64_t phase = (local / 1024) % 4;
                    if (phase == 3) {
                        for (int i = 0; i < 16; ++i) {
                            busy_work(256);
                        }
                    }
                }

                uint64_t t0 = timer::now_ns();
                bool ok = queue->enqueue(value++);
                uint64_t t1 = timer::now_ns();

                if (ok) {
                    if (lats.size() < 20000) {
                        lats.push_back(static_cast<double>(t1 - t0));
                    }
                    ++local;
                }
            }
            success_ops.fetch_add(local, std::memory_order_relaxed);
        });
    }

    for (int c = 0; c < consumers; ++c) {
        const int tid = producers + c;
        threads.emplace_back([&, tid] {
            if (pin_cpus && tid < static_cast<int>(affinity.size())) {
                cpu::pin_to_core(affinity[tid]);
            }

            uint64_t out = 0;
            while (timer::now_ns() < deadline) {
                (void)queue->dequeue(out);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    std::vector<double> all_lats;
    for (auto& vec : producer_lats) {
        all_lats.insert(all_lats.end(), vec.begin(), vec.end());
    }

    stats::WelfordAccumulator acc;
    for (double x : all_lats) {
        acc.push(x);
    }

    QueueRunResult out;
    out.ops_per_sec = static_cast<double>(success_ops.load(std::memory_order_relaxed)) / duration_sec;
    out.mean_ns = acc.mean();
    out.p95_ns = stats::p95(all_lats);
    out.p99_ns = stats::p99(all_lats);
    out.stddev_ns = acc.stddev();
    return out;
}

static void write_queue_result(
    CsvWriter& csv,
    const char* primitive,
    const QueueScenario& scenario,
    int threads,
    int rep,
    const QueueRunResult& r
) {
    TrialResult tr;
    tr.suite = "workload";
    tr.experiment = "producer_consumer_scenarios";
    tr.primitive = primitive;
    tr.threads = threads;
    tr.trial = rep;
    tr.ops_per_sec = r.ops_per_sec;
    tr.mean_latency_ns = r.mean_ns;
    tr.p95_ns = r.p95_ns;
    tr.p99_ns = r.p99_ns;
    tr.stddev_ns = r.stddev_ns;
    tr.notes = std::string("scenario=") + scenario.name;
    csv.write(tr);
}

template <typename QueueFactory>
static void run_queue_impl(
    const BenchmarkConfig& cfg,
    CsvWriter& csv,
    const char* primitive,
    QueueFactory factory
) {
    for (const auto& scenario : QUEUE_SCENARIOS) {
        printf("  Scenario: %-22s\n", scenario.name);
        for (int n : cfg.thread_counts) {
            for (int rep = 0; rep < cfg.repetitions; ++rep) {
                QueueRunResult r = bench_queue_scenario(
                    factory,
                    scenario,
                    n,
                    cfg.duration_sec,
                    cfg.pin_cpus
                );
                write_queue_result(csv, primitive, scenario, n, rep, r);

                if (rep == 0) {
                    printf("    %-16s  Thr=%2d  Ops/s=%12.0f  Mean=%9.1fns  P99=%9.1fns\n",
                           primitive, n, r.ops_per_sec, r.mean_ns, r.p99_ns);
                }
            }
        }
        printf("\n");
    }
}

inline void run_producer_consumer_experiment(const BenchmarkConfig& cfg, CsvWriter& csv) {
    printf("  %-18s  %-7s  %-12s  %-14s  %-12s\n",
           "Implementation", "Thr", "Ops/sec", "Mean(ns)", "P99(ns)");
    printf("  %s\n", std::string(72, '-').c_str());

    run_queue_impl(
        cfg,
        csv,
        "queue_mutex",
        [](size_t cap) { return std::make_unique<MutexQueue>(cap); }
    );

    run_queue_impl(
        cfg,
        csv,
        "queue_lockfree",
        [](size_t cap) {
            size_t ring = 1;
            while (ring < cap) ring <<= 1;
            return std::make_unique<LockFreeQueue>(ring);
        }
    );

    run_queue_impl(
        cfg,
        csv,
        "queue_semaphore",
        [](size_t cap) { return std::make_unique<SemaphoreQueue>(static_cast<int>(cap)); }
    );
}
