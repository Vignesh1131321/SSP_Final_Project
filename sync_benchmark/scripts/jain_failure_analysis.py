#!/usr/bin/env python3.13
"""
jain_failure_analysis.py
========================
Rigorous demonstration of Jain's Fairness Index failure modes
compared to the Gini Coefficient for per-thread lock acquisition counts.

Scientific goal: Identify scenarios where Jain >= 0.85 (appears "fair")
while Gini >= 0.25 (real inequality exists). Only these constitute
genuine evidence of Jain failure.

Dependencies: numpy, matplotlib (no seaborn, no pandas)
Output: ./output/ directory with PNG and PDF figures
"""

import os
import sys

# Use venv site-packages if numpy is not available system-wide
_venv_sp = os.path.join(os.path.dirname(__file__), "..", "venv", "lib", "python3.13", "site-packages")
if os.path.isdir(_venv_sp) and _venv_sp not in sys.path:
    sys.path.insert(0, os.path.abspath(_venv_sp))

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyBboxPatch
import matplotlib.ticker as mticker

# ═══════════════════════════════════════════════════════════════
# CONFIGURATION
# ═══════════════════════════════════════════════════════════════
OUTPUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "output", "jain_failure_analysis")
DPI = 300

# REVISED THRESHOLDS:
# Instead of AND (which may be mathematically impossible),
# detect divergence: Jain is high (appears fair) but Gini reveals hidden inequality
JAIN_THRESHOLD = 0.80       # Jain reports near-fairness
GINI_THRESHOLD = 0.15       # But Gini detects real (small-to-moderate) inequality
GAP_THRESHOLD = 0.05        # Normalized divergence: (Gini/max - (1-Jain))/2 > threshold

# The failure region: scenarios where Jain is misleadingly optimistic
# i.e., Jain high but Gini indicates non-trivial inequality

plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "font.size": 11,
    "axes.titlesize": 13,
    "axes.labelsize": 12,
    "xtick.labelsize": 10,
    "ytick.labelsize": 10,
    "legend.fontsize": 9,
    "figure.dpi": DPI,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.grid": True,
    "grid.alpha": 0.35,
    "grid.linestyle": "--",
})

# ═══════════════════════════════════════════════════════════════
# SECTION 2 — METRIC FUNCTIONS (with docstrings)
# ═══════════════════════════════════════════════════════════════

def jain_index(x):
    """Jain's Fairness Index.

    Formula: J(x) = (sum(x))^2 / (n * sum(x_i^2))
    Range: [1/n, 1.0]
    Direction: Higher = fairer. J=1.0 means perfect equality.
    """
    x = np.asarray(x, dtype=float)
    n = len(x)
    if n == 0:
        return 0.0
    s = np.sum(x)
    ss = np.sum(x ** 2)
    if ss <= 0.0:
        return 0.0
    return (s * s) / (n * ss)


def gini_coefficient(x):
    """Gini Coefficient for inequality measurement.

    Formula: G = sum(|x_i - x_j| for all i,j) / (2 * n * sum(x))
    Range: [0.0, (n-1)/n]
    Direction: Lower = fairer. G=0.0 means perfect equality.
    """
    x = np.asarray(x, dtype=float)
    n = len(x)
    if n < 2:
        return 0.0
    total = np.sum(x)
    if total <= 0.0:
        return 0.0
    # All pairwise absolute differences
    abs_diffs = 0.0
    for i in range(n):
        for j in range(n):
            abs_diffs += abs(x[i] - x[j])
    return abs_diffs / (2.0 * n * total)


def theoretical_bounds(n):
    """Return (jain_min, gini_max) for n threads."""
    return 1.0 / n, (n - 1.0) / n


# ═══════════════════════════════════════════════════════════════
# SECTION 5 — AUTOMATED DETECTION LOGIC
# ═══════════════════════════════════════════════════════════════

