# ============================================================
# Makefile — sync_benchmark (full SSP edition)
# ============================================================

CXX      ?= g++
MPICXX   ?= mpicxx
STD      := -std=c++17
WARN     := -Wall -Wextra -Wpedantic
RELEASE  := -O3 -march=native -DNDEBUG
DEBUG    := -g -O0 -fsanitize=thread,undefined

# Detect OpenMP availability
OMP_FLAG := $(shell $(CXX) -fopenmp -x c++ -E - </dev/null >/dev/null 2>&1 && echo "-fopenmp" || echo "-DNO_OPENMP")

CXXFLAGS  := $(STD) $(WARN) $(RELEASE) $(OMP_FLAG)
LDFLAGS   := -lpthread

SRCS := main.cpp \
        locks/lock_dispatch.cpp \
        experiments/scalability.cpp \
        experiments/contention.cpp  \
        experiments/false_sharing.cpp \
        experiments/fairness.cpp    \
        experiments/latency_dist.cpp \
        experiments/throughput_saturation.cpp

TARGET     := sync_bench
MPI_TARGET := mpi_bench

.PHONY: all debug clean run run_pinned false_sharing_publishable graphs workload_graphs mpi mpi_run help

all: $(TARGET)

$(TARGET): $(SRCS)
	@mkdir -p output
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build OK  →  ./$(TARGET)   (OpenMP flag: $(OMP_FLAG))"

debug:
	$(CXX) $(STD) $(WARN) $(DEBUG) -o $(TARGET)_debug $(SRCS) $(LDFLAGS)
	@echo "Debug build  →  ./$(TARGET)_debug"

mpi:
	$(MPICXX) -DUSE_MPI $(STD) $(RELEASE) -I. \
	    mpi/mpi_bench_main.cpp -o $(MPI_TARGET) $(LDFLAGS)
	@echo "MPI build OK  →  ./$(MPI_TARGET)"

clean:
	rm -f $(TARGET) $(TARGET)_debug $(MPI_TARGET)
	rm -f output/*.csv output/platform_info.txt

# ── Full run (all suites) ────────────────────────────────────
run: all
	@mkdir -p output
	./$(TARGET) \
	  --suite all \
	  --threads 1,2,3,4,6,8,10,12 \
	  --duration 5 \
	  --warmup 2 \
	  --repetitions 10 \
	  --outdir output \
	  --csv results.csv

# ── Pinned run (removes scheduler noise) ───────────────────
run_pinned: all
	@mkdir -p output
	./$(TARGET) \
	  --suite all \
	  --threads 1,2,3,4,6,8,10,12 \
	  --duration 5 \
	  --warmup 2 \
	  --repetitions 10 \
	  --pin-cpus \
	  --outdir output \
	  --csv results_pinned.csv

# ── Publication-grade false-sharing run ─────────────────────
false_sharing_publishable: all
	@mkdir -p output
	./$(TARGET) \
	  --suite false_sharing \
	  --threads 1,2,3,4,6,8,10,12 \
	  --duration 5 \
	  --warmup 2 \
	  --repetitions 10 \
	  --placement all \
	  --smt both \
	  --outdir output \
	  --csv false_sharing_publishable.csv

# ── Individual suites ────────────────────────────────────────
scalability: all
	./$(TARGET) --suite scalability --threads 1,2,4,8,16,32 --duration 3

openmp: all
	./$(TARGET) --suite openmp --threads 1,2,4,8 --duration 2

workloads: all
	./$(TARGET) --suite workloads --threads 1,2,4,8 --duration 2

# ── MPI run ──────────────────────────────────────────────────
mpi_run: mpi
	mpirun -np 4 ./$(MPI_TARGET) --duration 2 --reps 5

# ── Visualise ────────────────────────────────────────────────
graphs:
	python3 scripts/visualize.py \
	  --csv output/results.csv \
	  --outdir output/graphs
	@echo "Graphs → output/graphs/"

workload_graphs: all
	python3 scripts/workload_profiles.py \
	  --csv output/workload_publishable.csv \
	  --outdir output/workload_graphs_scenarios
	@echo "Workload graphs → output/workload_graphs_scenarios/"

help:
	./$(TARGET) --help
