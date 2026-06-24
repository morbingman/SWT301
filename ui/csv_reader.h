#pragma once

#include <string>
#include <vector>
#include <unordered_map>

namespace ui
{

struct Sample
{
    double n;            // fibonacci index (stored as double for ImPlot)
    double time_seconds; // as-is from CSV, no massaging
    size_t result_digits;
};

struct AlgoSeries
{
    std::string name;
    std::vector<Sample> samples; // in file order
};

struct BenchmarkData
{
    std::string source_path;
    std::vector<AlgoSeries> series; // one per algorithm, stable order
    std::string error;              // non-empty if load failed

    bool ok() const { return error.empty(); }
};

// Loads a CSV produced by Fibinacho (algorithm,n,time_seconds,result_digits).
// Always returns a BenchmarkData; check .ok() before using.
// Malformed rows are silently skipped; only a total-failure sets .error.
BenchmarkData load_csv(const std::string &path);

} // namespace ui
