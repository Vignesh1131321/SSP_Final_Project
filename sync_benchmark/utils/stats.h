/**
 * utils/stats.h — Statistical analysis utilities
 *
 * Provides:
 *   - Welford online mean/variance
 *   - Percentile computation (p50, p95, p99)
 *   - Confidence intervals (t-distribution, 95%)
 *   - Gini coefficient (for fairness analysis)
 *   - Coefficient of variation
 *   - Bootstrap resampling
 */
#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <cassert>

namespace stats {

/* ── Welford online accumulator ─────────────────────────── */
class WelfordAccumulator {
    uint64_t n_ = 0;
    double   mean_ = 0.0, M2_ = 0.0;
    double   min_ = 1e18, max_ = -1e18;
public:
    void push(double x) noexcept {
        ++n_;
        double delta = x - mean_;
        mean_ += delta / n_;
        double delta2 = x - mean_;
        M2_ += delta * delta2;
        if (x < min_) min_ = x;
        if (x > max_) max_ = x;
    }
    uint64_t count()    const noexcept { return n_; }
    double   mean()     const noexcept { return mean_; }
    double   variance() const noexcept { return (n_ > 1) ? M2_ / (n_-1) : 0.0; }
    double   stddev()   const noexcept { return std::sqrt(variance()); }
    double   min()      const noexcept { return min_; }
    double   max()      const noexcept { return max_; }
    double   cv()       const noexcept {
        return (mean_ > 0) ? (stddev() / mean_) * 100.0 : 0.0;
    }
};

/* ── Percentile (sorts in place) ─────────────────────────── */
inline double percentile(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    double idx = p * (v.size() - 1);
    size_t lo  = static_cast<size_t>(idx);
    size_t hi  = lo + 1;
    if (hi >= v.size()) return v.back();
    double frac = idx - lo;
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

inline double p50(std::vector<double> v) { return percentile(v, 0.50); }
inline double p95(std::vector<double> v) { return percentile(v, 0.95); }
inline double p99(std::vector<double> v) { return percentile(v, 0.99); }

/* ── 95% Confidence interval (t-distribution, n-1 df) ────── */
struct ConfidenceInterval {
    double mean, lower, upper, margin;
};

/* t critical values for 95% CI (two-tailed) */
static const double T_TABLE[] = {
    0,      // df=0 unused
    12.706, // df=1
    4.303,  // df=2
    3.182,  // df=3
    2.776,  // df=4
    2.571,  // df=5
    2.447,  // df=6
    2.365,  // df=7
    2.306,  // df=8
    2.262,  // df=9
    2.228,  // df=10
    2.179,  // df=15 (approx)
    2.145,  // df=20
    2.042,  // df=30
    1.984,  // df=60
    1.960   // df=inf
};

inline double t_critical(int df) {
    if (df <= 0)  return T_TABLE[1];
    if (df <= 10) return T_TABLE[df];
    if (df <= 15) return T_TABLE[11];
    if (df <= 20) return T_TABLE[12];
    if (df <= 30) return T_TABLE[13];
    if (df <= 60) return T_TABLE[14];
    return T_TABLE[15];
}

inline ConfidenceInterval confidence_interval_95(const std::vector<double>& data) {
    if (data.size() < 2) return {data.empty()?0:data[0], 0, 0, 0};
    int n = static_cast<int>(data.size());
    double sum  = std::accumulate(data.begin(), data.end(), 0.0);
    double mean = sum / n;
    double sq   = 0.0;
    for (double x : data) sq += (x - mean) * (x - mean);
    double s     = std::sqrt(sq / (n - 1));
    double se    = s / std::sqrt(n);
    double t     = t_critical(n - 1);
    double margin = t * se;
    return {mean, mean - margin, mean + margin, margin};
}

/* ── Gini coefficient ────────────────────────────────────── */
/*
 * Measures inequality of lock acquisitions among threads.
 * 0 = perfect equality (each thread acquires same number of times)
 * 1 = maximum inequality (one thread gets everything)
 */
inline double gini_coefficient(std::vector<double> vals) {
    if (vals.size() < 2) return 0.0;
    std::sort(vals.begin(), vals.end());
    int n = static_cast<int>(vals.size());
    double sum_num = 0.0, sum_den = 0.0;
    for (int i = 0; i < n; ++i) {
        sum_num += (2 * (i + 1) - n - 1) * vals[i];
        sum_den += vals[i];
    }
    if (sum_den == 0) return 0.0;
    return sum_num / (n * sum_den);
}

/* ── Jain fairness index ─────────────────────────────────── */
inline double jains_fairness_index(const std::vector<double>& vals) {
    if (vals.empty()) return 0.0;
    double sum = 0.0;
    double sum_sq = 0.0;
    for (double v : vals) {
        sum += v;
        sum_sq += v * v;
    }
    if (sum_sq <= 0.0) return 0.0;
    const double n = static_cast<double>(vals.size());
    return (sum * sum) / (n * sum_sq);
}

/* ── Max-min fairness ratio ─────────────────────────────── */
inline double max_min_ratio(const std::vector<double>& vals) {
    if (vals.empty()) return 0.0;
    auto [mn_it, mx_it] = std::minmax_element(vals.begin(), vals.end());
    if (*mx_it <= 0.0) return 0.0;
    return *mn_it / *mx_it;
}

/* ── Coefficient of variation for vector ─────────────────── */
inline double cv_percent(const std::vector<double>& vals) {
    if (vals.empty()) return 0.0;
    const double mean = std::accumulate(vals.begin(), vals.end(), 0.0) /
                        static_cast<double>(vals.size());
    if (mean <= 0.0) return 0.0;
    double sum_sq = 0.0;
    for (double v : vals) {
        const double d = v - mean;
        sum_sq += d * d;
    }
    const double var = (vals.size() > 1)
        ? (sum_sq / static_cast<double>(vals.size() - 1))
        : 0.0;
    return (std::sqrt(var) / mean) * 100.0;
}

/* ── Histogram bucket helper ─────────────────────────────── */
struct Histogram {
    std::vector<double> edges;   // bin left edges
    std::vector<uint64_t> counts;

    static Histogram build(const std::vector<double>& data,
                           int bins = 50) {
        Histogram h;
        if (data.empty()) return h;
        double lo = *std::min_element(data.begin(), data.end());
        double hi = *std::max_element(data.begin(), data.end());
        if (hi == lo) hi = lo + 1;
        double width = (hi - lo) / bins;
        h.edges.resize(bins + 1);
        h.counts.resize(bins, 0);
        for (int i = 0; i <= bins; ++i) h.edges[i] = lo + i * width;
        for (double x : data) {
            int b = static_cast<int>((x - lo) / width);
            if (b >= bins) b = bins - 1;
            h.counts[b]++;
        }
        return h;
    }
};

} // namespace stats
