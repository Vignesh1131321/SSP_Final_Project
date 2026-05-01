/**
 * mpi/mpi_bench_main.cpp
 * ──────────────────────
 * Standalone entry point for the MPI benchmark suite.
 * Compiled and run separately from the main sync_bench binary.
 *
 * Build:
 *   mpicxx -DUSE_MPI -std=c++17 -O3 -I.. \
 *          mpi_bench_main.cpp -o mpi_bench
 *
 * Run:
 *   mpirun -np 4 ./mpi_bench --duration 2 --reps 5
 *   mpirun -np 8 ./mpi_bench --duration 3 --reps 5
 */
#define USE_MPI
#include "../benchmark.h"
#include "../utils/csv_writer.h"
#include "../utils/timer.h"
#include "mpi_bench.h"

#include <mpi.h>
#include <iostream>
#include <string>
#include <cstdlib>

#ifdef _WIN32
#  include <direct.h>
   static void make_dir(const std::string& p) { _mkdir(p.c_str()); }
#else
#  include <sys/stat.h>
   static void make_dir(const std::string& p) { mkdir(p.c_str(), 0755); }
#endif

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int rank = 0, nranks = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nranks);

    BenchmarkConfig cfg;
    cfg.duration_sec  = 2.0;
    cfg.warmup_sec    = 0.5;
    cfg.repetitions   = 5;
    cfg.output_dir    = "output";
    cfg.csv_file      = "mpi_results.csv";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--duration"  && i+1 < argc) cfg.duration_sec = std::stod(argv[++i]);
        if (a == "--reps"      && i+1 < argc) cfg.repetitions  = std::stoi(argv[++i]);
        if (a == "--outdir"    && i+1 < argc) cfg.output_dir   = argv[++i];
    }

    if (rank == 0) {
        make_dir(cfg.output_dir);
        std::cout << "\n══ MPI Synchronisation Benchmark ═══════════════════\n";
        std::cout << "   Ranks    : " << nranks << "\n";
        std::cout << "   Duration : " << cfg.duration_sec << "s per trial\n";
        std::cout << "   Reps     : " << cfg.repetitions << "\n\n";
    }

    CsvWriter csv(cfg.output_dir + "/" + cfg.csv_file);
    if (rank == 0) {
        csv.write_header({"suite","experiment","primitive","threads",
                          "cs_cycles","trial","ops_per_sec","mean_latency_ns",
                          "p50_ns","p95_ns","p99_ns","stddev_ns","cv_percent",
                          "throughput_ops","false_sharing_factor",
                          "fairness_gini","cpu_migrations","ctx_switches",
                          "notes"});
    }

    run_mpi_experiment(cfg, csv);

    if (rank == 0) {
        csv.flush();
        std::cout << "\n✔  MPI results → " << cfg.output_dir
                  << "/" << cfg.csv_file << "\n\n";
    }

    MPI_Finalize();
    return 0;
}
