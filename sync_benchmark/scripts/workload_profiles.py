#!/usr/bin/env python3
"""
scripts/workload_profiles.py
────────────────────────────
Generate publication-style figures for the real-world workloads.

Outputs:
    - one figure per scenario for the concurrent hash table
    - one figure per scenario for the bounded producer-consumer queue
    - optional workload-level performance profiles for compact summary views

The scenario figures show throughput vs thread count with one curve per
implementation. This is the most direct publication figure because it preserves
absolute performance, scalability shape, and scenario-specific behavior.

Usage:
    python3 scripts/workload_profiles.py \
        --csv output/workload_publishable.csv \
        --outdir output/workload_graphs
"""

import argparse
import os
import re
import warnings

warnings.filterwarnings("ignore")

import numpy as np
import pandas as pd
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "font.size": 11,
    "axes.titlesize": 14,
    "axes.labelsize": 12,
    "legend.fontsize": 10,
    "figure.dpi": 160,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.grid": True,
    "grid.alpha": 0.30,
    "grid.linestyle": "--",
})

PALETTE = {
    "ht_coarse_mutex": "#457B9D",
    "ht_fine_rwlock": "#2A9D8F",
    "ht_lockfree": "#E63946",
    "queue_mutex": "#457B9D",
    "queue_lockfree": "#2A9D8F",
    "queue_semaphore": "#E63946",
}

LABELS = {
    "ht_coarse_mutex": "Coarse mutex",
    "ht_fine_rwlock": "Striped RW lock",
    "ht_lockfree": "Lock-free",
    "queue_mutex": "Mutex queue",
    "queue_lockfree": "Lock-free ring",
    "queue_semaphore": "Semaphore queue",
}

LINESTYLES = {
    "ht_coarse_mutex": "-",
    "ht_fine_rwlock": "--",
    "ht_lockfree": "-.",
    "queue_mutex": "-",
    "queue_lockfree": "--",
    "queue_semaphore": "-.",
}

WORKLOADS = {
    "hashtable": {
        "title": "Concurrent Hash Table",
        "experiment": "hashtable_scenarios",
        "scenarios": [
            "balanced_uniform",
            "read_dominant",
            "write_dominant",
            "churn_delete_heavy",
            "bursty_rw",
            "hotspot_contention",
        ],
        "output": "workload_hash_profile",
    },
    "producer_consumer": {
        "title": "Bounded Producer-Consumer Queue",
        "experiment": "producer_consumer_scenarios",
        "scenarios": [
            "balanced_1to1",
            "read_dominant",
            "write_dominant",
            "bursty_backpressure",
            "hotspot_small_buffer",
            "fanin_fanout",
        ],
        "output": "workload_queue_profile",
    },
}

SCENARIO_TITLES = {
    "balanced_uniform": "Balanced mixed access",
    "read_dominant": "Read-dominant",
    "write_dominant": "Write-dominant",
    "churn_delete_heavy": "Delete-heavy churn",
    "bursty_rw": "Bursty phase behavior",
    "hotspot_contention": "Hotspot contention",
    "balanced_1to1": "Balanced producer-consumer",
    "bursty_backpressure": "Bursty backpressure",
    "hotspot_small_buffer": "Small-buffer hotspot",
    "fanin_fanout": "Fan-in / fan-out",
}

SCENARIO_ORDER = {
    "balanced_uniform": 0,
    "read_dominant": 1,
    "write_dominant": 2,
    "churn_delete_heavy": 3,
    "bursty_rw": 4,
    "hotspot_contention": 5,
    "balanced_1to1": 0,
    "bursty_backpressure": 1,
    "hotspot_small_buffer": 2,
    "fanin_fanout": 3,
}


def scenario_from_notes(notes):
    if not isinstance(notes, str):
        return None
    match = re.search(r"scenario=([A-Za-z0-9_\-]+)", notes)
    return match.group(1) if match else None


def save_figure(fig, outdir, name):
    os.makedirs(outdir, exist_ok=True)
    for ext in ("png", "pdf"):
        fig.savefig(os.path.join(outdir, f"{name}.{ext}"), bbox_inches="tight", dpi=160)
    plt.close(fig)
    print(f"  Saved {name}.png/.pdf")


