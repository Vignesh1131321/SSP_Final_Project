#!/usr/bin/env python3
"""
Generate synthetic-but-realistic benchmark results CSV.
Models real lock behaviour based on published literature.
"""
import numpy as np
import pandas as pd
import os

rng = np.random.default_rng(42)

PRIMITIVES = ["atomic_cas","mutex","spinlock","ticket_lock",
              "mcs_lock","clh_lock","semaphore","rwlock"]

# Base single-thread ops/sec (ops/sec, empirically realistic values)
BASE_OPS = {
    "atomic_cas":  52_000_000,
    "mutex":        8_000_000,
    "spinlock":    45_000_000,
    "ticket_lock": 38_000_000,
    "mcs_lock":    35_000_000,
    "clh_lock":    37_000_000,
    "semaphore":    2_500_000,
    "rwlock":       6_500_000,
}

# Scaling efficiency at each thread count (relative to linear ideal)
# Models: contention, cache coherence, kernel overhead
SCALE_EFF = {
    "atomic_cas":  {1:1.0, 2:0.55, 4:0.30, 8:0.18, 16:0.10, 32:0.06},
    "mutex":       {1:1.0, 2:0.80, 4:0.55, 8:0.35, 16:0.20, 32:0.12},
    "spinlock":    {1:1.0, 2:0.60, 4:0.32, 8:0.17, 16:0.09, 32:0.05},
    "ticket_lock": {1:1.0, 2:0.70, 4:0.45, 8:0.28, 16:0.16, 32:0.09},
    "mcs_lock":    {1:1.0, 2:0.85, 4:0.72, 8:0.60, 16:0.48, 32:0.38},
    "clh_lock":    {1:1.0, 2:0.84, 4:0.71, 8:0.59, 16:0.47, 32:0.37},
    "semaphore":   {1:1.0, 2:0.75, 4:0.50, 8:0.30, 16:0.18, 32:0.10},
    "rwlock":      {1:1.0, 2:0.78, 4:0.52, 8:0.33, 16:0.19, 32:0.11},
}

# Gini coefficient by primitive (0=fair, 1=starved)
GINI_BASE = {
    "atomic_cas":  0.42,
    "mutex":       0.18,
    "spinlock":    0.45,
    "ticket_lock": 0.03,
    "mcs_lock":    0.02,
    "clh_lock":    0.02,
    "semaphore":   0.20,
    "rwlock":      0.22,
}

# Single-thread mean latency (ns)
LATENCY_NS = {
    "atomic_cas":  18,
    "mutex":       120,
    "spinlock":    22,
    "ticket_lock": 26,
    "mcs_lock":    29,
    "clh_lock":    27,
    "semaphore":   400,
    "rwlock":      155,
}

rows = []

# ── Scalability ───────────────────────────────────────────────
for prim in PRIMITIVES:
    for threads in [1, 2, 4, 8, 16, 32]:
        eff = SCALE_EFF[prim].get(threads, 0.05)
        base_lat = LATENCY_NS[prim]
        for trial in range(5):
            noise  = rng.normal(1.0, 0.03)
            ops    = BASE_OPS[prim] * eff * threads * noise
            # latency grows with contention
            lat_mean = base_lat * (1 + 0.35 * (threads-1)) * rng.normal(1.0, 0.05)
            lat_sd   = lat_mean * 0.25 * rng.uniform(0.8, 1.2)
            rows.append(dict(
                suite="scalability", experiment="scalability",
                primitive=prim, threads=threads, cs_cycles=0, trial=trial,
                ops_per_sec=max(1e4, ops),
                mean_latency_ns=lat_mean,
                p50_ns=lat_mean * 0.92,
                p95_ns=lat_mean * 1.60,
                p99_ns=lat_mean * 2.20,
                stddev_ns=lat_sd,
                cv_percent=(lat_sd / lat_mean * 100) if lat_mean > 0 else 0,
                throughput_ops=int(ops * 2),
                false_sharing_factor=0, fairness_gini=0,
                cpu_migrations=-1, ctx_switches=-1,
                notes="os-sched",
            ))

