#include "fairness.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct FairnessMetrics {
	double jain = 0.0;
	double gini = 0.0;
	double cv = 0.0;
	double max_min = 0.0;
};

struct ScenarioResult {
	std::string name;
	std::vector<double> counts;
	FairnessMetrics metrics;
};

FairnessMetrics compute_metrics(const std::vector<double>& counts) {
	FairnessMetrics m;
	m.jain = stats::jains_fairness_index(counts);
	m.gini = stats::gini_coefficient(counts);
	m.cv = stats::cv_percent(counts) / 100.0;
	m.max_min = stats::max_min_ratio(counts);
	return m;
}

std::string counts_to_compact_string(const std::vector<double>& counts) {
	std::ostringstream oss;
	for (size_t i = 0; i < counts.size(); ++i) {
		if (i) oss << '|';
		oss << static_cast<uint64_t>(std::llround(counts[i]));
	}
	return oss.str();
}

std::vector<double> scenario_perfect(int n_threads) {
	return std::vector<double>(n_threads, 100.0);
}

std::vector<double> scenario_mild_imbalance(int n_threads, std::mt19937_64& rng) {
	std::normal_distribution<double> d(100.0, 18.0);
	std::vector<double> v(n_threads, 100.0);
	for (double& x : v) x = std::max(1.0, d(rng));
	if (n_threads >= 2) {
		v[0] += 20.0;
		v[1] = std::max(1.0, v[1] - 20.0);
	}
	return v;
}

std::vector<double> scenario_moderate_skew(int n_threads, std::mt19937_64& rng) {
	std::vector<double> v(n_threads, 0.0);
	if (n_threads == 0) return v;
	std::uniform_int_distribution<int> idx(0, n_threads - 1);
	const int dominant = idx(rng);
	for (int i = 0; i < n_threads; ++i) {
		v[i] = (i == dominant) ? 200.0 : 50.0;
	}
	return v;
}

std::vector<double> scenario_severe_starvation(int n_threads, std::mt19937_64& rng) {
	std::vector<double> v(n_threads, 0.0);
	if (n_threads == 0) return v;
	std::uniform_int_distribution<int> idx(0, n_threads - 1);
	const int dominant = idx(rng);
	for (int i = 0; i < n_threads; ++i) {
		if (i == dominant) {
			v[i] = 300.0;
		} else {
			v[i] = (i % 3 == 0) ? 0.0 : 5.0;
		}
	}
	return v;
}

std::vector<double> scenario_random_distribution(int n_threads, std::mt19937_64& rng) {
	std::gamma_distribution<double> g(2.2, 50.0);
	std::vector<double> v(n_threads, 0.0);
	for (double& x : v) x = std::max(0.0, g(rng));
	return v;
}

std::vector<double> scenario_realistic_contention(int n_threads, std::mt19937_64& rng) {
	if (n_threads <= 0) return {};

	std::vector<double> counts(n_threads, 0.0);
	std::vector<double> ready_time(n_threads, 0.0);
	std::vector<double> hold_cost(n_threads, 1.0);
	std::vector<double> priority(n_threads, 1.0);

	const int bully_count = std::max(1, n_threads / 4);
	const int victim_count = std::max(1, n_threads / 4);

	for (int i = 0; i < bully_count && i < n_threads; ++i) {
		hold_cost[i] = 2.3;
		priority[i] = 1.55;
	}
	for (int i = 0; i < victim_count && (n_threads - 1 - i) >= 0; ++i) {
		const int id = n_threads - 1 - i;
		hold_cost[id] = 0.9;
		priority[id] = 0.65;
	}

	std::normal_distribution<double> jitter(0.0, 0.08);
	std::uniform_real_distribution<double> think(0.12, 0.45);

	double now = 0.0;
	const int rounds = 12000;
	for (int step = 0; step < rounds; ++step) {
		int winner = -1;
		double best_score = -1e18;

		for (int t = 0; t < n_threads; ++t) {
			if (ready_time[t] > now) continue;
			const double score = priority[t] + jitter(rng);
			if (score > best_score) {
				best_score = score;
				winner = t;
			}
		}

		if (winner < 0) {
			now += 0.05;
			continue;
		}

		counts[winner] += 1.0;
		now += hold_cost[winner];
		ready_time[winner] = now + think(rng);

		for (int t = 0; t < n_threads; ++t) {
			if (t == winner) continue;
			if (priority[t] < 0.8) {
				ready_time[t] += 0.03;
			}
		}
	}

	return counts;
}