def performance_profile(df, workload_name, spec, outdir):
    d = df[(df["experiment"] == spec["experiment"])].copy()
    if d.empty:
        print(f"  [skip] no rows for {workload_name}")
        return

    d["scenario"] = d["notes"].map(scenario_from_notes)
    d = d[d["scenario"].isin(spec["scenarios"])].copy()
    if d.empty:
        print(f"  [skip] no scenario-tagged rows for {workload_name}")
        return

    d["ops_per_sec"] = pd.to_numeric(d["ops_per_sec"], errors="coerce")
    d = d.dropna(subset=["ops_per_sec", "primitive", "threads", "scenario"])

    agg = (
        d.groupby(["scenario", "threads", "primitive"], as_index=False)["ops_per_sec"]
        .mean()
    )
    agg["best_ops"] = agg.groupby(["scenario", "threads"])["ops_per_sec"].transform("max")
    agg["ratio"] = agg["best_ops"] / agg["ops_per_sec"]

    primitives = list(agg["primitive"].drop_duplicates())
    if not primitives:
        print(f"  [skip] no primitives for {workload_name}")
        return

    max_tau = max(3.0, float(np.nanpercentile(agg["ratio"], 95)) * 1.15)
    taus = np.linspace(1.0, max_tau, 400)

    fig, ax = plt.subplots(figsize=(8.6, 5.6), constrained_layout=True)

    for prim in primitives:
        ratios = np.sort(agg.loc[agg["primitive"] == prim, "ratio"].to_numpy())
        if ratios.size == 0:
            continue
        y = np.searchsorted(ratios, taus, side="right") / ratios.size
        ax.plot(
            taus,
            y,
            label=LABELS.get(prim, prim),
            color=PALETTE.get(prim, "#444444"),
            linewidth=2.4,
        )

    ax.axvline(1.0, color="#666666", linestyle=":", linewidth=1.2)
    ax.set_xlim(1.0, max_tau)
    ax.set_ylim(0.0, 1.02)
    ax.set_xlabel(r"Performance ratio $\tau$  (best throughput / method throughput)")
    ax.set_ylabel("Fraction of scenario-thread configurations")
    ax.set_title(f"{spec['title']}  -  Performance Profile")
    ax.legend(loc="lower right", frameon=True, framealpha=0.92)

    subtitle = (
        f"{len(primitives)} implementations, {len(spec['scenarios'])} scenarios, "
        f"{agg['threads'].nunique()} thread counts"
    )
    ax.text(
        0.02,
        0.03,
        subtitle,
        transform=ax.transAxes,
        fontsize=9,
        color="#444444",
        bbox=dict(boxstyle="round,pad=0.25", facecolor="white", edgecolor="#dddddd", alpha=0.92),
    )

    save_figure(fig, outdir, spec["output"])


def scenario_throughput_plot(df, workload_name, spec, outdir):
    d = df[df["experiment"] == spec["experiment"]].copy()
    if d.empty:
        print(f"  [skip] no rows for {workload_name}")
        return

    d["scenario"] = d["notes"].map(scenario_from_notes)
    d = d[d["scenario"].isin(spec["scenarios"])].copy()
    if d.empty:
        print(f"  [skip] no scenario-tagged rows for {workload_name}")
        return

    d["ops_per_sec"] = pd.to_numeric(d["ops_per_sec"], errors="coerce")
    d = d.dropna(subset=["ops_per_sec", "primitive", "threads", "scenario"])

    grouped = (
        d.groupby(["scenario", "threads", "primitive"], as_index=False)
        .agg(
            ops_mean=("ops_per_sec", "mean"),
            ops_std=("ops_per_sec", "std"),
            trials=("trial", "count"),
        )
    )

    scenario_dir = os.path.join(outdir, workload_name)
    os.makedirs(scenario_dir, exist_ok=True)

    for scenario in spec["scenarios"]:
        s = grouped[grouped["scenario"] == scenario].copy()
        if s.empty:
            continue

        prims = list(s["primitive"].drop_duplicates())
        threads_sorted = sorted(s["threads"].drop_duplicates().tolist())
        max_ops = float(s["ops_mean"].max())
        upper = max_ops * 1.12 if max_ops > 0 else 1.0

        fig, ax = plt.subplots(figsize=(8.6, 5.6), constrained_layout=True)

        for prim in prims:
            grp = s[s["primitive"] == prim].sort_values("threads")
            ax.errorbar(
                grp["threads"],
                grp["ops_mean"] / 1e6,
                yerr=(grp["ops_std"].fillna(0.0) / 1e6),
                label=LABELS.get(prim, prim),
                color=PALETTE.get(prim, "#444444"),
                linestyle=LINESTYLES.get(prim, "-"),
                marker="o",
                linewidth=2.2,
                markersize=6,
                capsize=3,
                alpha=0.95,
            )

        ax.set_xlim(min(threads_sorted), max(threads_sorted))
        ax.set_ylim(0, upper / 1e6)
        ax.set_xlabel("Thread count")
        ax.set_ylabel("Throughput (Mops/sec)")
        ax.set_title(f"{spec['title']}  -  {SCENARIO_TITLES.get(scenario, scenario)}")
        ax.set_xticks(threads_sorted)
        ax.legend(loc="best", frameon=True, framealpha=0.92)
        ax.text(
            0.02,
            0.03,
            f"{len(prims)} implementations, {int(s['trials'].max())} repetitions per point",
            transform=ax.transAxes,
            fontsize=9,
            color="#444444",
            bbox=dict(boxstyle="round,pad=0.25", facecolor="white", edgecolor="#dddddd", alpha=0.92),
        )

        save_figure(fig, scenario_dir, f"{workload_name}_{scenario}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--csv", required=True)
    parser.add_argument("--outdir", required=True)
    args = parser.parse_args()

    df = pd.read_csv(args.csv)
    df.columns = df.columns.str.strip()

    for workload_name, spec in WORKLOADS.items():
        scenario_throughput_plot(df, workload_name, spec, args.outdir)
        performance_profile(df, workload_name, spec, args.outdir)


if __name__ == "__main__":
    main()