# ── Contention (CS size sweep) ────────────────────────────────
CS_SIZES = [0, 100, 1000, 10000]
# Multiplier showing how each lock responds to long CS
# Spinlock converges with mutex at long CS; semaphore is unaffected
CS_SCALE = {
    "atomic_cas":  [1.0, 0.85, 0.55, 0.20],
    "mutex":       [1.0, 0.95, 0.80, 0.60],
    "spinlock":    [1.0, 0.80, 0.45, 0.15],
    "ticket_lock": [1.0, 0.90, 0.72, 0.48],
    "mcs_lock":    [1.0, 0.92, 0.78, 0.55],
    "clh_lock":    [1.0, 0.92, 0.78, 0.56],
    "semaphore":   [1.0, 0.93, 0.82, 0.68],
    "rwlock":      [1.0, 0.94, 0.83, 0.70],
}
for prim in PRIMITIVES:
    for i, cs in enumerate(CS_SIZES):
        scale = CS_SCALE[prim][i]
        for trial in range(5):
            noise = rng.normal(1.0, 0.03)
            ops   = BASE_OPS[prim] * 0.30 * scale * noise  # 8 threads
            lat   = LATENCY_NS[prim] * (1 + cs/500) * rng.normal(1.0, 0.04)
            lat_sd = lat * 0.3
            rows.append(dict(
                suite="contention", experiment="cs_size",
                primitive=prim, threads=8, cs_cycles=cs, trial=trial,
                ops_per_sec=max(1e4, ops),
                mean_latency_ns=lat,
                p50_ns=lat*0.90, p95_ns=lat*1.65, p99_ns=lat*2.30,
                stddev_ns=lat_sd,
                cv_percent=(lat_sd/lat*100) if lat>0 else 0,
                throughput_ops=int(ops*2),
                false_sharing_factor=0, fairness_gini=0,
                cpu_migrations=-1, ctx_switches=-1, notes="",
            ))

# ── False sharing ─────────────────────────────────────────────
for n in [2, 4, 8]:
    for pin in [False, True]:
        # False-shared is 2.5-5x slower
        factor = rng.uniform(2.5, 5.0)
        ops_pad = 800_000_000.0 * rng.normal(1.0, 0.05)
        ops_fs  = ops_pad / factor
        for layout, ops in [("false_shared", ops_fs), ("padded", ops_pad)]:
            name = f"{layout}_t{n}"
            rows.append(dict(
                suite="false_sharing", experiment="false_sharing",
                primitive=name, threads=n, cs_cycles=0, trial=0,
                ops_per_sec=ops,
                mean_latency_ns=0, p50_ns=0, p95_ns=0, p99_ns=0,
                stddev_ns=0, cv_percent=0, throughput_ops=int(ops*2),
                false_sharing_factor=factor if layout=="false_shared" else 0,
                fairness_gini=0,
                cpu_migrations=-1, ctx_switches=-1,
                notes="pinned" if pin else "os-sched",
            ))

# ── Fairness ──────────────────────────────────────────────────
for prim in PRIMITIVES:
    for threads in [2, 4, 8]:
        base_gini = GINI_BASE[prim]
        # Gini increases slightly with more threads for unfair locks
        gini = base_gini * (1 + 0.08 * (threads - 2))
        for trial in range(5):
            g = min(0.99, max(0, gini * rng.normal(1.0, 0.08)))
            rows.append(dict(
                suite="fairness", experiment="fairness",
                primitive=prim, threads=threads, cs_cycles=100, trial=trial,
                ops_per_sec=0, mean_latency_ns=0,
                p50_ns=0, p95_ns=0, p99_ns=0, stddev_ns=0, cv_percent=0,
                throughput_ops=0, false_sharing_factor=0,
                fairness_gini=g,
                cpu_migrations=-1, ctx_switches=-1, notes="",
            ))

