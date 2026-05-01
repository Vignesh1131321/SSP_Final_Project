/**
 * experiments/false_sharing.h
 *
 * Experiment 3 — False Sharing
 * ─────────────────────────────
 * Two or more threads increment counters. In the "false-shared" case both
 * counters sit on the same cache line. In the padded case each counter is
 * aligned to separate cache lines.
 *
 * Measures:
 *   - ops/sec  (false-shared vs padded)
 *   - slowdown = padded throughput / false-shared throughput
 *
 * Publication-oriented controls:
 *   - repetitions (>=10 recommended)
 *   - thread sweep (default: 1,2,3,4,6,8,10,12)
 *   - SMT mode: on/off (off uses SMT-avoidance affinity map when possible)
 *   - placement policy: no pinning / compact / spread
 */
#pragma once
#include "../benchmark.h"
#include "../utils/csv_writer.h"
#include "../utils/timer.h"
#include "../utils/cpu_affinity.h"
#include "../utils/stats.h"
#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <string>
#include <fstream>
#include <cmath>
#include <cctype>

/* ── shared-line layout ──────────────────────────────────── */
struct FalseSharedCounters {
    volatile uint64_t a;
    volatile uint64_t b;
};

/* ── padded layout ───────────────────────────────────────── */
struct alignas(CACHE_LINE_SIZE) PaddedCounter {
    volatile uint64_t val;
    char _pad[CACHE_LINE_SIZE - sizeof(uint64_t)];
};
struct PaddedCounters {
    PaddedCounter a;
    PaddedCounter b;
};

enum class PlacementPolicy {
    None,
    Compact,
    Spread,
};

inline const char* placement_name(PlacementPolicy p) {
    switch (p) {
        case PlacementPolicy::None:    return "none";
        case PlacementPolicy::Compact: return "compact";
        case PlacementPolicy::Spread:  return "spread";
        default:                       return "none";
    }
}

inline std::vector<PlacementPolicy> resolve_placement_policies(const BenchmarkConfig& cfg) {
    std::string v = cfg.placement_policy;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c){ return (char)std::tolower(c); });

    // Backward compatibility: --pin-cpus historically meant one pinned mode.
    if (cfg.pin_cpus && v == "all") {
        return {PlacementPolicy::Spread};
    }

    if (v == "none") return {PlacementPolicy::None};
    if (v == "compact") return {PlacementPolicy::Compact};
    if (v == "spread") return {PlacementPolicy::Spread};
    return {PlacementPolicy::None, PlacementPolicy::Compact, PlacementPolicy::Spread};
}

inline std::vector<bool> resolve_smt_modes(const BenchmarkConfig& cfg) {
    std::string v = cfg.smt_mode;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    if (v == "on") return {false};
    if (v == "off") return {true};
    return {false, true};
}

inline std::vector<int> build_affinity_map(int n_threads, PlacementPolicy placement, bool smt_off) {
    if (placement == PlacementPolicy::None) {
        return {};
    }

    const int ncores = std::max(1, cpu::hardware_threads());
    std::vector<int> map(n_threads, 0);

    if (placement == PlacementPolicy::Compact) {
        for (int i = 0; i < n_threads; ++i) {
            map[i] = i % ncores;
        }
    } else {
        // Spread threads across available logical CPUs.
        const int step = std::max(1, ncores / std::max(1, n_threads));
        for (int i = 0; i < n_threads; ++i) {
            map[i] = (i * step) % ncores;
        }
    }

    // SMT-off mode: use every second logical CPU when possible.
    if (smt_off && ncores >= 2) {
        std::vector<int> adjusted(n_threads, 0);
        int stride = 2;
        // If thread count exceeds half logical CPUs, fall back gracefully.
        if (n_threads > (ncores / 2)) {
            stride = 1;
        }
        for (int i = 0; i < n_threads; ++i) {
            adjusted[i] = (map[i] * stride) % ncores;
        }
        return adjusted;
    }

    return map;
}