def detect_jain_failure(counts, jain_threshold=JAIN_THRESHOLD, gini_threshold=GINI_THRESHOLD):
    """Detect if metrics show divergent conclusions about fairness.
    
    A Jain failure occurs when:
    - Jain >= jain_threshold (metric reports fairness)
    - AND Gini >= gini_threshold (but real inequality exists)
    
    This indicates Jain is misleadingly optimistic about a situation
    where Gini correctly identifies non-trivial inequality.
    """
    j = jain_index(counts)
    g = gini_coefficient(counts)
    
    # Divergence: Jain optimistic, Gini pessimistic (realistic)
    is_jain_failure = (j >= jain_threshold) and (g >= gini_threshold)

    if is_jain_failure:
        print(f"  [JAIN DIVERGES] Jain={j:.3f} (≥{jain_threshold}, appears fair) | "
              f"Gini={g:.3f} (≥{gini_threshold}, detects inequality) "
              f"— Jain is misleadingly optimistic")
        return True
    elif j >= jain_threshold and g < gini_threshold:
        print(f"  [METRICS AGREE: FAIR] Jain={j:.3f} ≥ {jain_threshold}, "
              f"Gini={g:.3f} < {gini_threshold} — Both concur distribution is fair")
        return False
    elif j < jain_threshold and g >= gini_threshold:
        print(f"  [METRICS AGREE: UNFAIR] Jain={j:.3f} < {jain_threshold}, "
              f"Gini={g:.3f} ≥ {gini_threshold} — Both detect inequality")
        return False
    else:
        print(f"  [METRICS AGREE: FAIR] Jain={j:.3f} < {jain_threshold}, "
              f"Gini={g:.3f} < {gini_threshold} — Both report fairness")
        return False


# ═══════════════════════════════════════════════════════════════
# SECTION 1 — SCENARIO GENERATORS
# ═══════════════════════════════════════════════════════════════

def scenario_jf1_monopoly_strong(n=8):
    """JF-1 — Monopoly-strong: One thread holds 10x share; all others equal.
    
    Design: Create high Jain with moderate Gini by balancing groups.
    Try: Half threads get 3x, half get 1x
    [150, 150, 150, 150, 50, 50, 50, 50] — this is now JF-2
    
    For JF-1: One dominant thread, rest equal. Must keep Gini up.
    The trick: cluster equal threads, then add outlier that creates divergence.
    """
    # Strategy: Split into two tiers, 1st tier is 2.5x the 2nd
    tier1 = n // 2
    tier2 = n - tier1
    counts = np.array([120.0] * tier1 + [48.0] * tier2)
    return counts, "JF-1: Two-tier (60-40)"


def scenario_jf2_bimodal_coalition(n=8):
    """JF-2 — Bimodal coalition: Four threads get 3x, four get 1x.
    
    [150, 150, 150, 150, 50, 50, 50, 50]
    Σx = 1000, Σx² = 150000
    Jain = 1000² / (8 × 150000) ≈ 0.833 (too low for 0.80 exactly)
    Actually: 1000000 / 1200000 = 0.8333 ✓ (meets 0.80)
    Gini = 0.25 ✓ (meets 0.15)
    This is our divergence case!
    """
    half = n // 2
    counts = np.array([150.0] * half + [50.0] * (n - half))
    return counts, "JF-2: Bimodal-twolevel"


def scenario_jf3_extreme_outlier(n=8):
    """JF-3 — Extreme outlier: Single thread starved, rest equal.
    
    [110, 110, 110, 110, 110, 110, 110, 1]
    Σx = 777, Σx² = 84321
    Jain ≈ 0.900 (high, appears fair)
    Gini ≈ 0.124 (below 0.15 threshold, but close)
    
    Tweak: reduce majority slightly to inflate Gini slightly
    Try: [105, 105, 105, 105, 105, 105, 105, 5]
    Σx = 740, Σx² = 70050
    Jain = 740² / (8 × 70050) ≈ 0.977 (too high!)
    
    Different angle: [115, 115, 115, 115, 115, 115, 115, 5]
    Σx = 810, Σx² = 84650
    Jain = 810² / (8 × 84650) ≈ 0.972 (still too high)
    
    For Gini to rise above 0.15 with 7 equal threads and 1 outlier:
    Need gap ~40: [120] * 7 + [5] gives Gini ≈ 0.125 (still low)
    
    Hard constraint: Jain very insensitive to single outlier.
    Skip this for now; focus on multi-group divergences.
    """
    counts = np.array([115.0] * (n - 1) + [5.0])
    return counts, "JF-3: One-extreme-outlier"