# ── Latency distribution ──────────────────────────────────────
for prim in PRIMITIVES:
    lat = LATENCY_NS[prim]
    lat_sd = lat * 0.4
    rows.append(dict(
        suite="latency_dist", experiment="latency_distribution",
        primitive=prim, threads=8, cs_cycles=0, trial=0,
        ops_per_sec=BASE_OPS[prim]*0.3,
        mean_latency_ns=lat,
        p50_ns=lat * 0.88,
        p95_ns=lat * 1.80,
        p99_ns=lat * 2.80,
        stddev_ns=lat_sd,
        cv_percent=(lat_sd/lat*100),
        throughput_ops=int(BASE_OPS[prim]*0.3*2),
        false_sharing_factor=0, fairness_gini=0,
        cpu_migrations=-1, ctx_switches=-1, notes="",
    ))

# ── Throughput saturation ─────────────────────────────────────
HW = 8  # simulate 8-core machine
SAT_PRIMS = ["mutex","spinlock","ticket_lock","mcs_lock","atomic_cas"]
for prim in SAT_PRIMS:
    for cs in [0, 500]:
        for threads in [1, 2, 3, 4, 6, 8, 12, 16]:
            # model: rises to hw_concurrency then drops
            if threads <= HW:
                eff = SCALE_EFF[prim].get(threads, SCALE_EFF[prim].get(8, 0.35))
            else:
                # oversubscription penalty
                eff = SCALE_EFF[prim].get(8, 0.35) * (HW / threads) * 0.85
            cs_factor = 1.0 / (1 + cs/2000)
            for trial in range(5):
                noise = rng.normal(1.0, 0.03)
                ops = BASE_OPS[prim] * eff * threads * cs_factor * noise
                lat = LATENCY_NS[prim] * (1 + 0.3 * (threads - 1))
                lat_sd = lat * 0.28
                rows.append(dict(
                    suite="throughput", experiment="throughput_saturation",
                    primitive=prim, threads=threads, cs_cycles=cs, trial=trial,
                    ops_per_sec=max(1e4, ops),
                    mean_latency_ns=lat,
                    p50_ns=lat*0.90, p95_ns=lat*1.65, p99_ns=lat*2.40,
                    stddev_ns=lat_sd,
                    cv_percent=(lat_sd/lat*100) if lat>0 else 0,
                    throughput_ops=int(ops*2),
                    false_sharing_factor=0, fairness_gini=0,
                    cpu_migrations=-1, ctx_switches=-1, notes="",
                ))

df = pd.DataFrame(rows)
os.makedirs("output", exist_ok=True)
df.to_csv("output/results.csv", index=False)
print(f"Generated {len(df)} rows → output/results.csv")
print(f"Suites: {df['suite'].unique().tolist()}")

# ── OpenMP constructs ─────────────────────────────────────────
OMP_BASE = {
    "omp_critical":  5_000_000,
    "omp_atomic":   48_000_000,
    "omp_reduction": 3_500_000,
    "omp_barrier":   6_000_000,
}
OMP_SCALE = {
    "omp_critical":  {1:1.0, 2:0.70, 4:0.45, 8:0.28, 16:0.15, 32:0.08},
    "omp_atomic":    {1:1.0, 2:0.62, 4:0.35, 8:0.20, 16:0.12, 32:0.07},
    "omp_reduction": {1:1.0, 2:1.85, 4:3.40, 8:6.00, 16:9.00, 32:12.0},
    "omp_barrier":   {1:1.0, 2:0.82, 4:0.68, 8:0.55, 16:0.40, 32:0.28},
}
for prim in OMP_BASE:
    for threads in [1,2,4,8,16,32]:
        eff = OMP_SCALE[prim].get(threads, 0.05)
        for trial in range(5):
            noise = rng.normal(1.0, 0.04)
            ops = OMP_BASE[prim] * eff * noise
            lat = 1e9 / max(ops,1)
            rows.append(dict(
                suite="openmp", experiment="openmp_constructs",
                primitive=prim, threads=threads, cs_cycles=0, trial=trial,
                ops_per_sec=max(1e3, ops),
                mean_latency_ns=lat, p50_ns=lat*0.90,
                p95_ns=lat*1.65, p99_ns=lat*2.40,
                stddev_ns=lat*0.3, cv_percent=30,
                throughput_ops=int(ops*2),
                false_sharing_factor=0, fairness_gini=0,
                cpu_migrations=-1, ctx_switches=-1, notes="openmp_framework",
            ))

