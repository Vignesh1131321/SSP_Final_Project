#!/usr/bin/env python3
"""
scripts/visualize.py
────────────────────
Research-grade visualisation for the sync_benchmark suite.

Generates 13 publication-quality figures:
  01_scalability_throughput.pdf/png
  02_scalability_latency.pdf/png
  03_scalability_heatmap.pdf/png
  04_contention_throughput.pdf/png
  05_contention_latency.pdf/png
  06_contention_crossover.pdf/png
  07_false_sharing_bar.pdf/png
  08_false_sharing_factor.pdf/png
  09_fairness_gini.pdf/png
  10_latency_distribution_box.pdf/png
  11_latency_distribution_violin.pdf/png
  12_throughput_saturation.pdf/png
  13_efficiency_heatmap.pdf/png
  dashboard.html  ← interactive Plotly dashboard

Usage:
  python3 scripts/visualize.py --csv output/results.csv --outdir output/graphs
"""

import argparse
import os
import sys
import re
import warnings
warnings.filterwarnings("ignore")

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
from matplotlib.colors import LogNorm
from matplotlib.gridspec import GridSpec
import matplotlib.patches as mpatches
import seaborn as sns

# ── Try Plotly for interactive dashboard ─────────────────────
try:
    import plotly.graph_objects as go
    import plotly.express as px
    from plotly.subplots import make_subplots
    HAS_PLOTLY = True
except ImportError:
    HAS_PLOTLY = False
    print("  [info] plotly not found — skipping interactive dashboard")

# ── Aesthetic config ─────────────────────────────────────────
PALETTE = {
    "atomic_cas":  "#E63946",
    "mutex":       "#457B9D",
    "spinlock":    "#F4A261",
    "ticket_lock": "#2A9D8F",
    "mcs_lock":    "#8338EC",
    "clh_lock":    "#FB5607",
    "semaphore":   "#3A86FF",
    "rwlock":      "#06D6A0",
}
MARKERS = {
    "atomic_cas":  "o",
    "mutex":       "s",
    "spinlock":    "^",
    "ticket_lock": "D",
    "mcs_lock":    "v",
    "clh_lock":    "P",
    "semaphore":   "X",
    "rwlock":      "*",
}
LABEL = {
    "atomic_cas":  "Atomic CAS",
    "mutex":       "std::mutex",
    "spinlock":    "SpinLock (TAS)",
    "ticket_lock": "Ticket Lock",
    "mcs_lock":    "MCS Lock",
    "clh_lock":    "CLH Lock",
    "semaphore":   "Semaphore",
    "rwlock":      "RW Lock",
}

def pname(p):
    return LABEL.get(p, p)

def color(p):
    return PALETTE.get(p, "#888888")

def marker(p):
    return MARKERS.get(p, "o")

# ── Style ─────────────────────────────────────────────────────
plt.rcParams.update({
    "font.family":       "DejaVu Sans",
    "font.size":         11,
    "axes.titlesize":    13,
    "axes.labelsize":    12,
    "xtick.labelsize":   10,
    "ytick.labelsize":   10,
    "legend.fontsize":   9,
    "figure.dpi":        150,
    "axes.spines.top":   False,
    "axes.spines.right": False,
    "axes.grid":         True,
    "grid.alpha":        0.35,
    "grid.linestyle":    "--",
})

def savefig(fig, outdir, name):
    os.makedirs(outdir, exist_ok=True)
    for ext in ("png", "pdf"):
        path = os.path.join(outdir, f"{name}.{ext}")
        fig.savefig(path, bbox_inches="tight", dpi=150)
    plt.close(fig)
    print(f"  Saved {name}.png/.pdf")

# ═══════════════════════════════════════════════════════════════
# LOAD & PREPROCESS
# ═══════════════════════════════════════════════════════════════
def load_data(csv_path):
    df = pd.read_csv(csv_path)
    df.columns = df.columns.str.strip()
    # Normalise primitive names
    df["primitive"] = df["primitive"].str.strip()
    # Aggregate over trials: mean per (suite,experiment,primitive,threads,cs_cycles)
    numeric_cols = ["ops_per_sec","mean_latency_ns","p50_ns","p95_ns","p99_ns",
                    "stddev_ns","cv_percent","throughput_ops",
                    "false_sharing_factor","fairness_gini","fairness_jain",
                    "fairness_maxmin","fairness_cv"]
    for c in numeric_cols:
        if c in df.columns:
            df[c] = pd.to_numeric(df[c], errors="coerce")
    return df

def agg(df, group_cols, value_cols=None):
    """Mean + std aggregation."""
    if value_cols is None:
        value_cols = ["ops_per_sec","mean_latency_ns","p50_ns","p95_ns",
                  "p99_ns","stddev_ns","cv_percent","fairness_gini",
                  "fairness_jain","fairness_maxmin","fairness_cv",
                  "false_sharing_factor"]
    value_cols = [c for c in value_cols if c in df.columns]
    return df.groupby(group_cols)[value_cols].agg(["mean","std"]).reset_index()

# ═══════════════════════════════════════════════════════════════
# FIGURE 01 — Scalability: Throughput vs Thread Count
# ═══════════════════════════════════════════════════════════════
def fig_scalability_throughput(df, outdir):
    d = df[df["suite"] == "scalability"].copy()
    if d.empty:
        print("  [skip] no scalability data"); return

    g = d.groupby(["primitive","threads"])["ops_per_sec"].agg(["mean","std"]).reset_index()

    fig, ax = plt.subplots(figsize=(9, 5.5))
    for prim, grp in g.groupby("primitive"):
        grp = grp.sort_values("threads")
        ax.errorbar(grp["threads"], grp["mean"]/1e6,
                    yerr=grp["std"]/1e6,
                    label=pname(prim),
                    color=color(prim), marker=marker(prim),
                    linewidth=1.8, markersize=6, capsize=3)

    ax.set_xlabel("Thread Count")
    ax.set_ylabel("Throughput (Mops/sec)")
    ax.set_title("Fig 01 — Lock Scalability: Throughput vs Thread Count")
    ax.set_xscale("log", base=2)
    ax.xaxis.set_major_formatter(mticker.ScalarFormatter())
    ax.legend(bbox_to_anchor=(1.01, 1), loc="upper left", framealpha=0.85)
    fig.tight_layout()
    savefig(fig, outdir, "01_scalability_throughput")

# ═══════════════════════════════════════════════════════════════
# FIGURE 02 — Scalability: Mean Latency vs Thread Count
# ═══════════════════════════════════════════════════════════════
def fig_scalability_latency(df, outdir):
    d = df[df["suite"] == "scalability"].copy()
    if d.empty: return

    g = d.groupby(["primitive","threads"])["mean_latency_ns"].agg(["mean","std"]).reset_index()

    fig, ax = plt.subplots(figsize=(9, 5.5))
    for prim, grp in g.groupby("primitive"):
        grp = grp.sort_values("threads")
        ax.errorbar(grp["threads"], grp["mean"],
                    yerr=grp["std"],
                    label=pname(prim),
                    color=color(prim), marker=marker(prim),
                    linewidth=1.8, markersize=6, capsize=3)

    ax.set_xlabel("Thread Count")
    # In visualize.py, fig_scalability_latency() function
    ax.set_yscale('log', base=10)
    ax.set_ylabel('Mean Lock-Acquisition Latency (ns, log scale)')
    #ax.set_ylabel("Mean Lock-Acquisition Latency (ns)")
    ax.set_title("Fig 02 — Lock Scalability: Mean Latency vs Thread Count")
    ax.set_xscale("log", base=2)
    ax.xaxis.set_major_formatter(mticker.ScalarFormatter())
    ax.legend(bbox_to_anchor=(1.01, 1), loc="upper left", framealpha=0.85)
    fig.tight_layout()
    savefig(fig, outdir, "02_scalability_latency")

# ═══════════════════════════════════════════════════════════════
# FIGURE 03 — Scalability Heatmap (normalised throughput)
# ═══════════════════════════════════════════════════════════════
def fig_scalability_heatmap(df, outdir):
    d = df[df["suite"] == "scalability"].copy()
    if d.empty: return

    pivot = d.groupby(["primitive","threads"])["ops_per_sec"].mean().unstack("threads")
    # Normalise each primitive to its single-thread value
    norm = pivot.div(pivot[pivot.columns[0]], axis=0)

    fig, ax = plt.subplots(figsize=(10, max(4, len(pivot)*0.55 + 1)))
    yticklabels = [pname(p) for p in norm.index]
    sns.heatmap(norm, ax=ax, cmap="RdYlGn", center=1.0, linewidths=0.5,
                annot=True, fmt=".2f", yticklabels=yticklabels,
                xticklabels=[str(c) for c in norm.columns])
    ax.set_xlabel("Thread Count")
    ax.set_ylabel("Primitive")
    ax.set_title("Fig 03 — Normalised Throughput Heatmap\n"
                 "(1.0 = single-thread baseline; green = faster, red = slower)")
    fig.tight_layout()
    savefig(fig, outdir, "03_scalability_heatmap")

