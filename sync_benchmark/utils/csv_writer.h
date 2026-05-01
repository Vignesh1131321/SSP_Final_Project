/**
 * utils/csv_writer.h  —  Thread-safe CSV output
 */
#pragma once
#include "../benchmark.h"
#include <fstream>
#include <mutex>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>

class CsvWriter {
    std::ofstream  file_;
    std::mutex     mu_;

public:
    explicit CsvWriter(const std::string& path) : file_(path, std::ios::app) {
        if (!file_.is_open())
            throw std::runtime_error("Cannot open CSV: " + path);
    }

    void write_header(const std::vector<std::string>& cols) {
        std::lock_guard<std::mutex> g(mu_);
        // If the file already contains data, assume header exists and skip.
        file_.seekp(0, std::ios::end);
        if (file_.tellp() > 0) return;

        for (size_t i = 0; i < cols.size(); ++i) {
            file_ << cols[i];
            if (i + 1 < cols.size()) file_ << ',';
        }
        file_ << '\n';
    }

    void write(const TrialResult& r) {
        std::lock_guard<std::mutex> g(mu_);
        file_ << r.suite                                     << ','
              << r.experiment                                << ','
              << r.primitive                                 << ','
              << r.threads                                   << ','
              << r.cs_cycles                                 << ','
              << r.trial                                     << ','
              << std::fixed << std::setprecision(2)
              << r.ops_per_sec                               << ','
              << r.mean_latency_ns                           << ','
              << r.p50_ns                                    << ','
              << r.p95_ns                                    << ','
              << r.p99_ns                                    << ','
              << r.stddev_ns                                 << ','
              << r.cv_percent                                << ','
              << r.throughput_ops                            << ','
              << r.false_sharing_factor                      << ','
              << r.fairness_gini                             << ','
              << r.fairness_jain                             << ','
              << r.fairness_maxmin                           << ','
              << r.fairness_cv                               << ','
              << r.cpu_migrations                            << ','
              << r.ctx_switches                              << ','
              << '"' << r.notes << '"'                       << '\n';
        file_.flush();  // Force immediate write to disk
    }

    void flush() { file_.flush(); }

    ~CsvWriter() { 
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
    }
};