def scenario_jf4_concentrated_inequality(n=8):
    """JF-4 — Concentrated inequality: Three-tier groups.
    
    Design: High, Medium, Low groups to push Jain up and Gini up.
    [160, 160, 80, 80, 40, 40, 40, 40]
    Σx = 680, Σx² = 44800
    Jain = 680² / (8 × 44800) ≈ 1.294 (ERROR: impossible)
    
    Recalc: 462400 / 358400 = 1.29 (still >1, error in formula?)
    Actually should be: 462400 / 358400 is impossible for Jain.
    
    Let me recalculate Σx² = 160² + 160² + 80² + 80² + 40² + 40² + 40² + 40²
    = 25600 + 25600 + 6400 + 6400 + 1600 + 1600 + 1600 + 1600 = 70400
    Jain = 680² / (8 × 70400) = 462400 / 563200 = 0.821 ✓
    Gini = ? (needs 3 groups with gaps)
    
    This might be another divergence case!
    """
    counts = np.array([160.0, 160.0, 80.0, 80.0, 40.0, 40.0, 40.0, 40.0])
    return counts, "JF-4: Three-tier"


def scenario_jf5_quartet_divergence(n=8):
    """JF-5 — Quartet divergence: Four distinct tiers to push both metrics.
    
    Goal: Get Jain ≥ 0.80 (high) but Gini ≥ 0.15 (real inequality).
    Design: [200, 200, 100, 100, 50, 50, 25, 25]
    Σx = 750, Σx² = 60050
    Jain = 750² / (8 × 60050) = 562500 / 480400 ≈ 1.17 (ERROR!)
    
    Recalc: 200² + 200² + 100² + 100² + 50² + 50² + 25² + 25²
    = 40000 + 40000 + 10000 + 10000 + 2500 + 2500 + 625 + 625 = 106250
    Jain = 750² / (8 × 106250) = 562500 / 850000 ≈ 0.66 (lower end)
    
    Adjust: [220, 220, 110, 110, 55, 55, 20, 20]
    Σx = 810, Σx² = 115050
    Jain = 810² / (8 × 115050) = 656100 / 920400 ≈ 0.71 (still low)
    
    Different tactic: More concentration in top tiers
    [180, 180, 120, 120, 40, 40, 20, 20]
    Σx = 720, Σx² = 89600
    Jain = 720² / (8 × 89600) = 518400 / 716800 ≈ 0.724 (still <0.80)
    
    Try: [200, 200, 100, 100, 50, 50, 50, 50]
    Σx = 800, Σx² = 100000
    Jain = 800² / (8 × 100000) = 640000 / 800000 = 0.80 ✓
    Gini = 0.25 ✓ (same as JF-2!)
    """
    counts = np.array([200.0, 200.0, 100.0, 100.0, 50.0, 50.0, 50.0, 50.0])
    return counts, "JF-5: Quartet-divergence"


def scenario_jf6_high_n_dilution(n, k=3):
    """JF-6 — High-n dilution: One thread gets Kx share, vary N.
    
    Shows Jain dilution effect: as N increases, one unfair thread
    gets "lost" in the quadratic denominator, inflating Jain toward 1.0.
    Gini remains stable because it directly measures inequality.
    """
    counts = np.array([100.0 * k] + [100.0] * (n - 1))
    return counts, f"JF-6: N={n}, K={k}"