/* ── driver for one configuration ───────────────────────── */
static double measure_false_sharing(
    bool padded,
    double duration_sec,
    double warmup_sec,
    int n_threads,
    PlacementPolicy placement,
    bool smt_off)
{
    FalseSharedCounters fs_ctrs{};
    PaddedCounters      pd_ctrs{};

    std::atomic<bool> stop{false};
    std::vector<uint64_t> ops(n_threads, 0);
    std::vector<std::thread> threads;

    std::vector<int> affinity = build_affinity_map(n_threads, placement, smt_off);

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    for (int tid = 0; tid < n_threads; ++tid) {
        threads.emplace_back([&, tid] {
            if (!affinity.empty()) {
                cpu::pin_to_core(affinity[tid]);
            }

            uint64_t cnt = 0;
            uint64_t accum = 0;

            ready.fetch_add(1, std::memory_order_relaxed);
            while (!go.load(std::memory_order_acquire)) CPU_PAUSE();

            uint64_t wend = timer::now_ns() + (uint64_t)(warmup_sec * 1e9);
            while (timer::now_ns() < wend) {
                if (padded) {
                    for (int i = 0; i < 10; ++i) {
                        if (tid % 2 == 0) accum += pd_ctrs.a.val++;
                        else              accum += pd_ctrs.b.val++;
                    }
                } else {
                    for (int i = 0; i < 10; ++i) {
                        if (tid % 2 == 0) accum += fs_ctrs.a++;
                        else              accum += fs_ctrs.b++;
                    }
                }
                cnt += 10;
            }

            uint64_t mend = timer::now_ns() + (uint64_t)(duration_sec * 1e9);
            while (!stop.load(std::memory_order_relaxed) && timer::now_ns() < mend) {
                if (padded) {
                    for (int i = 0; i < 10; ++i) {
                        if (tid % 2 == 0) accum += pd_ctrs.a.val++;
                        else              accum += pd_ctrs.b.val++;
                    }
                } else {
                    for (int i = 0; i < 10; ++i) {
                        if (tid % 2 == 0) accum += fs_ctrs.a++;
                        else              accum += fs_ctrs.b++;
                    }
                }
                cnt += 10;
            }
            ops[tid] = cnt;
            (void)accum;
        });
    }

    while (ready.load(std::memory_order_relaxed) < n_threads) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    uint64_t total = 0;
    for (auto v : ops) total += v;
    return static_cast<double>(total) / duration_sec;
}

struct FalseSharingSummaryRow {
    int threads = 0;
    std::string placement;
    std::string smt;
    double fs_mean = 0.0;
    double fs_std = 0.0;
    double pad_mean = 0.0;
    double pad_std = 0.0;
    double factor_mean = 0.0;
    double factor_std = 0.0;
};

inline void write_false_sharing_summary_table(
    const std::string& outdir,
    const std::vector<FalseSharingSummaryRow>& rows)
{
    std::ofstream out(outdir + "/false_sharing_summary_table.csv", std::ios::trunc);
    if (!out.is_open()) return;

    out << "threads,placement,smt_mode,false_shared_mean_ops,false_shared_std_ops,";
    out << "padded_mean_ops,padded_std_ops,slowdown_mean,slowdown_std,formula\n";

    for (const auto& r : rows) {
        out << r.threads << ','
            << r.placement << ','
            << r.smt << ','
            << r.fs_mean << ','
            << r.fs_std << ','
            << r.pad_mean << ','
            << r.pad_std << ','
            << r.factor_mean << ','
            << r.factor_std << ','
            << "slowdown=padded_throughput/false_shared_throughput"
            << "\n";
    }
}

