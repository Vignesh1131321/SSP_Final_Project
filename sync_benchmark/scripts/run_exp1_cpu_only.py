#!/usr/bin/env python3
"""Run Experiment 1 as CPU-only collection and append one CSV row per configuration.

Method used: per-core busy/idle deltas from /proc/stat.
"""

import csv
import os
import statistics
import subprocess
import threading
import time
import argparse
from datetime import datetime
from pathlib import Path


class CPUMonitor:
    def __init__(self, poll_interval=0.05):
        self.poll_interval = poll_interval
        self.samples = []
        self.stop_event = threading.Event()
        self.thread = None

    @staticmethod
    def _read_proc_stat():
        stats = {}
        with open("/proc/stat", "r", encoding="utf-8") as f:
            for line in f:
                if not line.startswith("cpu"):
                    continue
                parts = line.split()
                if len(parts) < 5:
                    continue
                name = parts[0]
                user = int(parts[1])
                nice = int(parts[2])
                system = int(parts[3])
                idle = int(parts[4])
                iowait = int(parts[5]) if len(parts) > 5 else 0
                irq = int(parts[6]) if len(parts) > 6 else 0
                softirq = int(parts[7]) if len(parts) > 7 else 0
                steal = int(parts[8]) if len(parts) > 8 else 0

                idle_all = idle + iowait
                non_idle = user + nice + system + irq + softirq + steal
                total = idle_all + non_idle
                stats[name] = (non_idle, total)
        return stats

    def _loop(self):
        prev = self._read_proc_stat()
        while not self.stop_event.is_set():
            time.sleep(self.poll_interval)
            cur = self._read_proc_stat()
            sample = {}
            for cpu, (cur_non_idle, cur_total) in cur.items():
                if cpu == "cpu" or cpu not in prev:
                    continue
                prev_non_idle, prev_total = prev[cpu]
                d_non_idle = cur_non_idle - prev_non_idle
                d_total = cur_total - prev_total
                util = (d_non_idle / d_total) * 100.0 if d_total > 0 else 0.0
                sample[cpu] = max(0.0, min(100.0, util))
            if sample:
                self.samples.append(sample)
            prev = cur

    def start(self):
        self.samples = []
        self.stop_event.clear()
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        if self.thread is not None:
            self.thread.join(timeout=2)

        if not self.samples:
            return {
                "num_cores": 0,
                "samples": 0,
                "avg_per_core_util_pct": 0.0,
                "normalized_per_core_util": 0.0,
                "total_equiv_busy_cores": 0.0,
                "stdev_per_core_util_pct": 0.0,
            }

        per_core = {}
        for s in self.samples:
            for cpu, val in s.items():
                per_core.setdefault(cpu, []).append(val)

        core_means = [statistics.mean(v) for v in per_core.values() if v]
        avg_per_core = statistics.mean(core_means) if core_means else 0.0
        stdev_per_core = statistics.stdev(core_means) if len(core_means) > 1 else 0.0
        num_cores = len(core_means)

        return {
            "num_cores": num_cores,
            "samples": len(self.samples),
            "avg_per_core_util_pct": avg_per_core,
            "normalized_per_core_util": avg_per_core / 100.0,
            "total_equiv_busy_cores": (avg_per_core * num_cores) / 100.0,
            "stdev_per_core_util_pct": stdev_per_core,
        }