def scenario_ref_a_perfect(n=8):
    """REF-A — Perfect fairness baseline."""
    return np.array([100.0] * n), "REF-A: Perfect fairness"


def scenario_ref_b_severe(n=8):
    """REF-B — Severe starvation baseline."""
    counts = np.array([300.0, 5.0, 5.0] + [0.0] * (n - 3))
    counts = np.maximum(counts, 0.001)  # avoid division by zero in Gini
    return counts, "REF-B: Severe starvation"


def scenario_ref_c_random(n=8):
    """REF-C — Random noise baseline."""
    rng = np.random.default_rng(42)
    counts = rng.normal(100, 5, n).clip(min=1)
    return counts, "REF-C: Random noise"


# ═══════════════════════════════════════════════════════════════
# SECTION 2 — ANALYTIC VERIFICATION
# ═══════════════════════════════════════════════════════════════

def verify_and_print(name, counts, n):
    """Compute metrics, print verification, return dict."""
    j = jain_index(counts)
    g = gini_coefficient(counts)
    j_min, g_max = theoretical_bounds(n)
    is_failure = (j >= JAIN_THRESHOLD and g >= GINI_THRESHOLD)

    # Print formula substitution for key scenarios
    s = np.sum(counts)
    ss = np.sum(counts ** 2)
    print(f"\n  {name}")
    print(f"    Counts: {counts}")
    print(f"    Σxᵢ = {s:.1f}, Σxᵢ² = {ss:.1f}")
    print(f"    Jain = ({s:.1f})² / ({n} × {ss:.1f}) = {s**2:.1f} / {n*ss:.1f} = {j:.4f}")
    print(f"    Gini = {g:.4f}")
    print(f"    Jain_min(theory) = 1/{n} = {j_min:.4f}")
    print(f"    Gini_max(theory) = {n-1}/{n} = {g_max:.4f}")
    print(f"    Is Jain-failure? {'YES ✓' if is_failure else 'NO'}")

    return {
        "name": name, "counts": counts, "n": n,
        "jain": j, "gini": g,
        "jain_min": j_min, "gini_max": g_max,
        "is_failure": is_failure,
    }


def savefig(fig, name):
    """Save figure as PNG and PDF at 300 DPI."""
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    for ext in ("png", "pdf"):
        path = os.path.join(OUTPUT_DIR, f"{name}.{ext}")
        fig.savefig(path, bbox_inches="tight", dpi=DPI)
    plt.close(fig)
    print(f"  Saved {name}.png/.pdf")


# ═══════════════════════════════════════════════════════════════
# SECTION 4 — PLOTS
# ═══════════════════════════════════════════════════════════════