# ═══════════════════════════════════════════════════════════════
# FIGURE 04 — Contention: Throughput vs CS Size
# ═══════════════════════════════════════════════════════════════
def fig_contention_throughput(df, outdir):
    d = df[df["suite"] == "contention"].copy()
    if d.empty: return

    g = d.groupby(["primitive","cs_cycles"])["ops_per_sec"].agg(["mean","std"]).reset_index()

    fig, ax = plt.subplots(figsize=(9, 5.5))
    for prim, grp in g.groupby("primitive"):
        grp = grp.sort_values("cs_cycles")
        ax.errorbar(grp["cs_cycles"], grp["mean"]/1e6,
                    yerr=grp["std"]/1e6,
                    label=pname(prim), color=color(prim),
                    marker=marker(prim), linewidth=1.8, markersize=6, capsize=3)

    ax.set_xlabel("Critical-Section Work (busy-loop cycles)")
    ax.set_ylabel("Throughput (Mops/sec)")
    ax.set_title("Fig 04 — Contention Sensitivity: Throughput vs CS Work")
    ax.set_xscale("symlog", linthresh=1)
    ax.xaxis.set_major_formatter(mticker.ScalarFormatter())
    ax.legend(bbox_to_anchor=(1.01, 1), loc="upper left", framealpha=0.85)
    fig.tight_layout()
    savefig(fig, outdir, "04_contention_throughput")

# ═══════════════════════════════════════════════════════════════
# FIGURE 05 — Contention: Latency vs CS Size
# ═══════════════════════════════════════════════════════════════
def fig_contention_latency(df, outdir):
    d = df[df["suite"] == "contention"].copy()
    if d.empty: return

    g = d.groupby(["primitive","cs_cycles"])["p99_ns"].agg(["mean","std"]).reset_index()

    fig, ax = plt.subplots(figsize=(9, 5.5))
    for prim, grp in g.groupby("primitive"):
        grp = grp.sort_values("cs_cycles")
        ax.errorbar(grp["cs_cycles"], grp["mean"],
                    yerr=grp["std"],
                    label=pname(prim), color=color(prim),
                    marker=marker(prim), linewidth=1.8, markersize=6, capsize=3)

    ax.set_xlabel("Critical-Section Work (busy-loop cycles)")
    ax.set_ylabel("P99 Lock Latency (ns, log scale)")
    ax.set_title("Fig 05 — Contention Sensitivity: P99 Latency vs CS Work")
    ax.set_yscale("log", base=10)
    ax.set_xscale("symlog", linthresh=1)
    ax.set_xlim(left=0)
    ax.xaxis.set_major_formatter(mticker.ScalarFormatter())
    ax.legend(bbox_to_anchor=(1.01, 1), loc="upper left", framealpha=0.85)
    fig.tight_layout()
    savefig(fig, outdir, "05_contention_p99_latency")

# ═══════════════════════════════════════════════════════════════
# FIGURE 06 — Crossover: Spinlock vs Mutex
# ═══════════════════════════════════════════════════════════════
def fig_contention_crossover(df, outdir):
    d = df[df["suite"] == "contention"].copy()
    if d.empty: return

    target = ["spinlock", "mutex"]
    d = d[d["primitive"].isin(target)]
    if d.empty: return

    g = d.groupby(["primitive","cs_cycles"])["ops_per_sec"].mean().unstack("primitive")
    if "spinlock" not in g.columns or "mutex" not in g.columns: return

    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(g.index, g["spinlock"]/1e6, color=color("spinlock"),
            marker=marker("spinlock"), linewidth=2, label="SpinLock (TAS)")
    ax.plot(g.index, g["mutex"]/1e6, color=color("mutex"),
            marker=marker("mutex"), linewidth=2, label="std::mutex")

    # Mark crossover (including interpolation between sampled points)
    diff = g["spinlock"] - g["mutex"]
    sign_changes = diff[diff.shift(1) * diff < 0].index.tolist()
    for x in sign_changes:
        idx = g.index.get_loc(x)
        x0 = g.index[idx - 1] if idx > 0 else x
        d0 = diff.iloc[idx - 1] if idx > 0 else diff.loc[x]
        d1 = diff.loc[x]

        if idx > 0 and d1 != d0:
            # Linear interpolation between consecutive sampled CS points
            x_cross = float(x0 + (0 - d0) * (x - x0) / (d1 - d0))
        else:
            x_cross = float(x)

        ax.axvline(x_cross, color="grey", linestyle=":", linewidth=1.4)
        ax.text(x_cross * 1.03, ax.get_ylim()[0] * 1.1,
                f"Crossover\n≈{int(round(x_cross))} cyc",
                fontsize=9, color="grey")

    ax.set_xlabel("Critical-Section Work (busy-loop cycles)")
    ax.set_ylabel("Throughput (Mops/sec)")
    ax.set_title("Fig 06 — SpinLock vs Mutex: Crossover Point\n"
                 "(spinlock wins for short CS; mutex wins for long CS)")
    ax.set_xscale("symlog", linthresh=1)
    ax.set_xlim(left=0)
    ax.xaxis.set_major_formatter(mticker.ScalarFormatter())
    ax.legend()
    fig.tight_layout()
    savefig(fig, outdir, "06_contention_crossover")

# ═══════════════════════════════════════════════════════════════
# FALSE-SHARING HELPERS
# ═══════════════════════════════════════════════════════════════
def _parse_false_sharing_meta(d):
    d = d.copy()
    d["notes"] = d.get("notes", "").fillna("")
    d["layout"] = d["primitive"].apply(
        lambda p: "False-Shared" if "false_shared" in str(p) else "Padded")
    d["n_threads"] = pd.to_numeric(d.get("threads"), errors="coerce")
    d = d.dropna(subset=["n_threads"])
    d["n_threads"] = d["n_threads"].astype(int)

    placement = d["notes"].str.extract(r"placement=([^;]+)", expand=False)
    smt_mode = d["notes"].str.extract(r"smt=([^;]+)", expand=False)
    d["placement"] = placement.fillna("unknown")
    d["smt_mode"] = smt_mode.fillna("unknown")
    return d


def _ci95(std, n):
    n = np.maximum(n, 1)
    return 1.96 * (std / np.sqrt(n))


def export_false_sharing_table(df, outdir):
    d = df[df["suite"] == "false_sharing"].copy()
    if d.empty:
        return
    d = _parse_false_sharing_meta(d)

    g = d.groupby(["n_threads", "placement", "smt_mode", "layout"])["ops_per_sec"].agg(["mean", "std"]).reset_index()
    pivot = g.pivot_table(
        index=["n_threads", "placement", "smt_mode"],
        columns="layout",
        values=["mean", "std"],
        aggfunc="first",
    )

    for col in [("mean", "False-Shared"), ("std", "False-Shared"), ("mean", "Padded"), ("std", "Padded")]:
        if col not in pivot.columns:
            pivot[col] = np.nan

    table = pd.DataFrame({
        "threads": pivot.index.get_level_values(0),
        "placement": pivot.index.get_level_values(1),
        "smt_mode": pivot.index.get_level_values(2),
        "false_shared_mean_ops": pivot[("mean", "False-Shared")].values,
        "false_shared_std_ops": pivot[("std", "False-Shared")].values,
        "padded_mean_ops": pivot[("mean", "Padded")].values,
        "padded_std_ops": pivot[("std", "Padded")].values,
    })
    table["slowdown_mean"] = table["padded_mean_ops"] / table["false_shared_mean_ops"]
    table["formula"] = "slowdown = padded throughput / false-shared throughput"

    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, "false_sharing_summary_table_from_csv.csv")
    table.sort_values(["smt_mode", "placement", "threads"]).to_csv(path, index=False)
    print("  Saved false_sharing_summary_table_from_csv.csv")


