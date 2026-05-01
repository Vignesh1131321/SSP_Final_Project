/**
 * energy/rapl_energy.h
 * ─────────────────────
 * Section 7 — Energy Consumption Analysis
 *
 * Measures energy per synchronisation operation using Intel RAPL
 * (Running Average Power Limit) counters exposed via Linux MSRs or
 * the powercap sysfs interface (/sys/class/powercap/intel-rapl/).
 *
 * Computes:
 *   Energy per op = Total Energy (µJ) / Total Operations
 *
 * Also records:
 *   Average power (W) = Total Energy / Elapsed Time
 *
 * Requires on Linux: /sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj
 * Falls back gracefully on non-Intel or non-Linux systems.
 */
#pragma once
#include "../benchmark.h"
#include "../utils/csv_writer.h"
#include "../utils/timer.h"
#include "../locks/lock_dispatch.h"
#include "../experiments/experiment_runner.h"

#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <atomic>

/* ── RAPL sysfs reader ──────────────────────────────────── */
namespace rapl {

/* Returns energy in microjoules, or -1 if not available */
static inline int64_t read_energy_uj(int package = 0) {
#ifdef __linux__
    char path[256];
    snprintf(path, sizeof(path),
             "/sys/class/powercap/intel-rapl/intel-rapl:%d/energy_uj",
             package);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    int64_t val = -1;
    fscanf(f, "%lld", &val);
    fclose(f);
    return val;
#else
    (void)package;
    return -1;
#endif
}

/* Max counter value (wraps around) */
static inline int64_t read_max_energy_uj(int package = 0) {
#ifdef __linux__
    char path[256];
    snprintf(path, sizeof(path),
             "/sys/class/powercap/intel-rapl/intel-rapl:%d/max_energy_range_uj",
             package);
    FILE* f = fopen(path, "r");
    if (!f) return INT64_MAX;
    int64_t val = INT64_MAX;
    fscanf(f, "%lld", &val);
    fclose(f);
    return val;
#else
    (void)package;
    return INT64_MAX;
#endif
}

static inline bool available() {
    return read_energy_uj(0) >= 0;
}

/* Delta energy with wrap-around handling */
static inline int64_t delta_uj(int64_t before, int64_t after, int64_t max_uj) {
    if (after >= before) return after - before;
    return max_uj - before + after;  // wrap
}

} // namespace rapl

/* ── Energy benchmark result ─────────────────────────────── */
struct EnergyResult {
    double ops_per_sec    = 0;
    double total_ops      = 0;
    double elapsed_sec    = 0;
    double energy_uj      = -1;   // -1 = RAPL unavailable
    double energy_per_op_nj = -1; // nanojoules per operation
    double avg_power_w    = -1;   // watts
};

/* ── Measure energy for one lock under N threads ─────────── */
static EnergyResult measure_energy(PrimitiveKind kind,
                                    int n_threads,
                                    double dur_sec,
                                    double warmup_sec,
                                    bool pin) {
    auto lock = make_lock(kind);
    std::atomic<uint64_t> dummy{0};

    auto make_body = [&](int) -> std::function<void()> {
        return [&] {
            lock->acquire();
            busy_work(0);
            dummy.fetch_add(1, std::memory_order_relaxed);
            lock->release();
        };
    };

    // Discard warmup energy
    run_experiment(n_threads, warmup_sec, 0, make_body, pin);

    int64_t max_uj   = rapl::read_max_energy_uj(0);
    int64_t before   = rapl::read_energy_uj(0);

    auto res = run_experiment(n_threads, dur_sec, 0, make_body, pin);

    int64_t after    = rapl::read_energy_uj(0);

    EnergyResult er;
    er.ops_per_sec = res.ops_per_sec;
    er.total_ops   = static_cast<double>(res.total_ops);
    er.elapsed_sec = dur_sec;

    if (before >= 0 && after >= 0) {
        double delta = static_cast<double>(
            rapl::delta_uj(before, after, max_uj));
        er.energy_uj           = delta;
        er.energy_per_op_nj    = (er.total_ops > 0)
                                 ? (delta * 1000.0 / er.total_ops)  // µJ→nJ /op
                                 : -1;
        er.avg_power_w         = delta / 1e6 / dur_sec;
    }
    return er;
}

/* ── Public entry point ─────────────────────────────────── */
inline void run_energy_experiment(const BenchmarkConfig& cfg, CsvWriter& csv) {
    if (!rapl::available()) {
        printf("  [RAPL] Intel RAPL not available on this system.\n");
        printf("         Requires: Linux + Intel CPU + "
               "/sys/class/powercap/intel-rapl/\n");
        printf("         You may need: sudo chmod +r "
               "/sys/class/powercap/intel-rapl/*/energy_uj\n\n");
    } else {
        printf("  [RAPL] Energy counters available.\n\n");
    }

    printf("  %-14s  %6s  %14s  %12s  %12s  %10s\n",
           "Primitive", "Thr", "Ops/sec",
           "nJ/op", "Avg Power(W)", "Energy(µJ)");
    printf("  %s\n", std::string(72, '-').c_str());

    const int fixed_threads = std::min(8, cpu::hardware_threads());

    for (auto kind : all_primitives()) {
        const char* pname = primitive_name(kind);

        for (int rep = 0; rep < cfg.repetitions; ++rep) {
            auto er = measure_energy(kind, fixed_threads,
                                     cfg.duration_sec,
                                     cfg.warmup_sec,
                                     cfg.pin_cpus);

            // Write to CSV even when RAPL unavailable (shows -1)
            TrialResult tr;
            tr.suite           = "energy";
            tr.experiment      = "rapl_energy";
            tr.primitive       = pname;
            tr.threads         = fixed_threads;
            tr.trial           = rep;
            tr.ops_per_sec     = er.ops_per_sec;
            tr.throughput_ops  = static_cast<uint64_t>(er.total_ops);
            // Encode energy metrics in notes field
            tr.notes = "energy_uj="     + std::to_string(er.energy_uj)
                     + ";nj_per_op="    + std::to_string(er.energy_per_op_nj)
                     + ";avg_power_w="  + std::to_string(er.avg_power_w);
            csv.write(tr);

            if (rep == 0) {
                if (er.energy_per_op_nj >= 0)
                    printf("  %-14s  %6d  %14.0f  %12.4f  %12.4f  %10.0f\n",
                           pname, fixed_threads,
                           er.ops_per_sec,
                           er.energy_per_op_nj,
                           er.avg_power_w,
                           er.energy_uj);
                else
                    printf("  %-14s  %6d  %14.0f  %12s  %12s  %10s\n",
                           pname, fixed_threads,
                           er.ops_per_sec,
                           "N/A", "N/A", "N/A");
            }
        }
    }
}