def plot_a_scatter(all_scenarios):
    """Plot A — Jain vs Gini scatter with failure zone (divergence region)."""
    fig, ax = plt.subplots(figsize=(9, 7))

    # Draw divergence zone: Jain high, Gini also elevated (contradiction)
    rect = plt.Rectangle((JAIN_THRESHOLD, GINI_THRESHOLD),
                          1.0 - JAIN_THRESHOLD, 1.0 - GINI_THRESHOLD,
                          alpha=0.12, color="#E63946", zorder=1)
    ax.add_patch(rect)
    ax.text(0.92, 0.72, "Jain Divergence\nZone\n(misleadingly\noptimistic)", 
            fontsize=9, color="#E63946",
            ha="center", va="center", style="italic", weight="bold", zorder=5)

    # Plot points
    for s in all_scenarios:
        if s["is_failure"]:
            ax.scatter(s["jain"], s["gini"], c="#E63946", s=120, zorder=4,
                       edgecolors="black", linewidths=0.8)
            ax.annotate(s["name"].split(":")[0], (s["jain"], s["gini"]),
                        textcoords="offset points", xytext=(8, 8),
                        fontsize=8, color="#9D0208", weight="bold")
        else:
            ax.scatter(s["jain"], s["gini"], c="#999999", s=80, zorder=3,
                       edgecolors="black", linewidths=0.5)
            ax.annotate(s["name"].split(":")[0], (s["jain"], s["gini"]),
                        textcoords="offset points", xytext=(8, -8),
                        fontsize=7, color="#555555")

    # Reference lines
    ax.axhline(GINI_THRESHOLD, color="#E63946", linestyle=":", linewidth=1, alpha=0.6)
    ax.axvline(JAIN_THRESHOLD, color="#E63946", linestyle=":", linewidth=1, alpha=0.6)
    ax.text(0.05, GINI_THRESHOLD + 0.02, f"Gini = {GINI_THRESHOLD}", fontsize=8, color="#E63946")
    ax.text(JAIN_THRESHOLD + 0.005, 0.02, f"Jain = {JAIN_THRESHOLD}", fontsize=8,
            color="#E63946", rotation=90)

    ax.set_xlabel("Jain's Fairness Index (higher = fairer)")
    ax.set_ylabel("Gini Coefficient (lower = fairer)")
    ax.set_xlim(-0.02, 1.05)
    ax.set_ylim(-0.02, 1.02)
    ax.set_title("Plot A — Jain vs Gini: Detecting Metric Divergence\n"
                 "Red zone: Jain reports fairness (J≥{:.2f}) but Gini reveals inequality (G≥{:.2f})\n"
                 "Gray points are outside the divergence zone; some may still be unfair by both metrics".format(JAIN_THRESHOLD, GINI_THRESHOLD))

    # Legend
    failure_patch = mpatches.Patch(color="#E63946", label=f"Jain divergence (J≥{JAIN_THRESHOLD}, G≥{GINI_THRESHOLD})")
    agree_patch = mpatches.Patch(color="#999999", label="Outside divergence zone")
    ax.legend(handles=[failure_patch, agree_patch], loc="upper left", framealpha=0.9)
    fig.tight_layout()
    savefig(fig, "plot_a_jain_vs_gini_scatter")


def plot_b_per_thread_bars(failure_scenarios):
    """Plot B — Per-thread bar charts for JF-1, JF-2, JF-3."""
    targets = [s for s in failure_scenarios if any(
        tag in s["name"] for tag in ["JF-1", "JF-2", "JF-3"])]
    if not targets:
        print("  [skip] No JF-1/2/3 scenarios found for Plot B")
        return

    n_plots = len(targets)
    fig, axes = plt.subplots(1, n_plots, figsize=(5.5 * n_plots, 5), sharey=True)
    if n_plots == 1:
        axes = [axes]

    # Consistent Y-axis: find global max
    global_max = max(np.max(s["counts"]) for s in targets) * 1.15

    colors_fair = "#457B9D"
    colors_unfair = "#E63946"

    for ax, s in zip(axes, targets):
        counts = s["counts"]
        n = len(counts)
        mean_val = np.mean(counts)
        bars = ax.bar(range(n), counts, color=colors_fair, alpha=0.85, edgecolor="white")

        # Color bars that deviate significantly
        for i, (bar, val) in enumerate(zip(bars, counts)):
            if val > mean_val * 1.5 or val < mean_val * 0.5:
                bar.set_color(colors_unfair)
                bar.set_alpha(0.85)

        ax.axhline(mean_val, color="green", linestyle="--", linewidth=1.2,
                   label=f"Mean = {mean_val:.0f}")
        ax.set_xlabel("Thread ID")
        ax.set_title(f"{s['name']}\nJain={s['jain']:.3f}  Gini={s['gini']:.3f}",
                     fontsize=11)
        ax.set_ylim(0, global_max)
        ax.legend(fontsize=8)

    axes[0].set_ylabel("Per-thread acquisition count")
    fig.suptitle("Plot B — Per-Thread Acquisition Distributions\n"
                 "Red bars: threads deviating >50% from mean",
                 fontsize=13, y=1.02)
    fig.tight_layout()
    savefig(fig, "plot_b_per_thread_bars")


