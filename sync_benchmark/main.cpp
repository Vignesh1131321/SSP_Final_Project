/**
 * ============================================================
 * Research-Grade Synchronization Primitives Benchmark
 * Project: Study of Different Synchronization Mechanism
 *          Performance Among Threads
 * ============================================================
 *
 * RQ1  Scalability with thread count
 * RQ2  Critical-section size sensitivity
 * RQ3  False-sharing cache effects
 * RQ4  CPU pinning vs OS scheduling
 * RQ5  Lock fairness (Gini coefficient)
 * RQ6  Kernel vs user-space primitives
 * RQ7  OpenMP framework vs manual locks
 * RQ8  Distributed sync via MPI (separate binary: mpi/mpi_bench_main.cpp)
 * RQ9  Cross-platform OS comparison (platform/platform_info.h)
 *
 * Build (Linux, all features):
 *   g++ -std=c++17 -O3 -march=native -pthread -fopenmp \
 *       main.cpp locks/lock_dispatch.cpp \
 *       experiments/scalability.cpp experiments/contention.cpp \
 *       experiments/false_sharing.cpp experiments/fairness.cpp \
 *       experiments/latency_dist.cpp experiments/throughput_saturation.cpp \
 *       -o sync_bench
 *
 * Build (without OpenMP):
 *   g++ -std=c++17 -O3 -march=native -pthread -DNO_OPENMP \
 *       main.cpp locks/lock_dispatch.cpp ...
 *
 * Usage:
 *   ./sync_bench --suite all [options]
 *   ./sync_bench --help
 * ============================================================
 */

#include "benchmark.h"

// ── Core experiments ──────────────────────────────────────
#include "experiments/scalability.h"
#include "experiments/contention.h"
#include "experiments/false_sharing.h"
#include "experiments/fairness.h"
#include "experiments/latency_dist.h"
#include "experiments/throughput_saturation.h"

// ── New modules (SSP additions) ──────────────────────────
#ifndef NO_OPENMP
#  include "openmp/openmp_bench.h"
#endif
#include "mpi/mpi_bench.h"
#include "workloads/concurrent_hash_table.h"
#include "workloads/producer_consumer.h"
#include "workloads/graph_workload.h"
#include "platform/platform_info.h"

#include "utils/csv_writer.h"
#include "utils/stats.h"
#include "utils/cpu_affinity.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <cstdlib>
#include <cerrno>

/* ── Portable mkdir ───────────────────────────────────────── */
#ifdef _WIN32
#  include <direct.h>
   static void make_dir(const std::string& path) { _mkdir(path.c_str()); }
#else
#  include <sys/stat.h>
   static void make_dir(const std::string& path) { mkdir(path.c_str(), 0755); }
#endif

/* ── Banner ───────────────────────────────────────────────── */
static void print_banner() {
    std::cout <<
"╔══════════════════════════════════════════════════════════════╗\n"
"║   SYNCHRONIZATION PRIMITIVES BENCHMARK  —  Research Suite    ║\n"
"║   Study of Thread Synchronization Mechanism Performance      ║\n"
"║   RQ1-RQ9   |  C++ / OpenMP / MPI / Real-World Workloads     ║\n"
"╚══════════════════════════════════════════════════════════════╝\n\n";
}

/* ── Argument parsing ─────────────────────────────────────── */
static BenchmarkConfig parse_args(int argc, char** argv) {
    BenchmarkConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--suite" && i+1 < argc) {
            cfg.suite = argv[++i];
        } else if (arg == "--threads" && i+1 < argc) {
            cfg.thread_counts.clear();
            std::istringstream ss(argv[++i]);
            std::string tok;
            while (std::getline(ss, tok, ','))
                cfg.thread_counts.push_back(std::stoi(tok));
        } else if (arg == "--duration"    && i+1 < argc) { cfg.duration_sec  = std::stod(argv[++i]);
        } else if (arg == "--warmup"      && i+1 < argc) { cfg.warmup_sec    = std::stod(argv[++i]);
        } else if (arg == "--outdir"      && i+1 < argc) { cfg.output_dir    = argv[++i];
        } else if (arg == "--csv"         && i+1 < argc) { cfg.csv_file      = argv[++i];
        } else if (arg == "--repetitions" && i+1 < argc) { cfg.repetitions   = std::stoi(argv[++i]);
        } else if (arg == "--placement" && i+1 < argc)   { cfg.placement_policy = argv[++i];
        } else if (arg == "--smt" && i+1 < argc)         { cfg.smt_mode = argv[++i];
        } else if (arg == "--primitive" && i+1 < argc)   { cfg.primitive_filter = argv[++i];
        } else if (arg == "--verbose")  { cfg.verbose  = true;
        } else if (arg == "--pin-cpus") { cfg.pin_cpus = true;
        } else if (arg == "--cs-cycles" && i+1 < argc) { cfg.scalability_cs_cycles = std::stoull(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            std::cout <<
"Usage: sync_bench [options]\n"
"  --suite       Comma-separated or single suite name:\n"
"                  all | scalability | contention | false_sharing |\n"
"                  fairness | latency | throughput |\n"
"                  openmp | workloads\n"
"  --threads     Comma-separated list  (default: 1,2,3,4,6,8,10,12)\n"
"  --duration    Seconds per trial     (default: 5)\n"
"  --warmup      Warm-up seconds       (default: 2)\n"
"  --outdir      Output directory      (default: output)\n"
"  --csv         Result CSV filename   (default: results.csv)\n"
"  --repetitions Reps per config       (default: 10)\n"
"  --primitive   Filter Experiment 5 to one primitive\n"
"  --placement   False-sharing placement policy: all|none|compact|spread\n"
"  --smt         False-sharing SMT mode: both|on|off\n"
"  --pin-cpus    Legacy flag (maps to spread pinning intent)\n"
"  --cs-cycles   Busy-loop cycles inside CS for scalability exp (default: 100)\n"
"  --verbose     Extra console output\n\n"
"MPI (run separately):\n"
"  mpicxx -DUSE_MPI -std=c++17 -O3 -I. mpi/mpi_bench_main.cpp -o mpi_bench\n"
"  mpirun -np 4 ./mpi_bench --duration 2\n";
            std::exit(0);
        }
    }
    return cfg;
}