# ═══════════════════════════════════════════════════════════════
# FIGURE 07 — False Sharing Throughput with 95% CI
# ═══════════════════════════════════════════════════════════════
def fig_false_sharing_bar(df, outdir):
    d = df[df["suite"] == "false_sharing"].copy()
    if d.empty:
        return

    d = _parse_false_sharing_meta(d)
    g = d.groupby(["smt_mode", "placement", "layout", "n_threads"])["ops_per_sec"].agg(["mean", "std", "count"]).reset_index()
    g["ci95"] = _ci95(g["std"].fillna(0.0), g["count"].fillna(1.0))

    smt_values = [v for v in ["on", "off"] if v in set(g["smt_mode"])]
    if not smt_values:
        smt_values = sorted(g["smt_mode"].unique().tolist())

    fig, axes = plt.subplots(1, len(smt_values), figsize=(7 * len(smt_values), 5.2), sharey=True)
    if len(smt_values) == 1:
        axes = [axes]

    style = {
        ("False-Shared", "none"): ("#E63946", "o"),
        ("Padded", "none"): ("#2A9D8F", "o"),
        ("False-Shared", "compact"): ("#C1121F", "s"),
        ("Padded", "compact"): ("#2A9D8F", "s"),
        ("False-Shared", "spread"): ("#9D0208", "^"),
        ("Padded", "spread"): ("#1B7F79", "^"),
    }

    for ax, smt in zip(axes, smt_values):
        sub = g[g["smt_mode"] == smt].copy()
        if sub.empty:
            continue

        for (layout, placement), grp in sub.groupby(["layout", "placement"]):
            grp = grp.sort_values("n_threads")
            c, m = style.get((layout, placement), ("#555555", "o"))
            ax.errorbar(
                grp["n_threads"],
                grp["mean"] / 1e9,
                yerr=grp["ci95"] / 1e9,
                marker=m,
                linewidth=1.8,
                capsize=3,
                color=c,
                label=f"{layout}, {placement}"
            )

        ax.set_xlabel("Thread Count")
        ax.set_title(f"SMT {smt.upper()}")
        ax.grid(True, linestyle="--", alpha=0.35)

    axes[0].set_ylabel("Throughput (Gops/sec)")
    fig.suptitle(
        "Fig 07 — False-Sharing Throughput with 95% CI\n"
        "Formula in this experiment: slowdown = padded throughput / false-shared throughput"
    )
    handles, labels = axes[0].get_legend_handles_labels()
    if handles:
        axes[0].legend(handles, labels, fontsize=8, framealpha=0.85)
    fig.tight_layout()
    savefig(fig, outdir, "07_false_sharing_bar")


# ═══════════════════════════════════════════════════════════════
# FIGURE 08 — False-Sharing Slowdown with 95% CI
# ═══════════════════════════════════════════════════════════════
def fig_false_sharing_factor(df, outdir):
    d = df[df["suite"] == "false_sharing"].copy()
    if d.empty:
        return

    d = _parse_false_sharing_meta(d)
    d = d[d["layout"] == "False-Shared"].copy()
    d = d[d["false_sharing_factor"] > 0].copy()
    if d.empty:
        return

    g = d.groupby(["smt_mode", "placement", "n_threads"])["false_sharing_factor"].agg(["mean", "std", "count"]).reset_index()
    g["ci95"] = _ci95(g["std"].fillna(0.0), g["count"].fillna(1.0))

    smt_values = [v for v in ["on", "off"] if v in set(g["smt_mode"])]
    if not smt_values:
        smt_values = sorted(g["smt_mode"].unique().tolist())

    fig, axes = plt.subplots(1, len(smt_values), figsize=(7 * len(smt_values), 4.8), sharey=True)
    if len(smt_values) == 1:
        axes = [axes]

    placement_colors = {
        "none": "#457B9D",
        "compact": "#F4A261",
        "spread": "#6D597A",
    }

    for ax, smt in zip(axes, smt_values):
        sub = g[g["smt_mode"] == smt].copy()
        for placement, grp in sub.groupby("placement"):
            grp = grp.sort_values("n_threads")
            ax.errorbar(
                grp["n_threads"],
                grp["mean"],
                yerr=grp["ci95"],
                marker="o",
                linewidth=1.8,
                capsize=3,
                color=placement_colors.get(placement, "#555555"),
                label=placement,
            )
        ax.axhline(1.0, color="red", linestyle="--", linewidth=1, label="No Slowdown")
        ax.set_xlabel("Thread Count")
        ax.set_title(f"SMT {smt.upper()}")

    axes[0].set_ylabel("False-Sharing Slowdown (95% CI)")
    fig.suptitle(
        "Fig 08 — False-Sharing Slowdown Factor with 95% CI\n"
        "slowdown = padded throughput / false-shared throughput"
    )
    handles, labels = axes[0].get_legend_handles_labels()
    if handles:
        axes[0].legend(handles, labels, fontsize=8, framealpha=0.85)
    fig.tight_layout()
    savefig(fig, outdir, "08_false_sharing_factor")

# ═══════════════════════════════════════════════════════════════
# FIGURE 09 — Fairness: Gini Coefficient
# ═══════════════════════════════════════════════════════════════
def fig_fairness_gini(df, outdir):
    d = df[df["suite"] == "fairness"].copy()
    if d.empty: return

    g = d.groupby(["primitive","threads"])["fairness_gini"].agg(["mean","std"]).reset_index()

    thread_counts = sorted(g["threads"].unique())
    n_tc = len(thread_counts)
    fig, axes = plt.subplots(1, n_tc, figsize=(5 * n_tc, 5), sharey=True)
    if n_tc == 1: axes = [axes]

    for ax, tc in zip(axes, thread_counts):
        sub = g[g["threads"] == tc].sort_values("mean")
        bars = ax.barh([pname(p) for p in sub["primitive"]],
                       sub["mean"],
                       xerr=sub["std"],
                       color=[color(p) for p in sub["primitive"]],
                       alpha=0.85, capsize=3)
        ax.axvline(0, color="green", linestyle="--", linewidth=1.2,
                   label="Perfect Fairness (0)")
        ax.set_xlabel("Gini Coefficient")
        ax.set_title(f"{tc} Threads")
        ax.set_xlim(-0.05, 1.0)

    axes[0].set_ylabel("Primitive")
    fig.suptitle("Fig 09 — Lock Fairness: Gini Coefficient\n"
                 "(0 = perfect fairness, 1 = maximum starvation)",
                 fontsize=13)
    fig.tight_layout()
    savefig(fig, outdir, "09_fairness_gini")


def fig_fairness_jain(df, outdir):
    d = df[df["suite"] == "fairness"].copy()
    if d.empty or "fairness_jain" not in d.columns:
        return

    g = d.groupby(["primitive","threads"])["fairness_jain"].agg(["mean","std"]).reset_index()

    thread_counts = sorted(g["threads"].unique())
    n_tc = len(thread_counts)
    fig, axes = plt.subplots(1, n_tc, figsize=(5 * n_tc, 5), sharey=True)
    if n_tc == 1:
        axes = [axes]

    for ax, tc in zip(axes, thread_counts):
        sub = g[g["threads"] == tc].sort_values("mean", ascending=False)
        ax.barh([pname(p) for p in sub["primitive"]],
                sub["mean"],
                xerr=sub["std"],
                color=[color(p) for p in sub["primitive"]],
                alpha=0.85, capsize=3)
        ax.axvline(1.0, color="green", linestyle="--", linewidth=1.2,
                   label="Perfect Fairness (1)")
        ax.set_xlabel("Jain Fairness Index")
        ax.set_title(f"{tc} Threads")
        ax.set_xlim(0.0, 1.05)

    axes[0].set_ylabel("Primitive")
    fig.suptitle("Fig 09b — Lock Fairness: Jain Index\n"
                 "(1 = perfect fairness, lower = more inequality)",
                 fontsize=13)
    fig.tight_layout()
    savefig(fig, outdir, "09b_fairness_jain")


def _extract_thread_counts(notes):
    if pd.isna(notes):
        return np.nan
    m = re.search(r"synthetic_counts:([0-9\|]+)", str(notes))
    if not m:
        return np.nan
    vals = [float(x) for x in m.group(1).split("|") if x != ""]
    return vals


def _controlled_subset(df):
    d = df[df["suite"] == "fairness"].copy()
    if d.empty:
        return d
    d = d[d["experiment"].isin(["fairness_synthetic", "fairness_synthetic_scale"])].copy()
    return d


def _real_locks_subset(df):
    d = df[df["suite"] == "fairness"].copy()
    if d.empty:
        return d
    d = d[d["experiment"] == "fairness"].copy()
    return d


def fig_fairness_controlled_comparison(df, outdir):
    d = _controlled_subset(df)
    if d.empty:
        print("  [skip] no controlled synthetic fairness data")
        return

    d = d[d["threads"] == 8].copy()
    if d.empty:
        print("  [skip] no 8-thread controlled synthetic fairness data")
        return

    g = d.groupby("primitive")[["fairness_jain", "fairness_gini"]].mean().reset_index()
    g = g.sort_values("fairness_gini")
    x = np.arange(len(g))
    w = 0.36

    fig, ax = plt.subplots(figsize=(11.5, 5.8))
    ax.bar(x - w / 2, g["fairness_jain"], width=w, color="#457B9D", label="Jain")
    ax.bar(x + w / 2, g["fairness_gini"], width=w, color="#E63946", label="Gini")

    labels = [p.replace("_", " ") for p in g["primitive"]]
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=25, ha="right")
    ax.set_ylim(0, 1.05)
    ax.set_ylabel("Index Value")
    ax.set_title(
        "Fig 09c — Controlled Synthetic Scenarios (8 Threads): Jain vs Gini\n"
        "Explicitly highlighting Jain saturation vs Gini sensitivity"
    )
    ax.legend()

    fail_mask = (g["fairness_jain"] > 0.9) & (g["fairness_gini"] > 0.1)
    for _, row in g[fail_mask].iterrows():
        i = g.index[g["primitive"] == row["primitive"]][0]
        ax.annotate(
            "Jain fails to detect\ninequality in this scenario",
            xy=(i, max(row["fairness_jain"], row["fairness_gini"])),
            xytext=(0, 18),
            textcoords="offset points",
            ha="center",
            fontsize=8,
            color="#9D0208",
            arrowprops=dict(arrowstyle="->", color="#9D0208", lw=1),
        )

    fig.tight_layout()
    savefig(fig, outdir, "09c_controlled_jain_vs_gini")