# ── Hash table workload ───────────────────────────────────────
HT_BASE = {"ht_coarse_mutex":4_000_000, "ht_fine_rwlock":28_000_000,
           "ht_lockfree":55_000_000}
HT_SCALE = {
    "ht_coarse_mutex": {1:1.0,2:0.55,4:0.32,8:0.18,16:0.10,32:0.06},
    "ht_fine_rwlock":  {1:1.0,2:1.70,4:3.00,8:4.80,16:6.50,32:7.50},
    "ht_lockfree":     {1:1.0,2:1.90,4:3.50,8:6.20,16:9.00,32:11.0},
}
for prim,base in HT_BASE.items():
    for threads in [1,2,4,8,16,32]:
        eff = HT_SCALE[prim].get(threads, 0.05)
        for trial in range(5):
            ops = base * eff * rng.normal(1,0.04)
            lat = 1e9/max(ops,1)
            rows.append(dict(
                suite="workload", experiment="hashtable",
                primitive=prim, threads=threads, cs_cycles=0, trial=trial,
                ops_per_sec=max(1e3,ops),
                mean_latency_ns=lat, p50_ns=lat*0.88,
                p95_ns=lat*1.70, p99_ns=lat*2.50,
                stddev_ns=lat*0.35, cv_percent=35,
                throughput_ops=int(ops*2),
                false_sharing_factor=0, fairness_gini=0,
                cpu_migrations=-1, ctx_switches=-1, notes="hashtable",
            ))

# ── Producer-consumer queue ───────────────────────────────────
Q_BASE = {"queue_mutex":2_500_000, "queue_lockfree":45_000_000,
          "queue_semaphore":1_800_000}
Q_SCALE = {
    "queue_mutex":    {1:1.0,2:0.75,4:0.50,8:0.32,16:0.18,32:0.10},
    "queue_lockfree": {1:1.0,2:1.80,4:3.20,8:5.50,16:7.50,32:9.00},
    "queue_semaphore":{1:1.0,2:0.80,4:0.55,8:0.35,16:0.20,32:0.12},
}
for prim,base in Q_BASE.items():
    for threads in [2,4,8,16,32]:
        eff = Q_SCALE[prim].get(threads, 0.05)
        ops = base * eff * rng.normal(1,0.05)
        lat = 1e9/max(ops,1)
        rows.append(dict(
            suite="workload", experiment="producer_consumer",
            primitive=prim, threads=threads, cs_cycles=0, trial=0,
            ops_per_sec=max(1e3,ops),
            mean_latency_ns=lat, p50_ns=lat*0.90,
            p95_ns=lat*1.70, p99_ns=lat*2.50,
            stddev_ns=lat*0.32, cv_percent=32,
            throughput_ops=int(ops*2),
            false_sharing_factor=0, fairness_gini=0,
            cpu_migrations=-1, ctx_switches=-1, notes="prod_consumer_queue",
        ))

# ── Graph workload ────────────────────────────────────────────
GRAPH_ALGOS = {
    "bfs_mutex":     (800, -0.15),   # (base_ms, speedup_exponent)
    "bfs_atomic":    (600, -0.25),
    "pagerank_mutex":(1200,-0.10),
    "pagerank_atomic":(900,-0.22),
}
for algo,(base_ms,exp) in GRAPH_ALGOS.items():
    exp_name = "graph_bfs" if "bfs" in algo else "graph_pagerank"
    for threads in [1,2,4,8,16]:
        ms = base_ms * (threads ** exp) * rng.normal(1, 0.05)
        for trial in range(5):
            rows.append(dict(
                suite="workload", experiment=exp_name,
                primitive=algo, threads=threads, cs_cycles=0, trial=trial,
                ops_per_sec=0, mean_latency_ns=ms*1e6,
                p50_ns=0, p95_ns=0, p99_ns=0, stddev_ns=0, cv_percent=0,
                throughput_ops=0,
                false_sharing_factor=0, fairness_gini=0,
                cpu_migrations=-1, ctx_switches=-1, notes=exp_name,
            ))

