#ifndef __BENCHMARK_WRITER_H
#define __BENCHMARK_WRITER_H

#include <string>

/* BenchmarkWriter owns everything about how a benchmark sample becomes a
 * line in a file. The search/timing code in main.cpp only calls
 * write_sample() -- it never touches std::ofstream, CSV formatting, or
 * any other format-specific detail directly. Swapping the output format
 * later (JSON, SQLite, etc.) only requires changing this class.
 */
class BenchmarkWriter
{
public:
    // Opens out_path and writes the header. Throws std::ios_base::failure
    // (or similar) on failure to open -- callers should let this propagate
    // or check is_open() before use.
    explicit BenchmarkWriter(const std::string &out_path);
    ~BenchmarkWriter();

    // no copying -- one writer owns one open file
    BenchmarkWriter(const BenchmarkWriter &) = delete;
    BenchmarkWriter &operator=(const BenchmarkWriter &) = delete;

    // Records one (algorithm, n, time, result-digit-count) sample and
    // flushes immediately, so data already written survives if the
    // process is interrupted before the next sample is ready.
    void write_sample(const std::string &algorithm, const std::string &n,
                       double time_seconds, std::size_t result_digits);

    void close();

private:
    struct Impl;
    Impl *impl;
};

#endif//__BENCHMARK_WRITER_H