def fig_fairness_controlled_distributions(df, outdir):
    d = _controlled_subset(df)
    if d.empty:
        print("  [skip] no controlled synthetic fairness data for distributions")
        return

    d = d[d["threads"] == 8].copy()
    if d.empty:
        return

    d["counts"] = d["notes"].apply(_extract_thread_counts)
    d = d[d["counts"].notna()].copy()
    if d.empty:
        print("  [skip] synthetic per-thread counts missing in notes")
        return

    wanted = ["B_mild_imbalance", "D_severe_starvation"]
    sub = d[d["primitive"].isin(wanted)].copy()
    if sub.empty:
        sub = d.sort_values("fairness_gini", ascending=False).head(2)

    fig, axes = plt.subplots(1, len(sub), figsize=(6 * len(sub), 4.8), sharey=True)
    if len(sub) == 1:
        axes = [axes]

    for ax, (_, row) in zip(axes, sub.iterrows()):
        vals = row["counts"]
        ax.bar(np.arange(len(vals)), vals, color="#457B9D", alpha=0.9)
        ax.set_xlabel("Thread ID")
        ax.set_title(
            f"{row['primitive']}\n"
            f"Jain={row['fairness_jain']:.3f}, Gini={row['fairness_gini']:.3f}"
        )
        ax.grid(True, axis="y", linestyle="--", alpha=0.35)

    axes[0].set_ylabel("Per-thread acquisition count")
    fig.suptitle(
        "Fig 09d — Controlled Scenario Distribution Evidence\n"
        "Same-family Jain values can hide large distributional inequality"
    )
    fig.tight_layout()
    savefig(fig, outdir, "09d_controlled_per_thread_distributions")


def fig_fairness_locks_comparison(df, outdir):
    d = _real_locks_subset(df)
    if d.empty:
        print("  [skip] no real lock fairness data")
        return

    lock_order = ["mutex", "spinlock", "mcs_lock", "semaphore"]
    d = d[d["primitive"].isin(lock_order)].copy()
    if d.empty:
        print("  [skip] no 4-lock data in fairness suite")
        return

    g = d.groupby(["threads", "primitive"])["fairness_gini"].agg(["mean", "std"]).reset_index()
    threads = sorted(g["threads"].unique())
    fig, axes = plt.subplots(1, len(threads), figsize=(5.5 * len(threads), 4.8), sharey=True)
    if len(threads) == 1:
        axes = [axes]

    for ax, tc in zip(axes, threads):
        sub = g[g["threads"] == tc].copy()
        sub["primitive"] = pd.Categorical(sub["primitive"], categories=lock_order, ordered=True)
        sub = sub.sort_values("primitive")
        x = np.arange(len(sub))
        ax.bar(
            x,
            sub["mean"],
            width=0.6,
            color=[color(p) for p in sub["primitive"]],
            alpha=0.9,
            yerr=sub["std"],
            capsize=3,
            edgecolor="none",
        )
        ax.set_xticks(x)
        ax.set_xticklabels([pname(p) for p in sub["primitive"]], rotation=25, ha="right")
        ax.set_ylim(0, 1.05)
        ax.set_title(f"{tc} Threads")
        ax.grid(True, axis="y", linestyle="--", alpha=0.35)

    axes[0].set_ylabel("Gini Coefficient")
    fig.suptitle(
        "Fig 09e — Real Locks (4 Primitives): Gini Coefficient Only\n"
        "(0 = perfect fairness, 1 = maximum starvation)"
    )
    fig.tight_layout(rect=[0, 0, 1, 0.92])
    savefig(fig, outdir, "09e_locks_gini_only")


def export_fairness_failure_table(df, outdir):
    d = _controlled_subset(df)
    if d.empty:
        return

    d = d[d["threads"] == 8].copy()
    if d.empty:
        return

    g = d.groupby("primitive")[["fairness_jain", "fairness_gini"]].mean().reset_index()

    def obs(r):
        if r["fairness_jain"] > 0.9 and r["fairness_gini"] > 0.1:
            return "Jain hides inequality"
        if r["fairness_gini"] >= 0.5:
            return "Severe inequality/starvation"
        if r["fairness_gini"] <= 0.05:
            return "Near-perfect fairness"
        return "Moderate inequality"

    g["observation"] = g.apply(obs, axis=1)
    g = g.rename(columns={
        "primitive": "scenario",
        "fairness_jain": "jain_8t",
        "fairness_gini": "gini_8t",
    }).sort_values("gini_8t")

    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, "fairness_jain_vs_gini_summary.csv")
    g.to_csv(path, index=False)
    print("  Saved fairness_jain_vs_gini_summary.csv")

# ═══════════════════════════════════════════════════════════════
# FIGURE 10 — Latency Distribution: Empirical CDF
# ═══════════════════════════════════════════════════════════════
def _parse_hist_from_notes(notes):
    if pd.isna(notes):
        return None
    m = re.search(r"hist=([^\"]+)$", str(notes))
    if not m:
        return None
    pairs = []
    for tok in m.group(1).split(";"):
        if not tok or ":" not in tok:
            continue
        e, c = tok.split(":", 1)
        try:
            edge = float(e)
            cnt = float(c)
        except ValueError:
            continue
        if cnt > 0:
            pairs.append((edge, cnt))
    if not pairs:
        return None
    pairs.sort(key=lambda x: x[0])
    edges = np.array([p[0] for p in pairs], dtype=float)
    counts = np.array([p[1] for p in pairs], dtype=float)
    cdf = np.cumsum(counts) / counts.sum()
    return edges, cdf


def fig_latency_boxplot(df, outdir):
    d = df[df["suite"] == "latency_dist"].copy()
    if d.empty:
        return

    # Show high-contention operating point where tail behavior matters most.
    t_focus = int(d["threads"].max())
    d = d[d["threads"] == t_focus].copy()
    if d.empty:
        return

    fig, ax = plt.subplots(figsize=(10.5, 6.2))

    prim_order = (d.groupby("primitive")["p50_ns"].mean().sort_values().index.tolist())
    for prim in prim_order:
        grp = d[d["primitive"] == prim]
        c = color(prim)

        # Plot each trial's histogram-derived CDF as a faint line.
        cdf_series = []
        for _, row in grp.iterrows():
            parsed = _parse_hist_from_notes(row.get("notes", ""))
            if parsed is None:
                continue
            x, y = parsed
            cdf_series.append((x, y))
            ax.step(x, y, where="post", color=c, alpha=0.18, linewidth=1.2)

        # Add mean CDF + trial variability band on a shared log grid.
        if cdf_series:
            x_min = max(1.0, min(float(s[0][0]) for s in cdf_series))
            x_max = max(float(s[0][-1]) for s in cdf_series)
            if x_max > x_min:
                grid = np.logspace(np.log10(x_min), np.log10(x_max), 250)
                ys = []
                for xs, ys_i in cdf_series:
                    ys.append(np.interp(grid, xs, ys_i, left=0.0, right=1.0))
                Y = np.vstack(ys)
                y_mean = Y.mean(axis=0)
                y_lo = np.clip(Y.mean(axis=0) - Y.std(axis=0), 0.0, 1.0)
                y_hi = np.clip(Y.mean(axis=0) + Y.std(axis=0), 0.0, 1.0)
                ax.fill_between(grid, y_lo, y_hi, color=c, alpha=0.12)
                ax.plot(grid, y_mean, color=c, linewidth=2.0)

        # Overlay mean percentile markers for quick comparison.
        p50 = grp["p50_ns"].mean()
        p95 = grp["p95_ns"].mean()
        p99 = grp["p99_ns"].mean()
        ax.axvline(p50, color=c, linestyle="-", linewidth=1.4, alpha=0.9)
        ax.axvline(p95, color=c, linestyle="--", linewidth=1.2, alpha=0.9)
        ax.axvline(p99, color=c, linestyle=":", linewidth=1.2, alpha=0.9)
        ax.plot([], [], color=c, linewidth=2.0, label=pname(prim))

    ax.set_xscale("log")
    ax.set_ylim(0.0, 1.0)
    ax.set_xlabel("Lock Acquisition Latency (ns)")
    ax.set_ylabel("Empirical CDF")
    ax.set_title(
        f"Fig 10 — Empirical CDF from Histogram Bins (threads={t_focus})\n"
        "Faint lines = per-trial CDF; vertical lines = mean P50/P95/P99"
    )
    ax.yaxis.set_major_locator(mticker.MultipleLocator(0.1))
    ax.text(0.01, 0.02,
        "Limitation: CDF reconstructed from histogram bins in CSV notes (not raw samples).",
        transform=ax.transAxes, ha="left", va="bottom", fontsize=8,
        bbox=dict(boxstyle="round,pad=0.2", fc="white", ec="#777", alpha=0.8))
    ax.legend(loc="lower right", framealpha=0.9)
    fig.tight_layout()
    savefig(fig, outdir, "10_latency_cdf")