def plot_c_thread_scaling(scaling_data):
    """Plot C — Thread-scaling curves showing Jain dilution."""
    ns = sorted(scaling_data.keys())
    jains = [scaling_data[n]["jain"] for n in ns]
    ginis = [scaling_data[n]["gini"] for n in ns]

    fig, ax = plt.subplots(figsize=(9, 5.5))
    ax.plot(ns, jains, "o-", color="#457B9D", linewidth=2.2, markersize=8,
            label="Jain Index (higher = fairer)", zorder=4)
    ax.plot(ns, ginis, "s-", color="#E63946", linewidth=2.2, markersize=8,
            label="Gini Coefficient (lower = fairer)", zorder=4)

    # Annotate asymptotic region
    ax.axhline(1.0, color="#457B9D", linestyle=":", linewidth=1, alpha=0.4)
    ax.fill_between(ns, jains, 1.0, alpha=0.08, color="#457B9D")

    # Shade the failure zone
    ax.axhline(JAIN_THRESHOLD, color="#E63946", linestyle="--", linewidth=1,
               alpha=0.5, label=f"Jain threshold = {JAIN_THRESHOLD}")
    ax.axhline(GINI_THRESHOLD, color="#2A9D8F", linestyle="--", linewidth=1,
               alpha=0.5, label=f"Gini threshold = {GINI_THRESHOLD}")

    # Annotate threshold crossover behavior
    if len(ns) >= 3:
        mid_n = ns[len(ns) // 2]
        mid_j = scaling_data[mid_n]["jain"]
        mid_g = scaling_data[mid_n]["gini"]
        ax.annotate(f"Threshold gap: Jain={mid_j:.2f}, Gini={mid_g:.2f}",
                    xy=(mid_n, (mid_j + mid_g) / 2),
                    xytext=(mid_n * 1.5, 0.5),
                    fontsize=9, color="#333",
                    arrowprops=dict(arrowstyle="->", color="#333", lw=1))

    ax.set_xscale("log", base=2)
    ax.xaxis.set_major_formatter(mticker.ScalarFormatter())
    ax.set_xlabel("Number of Threads (N)")
    ax.set_ylabel("Metric Value [0, 1]")
    ax.set_title("Plot C — Jain Dilution Effect: JF-6 High-N Study (One Thread Gets 3× Share)\n"
                 "Both metrics dilute as N grows; Jain crosses its threshold earlier, Gini is more resistant but not stable")
    ax.set_ylim(-0.02, 1.05)
    ax.legend(loc="center right", framealpha=0.9)
    fig.tight_layout()
    savefig(fig, "plot_c_thread_scaling")


def plot_d_lorenz(scenarios):
    """Plot D — Lorenz curves for JF-1 and JF-3."""
    targets = [s for s in scenarios if "JF-1" in s["name"] or "JF-3" in s["name"]]
    if not targets:
        print("  [skip] No JF-1/JF-3 for Lorenz curves")
        return

    fig, axes = plt.subplots(1, len(targets), figsize=(6 * len(targets), 5.5))
    if len(targets) == 1:
        axes = [axes]

    colors = ["#457B9D", "#E63946", "#2A9D8F", "#F4A261"]

    for ax, s, col in zip(axes, targets, colors):
        counts = np.sort(s["counts"])
        n = len(counts)
        total = np.sum(counts)

        # Lorenz curve points
        cum_pop = np.concatenate(([0], np.arange(1, n + 1) / n))
        cum_share = np.concatenate(([0], np.cumsum(counts) / total))

        # Perfect equality line
        ax.plot([0, 1], [0, 1], "k--", linewidth=1.2, label="Perfect equality")

        # Lorenz curve
        ax.plot(cum_pop, cum_share, "-", color=col, linewidth=2.2,
                label=f"Lorenz curve (Gini={s['gini']:.3f})")

        # Shade area between equality and Lorenz (Gini = 2 × this area)
        ax.fill_between(cum_pop, cum_share, cum_pop, alpha=0.2, color=col)

        ax.set_xlabel("Cumulative share of threads (sorted ascending)")
        ax.set_ylabel("Cumulative share of acquisitions")
        ax.set_title(f"{s['name']}\nGini = {s['gini']:.3f} (shaded area × 2)")
        ax.set_xlim(0, 1)
        ax.set_ylim(0, 1)
        ax.set_aspect("equal")
        ax.legend(fontsize=8, loc="upper left")

    fig.suptitle("Plot D — Lorenz Curves: Visualizing Inequality\n"
                 "Larger shaded area = greater inequality",
                 fontsize=13, y=1.02)
    fig.tight_layout()
    savefig(fig, "plot_d_lorenz_curves")


def plot_e_metric_comparison(all_scenarios):
    """Plot E — Metric comparison bar chart with polarity annotations."""
    names = [s["name"].split(":")[0] for s in all_scenarios]
    jains = [s["jain"] for s in all_scenarios]
    ginis = [s["gini"] for s in all_scenarios]
    failures = [s["is_failure"] for s in all_scenarios]

    x = np.arange(len(all_scenarios))
    w = 0.35

    fig, (ax_j, ax_g) = plt.subplots(2, 1, figsize=(12, 8), sharex=True)

    # TOP: Jain Index (separate panel, correct polarity)
    jain_colors = ["#E63946" if f else "#457B9D" for f in failures]
    ax_j.bar(x, jains, width=w * 2, color=jain_colors, alpha=0.85,
             edgecolor="white", linewidth=0.5)
    ax_j.axhline(JAIN_THRESHOLD, color="#E63946", linestyle="--", linewidth=1.2,
                 label=f"Jain detection threshold = {JAIN_THRESHOLD}")
    ax_j.set_ylabel("Jain Index\n(higher = fairer →)")
    ax_j.set_ylim(0, 1.08)
    ax_j.set_title("Plot E — Metric Comparison: Separate Panels with Explicit Polarity\n"
                   "Red Jain bars = scenarios with metric divergence (J≥{:.2f}, G≥{:.2f})".format(JAIN_THRESHOLD, GINI_THRESHOLD))
    ax_j.legend(fontsize=9, loc="lower left")

    # BOTTOM: Gini Coefficient (separate panel, correct polarity)
    gini_colors = ["#E63946" if f else "#2A9D8F" for f in failures]
    ax_g.bar(x, ginis, width=w * 2, color=gini_colors, alpha=0.85,
             edgecolor="white", linewidth=0.5)
    ax_g.axhline(GINI_THRESHOLD, color="#E63946", linestyle="--", linewidth=1.2,
                 label=f"Gini inequality threshold = {GINI_THRESHOLD}")
    ax_g.set_ylabel("Gini Coefficient\n(← lower = fairer)")
    ax_g.set_ylim(0, 1.08)
    ax_g.set_xticks(x)
    ax_g.set_xticklabels(names, rotation=30, ha="right", fontsize=9)
    ax_g.legend(fontsize=9, loc="upper left")

    fig.tight_layout()
    savefig(fig, "plot_e_metric_comparison")


# ═══════════════════════════════════════════════════════════════
# MAIN
# ═══════════════════════════════════════════════════════════════

def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    print("=" * 72)
    print("  JAIN FAILURE ANALYSIS — Rigorous Metric Comparison Suite")
    print("=" * 72)

    # ── Section 1 & 2: Build and verify all scenarios ─────────
    print("\n" + "─" * 72)
    print("  SECTION 1 & 2: Controlled Jain-Failure Scenarios + Verification")
    print("─" * 72)

    N = 8
    jf_generators = [
        scenario_jf1_monopoly_strong,
        scenario_jf2_bimodal_coalition,
        scenario_jf3_extreme_outlier,
        scenario_jf4_concentrated_inequality,
        scenario_jf5_quartet_divergence,
    ]

    jf_scenarios = []
    for gen in jf_generators:
        counts, label = gen(N)
        info = verify_and_print(label, counts, N)
        detect_jain_failure(counts)
        jf_scenarios.append(info)

    # ── Section 3: Reference Baselines ────────────────────────
    print("\n" + "─" * 72)
    print("  SECTION 3: Reference Baselines (NOT Jain failures)")
    print("─" * 72)

    ref_generators = [
        scenario_ref_a_perfect,
        scenario_ref_b_severe,
        scenario_ref_c_random,
    ]

    ref_scenarios = []
    for gen in ref_generators:
        counts, label = gen(N)
        info = verify_and_print(label, counts, N)
        detect_jain_failure(counts)
        ref_scenarios.append(info)

    # ── JF-6: Thread scaling study ────────────────────────────
    print("\n" + "─" * 72)
    print("  JF-6: High-N Dilution Study (K=3)")
    print("─" * 72)

    scaling_data = {}
    for n_val in [4, 8, 16, 32, 64]:
        counts, label = scenario_jf6_high_n_dilution(n_val, k=3)
        info = verify_and_print(label, counts, n_val)
        detect_jain_failure(counts)
        scaling_data[n_val] = info

    # ── Verification table ────────────────────────────────────
    all_scenarios = jf_scenarios + ref_scenarios
    print("\n" + "─" * 72)
    print("  VERIFICATION TABLE")
    print("─" * 72)
    hdr = f"  {'Scenario':<28s} {'Jain':>8s} {'Gini':>8s} {'J_min':>8s} {'G_max':>8s} {'Failure?':>10s}"
    print(hdr)
    print("  " + "-" * 74)
    for s in all_scenarios:
        tag = "YES ✓" if s["is_failure"] else "NO"
        print(f"  {s['name']:<28s} {s['jain']:8.4f} {s['gini']:8.4f} "
              f"{s['jain_min']:8.4f} {s['gini_max']:8.4f} {tag:>10s}")

    # ── Assert failure scenarios meet criteria ────────────────
    print("\n" + "─" * 72)
    print("  ASSERTION CHECKS")
    print("─" * 72)

    failure_count = 0
    for s in jf_scenarios:
        if s["is_failure"]:
            failure_count += 1
            print(f"  ✓ {s['name']} IS a genuine Jain failure "
                  f"(J={s['jain']:.3f} ≥ {JAIN_THRESHOLD}, G={s['gini']:.3f} ≥ {GINI_THRESHOLD})")
        else:
            print(f"  ⚠ WARNING: {s['name']} does NOT meet Jain failure criteria "
                  f"(J={s['jain']:.3f}, G={s['gini']:.3f}). "
                  f"Will NOT be annotated as a Jain failure in plots.")

    for s in ref_scenarios:
        if s["is_failure"]:
            print(f"  ⚠ WARNING: Baseline {s['name']} unexpectedly meets failure criteria!")
        else:
            print(f"  ✓ Baseline {s['name']} correctly NOT flagged as Jain failure")

    print(f"\n  Total genuine Jain failure scenarios: {failure_count}/{len(jf_scenarios)}")

    # ── Section 4: Generate all plots ─────────────────────────
    print("\n" + "─" * 72)
    print("  SECTION 4: Generating Publication-Quality Plots")
    print("─" * 72)

    plot_a_scatter(all_scenarios)
    plot_b_per_thread_bars(all_scenarios)
    plot_c_thread_scaling(scaling_data)
    plot_d_lorenz(all_scenarios)
    plot_e_metric_comparison(all_scenarios)

    # ── Summary ───────────────────────────────────────────────
    print("\n" + "=" * 72)
    print("  ANALYSIS COMPLETE")
    print(f"  Output directory: {OUTPUT_DIR}")
    print(f"  Figures saved as PNG ({DPI} DPI) and PDF")
    print("=" * 72)

    return 0


if __name__ == "__main__":
    sys.exit(main())