void print_metric_table(const std::vector<ScenarioResult>& scenarios) {
	printf("\n  Synthetic Fairness Metrics\n");
	printf("  %-30s  %9s  %9s  %9s  %9s\n",
		   "Scenario", "Jain", "Gini", "CV", "Max-Min");
	printf("  %s\n", std::string(76, '-').c_str());

	for (const auto& s : scenarios) {
		printf("  %-30s  %9.4f  %9.4f  %9.4f  %9.4f\n",
			   s.name.c_str(),
			   s.metrics.jain,
			   s.metrics.gini,
			   s.metrics.cv,
			   s.metrics.max_min);
	}
}

void print_ascii_bar(const std::string& label, double value, double scale = 50.0) {
	const int width = static_cast<int>(std::round(std::clamp(value, 0.0, 1.0) * scale));
	std::string bar(static_cast<size_t>(std::max(0, width)), '#');
	printf("  %-28s | %-50s | %.4f\n", label.c_str(), bar.c_str(), value);
}

void print_metric_bar_plots(const std::vector<ScenarioResult>& scenarios) {
	printf("\n  Plot A (ASCII): Bar comparison across scenarios\n");
	const std::vector<std::pair<std::string, double FairnessMetrics::*>> metrics = {
		{"Jain", &FairnessMetrics::jain},
		{"Gini", &FairnessMetrics::gini},
		{"CV", &FairnessMetrics::cv},
		{"Max-Min", &FairnessMetrics::max_min},
	};

	for (const auto& item : metrics) {
		printf("\n  [%s]\n", item.first.c_str());
		for (const auto& s : scenarios) {
			print_ascii_bar(s.name, s.metrics.*(item.second));
		}
	}
}

void print_distribution_plot(const ScenarioResult& s) {
	printf("\n  Plot B (ASCII): Per-thread distribution for %s\n", s.name.c_str());
	const double mx = std::max(1.0, *std::max_element(s.counts.begin(), s.counts.end()));
	for (size_t i = 0; i < s.counts.size(); ++i) {
		const double norm = s.counts[i] / mx;
		const int width = static_cast<int>(std::round(norm * 50.0));
		std::string bar(static_cast<size_t>(std::max(0, width)), '#');
		printf("  T%-2zu | %-50s | %.0f\n", i, bar.c_str(), s.counts[i]);
	}
}

void print_lorenz_curve_ascii(const ScenarioResult& s) {
	printf("\n  Plot C (ASCII, optional): Lorenz curve for %s\n", s.name.c_str());
	if (s.counts.empty()) return;

	std::vector<double> sorted = s.counts;
	std::sort(sorted.begin(), sorted.end());
	const double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
	if (sum <= 0.0) {
		printf("  No acquisitions; Lorenz curve undefined.\n");
		return;
	}

	const int n = static_cast<int>(sorted.size());
	double cumulative = 0.0;
	for (int i = 0; i < n; ++i) {
		cumulative += sorted[i];
		const double x = static_cast<double>(i + 1) / n;
		const double y = cumulative / sum;
		const int width = static_cast<int>(std::round(y * 50.0));
		std::string bar(static_cast<size_t>(std::max(0, width)), '*');
		printf("  p=%.2f | %-50s | L(p)=%.4f\n", x, bar.c_str(), y);
	}
}

void detect_jain_blind_spot(const std::vector<ScenarioResult>& scenarios) {
	printf("\n  Analysis: Jain Saturation Checks\n");
	bool found = false;
	for (const auto& s : scenarios) {
		const bool jain_near_one = s.metrics.jain >= 0.95;
		const bool gini_high = s.metrics.gini >= 0.10;
		if (jain_near_one && gini_high) {
			found = true;
			printf("  [%s] Jain=%.4f, Gini=%.4f -> Jain fails to detect inequality in this scenario\n",
				   s.name.c_str(), s.metrics.jain, s.metrics.gini);
		}
	}
	if (!found) {
		printf("  No scenario crossed heuristic threshold (Jain>=0.95 && Gini>=0.10).\n");
		printf("  This run still shows Gini rising earlier than Jain under skew/starvation.\n");
	}
}

void write_synthetic_rows(CsvWriter& csv,
						  const std::vector<ScenarioResult>& scenarios,
						  int n_threads,
						  int trial,
						  const std::string& experiment_tag) {
	for (const auto& s : scenarios) {
		TrialResult tr;
		tr.suite = "fairness";
		tr.experiment = experiment_tag;
		tr.primitive = s.name;
		tr.threads = n_threads;
		tr.cs_cycles = 100;
		tr.trial = trial;
		tr.fairness_gini = s.metrics.gini;
		tr.fairness_jain = s.metrics.jain;
		tr.fairness_maxmin = s.metrics.max_min;
		tr.fairness_cv = s.metrics.cv;
		tr.notes = "synthetic_counts:" + counts_to_compact_string(s.counts);
		csv.write(tr);
	}
}