# ═══════════════════════════════════════════════════════════════
# FIGURE 11 — Latency Percentile Ladder
# ═══════════════════════════════════════════════════════════════
def fig_latency_percentile_ladder(df, outdir):
    d = df[df["suite"] == "latency_dist"].copy()
    if d.empty: return

    g = d.groupby("primitive")[["p50_ns","p95_ns","p99_ns"]].mean()
    g = g.sort_values("p50_ns")

    ci = None
    if d["trial"].nunique() > 1:
     agg = d.groupby("primitive")[["p50_ns","p95_ns","p99_ns"]].agg(["std", "count"])
     ci = pd.DataFrame(index=g.index)
     for q in ["p50_ns", "p95_ns", "p99_ns"]:
         std = agg[(q, "std")].reindex(g.index).fillna(0.0)
         cnt = agg[(q, "count")].reindex(g.index).replace(0, np.nan)
         ci[q] = 1.96 * (std / np.sqrt(cnt))

    x = np.arange(len(g))
    w = 0.25
    fig, ax = plt.subplots(figsize=(11, 5.5))
    ax.bar(x - w, g["p50_ns"], w, label="P50", alpha=0.85,
        color=[color(p) for p in g.index],
        yerr=(ci["p50_ns"].values if ci is not None else None),
        capsize=(3 if ci is not None else 0), ecolor="black", linewidth=0)
    ax.bar(x,     g["p95_ns"], w, label="P95", alpha=0.65,
        color=[color(p) for p in g.index],
        yerr=(ci["p95_ns"].values if ci is not None else None),
        capsize=(3 if ci is not None else 0), ecolor="black", linewidth=0)
    ax.bar(x + w, g["p99_ns"], w, label="P99", alpha=0.45,
        color=[color(p) for p in g.index],
        yerr=(ci["p99_ns"].values if ci is not None else None),
        capsize=(3 if ci is not None else 0), ecolor="black", linewidth=0)

    ax.set_xticks(x)
    ax.set_xticklabels([pname(p) for p in g.index], rotation=30, ha="right")
    ax.set_ylabel("Latency (ns)")
    ax.set_yscale("log")
    ci_label = "95% CI across trials" if ci is not None else "No CI (single trial)"
    ax.set_title("Fig 11 — Tail Latency Comparison: P50 / P95 / P99\n"
                 f"(lower is better; large P99 → high jitter) | {ci_label}")

    ratio = (g["p99_ns"] / g["p50_ns"]).sort_values()
    ratio_text = ", ".join([f"{pname(p)}={r:.2f}x" for p, r in ratio.items()])
    ax.text(0.01, 0.99, f"P99/P50 jitter ratio: {ratio_text}",
         transform=ax.transAxes, ha="left", va="top", fontsize=9,
         bbox=dict(boxstyle="round,pad=0.25", fc="white", ec="#666", alpha=0.85))

    # Inset panel to make low-latency region (MCS advantage) visually explicit.
    inset = ax.inset_axes([0.60, 0.08, 0.36, 0.36])
    inset.bar(x - w, g["p50_ns"], w, alpha=0.85, color=[color(p) for p in g.index])
    inset.bar(x,     g["p95_ns"], w, alpha=0.65, color=[color(p) for p in g.index])
    inset.bar(x + w, g["p99_ns"], w, alpha=0.45, color=[color(p) for p in g.index])
    inset.set_xticks([])
    inset.set_ylim(0, 1800)
    inset.set_title("Inset: 0-1800 ns", fontsize=8)
    inset.grid(True, alpha=0.25, linestyle="--")

    ax.legend()
    fig.tight_layout()
    savefig(fig, outdir, "11_latency_percentile_ladder")

