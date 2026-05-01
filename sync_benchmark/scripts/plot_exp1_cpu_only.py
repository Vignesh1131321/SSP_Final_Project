#!/usr/bin/env python3
"""Create a research-grade graph for Experiment 1 CPU utilization."""

from pathlib import Path
import pandas as pd
import matplotlib.pyplot as plt


def main():
    in_csv = Path("output/cpu_analysis/exp1_cpu_only_utilization_cs2000.csv")
    out_dir = Path("output/cpu_analysis/new_graphs")
    out_dir.mkdir(parents=True, exist_ok=True)

    if not in_csv.exists():
        raise SystemExit(f"Missing input CSV: {in_csv}")

    df = pd.read_csv(in_csv)
    df = df[df["status"] == "ok"].copy()
    if df.empty:
        raise SystemExit("No successful rows found in CSV.")

    df["threads"] = df["threads"].astype(int)
    df["normalized_per_core_util"] = df["normalized_per_core_util"].astype(float)

    summary = (
        df.groupby(["primitive", "threads"], as_index=False)["normalized_per_core_util"]
        .agg(["mean", "std"])  # type: ignore[arg-type]
        .reset_index()
    )

    # Use uniform categorical spacing on x-axis regardless of numeric gaps
    thread_vals = sorted(df["threads"].unique())
    thread_pos = {t: i for i, t in enumerate(thread_vals)}

    plt.style.use("seaborn-v0_8-whitegrid")
    fig, ax = plt.subplots(figsize=(10, 6), dpi=180)

    colors = {
        "mutex": "#1f77b4",
        "spinlock": "#d62728",
        "mcs_lock": "#2ca02c",
        "semaphore": "#9467bd",
    }
    markers = {
        "mutex": "o",
        "spinlock": "s",
        "mcs_lock": "^",
        "semaphore": "D",
    }

    for primitive in sorted(summary["primitive"].unique()):
        sub = summary[summary["primitive"] == primitive].sort_values("threads")
        yerr = sub["std"].fillna(0.0)
        x = [thread_pos[int(t)] for t in sub["threads"]]
        ax.errorbar(
            x,
            sub["mean"],
            yerr=yerr,
            label=primitive,
            color=colors.get(primitive, None),
            marker=markers.get(primitive, "o"),
            linewidth=2.2,
            markersize=6,
            capsize=3,
            alpha=0.95,
        )

    ax.set_title("Experiment 1: Normalized Per-Core CPU Utilization vs Thread Count", fontsize=13, pad=12)
    ax.set_xlabel("Thread Count", fontsize=11)
    ax.set_ylabel("Normalized Per-Core CPU Utilization", fontsize=11)
    ax.set_ylim(bottom=0)
    ax.set_xticks(list(range(len(thread_vals))))
    ax.set_xticklabels([str(t) for t in thread_vals])
    ax.set_xlim(-0.5, len(thread_vals) - 0.5)
    ax.grid(True, linestyle="--", alpha=0.35)
    ax.legend(title="Lock", frameon=True)

    fig.tight_layout()
    png_path = out_dir / "exp1_cpu_utilization_research_grade.png"
    pdf_path = out_dir / "exp1_cpu_utilization_research_grade.pdf"
    fig.savefig(png_path, dpi=300)
    fig.savefig(pdf_path)

    print(f"Wrote graph: {png_path}")
    print(f"Wrote graph: {pdf_path}")


if __name__ == "__main__":
    main()