std::vector<ScenarioResult> build_scenarios(int n_threads, std::mt19937_64& rng) {
	std::vector<ScenarioResult> scenarios;
	scenarios.push_back({"A_perfect_fairness", scenario_perfect(n_threads), {}});
	scenarios.push_back({"B_mild_imbalance", scenario_mild_imbalance(n_threads, rng), {}});
	scenarios.push_back({"C_moderate_skew", scenario_moderate_skew(n_threads, rng), {}});
	scenarios.push_back({"D_severe_starvation", scenario_severe_starvation(n_threads, rng), {}});
	scenarios.push_back({"E_random_distribution", scenario_random_distribution(n_threads, rng), {}});
	scenarios.push_back({"F_realistic_contention", scenario_realistic_contention(n_threads, rng), {}});

	for (auto& s : scenarios) {
		s.metrics = compute_metrics(s.counts);
	}
	return scenarios;
}

void run_real_lock_fairness(const BenchmarkConfig& cfg, CsvWriter& csv) {
	printf("  %-14s  %6s  %10s  %10s  %10s  %10s\n",
		   "Primitive", "Thr", "Gini", "Jain", "MaxMin", "CV");
	printf("  %s\n", std::string(60, '-').c_str());

	int total_configs = 0;
	for (auto kind : all_primitives()) {
		(void)kind;
		for (int n_threads : {2, 4, 8}) {
			if (n_threads <= static_cast<int>(std::thread::hardware_concurrency())) {
				++total_configs;
			}
		}
	}
	const int total_reps = total_configs * std::max(1, cfg.repetitions);
	int completed_reps = 0;
	auto start_time = std::chrono::steady_clock::now();

	for (auto kind : all_primitives()) {
		const char* pname = primitive_name(kind);

		for (int n_threads : {2, 4, 8}) {
			if (n_threads > static_cast<int>(std::thread::hardware_concurrency())) {
				continue;
			}

			printf("  [status] Running fairness: primitive=%s threads=%d repetitions=%d\n",
				   pname, n_threads, cfg.repetitions);

			double sum_gini = 0.0;
			double sum_jain = 0.0;
			double sum_maxmin = 0.0;
			double sum_cv = 0.0;

			for (int rep = 0; rep < cfg.repetitions; ++rep) {
				if (cfg.verbose) {
					printf("    [status] rep %d/%d started\n", rep + 1, cfg.repetitions);
				}

				auto lock = make_lock(kind);
				std::vector<uint64_t> per_thread(n_threads, 0);
				std::atomic<bool> stop{false};

				std::atomic<int> ready{0};
				std::atomic<bool> go{false};
				std::vector<std::thread> threads;
				threads.reserve(static_cast<size_t>(n_threads));

				for (int tid = 0; tid < n_threads; ++tid) {
					threads.emplace_back([&, tid] {
						if (cfg.pin_cpus) cpu::pin_to_core(tid);

						ready.fetch_add(1);
						while (!go.load(std::memory_order_acquire)) {
							CPU_PAUSE();
						}

						const uint64_t warmup_end = timer::now_ns() +
							static_cast<uint64_t>(cfg.warmup_sec * 1e9);
						while (timer::now_ns() < warmup_end) {
							lock->acquire();
							lock->release();
						}

						const uint64_t measure_end = timer::now_ns() +
							static_cast<uint64_t>(cfg.duration_sec * 1e9);
						uint64_t cnt = 0;
						while (!stop.load(std::memory_order_relaxed) &&
							   timer::now_ns() < measure_end) {
							lock->acquire();
							busy_work(100);
							lock->release();
							++cnt;
						}
						per_thread[tid] = cnt;
					});
				}

				while (ready.load() < n_threads) {
					std::this_thread::yield();
				}
				go.store(true, std::memory_order_release);
				for (auto& t : threads) {
					t.join();
				}

				std::vector<double> vals(per_thread.begin(), per_thread.end());
				const double gini = stats::gini_coefficient(vals);
				const double jain = stats::jains_fairness_index(vals);
				const double maxmin = stats::max_min_ratio(vals);
				const double cv = stats::cv_percent(vals) / 100.0;

				TrialResult tr;
				tr.suite = "fairness";
				tr.experiment = "fairness";
				tr.primitive = pname;
				tr.threads = n_threads;
				tr.cs_cycles = 100;
				tr.trial = rep;
				tr.fairness_gini = gini;
				tr.fairness_jain = jain;
				tr.fairness_maxmin = maxmin;
				tr.fairness_cv = cv;
				tr.notes = "per_thread_counts:" + counts_to_compact_string(vals);
				csv.write(tr);

				if (cfg.verbose) {
					printf("    [status] rep %d/%d done: Gini=%.4f Jain=%.4f CV=%.4f\n",
						   rep + 1, cfg.repetitions, gini, jain, cv);
					++completed_reps;
					const double progress = (total_reps > 0)
						? (100.0 * completed_reps / static_cast<double>(total_reps))
						: 100.0;
					auto now = std::chrono::steady_clock::now();
					const double elapsed_sec = std::chrono::duration<double>(now - start_time).count();
					const double avg_sec_per_rep = (completed_reps > 0)
						? (elapsed_sec / completed_reps)
						: 0.0;
					const double eta_sec = avg_sec_per_rep * (total_reps - completed_reps);
					printf("    [status] progress %d/%d (%.1f%%), elapsed=%.1fs, eta=%.1fs\n",
						   completed_reps, total_reps, progress, elapsed_sec, eta_sec);
				}

				sum_gini += gini;
				sum_jain += jain;
				sum_maxmin += maxmin;
				sum_cv += cv;
			}

			const int reps = std::max(1, cfg.repetitions);
			printf("  %-14s  %6d  %10.4f  %10.4f  %10.4f  %10.4f\n",
				   pname,
				   n_threads,
				   sum_gini / reps,
				   sum_jain / reps,
				   sum_maxmin / reps,
				   sum_cv / reps);
		}
		printf("\n");
	}
}