# ═══════════════════════════════════════════════════════════════
# FIGURE 12 — Throughput Saturation Curves
# ═══════════════════════════════════════════════════════════════
def fig_throughput_saturation(df, outdir):
    d = df[df["suite"] == "throughput"].copy()
    if d.empty: return

    for cs_val, grp in d.groupby("cs_cycles"):
        g = grp.groupby(["primitive","threads"])["ops_per_sec"].mean().unstack("threads")
        if g.empty: continue

        fig, ax = plt.subplots(figsize=(9, 5.5))
        for prim in g.index:
            row = g.loc[prim].dropna()
            if len(row) < 2: continue
            ax.plot(row.index, row.values/1e6,
                    label=pname(prim), color=color(prim),
                    marker=marker(prim), linewidth=2, markersize=6)

        hw = int(d["threads"].max() // 2)
        if hw > 0:
            ax.axvline(hw, color="grey", linestyle=":", linewidth=1.2)
            ax.text(hw * 1.02, ax.get_ylim()[0], f"hw_concurrency={hw}",
                    fontsize=8, color="grey")

        ax.set_xlabel("Thread Count")
        ax.set_ylabel("Throughput (Mops/sec)")
        ax.set_title(f"Fig 12 — Throughput Saturation Curve  (CS = {cs_val} cycles)\n"
                     "Vertical line = hardware thread count")
        ax.legend(bbox_to_anchor=(1.01, 1), loc="upper left", framealpha=0.85)
        fig.tight_layout()
        savefig(fig, outdir, f"12_throughput_saturation_cs{cs_val}")

# ═══════════════════════════════════════════════════════════════
# FIGURE 13 — Parallel Efficiency Heatmap
# ═══════════════════════════════════════════════════════════════
def fig_efficiency_heatmap(df, outdir):
    d = df[df["suite"] == "throughput"].copy()
    if d.empty: return

    d = d[d["cs_cycles"] == d["cs_cycles"].min()]
    pivot = d.groupby(["primitive","threads"])["ops_per_sec"].mean().unstack("threads")
    if pivot.empty: return

    # Normalise to single-thread
    t1_col = pivot.columns[0]
    eff = pivot.div(pivot[t1_col], axis=0).div(pivot.columns / pivot.columns[0], axis=1) * 100

    fig, ax = plt.subplots(figsize=(10, max(4, len(pivot)*0.6 + 1)))
    yticklabels = [pname(p) for p in eff.index]
    sns.heatmap(eff, ax=ax, cmap="RdYlGn", vmin=0, vmax=100,
                annot=True, fmt=".0f", linewidths=0.5,
                yticklabels=yticklabels,
                xticklabels=[str(c) for c in eff.columns])
    ax.set_xlabel("Thread Count")
    ax.set_ylabel("Primitive")
    ax.set_title("Fig 13 — Parallel Efficiency Heatmap (%)\n"
                 "(100% = linear scaling; <100% = lock overhead / contention)")
    fig.tight_layout()
    savefig(fig, outdir, "13_efficiency_heatmap")

# ═══════════════════════════════════════════════════════════════
# FIGURE 14 — Summary Radar Chart
# ═══════════════════════════════════════════════════════════════
def fig_radar_summary(df, outdir):
    """Spider chart comparing primitives across 5 dimensions."""
    scal_d = df[df["suite"] == "scalability"]
    lat_d  = df[df["suite"] == "latency_dist"]
    fair_d = df[df["suite"] == "fairness"]
    cont_d = df[df["suite"] == "contention"]

    if scal_d.empty: return

    primitives = scal_d["primitive"].unique().tolist()
    dims = ["Throughput", "Low Latency", "Fairness", "Contention\nTolerance", "Scalability"]
    N = len(dims)
    angles = np.linspace(0, 2*np.pi, N, endpoint=False).tolist()
    angles += angles[:1]

    fig, ax = plt.subplots(figsize=(8, 8),
                            subplot_kw=dict(polar=True))

    for prim in primitives:
        scores = []

        # 1. Throughput (single thread, normalized 0-1)
        t1 = scal_d[(scal_d["primitive"]==prim) & (scal_d["threads"]==1)]["ops_per_sec"]
        scores.append(float(t1.mean()) if not t1.empty else 0)

        # 2. Low Latency (inverted p99)
        p99 = lat_d[lat_d["primitive"]==prim]["p99_ns"]
        scores.append(1.0 / (float(p99.mean()) + 1) if not p99.empty else 0)

        # 3. Fairness (inverted Gini, 0=good)
        gini = fair_d[fair_d["primitive"]==prim]["fairness_gini"]
        scores.append(1.0 - float(gini.mean()) if not gini.empty else 0.5)

        # 4. Contention tolerance (ops at max cs_cycles)
        max_cs = cont_d["cs_cycles"].max() if not cont_d.empty else 0
        ct = cont_d[(cont_d["primitive"]==prim) & (cont_d["cs_cycles"]==max_cs)]["ops_per_sec"]
        scores.append(float(ct.mean()) if not ct.empty else 0)

        # 5. Scalability (throughput at max threads / single thread)
        max_t = scal_d["threads"].max()
        t_max = scal_d[(scal_d["primitive"]==prim) & (scal_d["threads"]==max_t)]["ops_per_sec"]
        t_one = scal_d[(scal_d["primitive"]==prim) & (scal_d["threads"]==1)]["ops_per_sec"]
        if not t_max.empty and not t_one.empty and float(t_one.mean()) > 0:
            scores.append(float(t_max.mean()) / float(t_one.mean()))
        else:
            scores.append(1.0)

        # Normalize all to [0,1]
        # (rough: divide by max across all primitives — done after collecting)
        scores_raw = scores
        for i in range(N):
            scores[i] = scores_raw[i]

        values = scores + scores[:1]
        ax.plot(angles, values, color=color(prim), linewidth=1.5, label=pname(prim))
        ax.fill(angles, values, color=color(prim), alpha=0.1)

    ax.set_thetagrids(np.degrees(np.array(angles[:-1])), dims, fontsize=10)
    ax.set_title("Fig 14 — Multi-Dimensional Primitive Comparison\n"
                 "(unnormalised; useful for relative shape comparison)",
                 pad=20)
    ax.legend(bbox_to_anchor=(1.3, 1.1), loc="upper left", fontsize=8)
    fig.tight_layout()
    savefig(fig, outdir, "14_radar_summary")

# ═══════════════════════════════════════════════════════════════
# PLOTLY INTERACTIVE DASHBOARD
# ═══════════════════════════════════════════════════════════════
def build_dashboard(df, outdir):
    if not HAS_PLOTLY:
        return

    fig = make_subplots(
        rows=3, cols=2,
        subplot_titles=(
            "Scalability: Throughput vs Threads",
            "Scalability: Mean Latency vs Threads",
            "Contention: Throughput vs CS Work",
            "Tail Latency (P50 / P95 / P99)",
            "Lock Fairness (Gini Coefficient)",
            "Throughput Saturation",
        ),
        vertical_spacing=0.12,
        horizontal_spacing=0.08,
    )

    palette_plotly = {k: v for k, v in PALETTE.items()}

    # ── Panel 1: Scalability throughput ──────────────────────
    scal = df[df["suite"] == "scalability"]
    if not scal.empty:
        for prim, grp in scal.groupby("primitive"):
            g = grp.groupby("threads")["ops_per_sec"].mean().reset_index()
            fig.add_trace(go.Scatter(
                x=g["threads"], y=g["ops_per_sec"]/1e6,
                mode="lines+markers", name=pname(prim),
                line=dict(color=palette_plotly.get(prim,"#888")),
                legendgroup=prim, showlegend=True,
            ), row=1, col=1)

    # ── Panel 2: Scalability latency ─────────────────────────
    if not scal.empty:
        for prim, grp in scal.groupby("primitive"):
            g = grp.groupby("threads")["mean_latency_ns"].mean().reset_index()
            fig.add_trace(go.Scatter(
                x=g["threads"], y=g["mean_latency_ns"],
                mode="lines+markers", name=pname(prim),
                line=dict(color=palette_plotly.get(prim,"#888")),
                legendgroup=prim, showlegend=False,
            ), row=1, col=2)

    # ── Panel 3: Contention throughput ───────────────────────
    cont = df[df["suite"] == "contention"]
    if not cont.empty:
        for prim, grp in cont.groupby("primitive"):
            g = grp.groupby("cs_cycles")["ops_per_sec"].mean().reset_index()
            fig.add_trace(go.Scatter(
                x=g["cs_cycles"], y=g["ops_per_sec"]/1e6,
                mode="lines+markers", name=pname(prim),
                line=dict(color=palette_plotly.get(prim,"#888")),
                legendgroup=prim, showlegend=False,
            ), row=2, col=1)

    # ── Panel 4: Percentile ladder ────────────────────────────
    lat = df[df["suite"] == "latency_dist"]
    if not lat.empty:
        g = lat.groupby("primitive")[["p50_ns","p95_ns","p99_ns"]].mean().reset_index()
        g = g.sort_values("p50_ns")
        for col_name, offset, alpha in [("p50_ns","P50",1.0),
                                        ("p95_ns","P95",0.65),
                                        ("p99_ns","P99",0.4)]:
            fig.add_trace(go.Bar(
                x=[pname(p) for p in g["primitive"]], y=g[col_name],
                name=offset, opacity=alpha,
                marker_color=[palette_plotly.get(p,"#888") for p in g["primitive"]],
                legendgroup=offset, showlegend=True,
            ), row=2, col=2)

    # ── Panel 5: Fairness Gini ────────────────────────────────
    fair = df[df["suite"] == "fairness"]
    if not fair.empty:
        g = fair.groupby(["primitive","threads"])["fairness_gini"].mean().reset_index()
        for tc, sub in g.groupby("threads"):
            fig.add_trace(go.Bar(
                x=[pname(p) for p in sub["primitive"]], y=sub["fairness_gini"],
                name=f"{tc} threads",
                marker_color=[palette_plotly.get(p,"#888") for p in sub["primitive"]],
                legendgroup=f"fair_{tc}", showlegend=True,
            ), row=3, col=1)

    # ── Panel 6: Saturation ───────────────────────────────────
    thr = df[df["suite"] == "throughput"]
    if not thr.empty:
        thr0 = thr[thr["cs_cycles"] == thr["cs_cycles"].min()]
        for prim, grp in thr0.groupby("primitive"):
            g = grp.groupby("threads")["ops_per_sec"].mean().reset_index()
            fig.add_trace(go.Scatter(
                x=g["threads"], y=g["ops_per_sec"]/1e6,
                mode="lines+markers", name=pname(prim),
                line=dict(color=palette_plotly.get(prim,"#888")),
                legendgroup=prim, showlegend=False,
            ), row=3, col=2)

    fig.update_layout(
        height=1100,
        title_text="<b>Synchronisation Primitives Benchmark — Interactive Dashboard</b>",
        title_font_size=16,
        template="plotly_white",
        legend=dict(orientation="v", x=1.02, y=1, borderwidth=1),
    )
    # Axis labels
    fig.update_xaxes(title_text="Thread Count",        row=1, col=1)
    fig.update_xaxes(title_text="Thread Count",        row=1, col=2)
    fig.update_xaxes(title_text="CS Cycles",           row=2, col=1)
    fig.update_xaxes(title_text="Primitive",           row=2, col=2)
    fig.update_xaxes(title_text="Primitive",           row=3, col=1)
    fig.update_xaxes(title_text="Thread Count",        row=3, col=2)

    fig.update_yaxes(title_text="Mops/sec",            row=1, col=1)
    fig.update_yaxes(title_text="Latency (ns)",        row=1, col=2)
    fig.update_yaxes(title_text="Mops/sec",            row=2, col=1)
    fig.update_yaxes(title_text="Latency (ns)",        row=2, col=2)
    fig.update_yaxes(title_text="Gini",                row=3, col=1)
    fig.update_yaxes(title_text="Mops/sec",            row=3, col=2)
    fig.update_yaxes(type="log",                       row=1, col=2)

    os.makedirs(outdir, exist_ok=True)
    html_path = os.path.join(outdir, "dashboard.html")
    fig.write_html(html_path, include_plotlyjs="cdn")
    print(f"  Saved dashboard.html  (open in browser for interactive view)")


# ═══════════════════════════════════════════════════════════════
# MAIN
# ═══════════════════════════════════════════════════════════════
def main():
    ap = argparse.ArgumentParser(description="Visualise sync_benchmark results")
    ap.add_argument("--csv",    default="output/results.csv")
    ap.add_argument("--outdir", default="output/graphs")
    args = ap.parse_args()

    if not os.path.exists(args.csv):
        print(f"ERROR: CSV not found: {args.csv}")
        sys.exit(1)

    print(f"\nLoading {args.csv} ...")
    df = load_data(args.csv)
    print(f"  {len(df)} rows, suites: {df['suite'].unique().tolist()}\n")

    print("Generating figures ...")

    fig_scalability_throughput(df, args.outdir)
    fig_scalability_latency(df, args.outdir)
    fig_scalability_heatmap(df, args.outdir)
    fig_contention_throughput(df, args.outdir)
    fig_contention_latency(df, args.outdir)
    fig_contention_crossover(df, args.outdir)
    fig_false_sharing_bar(df, args.outdir)
    fig_false_sharing_factor(df, args.outdir)
    export_false_sharing_table(df, args.outdir)
    fig_fairness_gini(df, args.outdir)
    fig_fairness_jain(df, args.outdir)
    fig_fairness_controlled_comparison(df, args.outdir)
    fig_fairness_controlled_distributions(df, args.outdir)
    fig_fairness_locks_comparison(df, args.outdir)
    export_fairness_failure_table(df, args.outdir)
    fig_latency_boxplot(df, args.outdir)
    fig_latency_percentile_ladder(df, args.outdir)
    fig_throughput_saturation(df, args.outdir)
    fig_efficiency_heatmap(df, args.outdir)
    fig_radar_summary(df, args.outdir)

    build_dashboard(df, args.outdir)

    print(f"\n✔  All figures saved to {args.outdir}/\n")

if __name__ == "__main__":
    main()

# ═══════════════════════════════════════════════════════════════
# FIGURE 15 — OpenMP vs Manual Locks (RQ7)
# ═══════════════════════════════════════════════════════════════
def fig_openmp_comparison(df, outdir):
    omp  = df[df["suite"] == "openmp"].copy()
    scal = df[(df["suite"] == "scalability") &
              (df["primitive"].isin(["mutex","spinlock","atomic_cas"]))].copy()
    if omp.empty: return

    g_omp  = omp.groupby(["primitive","threads"])["ops_per_sec"].mean().reset_index()
    g_scal = scal.groupby(["primitive","threads"])["ops_per_sec"].mean().reset_index()

    fig, axes = plt.subplots(1, 2, figsize=(14, 5.5))

    # Throughput comparison
    ax = axes[0]
    for prim, grp in g_omp.groupby("primitive"):
        grp = grp.sort_values("threads")
        ax.plot(grp["threads"], grp["ops_per_sec"]/1e6,
                label=prim, linestyle="--", marker="s",
                linewidth=1.8, markersize=6)
    for prim, grp in g_scal.groupby("primitive"):
        grp = grp.sort_values("threads")
        ax.plot(grp["threads"], grp["ops_per_sec"]/1e6,
                label=pname(prim), color=color(prim),
                marker=marker(prim), linewidth=1.8, markersize=6)
    ax.set_xlabel("Thread / Core Count")
    ax.set_ylabel("Throughput (Mops/sec)")
    ax.set_title("OpenMP Constructs vs Manual Locks\n(RQ7)")
    ax.legend(fontsize=8, ncol=2)

    # Latency comparison
    ax = axes[1]
    lat_omp  = omp.groupby("primitive")["mean_latency_ns"].mean().sort_values()
    lat_scal = scal.groupby("primitive")["mean_latency_ns"].mean().sort_values()
    combined = pd.concat([
        lat_omp.rename(index=lambda x: x),
        lat_scal.rename(index=lambda x: pname(x))
    ])
    combined = combined.sort_values()
    bars = ax.barh(range(len(combined)), combined.values,
                   color=["#457B9D" if "omp" in str(i) else "#E63946"
                          for i in combined.index],
                   alpha=0.85)
    ax.set_yticks(range(len(combined)))
    ax.set_yticklabels(combined.index, fontsize=9)
    ax.set_xlabel("Mean Latency (ns)")
    ax.set_title("Mean Latency: OpenMP vs Manual\n(lower = better)")
    ax.axvline(0, color="black", linewidth=0.5)

    fig.suptitle("Fig 15 — RQ7: OpenMP Framework vs Manual Locking",
                 fontsize=13, fontweight="bold")
    fig.tight_layout()
    savefig(fig, outdir, "15_openmp_vs_manual")


# ═══════════════════════════════════════════════════════════════
# FIGURE 16 — Real-World Workloads: Hash Table
# ═══════════════════════════════════════════════════════════════
def fig_workload_hashtable(df, outdir):
    d = df[(df["suite"] == "workload") &
           (df["experiment"] == "hashtable")].copy()
    if d.empty: return

    g = d.groupby(["primitive","threads"])["ops_per_sec"].mean().reset_index()

    fig, ax = plt.subplots(figsize=(9, 5.5))
    ht_colors = {
        "ht_coarse_mutex": "#E63946",
        "ht_fine_rwlock":  "#457B9D",
        "ht_lockfree":     "#2A9D8F",
    }
    ht_labels = {
        "ht_coarse_mutex": "Coarse Mutex (global lock)",
        "ht_fine_rwlock":  "Fine-Grained RW Lock (64 shards)",
        "ht_lockfree":     "Lock-Free (CAS ring)",
    }
    for prim, grp in g.groupby("primitive"):
        grp = grp.sort_values("threads")
        ax.plot(grp["threads"], grp["ops_per_sec"]/1e6,
                label=ht_labels.get(prim, prim),
                color=ht_colors.get(prim,"#888"),
                marker="o", linewidth=2, markersize=7)

    ax.set_xlabel("Thread Count")
    ax.set_ylabel("Throughput (Mops/sec)")
    ax.set_title("Fig 16 — Real-World Workload: Concurrent Hash Table\n"
                 "Insert 33% / Lookup 50% / Delete 17%")
    ax.legend()
    fig.tight_layout()
    savefig(fig, outdir, "16_workload_hashtable")


# ═══════════════════════════════════════════════════════════════
# FIGURE 17 — Real-World Workloads: Producer–Consumer Queue
# ═══════════════════════════════════════════════════════════════
def fig_workload_queue(df, outdir):
    d = df[(df["suite"] == "workload") &
           (df["experiment"] == "producer_consumer")].copy()
    if d.empty: return

    g = d.groupby(["primitive","threads"])["ops_per_sec"].mean().reset_index()

    q_colors = {
        "queue_mutex":     "#457B9D",
        "queue_lockfree":  "#2A9D8F",
        "queue_semaphore": "#F4A261",
    }
    q_labels = {
        "queue_mutex":     "Mutex + condition_variable",
        "queue_lockfree":  "Lock-Free MPMC Ring",
        "queue_semaphore": "Semaphore-based",
    }

    fig, ax = plt.subplots(figsize=(9, 5.5))
    for prim, grp in g.groupby("primitive"):
        grp = grp.sort_values("threads")
        ax.plot(grp["threads"], grp["ops_per_sec"]/1e6,
                label=q_labels.get(prim, prim),
                color=q_colors.get(prim,"#888"),
                marker="s", linewidth=2, markersize=7)

    ax.set_xlabel("Thread Count (Producers + Consumers)")
    ax.set_ylabel("Producer Throughput (Mops/sec)")
    ax.set_title("Fig 17 — Real-World Workload: Producer–Consumer Queue\n"
                 "Equal producers and consumers, bounded buffer")
    ax.legend()
    fig.tight_layout()
    savefig(fig, outdir, "17_workload_producer_consumer")


# ═══════════════════════════════════════════════════════════════
# FIGURE 18 — Real-World Workloads: Graph Algorithms
# ═══════════════════════════════════════════════════════════════
def fig_workload_graph(df, outdir):
    bfs  = df[(df["suite"]=="workload")&(df["experiment"]=="graph_bfs")].copy()
    rank = df[(df["suite"]=="workload")&(df["experiment"]=="graph_pagerank")].copy()
    if bfs.empty and rank.empty: return

    fig, axes = plt.subplots(1, 2, figsize=(13, 5.5))

    def plot_graph_algo(ax, data, title):
        if data.empty: return
        g = data.groupby(["primitive","threads"])["mean_latency_ns"].mean() / 1e6
        g = g.reset_index()
        g.columns = ["primitive","threads","time_ms"]
        for prim, grp in g.groupby("primitive"):
            grp = grp.sort_values("threads")
            ax.plot(grp["threads"], grp["time_ms"],
                    label=prim, marker="o", linewidth=2, markersize=7)
        ax.set_xlabel("Thread Count")
        ax.set_ylabel("Execution Time (ms, lower=better)")
        ax.set_title(title)
        ax.legend()

    plot_graph_algo(axes[0], bfs,  "BFS: Mutex vs Atomic-visited")
    plot_graph_algo(axes[1], rank, "PageRank: Mutex vs Atomic fetch_add")

    fig.suptitle("Fig 18 — Real-World Workload: Parallel Graph Algorithms\n"
                 "(n=50,000 nodes, avg degree=10)",
                 fontsize=13, fontweight="bold")
    fig.tight_layout()
    savefig(fig, outdir, "18_workload_graph")


# ═══════════════════════════════════════════════════════════════
# FIGURE 19 — Hyper-Threading Analysis (RQ10)
# ═══════════════════════════════════════════════════════════════
def fig_hyperthreading(df, outdir):
    d = df[(df["suite"]=="hardware")&(df["experiment"]=="hyperthreading")].copy()
    if d.empty: return

    g = d.groupby(["primitive","threads"])["ops_per_sec"].mean().reset_index()

    fig, ax = plt.subplots(figsize=(9, 5.5))
    for prim, grp in g.groupby("primitive"):
        grp = grp.sort_values("threads")
        ax.plot(grp["threads"], grp["ops_per_sec"]/1e6,
                label=pname(prim), color=color(prim),
                marker=marker(prim), linewidth=2, markersize=7)

    ax.set_xlabel("Thread Count")
    ax.set_ylabel("Throughput (Mops/sec)")
    ax.set_title("Fig 19 — Hyper-Threading Analysis (RQ10)\n"
                 "Physical cores only → HT → Oversubscribed")
    ax.legend()
    fig.tight_layout()
    savefig(fig, outdir, "19_hyperthreading")


# ═══════════════════════════════════════════════════════════════
# FIGURE 20 — NUMA Analysis (RQ10)
# ═══════════════════════════════════════════════════════════════
def fig_numa(df, outdir):
    d = df[(df["suite"]=="hardware")&(df["experiment"]=="numa")].copy()
    if d.empty:
        print("  [skip] no NUMA data (expected on single-node systems)")
        return

    g = d.groupby(["primitive","notes"])["ops_per_sec"].mean().unstack("notes")
    if g.empty: return

    fig, ax = plt.subplots(figsize=(9, 5))
    x = np.arange(len(g.index))
    w = 0.35
    cols = list(g.columns)
    for i, col in enumerate(cols):
        ax.bar(x + i*w, g[col]/1e6, w, label=col, alpha=0.85)

    ax.set_xticks(x + w*(len(cols)-1)/2)
    ax.set_xticklabels([pname(p) for p in g.index], rotation=20, ha="right")
    ax.set_ylabel("Throughput (Mops/sec)")
    ax.set_title("Fig 20 — NUMA Analysis (RQ10)\n"
                 "Same-node vs cross-node memory access")
    ax.legend()
    fig.tight_layout()
    savefig(fig, outdir, "20_numa_analysis")


# ═══════════════════════════════════════════════════════════════
# FIGURE 21 — Energy per Operation (RAPL, RQ energy)
# ═══════════════════════════════════════════════════════════════
def fig_energy(df, outdir):
    d = df[df["suite"]=="energy"].copy()
    if d.empty: return

    # Parse nj_per_op from notes
    import re
    def parse_nj(notes):
        try:
            m = re.search(r"nj_per_op=([\-\d\.]+)", str(notes))
            if m:
                v = float(m.group(1))
                return v if v >= 0 else np.nan
        except Exception:
            pass
        return np.nan

    d["nj_per_op"] = d["notes"].apply(parse_nj)
    d = d.dropna(subset=["nj_per_op"])
    if d.empty:
        print("  [skip] RAPL not available — no energy data")
        return

    g = d.groupby("primitive")[["nj_per_op","ops_per_sec"]].mean()
    g = g.sort_values("nj_per_op")

    fig, axes = plt.subplots(1, 2, figsize=(13, 5.5))

    # nJ per op
    ax = axes[0]
    bars = ax.barh([pname(p) for p in g.index], g["nj_per_op"],
                   color=[color(p) for p in g.index], alpha=0.85)
    ax.set_xlabel("Energy per Operation (nanojoules)")
    ax.set_title("Energy per Lock Operation\n(lower = more efficient)")

    # Throughput vs energy scatter
    ax = axes[1]
    for prim in g.index:
        ax.scatter(g.loc[prim, "ops_per_sec"]/1e6,
                   g.loc[prim, "nj_per_op"],
                   color=color(prim), s=120, label=pname(prim),
                   zorder=3)
        ax.annotate(pname(prim),
                    (g.loc[prim,"ops_per_sec"]/1e6, g.loc[prim,"nj_per_op"]),
                    textcoords="offset points", xytext=(5,3), fontsize=8)
    ax.set_xlabel("Throughput (Mops/sec)")
    ax.set_ylabel("Energy per Op (nJ)")
    ax.set_title("Energy–Throughput Trade-off\n(bottom-right = best)")

    fig.suptitle("Fig 21 — Energy Consumption Analysis (Intel RAPL)",
                 fontsize=13, fontweight="bold")
    fig.tight_layout()
    savefig(fig, outdir, "21_energy_rapl")


# ═══════════════════════════════════════════════════════════════
# FIGURE 22 — Complete Experiment Summary (all suites, one page)
# ═══════════════════════════════════════════════════════════════
def fig_master_summary(df, outdir):
    """4×2 grid — one panel per suite, each showing ops/sec by primitive."""
    suites = [
        ("scalability",  "Scalability",         "threads"),
        ("contention",   "Contention (CS size)", "cs_cycles"),
        ("fairness",     "Fairness (Gini)",      None),
        ("latency_dist", "Tail Latency P99",     None),
        ("openmp",       "OpenMP vs Manual",     "threads"),
        ("workload",     "Real-World Workloads", "threads"),
        ("hardware",     "HW: HT Analysis",      "threads"),
        ("throughput",   "Saturation",           "threads"),
    ]

    fig, axes = plt.subplots(4, 2, figsize=(14, 20))
    axes = axes.flatten()

    for i, (suite, title, x_col) in enumerate(suites):
        ax = axes[i]
        d = df[df["suite"]==suite]
        if d.empty:
            ax.text(0.5, 0.5, "No data", transform=ax.transAxes,
                    ha="center", va="center", color="grey")
            ax.set_title(title)
            continue

        if suite == "fairness":
            g = d.groupby("primitive")["fairness_gini"].mean().sort_values()
            ax.barh([pname(p) for p in g.index], g.values,
                    color=[color(p) for p in g.index], alpha=0.85)
            ax.set_xlabel("Gini Coefficient")
        elif suite == "latency_dist":
            g = d.groupby("primitive")["p99_ns"].mean().sort_values()
            ax.barh([pname(p) for p in g.index], g.values,
                    color=[color(p) for p in g.index], alpha=0.85)
            ax.set_xlabel("P99 Latency (ns)")
        elif x_col:
            g = d.groupby(["primitive", x_col])["ops_per_sec"].mean().reset_index()
            for prim, grp in g.groupby("primitive"):
                grp = grp.sort_values(x_col)
                ax.plot(grp[x_col], grp["ops_per_sec"]/1e6,
                        color=color(prim), marker=marker(prim),
                        linewidth=1.5, markersize=5,
                        label=pname(prim))
            ax.set_xlabel(x_col)
            ax.set_ylabel("Mops/sec")
        else:
            g = d.groupby("primitive")["ops_per_sec"].mean().sort_values()
            ax.barh([pname(p) for p in g.index], g.values/1e6,
                    color=[color(p) for p in g.index], alpha=0.85)
            ax.set_xlabel("Mops/sec")

        ax.set_title(title, fontsize=11)
        ax.grid(True, alpha=0.3, linestyle="--")

    fig.suptitle("Fig 22 — Master Summary: All Experiments\n"
                 "Study of Synchronisation Mechanism Performance Among Threads",
                 fontsize=14, fontweight="bold", y=1.01)
    fig.tight_layout()
    savefig(fig, outdir, "22_master_summary")


# ── Patch main() to call new figure functions ─────────────────
_original_main = main

def main():
    ap = argparse.ArgumentParser(description="Visualise sync_benchmark results")
    ap.add_argument("--csv",    default="output/results.csv")
    ap.add_argument("--outdir", default="output/graphs")
    args = ap.parse_args()

    if not os.path.exists(args.csv):
        print(f"ERROR: CSV not found: {args.csv}")
        sys.exit(1)

    print(f"\nLoading {args.csv} ...")
    df = load_data(args.csv)
    print(f"  {len(df)} rows, suites: {df['suite'].unique().tolist()}\n")

    print("Generating figures ...")

    # Original 14 figures
    fig_scalability_throughput(df, args.outdir)
    fig_scalability_latency(df, args.outdir)
    fig_scalability_heatmap(df, args.outdir)
    fig_contention_throughput(df, args.outdir)
    fig_contention_latency(df, args.outdir)
    fig_contention_crossover(df, args.outdir)
    fig_false_sharing_bar(df, args.outdir)
    fig_false_sharing_factor(df, args.outdir)
    export_false_sharing_table(df, args.outdir)
    fig_fairness_gini(df, args.outdir)
    fig_fairness_jain(df, args.outdir)
    fig_latency_boxplot(df, args.outdir)
    fig_latency_percentile_ladder(df, args.outdir)
    fig_throughput_saturation(df, args.outdir)
    fig_efficiency_heatmap(df, args.outdir)
    fig_radar_summary(df, args.outdir)

    # New SSP figures
    fig_openmp_comparison(df, args.outdir)
    fig_workload_hashtable(df, args.outdir)
    fig_workload_queue(df, args.outdir)
    fig_workload_graph(df, args.outdir)
    fig_hyperthreading(df, args.outdir)
    fig_numa(df, args.outdir)
    fig_energy(df, args.outdir)
    fig_master_summary(df, args.outdir)

    build_dashboard(df, args.outdir)

    print(f"\n✔  All figures saved to {args.outdir}/\n")

if __name__ == "__main__":
    main()
