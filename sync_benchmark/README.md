# Synchronisation Primitives Benchmark

## Study of Different Synchronisation Mechanism Performance Among Threads

A research-grade, multi-experiment benchmark suite for comparing synchronisation
primitives on Linux/x86-64. Designed to support a peer-reviewed systems paper.

---

## Research Questions

| # | Question |
|---|----------|
| Q1 | How do synchronisation primitives **scale** with thread count? |
| Q2 | What happens when **critical-section size** changes? |
| Q3 | How much slowdown is caused by **false sharing**? |
| Q4 | How does **CPU pinning** vs OS scheduling affect performance? |
| Q5 | Which primitives provide **fairness** (low acquisition variance)? |
| Q6 | How do **kernel-space** (semaphore, mutex) vs **user-space** (spinlock, MCS) primitives compare? |

---

## Primitives Compared

| Primitive | Kind | Fairness | Space |
|-----------|------|----------|-------|
| `atomic_cas` | User (CAS loop) | None | O(1) |
| `std::mutex` | Hybrid (futex) | Weak | O(1) |
| `SpinLock` (TAS) | User | None | O(1) |
| `TicketLock` | User (FIFO) | Strong | O(1) |
| `MCS Lock` | User (queue) | FIFO | O(P) |
| `CLH Lock` | User (queue) | FIFO | O(P) |
| `Semaphore` (POSIX) | Kernel | Weak | O(1) |
| `std::shared_mutex` | Hybrid | Weak | O(1) |

---

## Experiments

### Experiment 1 — Scalability
Measures throughput and latency as thread count sweeps `1,2,4,8,16,32`.
Critical section = 0 cycles (pure lock overhead).

### Experiment 2 — Contention / CS Size
Sweeps critical-section work: `0, 100, 1000, 10000` busy-loop cycles.
Shows the crossover where spinlocks outperform mutexes (short CS)
and where they lose (long CS).

### Experiment 3 — False Sharing
Two threads increment counters on:
- **Same cache line** (false sharing)
- **Separate cache lines** (padded)

Reports a false-sharing slowdown factor.