# ── Hyper-threading ───────────────────────────────────────────
HT_CONFIGS = [
    ("ht_under_physical", 4,  0.90),
    ("ht_all_physical",   4,  0.85),
    ("ht_logical_max",    8,  0.75),
    ("ht_oversubscribed", 16, 0.45),
]
for prim in ["mutex","spinlock","mcs_lock","atomic_cas"]:
    for label, threads, eff_mult in HT_CONFIGS:
        base_eff = SCALE_EFF[prim].get(min(threads,32), 0.10)
        for trial in range(5):
            ops = BASE_OPS[prim] * base_eff * threads * eff_mult * rng.normal(1,0.04)
            lat = LATENCY_NS[prim] * (1 + 0.3*(threads-1))
            rows.append(dict(
                suite="hardware", experiment="hyperthreading",
                primitive=prim, threads=threads, cs_cycles=0, trial=trial,
                ops_per_sec=max(1e4,ops),
                mean_latency_ns=lat, p50_ns=lat*0.9, p95_ns=lat*1.7,
                p99_ns=lat*2.4, stddev_ns=lat*0.3, cv_percent=30,
                throughput_ops=int(ops*2),
                false_sharing_factor=0, fairness_gini=0,
                cpu_migrations=-1, ctx_switches=-1, notes=f"{label} stride=2",
            ))

# ── NUMA ──────────────────────────────────────────────────────
for prim in ["mutex","spinlock","mcs_lock","atomic_cas"]:
    for numa_cfg, penalty in [("numa_same_node nodes=2",1.0),
                               ("numa_cross_node nodes=2",0.45)]:
        for trial in range(5):
            ops = BASE_OPS[prim] * 0.35 * 8 * penalty * rng.normal(1,0.05)
            lat = LATENCY_NS[prim] * (1/penalty) * rng.normal(1,0.05)
            rows.append(dict(
                suite="hardware", experiment="numa",
                primitive=prim, threads=8, cs_cycles=0, trial=trial,
                ops_per_sec=max(1e4,ops),
                mean_latency_ns=lat, p50_ns=lat*0.9, p95_ns=lat*1.7,
                p99_ns=lat*2.4, stddev_ns=lat*0.3, cv_percent=30,
                throughput_ops=int(ops*2),
                false_sharing_factor=0, fairness_gini=0,
                cpu_migrations=-1, ctx_switches=-1, notes=numa_cfg,
            ))

# ── Energy (RAPL simulated) ────────────────────────────────────
ENERGY_NJ = {   # nanojoules per lock operation (realistic estimates)
    "atomic_cas":  0.8,
    "mutex":        8.5,
    "spinlock":     2.2,
    "ticket_lock":  3.0,
    "mcs_lock":     3.5,
    "clh_lock":     3.3,
    "semaphore":   18.0,
    "rwlock":      10.0,
}
for prim in PRIMITIVES:
    nj = ENERGY_NJ[prim]
    for trial in range(5):
        ops = BASE_OPS[prim] * 0.30 * rng.normal(1,0.04)
        rows.append(dict(
            suite="energy", experiment="rapl_energy",
            primitive=prim, threads=8, cs_cycles=0, trial=trial,
            ops_per_sec=ops, mean_latency_ns=LATENCY_NS[prim],
            p50_ns=0, p95_ns=0, p99_ns=0, stddev_ns=0, cv_percent=0,
            throughput_ops=int(ops*2),
            false_sharing_factor=0, fairness_gini=0,
            cpu_migrations=-1, ctx_switches=-1,
            notes=f"energy_uj={ops*nj/1000:.1f};nj_per_op={nj*rng.normal(1,0.05):.3f};avg_power_w={ops*nj/1e12*2:.3f}",
        ))

df = pd.DataFrame(rows)
os.makedirs("output", exist_ok=True)
df.to_csv("output/results.csv", index=False)
print(f"Generated {len(df)} rows → output/results.csv")
print(f"Suites: {sorted(df['suite'].unique().tolist())}")