/* ═══════════════════════════════════════════════════════════ */
int main(int argc, char** argv) {
    print_banner();
    BenchmarkConfig cfg = parse_args(argc, argv);

    // Print + record platform info
    auto sysinfo = platform::query();
    platform::print(sysinfo);

    make_dir(cfg.output_dir);
    platform::write_platform_row(sysinfo,
        cfg.output_dir + "/platform_info.txt");

    // Global CSV sink
    CsvWriter csv(cfg.output_dir + "/" + cfg.csv_file);
    csv.write_header({
        "suite","experiment","primitive","threads",
        "cs_cycles","trial",
        "ops_per_sec","mean_latency_ns","p50_ns","p95_ns","p99_ns",
        "stddev_ns","cv_percent","throughput_ops",
        "false_sharing_factor","fairness_gini","fairness_jain",
        "fairness_maxmin","fairness_cv",
        "cpu_migrations","ctx_switches","notes"
    });

    bool run_all = (cfg.suite == "all");

    // ── Original 6 experiments ──────────────────────────────
    if (run_all || cfg.suite == "scalability") {
        std::cout << "\n══ EXPERIMENT 1 — Lock Scalability (RQ1, RQ4) ══════════\n";
        run_scalability_experiment(cfg, csv);
    }
    if (run_all || cfg.suite == "contention") {
        std::cout << "\n══ EXPERIMENT 2 — Contention / CS Size (RQ2, RQ6) ══════\n";
        run_contention_experiment(cfg, csv);
    }
    if (run_all || cfg.suite == "false_sharing") {
        std::cout << "\n══ EXPERIMENT 3 — False Sharing (RQ3) ══════════════════\n";
        run_false_sharing_experiment(cfg, csv);
    }
    if (run_all || cfg.suite == "fairness") {
        std::cout << "\n══ EXPERIMENT 4 — Lock Fairness / Gini (RQ5) ═══════════\n";
        run_fairness_experiment(cfg, csv);
    }
    if (run_all || cfg.suite == "latency") {
        std::cout << "\n══ EXPERIMENT 5 — Latency Distribution (RQ1) ═══════════\n";
        run_latency_distribution_experiment(cfg, csv);
    }
    if (run_all || cfg.suite == "throughput") {
        std::cout << "\n══ EXPERIMENT 6 — Throughput Saturation (RQ1, RQ2) ═════\n";
        run_throughput_saturation_experiment(cfg, csv);
    }

    // ── New experiments (SSP RQ7–RQ10) ─────────────────────
#ifndef NO_OPENMP
    if (run_all || cfg.suite == "openmp") {
        std::cout << "\n══ EXPERIMENT 7 — OpenMP vs Manual Locks (RQ7) ═════════\n";
        run_openmp_experiment(cfg, csv);
    }
#endif

    if (run_all || cfg.suite == "workloads") {
        std::cout << "\n══ EXPERIMENT 8 — Concurrent Hash Table (RQ6, RQ7) ═════\n";
        run_hashtable_experiment(cfg, csv);

        std::cout << "\n══ EXPERIMENT 9 — Producer–Consumer Queue (RQ6) ════════\n";
        run_producer_consumer_experiment(cfg, csv);

        std::cout << "\n══ EXPERIMENT 10 — Parallel Graph Processing (RQ7) ═════\n";
        run_graph_experiment(cfg, csv);
    }

    csv.flush();

    std::cout << "\n✔  All experiments complete.\n";
    std::cout << "   Results  : " << cfg.output_dir << "/" << cfg.csv_file << "\n";
    std::cout << "   Platform : " << cfg.output_dir << "/platform_info.txt\n";
    std::cout << "   Graphs   : python3 scripts/visualize.py "
              << "--csv " << cfg.output_dir << "/" << cfg.csv_file
              << " --outdir " << cfg.output_dir << "/graphs\n\n";
    return 0;
}