### Experiment 4 — Fairness (Gini Coefficient)
//Note: Check for fairness measures used in other reasearch papers, use that index and gini index, compare the results, argue that gini index is better
Records per-thread acquisition counts and computes the
[Gini coefficient](https://en.wikipedia.org/wiki/Gini_coefficient):
- `0` = perfect equality (every thread acquires equally often)
- `1` = maximum starvation (one thread monopolises the lock)

### Experiment 5 — Latency Distribution
Records p50 / p95 / p99 / p99.9 tail latency at 8 threads.
Histogram data written to CSV for CDF plots.

### Experiment 6 — Throughput Saturation
Ramps threads from `1` to `2× hardware_concurrency` to show:
- Where throughput plateaus
- Oversubscription penalty
- Parallel efficiency drop-off

### Experiment 8 — Concurrent Hash Table (Real Workload)
Application-level key-value operations measured across three implementations
(`coarse mutex`, `striped rw-lock`, `lock-free`) and six scenarios:
- `balanced_uniform`
- `read_dominant`
- `write_dominant`
- `churn_delete_heavy`
- `bursty_rw`
- `hotspot_contention`

### Experiment 9 — Bounded Producer-Consumer Queue (Real Workload)
Bounded MPMC queue behavior measured across three implementations
(`mutex queue`, `lock-free ring`, `semaphore queue`) and six scenarios:
- `balanced_1to1`
- `read_dominant`
- `write_dominant`
- `bursty_backpressure`
- `hotspot_small_buffer`
- `fanin_fanout`

---

## Metrics Collected

| Metric | Description |
|--------|-------------|
| `ops_per_sec` | Aggregate operations per second |
| `mean_latency_ns` | Mean lock-acquisition time (ns) |
| `p50_ns / p95_ns / p99_ns` | Percentile latencies |
| `stddev_ns` | Standard deviation of latency |
| `cv_percent` | Coefficient of variation (%) |
| `fairness_gini` | Gini coefficient of acquisition counts |
| `false_sharing_factor` | Slowdown = padded throughput / false-shared throughput |
| `cpu_migrations` | CPU migration events (via perf_event) |
| `ctx_switches` | Context switch count |

---

## Project Layout

```
sync_benchmark/
├── main.cpp                       ← entry point, CLI, suite dispatch
├── benchmark.h                    ← config, types, constants
│
├── locks/
│   ├── spinlock.h                 ← TAS + TTAS spinlock
│   ├── ticket_lock.h              ← FIFO ticket lock
│   ├── mcs_lock.h                 ← MCS queue lock (local spinning)
│   ├── clh_lock.h                 ← CLH queue lock (predecessor spinning)
│   ├── semaphore_lock.h           ← POSIX binary + counting semaphore
│   └── lock_dispatch.h            ← unified ILock interface + factory
│
├── experiments/
│   ├── experiment_runner.h        ← generic thread-driver + stats collector
│   ├── scalability.h / .cpp       ← Experiment 1
│   ├── contention.h / .cpp        ← Experiment 2
│   ├── false_sharing.h / .cpp     ← Experiment 3
│   ├── fairness.h / .cpp          ← Experiment 4
│   ├── latency_dist.h / .cpp      ← Experiment 5
│   └── throughput_saturation.h / .cpp  ← Experiment 6
│
├── utils/
│   ├── stats.h                    ← Welford, percentiles, Gini, histogram
│   ├── timer.h                    ← CLOCK_MONOTONIC_RAW + RDTSC
│   ├── cpu_affinity.h             ← pthread_setaffinity_np wrapper
│   ├── perf_counters.h            ← perf_event_open hardware counters
│   └── csv_writer.h               ← thread-safe CSV sink
│
├── scripts/
│   ├── visualize.py               ← 14 publication figures + dashboard
│   ├── generate_synthetic_data.py ← demo data generator
│   └── requirements.txt
│
├── output/
│   ├── results.csv                ← benchmark output
│   └── graphs/                   ← generated figures (PNG + PDF)
│
├── Makefile
├── CMakeLists.txt
└── README.md
```

---

## Build

### Prerequisites
- GCC ≥ 9 or Clang ≥ 10
- Linux (for `perf_event_open` and `pthread_setaffinity_np`)
- C++17

```bash
# Release build
make

# Debug + ThreadSanitizer
make debug
```

---

## Run

```bash
# Full suite (all experiments, all primitives)
make run

# With CPU pinning (removes OS-scheduler noise)
make run_pinned

# Publication-grade false-sharing protocol
make false_sharing_publishable

# Custom configuration
./sync_bench \
  --suite      all               \
  --threads    1,2,3,4,6,8,10,12 \
  --duration   5                 \
  --warmup     2                 \
  --repetitions 10               \
  --placement  all               \
  --smt        both              \
  --outdir     output            \
  --csv        results.csv       \
  --verbose
```

### Suite options
| Suite | Description |
|-------|-------------|
| `all` | All experiments |
| `scalability` | Thread count sweep |
| `contention` | CS size sweep |
| `false_sharing` | Cache line effects |
| `fairness` | Gini acquisition analysis |
| `latency` | Tail latency distribution |
| `throughput` | Saturation curve |
| `openmp` | OpenMP synchronization suite |
| `workloads` | Real-world workload scenarios (Experiments 8/9/10) |

### Workload-only run

```bash
./sync_bench \
  --suite workloads \
  --threads 1,2,4,8,12 \
  --duration 3 \
  --repetitions 5 \
  --outdir output \
  --csv workload_results.csv
```

Scenario identity is written into the CSV `notes` field as:
`scenario=<scenario_name>`.

### Scenario-level graphs

For publication, the recommended figures are the per-scenario throughput
plots generated by:

```bash
python3 scripts/workload_profiles.py \
  --csv output/workload_publishable.csv \
  --outdir output/workload_graphs_scenarios
```

This emits one graph per scenario for both workloads, plus the compact
performance-profile summaries. The implementation rationale is documented in
[docs/workload_methodology.md](docs/workload_methodology.md).

### False-sharing publication controls
| Option | Values | Purpose |
|--------|--------|---------|
| `--placement` | `all`, `none`, `compact`, `spread` | Compare scheduler placement policies |
| `--smt` | `both`, `on`, `off` | Split SMT effects (`off` uses SMT-avoidance affinity map when possible) |

The false-sharing slowdown formula is explicit in outputs and captions:
`slowdown = padded throughput / false-shared throughput`

---

## Visualise

```bash
# Install Python dependencies
pip3 install -r scripts/requirements.txt

# Generate all 14+ figures
make graphs
# or
python3 scripts/visualize.py --csv output/results.csv --outdir output/graphs
```

### Figures Generated

| File | Description |
|------|-------------|
| `01_scalability_throughput` | Throughput (Mops/s) vs thread count |
| `02_scalability_latency` | Mean latency vs thread count |
| `03_scalability_heatmap` | Normalised throughput heatmap |
| `04_contention_throughput` | Throughput vs CS work cycles |
| `05_contention_p99_latency` | P99 latency vs CS work |
| `06_contention_crossover` | SpinLock–Mutex crossover point |
| `07_false_sharing_bar` | False-shared vs padwith 95% CI (none/compact/spread, SMT on/off
| `08_false_sharing_factor` | Slowdown factor (pinned vs OS-sched) |
| `09_fairness_gini` | Gini coefficient per primitive |
| `10_latency_violin` | Latency distribution (violin) |
| `11_latency_percentile_ladder` | P50/P95/P99 bar comparison |
| `12_throughput_saturation_csN` | Saturation curve per CS size |
| `13_efficiency_heatmap` | Parallel efficiency % heatmap |
| `14_radar_summary` | Spider chart (multi-dimensional) |
| `dashboard.html` | Interactive Plotly dashboard |

---

## Hardware Performance Counters

On Linux, add `perf stat` for additional metrics:

```bash
perf stat -e cycles,instructions,cache-misses,\
             cache-references,context-switches,\
             cpu-migrations,branch-misses \
  ./sync_bench --suite scalability --threads 1,2,4,8 --duration 3
```

The `utils/perf_counters.h` module also reads counters programmatically
via `perf_event_open()` — no root required for software events.

---

## Statistical Methodology

- **Warmup phase**: discards first N seconds to allow JIT compilation,
  TLB warm-up, and DVFS stabilisation.
- **Welford online algorithm**: numerically stable mean + variance in a
  single pass (avoids catastrophic cancellation).
- **Reservoir sampling**: collects at most 50,000 latency samples per
  thread without unbounded memory growth.at least 10× and results are
  reported as mean ± std with 95% confidence intervalsconfiguration is repeated 5× and results are
  reported as mean ± std.
- **95% Confidence Intervals**: computed via t-distribution (n-1 df).
- **Gini coefficient**: measures acquisition-count inequality across
  threads — a principled fairness metric from economics.

---

## Key Expected Results

| Finding | Explanation |
|---------|-------------|
| MCS/CLH scale best | Local spinning eliminates cache-line bouncing on unlock |
| Spinlock wins at CS=0, loses at CS=10000 | Context-switch overhead amortised over long CS |
| Semaphore has highest latency | Every acquire/release crosses the kernel boundary |
| Ticket lock has near-zero Gini | Strict FIFO ordering eliminates starvation |
| AtomicCAS has highest Gini | No fairness guarantee, fastest thread wins |
| False-sharing factor 2.5–5× | Coherence invalidations dominate memory bandwidth |

---

## Citation

If you use this benchmark in your research, please cite:

```bibtex
@misc{syncbench2024,
  title  = {Study of Different Synchronisation Mechanism Performance Among Threads},
  year   = {2024},
  note   = {Research-grade multi-experiment C++17 benchmark suite},
}
```
