#!/usr/bin/env python3
import argparse
import csv
import re
import subprocess
import sys
import tempfile
from pathlib import Path


PERF_EVENTS = [
    "cycles",
    "instructions",
    "cache-misses",
    "branch-misses",
]

PERF_FIELD_MAP = {
    "cycles": "perf_cycles",
    "instructions": "perf_instructions",
    "cache-misses": "perf_cache_misses",
    "branch-misses": "perf_branch_misses",
}

PERF_LINE_RE = re.compile(r"^\s*([0-9,]+)\s+([a-zA-Z0-9\-]+)\b")


def parse_perf_stat(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            match = PERF_LINE_RE.search(line)
            if not match:
                continue
            number_s, event_name = match.groups()
            if event_name in PERF_FIELD_MAP:
                values[PERF_FIELD_MAP[event_name]] = str(int(number_s.replace(",", "")))
    missing = [field for field in PERF_FIELD_MAP.values() if field not in values]
    if missing:
        raise RuntimeError(f"perf stat output missing counters: {', '.join(missing)}")
    return values


def load_csv(path: Path):
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        rows = list(reader)
        return rows, reader.fieldnames or []


def write_csv(path: Path, rows, fieldnames):
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def run_one_row(bench_dir: Path, row: dict, trial_csv: Path, perf_txt: Path, duration: int, warmup: int) -> tuple[dict, dict[str, str]]:
    benchmark = bench_dir / "sync_bench"
    if not benchmark.exists():
        raise FileNotFoundError(f"missing benchmark binary: {benchmark}")

    thread_count = str(int(row["threads"]))
    cmd = [
        "perf",
        "stat",
        "-e",
        ",".join(PERF_EVENTS),
        "-o",
        str(perf_txt),
        "--",
        str(benchmark),
        "--suite",
        "latency",
        "--primitive",
        str(row["primitive"]),
        "--threads",
        thread_count,
        "--duration",
        str(duration),
        "--warmup",
        str(warmup),
        "--repetitions",
        "1",
        "--pin-cpus",
        "--outdir",
        str(trial_csv.parent),
        "--csv",
        trial_csv.name,
    ]
    subprocess.run(cmd, cwd=bench_dir, check=True)

    run_rows, _ = load_csv(trial_csv)
    if len(run_rows) != 1:
        raise RuntimeError(f"expected one row from benchmark run, got {len(run_rows)}")

    perf_vals = parse_perf_stat(perf_txt)
    return run_rows[0], perf_vals


def main() -> int:
    parser = argparse.ArgumentParser(description="Collect per-row perf counters for Experiment 5.")
    parser.add_argument("--orig-csv", required=True, help="Experiment 5 source CSV used as the row schedule")
    parser.add_argument("--out-csv", required=True, help="Output CSV with per-row perf counters")
    parser.add_argument("--bench-dir", default=None, help="Path to the sync_benchmark directory")
    parser.add_argument("--duration", type=int, default=3)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--limit", type=int, default=0, help="Run only the first N rows for validation")
    args = parser.parse_args()

    orig_path = Path(args.orig_csv).resolve()
    out_path = Path(args.out_csv).resolve()
    bench_dir = Path(args.bench_dir).resolve() if args.bench_dir else Path(__file__).resolve().parents[1]

    orig_rows, orig_fields = load_csv(orig_path)
    if not orig_rows:
        raise RuntimeError(f"no rows found in {orig_path}")

    rows_to_run = orig_rows[: args.limit] if args.limit and args.limit > 0 else orig_rows

    out_fields = list(orig_fields)
    for field in PERF_FIELD_MAP.values():
        if field not in out_fields:
            out_fields.append(field)

    with tempfile.TemporaryDirectory(prefix="ssp_exp5_rowperf_") as tmp_root:
        tmp_root_path = Path(tmp_root)
        collected_rows = []

        for index, source_row in enumerate(rows_to_run, start=1):
            primitive = source_row.get("primitive", "")
            threads = source_row.get("threads", "")
            trial = source_row.get("trial", "")
            print(f"[{index}/{len(rows_to_run)}] primitive={primitive} threads={threads} trial={trial}", flush=True)

            trial_dir = tmp_root_path / f"run_{index:03d}"
            trial_dir.mkdir(parents=True, exist_ok=True)
            trial_csv = trial_dir / "latency_research_8rep_rowperf.csv"
            perf_txt = trial_dir / "perf_stat.txt"

            run_row, perf_vals = run_one_row(bench_dir, source_row, trial_csv, perf_txt, args.duration, args.warmup)

            out_row = dict(source_row)
            out_row.update(run_row)
            out_row["trial"] = source_row.get("trial", out_row.get("trial", ""))
            for field, value in perf_vals.items():
                out_row[field] = value
            collected_rows.append(out_row)

        write_csv(out_path, collected_rows, out_fields)

    print(f"Saved {len(collected_rows)} rows to {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())