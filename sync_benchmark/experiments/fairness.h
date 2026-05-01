/**
 * experiments/fairness.h
 *
 * Experiment 4 — Lock Fairness
 * ─────────────────────────────
 * Each thread records how many times it acquires the lock.
 * We compute the Gini coefficient over the per-thread counts.
 *
 *   Gini = 0  →  perfect fairness (every thread acquires equally)
 *   Gini = 1  →  maximum starvation (one thread monopolises)
 *
 * Research question answered:
 *   Q5 — Lock fairness: variance of acquisition order
 */
#pragma once
#include "../benchmark.h"
#include "../utils/csv_writer.h"
#include "../utils/stats.h"
#include "../utils/timer.h"
#include "../utils/cpu_affinity.h"
#include "../locks/lock_dispatch.h"
#include <vector>
#include <thread>
#include <cstdio>
#include <string>

void run_fairness_experiment(const BenchmarkConfig& cfg, CsvWriter& csv);