inline void run_false_sharing_experiment(const BenchmarkConfig& cfg, CsvWriter& csv) {
    printf("  %-7s %-8s %-7s %12s %12s %10s\n",
           "threads", "policy", "smt", "fs_mean", "pad_mean", "slowdown");
    printf("  %s\n", std::string(70, '-').c_str());

    std::vector<int> thread_counts = cfg.thread_counts.empty()
        ? std::vector<int>{1, 2, 3, 4, 6, 8, 10, 12}
        : cfg.thread_counts;

    const int hw_threads = std::max(1, (int)std::thread::hardware_concurrency());
    const auto placements = resolve_placement_policies(cfg);
    const auto smt_modes = resolve_smt_modes(cfg);

    std::vector<FalseSharingSummaryRow> summary_rows;

    for (int n : thread_counts) {
        if (n <= 0) continue;
        if (n > hw_threads) {
            printf("  threads=%-3d skipped (exceeds hw concurrency=%d)\n", n, hw_threads);
            continue;
        }

        for (PlacementPolicy p : placements) {
            for (bool smt_off : smt_modes) {
                // Compact/spread need pinning. None ignores SMT mode semantics.
                if (p == PlacementPolicy::None && smt_off) {
                    continue;
                }

                std::vector<double> fs_trials;
                std::vector<double> pad_trials;
                std::vector<double> factor_trials;
                fs_trials.reserve((size_t)cfg.repetitions);
                pad_trials.reserve((size_t)cfg.repetitions);
                factor_trials.reserve((size_t)cfg.repetitions);

                for (int rep = 0; rep < cfg.repetitions; ++rep) {
                    double ops_fs = measure_false_sharing(false, cfg.duration_sec, cfg.warmup_sec,
                                                          n, p, smt_off);
                    double ops_pad = measure_false_sharing(true, cfg.duration_sec, cfg.warmup_sec,
                                                           n, p, smt_off);
                    double factor = (ops_fs > 0.0) ? (ops_pad / ops_fs) : 0.0;

                    fs_trials.push_back(ops_fs);
                    pad_trials.push_back(ops_pad);
                    factor_trials.push_back(factor);

                    const std::string note = std::string("placement=") + placement_name(p)
                        + ";smt=" + (smt_off ? "off" : "on")
                        + ";formula=slowdown=padded_throughput/false_shared_throughput";

                    TrialResult tr_fs;
                    tr_fs.suite = "false_sharing";
                    tr_fs.experiment = "false_sharing";
                    tr_fs.primitive = "false_shared";
                    tr_fs.threads = n;
                    tr_fs.trial = rep + 1;
                    tr_fs.ops_per_sec = ops_fs;
                    tr_fs.false_sharing_factor = factor;
                    tr_fs.notes = note;
                    csv.write(tr_fs);

                    TrialResult tr_pad;
                    tr_pad.suite = "false_sharing";
                    tr_pad.experiment = "false_sharing";
                    tr_pad.primitive = "padded";
                    tr_pad.threads = n;
                    tr_pad.trial = rep + 1;
                    tr_pad.ops_per_sec = ops_pad;
                    tr_pad.false_sharing_factor = factor;
                    tr_pad.notes = note;
                    csv.write(tr_pad);
                }

                auto mean_std = [](const std::vector<double>& v) {
                    double mean = 0.0;
                    for (double x : v) mean += x;
                    mean /= std::max<size_t>(1, v.size());
                    double var = 0.0;
                    for (double x : v) {
                        const double d = x - mean;
                        var += d * d;
                    }
                    var /= (v.size() > 1 ? (double)(v.size() - 1) : 1.0);
                    return std::make_pair(mean, std::sqrt(var));
                };

                auto fs_ms = mean_std(fs_trials);
                auto pd_ms = mean_std(pad_trials);
                auto fc_ms = mean_std(factor_trials);

                printf("  %-7d %-8s %-7s %12.0f %12.0f %10.2fx\n",
                       n, placement_name(p), smt_off ? "off" : "on",
                       fs_ms.first, pd_ms.first, fc_ms.first);

                FalseSharingSummaryRow row;
                row.threads = n;
                row.placement = placement_name(p);
                row.smt = smt_off ? "off" : "on";
                row.fs_mean = fs_ms.first;
                row.fs_std = fs_ms.second;
                row.pad_mean = pd_ms.first;
                row.pad_std = pd_ms.second;
                row.factor_mean = fc_ms.first;
                row.factor_std = fc_ms.second;
                summary_rows.push_back(row);
            }
        }
    }

    write_false_sharing_summary_table(cfg.output_dir, summary_rows);
    printf("  Summary table: %s/false_sharing_summary_table.csv\n", cfg.output_dir.c_str());
}