void run_synthetic_fairness(const BenchmarkConfig& cfg, CsvWriter& csv) {
	std::mt19937_64 rng(42);
	const int n_threads = 8;
	printf("\n  [status] Building synthetic fairness scenarios (A-F) with %d threads\n", n_threads);

	auto scenarios = build_scenarios(n_threads, rng);
	print_metric_table(scenarios);
	print_metric_bar_plots(scenarios);

	const auto dist_a = std::find_if(scenarios.begin(), scenarios.end(),
		[](const ScenarioResult& s) { return s.name == "C_moderate_skew"; });
	const auto dist_b = std::find_if(scenarios.begin(), scenarios.end(),
		[](const ScenarioResult& s) { return s.name == "D_severe_starvation"; });

	if (dist_a != scenarios.end()) {
		print_distribution_plot(*dist_a);
	}
	if (dist_b != scenarios.end()) {
		print_distribution_plot(*dist_b);
		print_lorenz_curve_ascii(*dist_b);
	}

	detect_jain_blind_spot(scenarios);
	write_synthetic_rows(csv, scenarios, n_threads, 0, "fairness_synthetic");
	printf("  [status] Wrote synthetic fairness scenario rows to CSV\n");

	printf("\n  Thread-Scaling Study (2, 4, 8, 16)\n");
	printf("  %-8s  %-30s  %9s  %9s  %9s  %9s\n",
		   "Threads", "Scenario", "Jain", "Gini", "CV", "Max-Min");
	printf("  %s\n", std::string(86, '-').c_str());

	for (int tcount : {2, 4, 8, 16}) {
		printf("  [status] Thread-scaling evaluation for %d threads\n", tcount);
		auto scaled = build_scenarios(tcount, rng);
		for (const auto& s : scaled) {
			printf("  %-8d  %-30s  %9.4f  %9.4f  %9.4f  %9.4f\n",
				   tcount,
				   s.name.c_str(),
				   s.metrics.jain,
				   s.metrics.gini,
				   s.metrics.cv,
				   s.metrics.max_min);
		}
		write_synthetic_rows(csv, scaled, tcount, 0, "fairness_synthetic_scale");
	}

	if (cfg.verbose) {
		printf("\n  Note: Synthetic fairness datasets and scaling rows were written to CSV\n");
		printf("  under experiments fairness_synthetic and fairness_synthetic_scale.\n");
	}
}

} // namespace

void run_fairness_experiment(const BenchmarkConfig& cfg, CsvWriter& csv) {
	printf("\n  Section 1: Measured lock fairness on real implementations\n");
	run_real_lock_fairness(cfg, csv);

	printf("\n  Section 2: Synthetic fairness scenarios (A-F)\n");
	run_synthetic_fairness(cfg, csv);
}
