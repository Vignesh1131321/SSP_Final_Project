#!/usr/bin/env python3
"""
Build CPU-only CSVs for Experiment 1 from per-trial JSON stats.

Inputs:
  output/cpu_analysis/cpu_stats/cpu_stats_<primitive>_<threads>t_trial<trial>.json

Outputs:
  output/cpu_analysis/exp1_cpu_util_summary.csv
  output/cpu_analysis/exp1_cpu_per_core.csv
"""

import csv
import json
import re
from pathlib import Path


def parse_name(path: Path):
    m = re.match(r"^cpu_stats_(.+)_([0-9]+)t_trial([0-9]+)\.json$", path.name)
    if not m:
        return None
    primitive = m.group(1)
    threads = int(m.group(2))
    trial = int(m.group(3))
    return primitive, threads, trial


def main() -> int:
    root = Path("output/cpu_analysis")
    stats_dir = root / "cpu_stats"
    summary_csv = root / "exp1_cpu_util_summary.csv"
    per_core_csv = root / "exp1_cpu_per_core.csv"

    stats_files = sorted(stats_dir.glob("cpu_stats_*t_trial*.json"))
    if not stats_files:
        print(f"No CPU stats files found in: {stats_dir}")
        return 1

    summary_rows = []
    per_core_rows = []

    for p in stats_files:
        parsed = parse_name(p)
        if parsed is None:
            continue

        primitive, threads, trial = parsed
        with p.open("r", encoding="utf-8") as f:
            data = json.load(f)

        agg = data.get("aggregate", {})
        avg_per_core = float(agg.get("mean", 0.0))
        num_cores = int(agg.get("num_cores", 0))
        total_equiv_cores = (avg_per_core * num_cores) / 100.0 if num_cores > 0 else 0.0
        normalized_per_core = avg_per_core / 100.0

        summary_rows.append({
            "primitive": primitive,
            "threads": threads,
            "trial": trial,
            "num_cores": num_cores,
            "avg_per_core_util_pct": f"{avg_per_core:.6f}",
            "total_equiv_busy_cores": f"{total_equiv_cores:.6f}",
            "normalized_per_core_util": f"{normalized_per_core:.6f}",
            "source_json": p.name,
        })

        for core_name, stats in data.items():
            if core_name == "aggregate":
                continue
            core_mean = float(stats.get("mean", 0.0))
            per_core_rows.append({
                "primitive": primitive,
                "threads": threads,
                "trial": trial,
                "core": core_name,
                "core_mean_util_pct": f"{core_mean:.6f}",
                "source_json": p.name,
            })

    summary_rows.sort(key=lambda r: (r["threads"], r["primitive"], r["trial"]))
    per_core_rows.sort(key=lambda r: (r["threads"], r["primitive"], r["trial"], r["core"]))

    with summary_csv.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "primitive",
                "threads",
                "trial",
                "num_cores",
                "avg_per_core_util_pct",
                "total_equiv_busy_cores",
                "normalized_per_core_util",
                "source_json",
            ],
        )
        writer.writeheader()
        writer.writerows(summary_rows)

    with per_core_csv.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "primitive",
                "threads",
                "trial",
                "core",
                "core_mean_util_pct",
                "source_json",
            ],
        )
        writer.writeheader()
        writer.writerows(per_core_rows)

    print(f"Wrote summary CSV: {summary_csv} ({len(summary_rows)} rows)")
    print(f"Wrote per-core CSV: {per_core_csv} ({len(per_core_rows)} rows)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
