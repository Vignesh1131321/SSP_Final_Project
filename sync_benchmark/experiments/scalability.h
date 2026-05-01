/**
 * experiments/scalability.h / scalability.cpp
 *
 * Experiment 1 — Lock Scalability
 * ───────────────────────────────
 * Measures throughput (ops/sec) and mean latency for each
 * primitive as thread count increases from 1 to max_threads.
 * Critical section size is fixed at 0 (pure lock overhead).
 *
 * Research question answered:
 *   Q1 — How do synchronisation primitives scale with thread count?
 *   Q4 — Effect of CPU pinning vs OS scheduling
 */
#pragma once
#include "../benchmark.h"
#include "../utils/csv_writer.h"
#include "../locks/lock_dispatch.h"
#include "experiment_runner.h"

#include <iostream>
#include <iomanip>
#include <atomic>
#include <vector>

void run_scalability_experiment(const BenchmarkConfig& cfg, CsvWriter& csv);