def load_completed(csv_path: Path):
    done = set()
    if not csv_path.exists() or csv_path.stat().st_size == 0:
        return done

    with csv_path.open("r", newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            key = (row.get("primitive", ""), int(row.get("threads", "0")), int(row.get("trial", "0")))
            done.add(key)
    return done


def append_row(csv_path: Path, row: dict):
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    write_header = (not csv_path.exists()) or csv_path.stat().st_size == 0

    fieldnames = [
        "timestamp",
        "primitive",
        "threads",
        "trial",
        "cs_cycles",
        "duration_sec",
        "warmup_sec",
        "num_cores",
        "samples",
        "avg_per_core_util_pct",
        "stdev_per_core_util_pct",
        "normalized_per_core_util",
        "total_equiv_busy_cores",
        "status",
        "notes",
    ]

    with csv_path.open("a", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        if write_header:
            writer.writeheader()
        writer.writerow(row)


def run_one(sync_bench: str, primitive: str, threads: int, duration: int, warmup: int, cs_cycles: int):
    cmd = [
        sync_bench,
        "--suite", "scalability",
        "--threads", str(threads),
        "--duration", str(duration),
        "--warmup", str(warmup),
        "--repetitions", "1",
        "--outdir", "output/cpu_analysis/tmp_perf",
        "--csv", "perf_sink.csv",
        "--primitive", primitive,
        "--cs-cycles", str(cs_cycles),
    ]

    monitor = CPUMonitor(poll_interval=0.05)
    monitor.start()
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=max(90, duration + warmup + 20))
        metrics = monitor.stop()
        return result.returncode, metrics, result.stderr.strip()
    except subprocess.TimeoutExpired:
        metrics = monitor.stop()
        return 124, metrics, "timeout"


def main():
    parser = argparse.ArgumentParser(description="CPU-only Experiment 1 collector")
    parser.add_argument("--threads", default="1,2,4,8,16,32", help="Comma-separated thread counts")
    parser.add_argument("--primitives", default="mutex,spinlock,mcs_lock,semaphore", help="Comma-separated primitive list")
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--duration", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--cs-cycles", type=int, default=2000,
                        help="Busy-loop cycles inside CS (default: 2000; original was 100)")
    parser.add_argument("--output-csv", default="output/cpu_analysis/exp1_cpu_only_utilization_cs2000.csv")
    args = parser.parse_args()

    out_csv = Path(args.output_csv)

    sync_bench = "./sync_bench"
    duration_sec = args.duration
    warmup_sec = args.warmup
    cs_cycles = args.cs_cycles
    thread_counts = [int(x.strip()) for x in args.threads.split(",") if x.strip()]
    primitives = [x.strip() for x in args.primitives.split(",") if x.strip()]
    repetitions = args.repetitions

    if not Path(sync_bench).exists():
        raise SystemExit("sync_bench not found. Run from sync_benchmark directory.")

    completed = load_completed(out_csv)

    total = len(primitives) * len(thread_counts) * repetitions
    idx = 0

    for primitive in primitives:
        for threads in thread_counts:
            for trial in range(repetitions):
                idx += 1
                key = (primitive, threads, trial)
                if key in completed:
                    print(f"[{idx}/{total}] SKIP existing {primitive} t={threads} trial={trial}")
                    continue

                print(f"[{idx}/{total}] RUN {primitive} t={threads} trial={trial}")
                rc, m, err = run_one(sync_bench, primitive, threads, duration_sec, warmup_sec, cs_cycles)
                row = {
                    "timestamp": datetime.now().isoformat(timespec="seconds"),
                    "primitive": primitive,
                    "threads": threads,
                    "trial": trial,
                    "cs_cycles": cs_cycles,
                    "duration_sec": duration_sec,
                    "warmup_sec": warmup_sec,
                    "num_cores": m["num_cores"],
                    "samples": m["samples"],
                    "avg_per_core_util_pct": f"{m['avg_per_core_util_pct']:.6f}",
                    "stdev_per_core_util_pct": f"{m['stdev_per_core_util_pct']:.6f}",
                    "normalized_per_core_util": f"{m['normalized_per_core_util']:.6f}",
                    "total_equiv_busy_cores": f"{m['total_equiv_busy_cores']:.6f}",
                    "status": "ok" if rc == 0 else f"err_{rc}",
                    "notes": "per-core /proc/stat monitor" if rc == 0 else err,
                }
                append_row(out_csv, row)
                time.sleep(0.2)

    print(f"Done. CPU-only CSV: {out_csv}")


if __name__ == "__main__":
    main()
