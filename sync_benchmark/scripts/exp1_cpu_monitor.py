#!/usr/bin/env python3
"""
Experiment 1 CPU Utilization Monitoring
========================================
Runs scalability experiment with per-trial CPU utilization monitoring.
Collects per-core CPU usage and calculates normalized metrics.
"""

import subprocess
import os
import sys
import csv
import json
import time
import signal
import threading
from datetime import datetime
from pathlib import Path
import statistics

class CPUMonitor:
    """Monitor CPU utilization in real-time."""
    
    def __init__(self, poll_interval=0.1):
        self.poll_interval = poll_interval
        self.samples = []
        self.stop_event = threading.Event()
        self.thread = None
        self.num_cores = os.cpu_count()
        
    def read_cpu_stats(self):
        """Read CPU stats from /proc/stat"""
        try:
            with open('/proc/stat', 'r') as f:
                lines = f.readlines()
            
            cpu_stats = {}
            for line in lines:
                if line.startswith('cpu'):
                    parts = line.split()
                    if len(parts) < 5:
                        continue
                    cpu_name = parts[0]
                    user = int(parts[1])
                    nice = int(parts[2])
                    system = int(parts[3])
                    idle = int(parts[4])
                    
                    work = user + nice + system
                    total = work + idle
                    if total > 0:
                        util = (work / total) * 100
                    else:
                        util = 0
                    
                    cpu_stats[cpu_name] = {
                        'user': user,
                        'nice': nice,
                        'system': system,
                        'idle': idle,
                        'utilization': util
                    }
            return cpu_stats
        except Exception as e:
            print(f"Error reading CPU stats: {e}", file=sys.stderr)
            return {}
    
    def monitor_loop(self):
        """Continuously monitor CPU utilization."""
        prev_stats = self.read_cpu_stats()
        
        while not self.stop_event.is_set():
            time.sleep(self.poll_interval)
            
            curr_stats = self.read_cpu_stats()
            sample = {}
            
            # Calculate per-core utilization
            for cpu_name in curr_stats:
                if cpu_name == 'cpu':  # Skip aggregate
                    continue
                    
                if cpu_name not in prev_stats:
                    continue
                
                prev = prev_stats[cpu_name]
                curr = curr_stats[cpu_name]
                
                prev_work = prev['user'] + prev['nice'] + prev['system']
                prev_total = prev_work + prev['idle']
                
                curr_work = curr['user'] + curr['nice'] + curr['system']
                curr_total = curr_work + curr['idle']
                
                delta_work = curr_work - prev_work
                delta_total = curr_total - prev_total
                
                if delta_total > 0:
                    util = (delta_work / delta_total) * 100
                else:
                    util = 0
                
                sample[cpu_name] = util
            
            if sample:
                self.samples.append(sample)
            
            prev_stats = curr_stats
    
    def start(self):
        """Start monitoring thread."""
        self.stop_event.clear()
        self.samples = []
        self.thread = threading.Thread(target=self.monitor_loop, daemon=True)
        self.thread.start()
    
    def stop(self):
        """Stop monitoring and return statistics."""
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=2)
        
        if not self.samples:
            return {}
        
        # Calculate statistics per core
        stats = {}
        
        # Aggregate all values per core
        core_values = {}
        for sample in self.samples:
            for core_name, util in sample.items():
                if core_name not in core_values:
                    core_values[core_name] = []
                core_values[core_name].append(util)
        
        # Calculate metrics
        for core_name in sorted(core_values.keys()):
            values = core_values[core_name]
            if values:
                stats[core_name] = {
                    'mean': statistics.mean(values),
                    'median': statistics.median(values),
                    'max': max(values),
                    'min': min(values),
                    'stdev': statistics.stdev(values) if len(values) > 1 else 0
                }
        
        # Calculate aggregate metrics
        all_values = [v for vals in core_values.values() for v in vals]
        if all_values:
            stats['aggregate'] = {
                'mean': statistics.mean(all_values),
                'median': statistics.median(all_values),
                'max': max(all_values),
                'min': min(all_values),
                'stdev': statistics.stdev(all_values) if len(all_values) > 1 else 0,
                'num_cores': len(core_values)
            }
        
        return stats


def run_benchmark_with_monitoring(bench_path, threads, lock_type, trial, output_csv, output_json_dir):
    """Run benchmark with CPU monitoring for a single configuration."""
    
    monitor = CPUMonitor(poll_interval=0.05)
    
    # Build command. Pass outdir separately so benchmark writes CSV inside it.
    output_dir = os.path.dirname(output_csv) or '.'
    cmd = [
        bench_path,
        '--suite', 'scalability',
        '--threads', str(threads),
        '--duration', '5',
        '--warmup', '2',
        '--repetitions', '1',
        '--outdir', output_dir,
        '--csv', os.path.basename(output_csv),
        '--primitive', lock_type
    ]
    
    print(f"[MONITOR] Running: {lock_type} with {threads} threads (trial {trial})")
    print(f"  Command: {' '.join(cmd)}")
    
    try:
        # Start monitoring
        monitor.start()
        
        # Run benchmark
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        
        # Stop monitoring
        cpu_stats = monitor.stop()
        
        if result.returncode != 0:
            print(f"[ERROR] Benchmark failed with return code {result.returncode}", file=sys.stderr)
            print(f"  stdout: {result.stdout}", file=sys.stderr)
            print(f"  stderr: {result.stderr}", file=sys.stderr)
            return False
        
        # Save CPU stats
        if cpu_stats and output_json_dir:
            os.makedirs(output_json_dir, exist_ok=True)
            stats_file = os.path.join(
                output_json_dir,
                f"cpu_stats_{lock_type}_{threads}t_trial{trial}.json"
            )
            with open(stats_file, 'w') as f:
                json.dump(cpu_stats, f, indent=2)
            print(f"  CPU stats saved to: {stats_file}")
        
        return True
        
    except subprocess.TimeoutExpired:
        print(f"[ERROR] Benchmark timeout for {lock_type} with {threads} threads", file=sys.stderr)
        monitor.stop()
        return False
    except Exception as e:
        print(f"[ERROR] Exception: {e}", file=sys.stderr)
        monitor.stop()
        return False


def main():
    """Main execution."""
    
    # Configuration
    sync_bench_path = "./sync_bench"
    output_dir = "output/cpu_analysis"
    output_csv = os.path.join(output_dir, "exp1_cpu_utilization.csv")
    output_json_dir = os.path.join(output_dir, "cpu_stats")
    
    # Thread counts and primitives to test (reduced set for quicker collection)
    thread_counts = [1, 2, 4, 8, 16, 32]
    primitives = ['atomic_cas', 'std::mutex', 'spinlock', 'ticket', 'mcs']
    repetitions = 5  # Number of trials per config (updated per request)
    
    # Create output directory
    os.makedirs(output_dir, exist_ok=True)
    
    # Check if benchmark exists
    if not os.path.exists(sync_bench_path):
        print(f"[ERROR] Benchmark not found at {sync_bench_path}", file=sys.stderr)
        print("Build the benchmark first with: make", file=sys.stderr)
        return 1
    
    print("=" * 70)
    print("EXPERIMENT 1 CPU UTILIZATION MONITORING")
    print("=" * 70)
    print(f"Output CSV: {output_csv}")
    print(f"Output Dir: {output_dir}")
    print(f"Threads:    {thread_counts}")
    print(f"Primitives: {primitives}")
    print(f"Trials:     {repetitions}")
    print("=" * 70)
    print()
    
    # Run experiments
    total_configs = len(thread_counts) * len(primitives) * repetitions
    current = 0
    
    for threads in thread_counts:
        for primitive in primitives:
            for trial in range(repetitions):
                current += 1
                print(f"[{current}/{total_configs}] Starting trial...")
                
                if not run_benchmark_with_monitoring(
                    sync_bench_path,
                    threads,
                    primitive,
                    trial,
                    output_csv,
                    output_json_dir
                ):
                    print(f"[SKIP] Failed to complete {primitive} with {threads} threads")
                
                time.sleep(0.5)  # Brief pause between configs
    
    print()
    print("=" * 70)
    print(f"Experiment complete!")
    print(f"Results saved to: {output_csv}")
    print(f"CPU stats saved to: {output_json_dir}")
    print("=" * 70)
    
    return 0


if __name__ == '__main__':
    sys.exit(main())
