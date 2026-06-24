#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "benchmark_writer.h"
#include "num/algorithms.h"

using big::number;
using sec_t = std::chrono::duration<double>;

struct Algo
{
    const char *name;
    number (*fn)(number);
};

// every available algorithm, keyed by name for --algo selection
static const std::vector<Algo> ALL_ALGOS = {
    { "naive",          fibonacci_naive },
    { "linear",         fibonacci_linear },
    { "matmul_simple",  fibonacci_matmul_simple },
    { "matmul_fastexp", fibonacci_matmul_fastexp },
    { "field_ext",      fibonacci_field_ext },
};

// ---------------------------------------------------------------------------
// shared timeout helper -- used by both single-value mode (to avoid hanging
// forever on e.g. naive(50)) and benchmark mode (as a safety cap so one
// runaway call can't stall the whole run indefinitely).
// ---------------------------------------------------------------------------

/* Runs fn(n) on a background thread and waits up to `cap` for it to finish.
 * Returns true and fills runtime/result if it completed in time.
 * Returns false on timeout; the thread is detached and left to finish or
 * die on its own (never forcibly killed -- that would be undefined behavior).
 */
bool run_with_timeout(number (*fn)(number), number n, sec_t cap, sec_t &runtime, number &result)
{
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto run_atomic = std::make_shared<std::atomic<double>>(0.0);
    auto result_ptr = std::make_shared<number>();

    std::thread runner([fn, n, done, run_atomic, result_ptr]
    {
        auto start = std::chrono::steady_clock::now();
        *result_ptr = fn(n);
        sec_t delta = std::chrono::steady_clock::now() - start;
        run_atomic->store(delta.count());
        done->store(true);
    });

    auto start = std::chrono::steady_clock::now();
    const sec_t poll(0.005);
    while (std::chrono::steady_clock::now() - start < cap)
    {
        if (done->load())
        {
            runner.join();
            runtime = sec_t(run_atomic->load());
            result = *result_ptr;
            return true;
        }
        std::this_thread::sleep_for(poll);
    }

    runner.detach();
    return false;
}

// ---------------------------------------------------------------------------
// single-value mode: compute F(n) with each selected algorithm once
// ---------------------------------------------------------------------------

void run_single(number n, const std::vector<Algo> &algos, sec_t cap)
{
    std::cout << "Computing F_" << n.str(true) << " with each implementation:\n\n";
    std::cout << std::left << std::setw(16) << "algorithm"
               << std::setw(14) << "time (s)"
               << "result\n";
    std::cout << std::string(60, '-') << "\n";

    for (const auto &algo : algos)
    {
        sec_t elapsed;
        number result;
        if (run_with_timeout(algo.fn, n, cap, elapsed, result))
        {
            std::cout << std::left << std::setw(16) << algo.name
                       << std::setw(14) << std::setprecision(6) << elapsed.count()
                       << result.str() << "\n";
        }
        else
        {
            std::cout << std::left << std::setw(16) << algo.name
                       << "timed out (exceeded " << cap.count() << "s)\n";
        }
    }
}

// ---------------------------------------------------------------------------
// benchmark mode: --cml (cumulative budget)
// For each algorithm (run separately), keep growing n as long as the total
// elapsed time across ALL calls so far stays within budget. Each sample is
// written straight to the output file as it's produced (see run_benchmark).
// ---------------------------------------------------------------------------

void benchmark_algo_cml(const Algo &algo, sec_t budget, BenchmarkWriter &writer)
{
    // mirrors eval.cpp's growth strategy: small step-by-step search first
    // (gives fine-grained resolution for algorithms like `naive` that only
    // reach small n), then geometric growth once past that, so fast
    // algorithms aren't stuck doing millions of tiny +1 steps.
    //
    // This is a single continuous loop against one total time budget for
    // the whole algorithm (not a fresh budget per call, and no per-call
    // timeout wrapper -- thread-spawn overhead would dominate fast calls
    // and badly skew the timing). The call in progress when the budget is
    // exceeded is allowed to finish, so total elapsed time may run slightly
    // over budget; the cumulative check is what keeps this from running away.
    //
    // This function knows nothing about how a sample becomes a line in a
    // file -- it just hands each one to `writer`, which owns that entirely.
    constexpr std::uint64_t small_search_limit = 200;

    number n = 0;
    auto algo_start = std::chrono::steady_clock::now();

    std::cout << "Benchmarking " << algo.name << " (cumulative budget)...\n";

    number best_n = 0;
    sec_t total_elapsed(0);
    size_t sample_count = 0;

    while (true)
    {
        number result = algo.fn(n);
        sec_t now_elapsed = std::chrono::steady_clock::now() - algo_start;

        // time_seconds here is cumulative elapsed time since this algorithm's run started
        writer.write_sample(algo.name, n.str(true), now_elapsed.count(), result.str(true).size());
        ++sample_count;

        best_n = n;
        total_elapsed = now_elapsed;

        if (now_elapsed >= budget)
        {
            break; // budget exhausted -- this call was allowed to finish, now stop
        }

        n = (n < small_search_limit) ? (n + 1) : (n + (n >> 1) - (n >> 3));
    }

    std::cout << "  reached n=" << best_n.str() << " in " << total_elapsed.count()
               << "s (cumulative, " << sample_count << " samples)\n";
}

// ---------------------------------------------------------------------------
// benchmark mode: --sng (per-call budget, like eval.cpp)
// For each algorithm (run separately), keep growing n as long as EACH
// INDIVIDUAL call finishes within budget. Total wall-clock search time is
// unbounded -- only a single call's own duration is checked. Stops the
// moment one call exceeds the budget; that call is not counted as a sample,
// and the previous n is reported as the result.
// ---------------------------------------------------------------------------

void benchmark_algo_sng(const Algo &algo, sec_t budget, BenchmarkWriter &writer)
{
    // This function knows nothing about how a sample becomes a line in a
    // file -- it just hands each one to `writer`, which owns that entirely.
    constexpr std::uint64_t small_search_limit = 200;

    number n = 0;
    number best_n = 0;
    sec_t best_time(0);
    size_t sample_count = 0;

    std::cout << "Benchmarking " << algo.name << " (per-call budget)...\n";

    while (true)
    {
        auto call_start = std::chrono::steady_clock::now();
        number result = algo.fn(n);
        sec_t call_time = std::chrono::steady_clock::now() - call_start;

        if (call_time > budget)
        {
            // this call exceeded the budget -- log it so the graph shows
            // where the wall is, then stop (same as --cml behaviour)
            writer.write_sample(algo.name, n.str(true), call_time.count(), result.str(true).size());
            ++sample_count;
            best_n = n;
            best_time = call_time;
            break;
        }

        // time_seconds here is this single call's own duration
        writer.write_sample(algo.name, n.str(true), call_time.count(), result.str(true).size());
        ++sample_count;

        best_n = n;
        best_time = call_time;

        n = (n < small_search_limit) ? (n + 1) : (n + (n >> 1) - (n >> 3));
    }

    std::cout << "  reached n=" << best_n.str() << " (last call took " << best_time.count()
               << "s, " << sample_count << " samples)\n";
}

enum class BenchMode { Cumulative, PerCall };

void run_benchmark(sec_t budget, const std::vector<Algo> &algos, const std::string &out_path, BenchMode mode)
{
    BenchmarkWriter writer(out_path);

    for (const auto &algo : algos)
    {
        // each algorithm is benchmarked one at a time, fully sequentially,
        // so a slow one can never block or skew another's timing.
        // Samples go straight to `writer`, which persists them immediately
        // (see BenchmarkWriter::write_sample), so nothing is lost even if
        // the run is interrupted mid-algorithm.
        if (mode == BenchMode::Cumulative)
        {
            benchmark_algo_cml(algo, budget, writer);
        }
        else
        {
            benchmark_algo_sng(algo, budget, writer);
        }
    }
    writer.close();

    std::cout << "\nDone. Results written to " << out_path << "\n";
}

// ---------------------------------------------------------------------------
// CLI parsing
// ---------------------------------------------------------------------------

std::vector<std::string> split_csv(const std::string &s)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size())
    {
        size_t comma = s.find(',', start);
        if (comma == std::string::npos)
        {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, comma - start));
        start = comma + 1;
    }
    return out;
}

std::vector<Algo> select_algos(const std::string &names_csv)
{
    std::vector<Algo> selected;
    for (const auto &name : split_csv(names_csv))
    {
        bool found = false;
        for (const auto &algo : ALL_ALGOS)
        {
            if (name == algo.name)
            {
                selected.push_back(algo);
                found = true;
                break;
            }
        }
        if (!found)
        {
            std::cerr << "Unknown algorithm: " << name << " (skipping)\n";
        }
    }
    return selected;
}

void print_help()
{
    std::cout <<
        "Fibinacho -- compare Fibonacci algorithms for speed and reach\n"
        "\n"
        "USAGE:\n"
        "  Fibinacho [n] [--algo names]\n"
        "      Compute F(n) once with each selected algorithm (default n=30).\n"
        "      A timed-out call is reported, not allowed to hang (cap = --time * 1.5).\n"
        "\n"
        "  Fibinacho --benchmark [--cml|--sng] [--time seconds] [--algo names] [--out file]\n"
        "      Find how large an n each algorithm can reach within a time budget.\n"
        "      Results are streamed to a CSV file as they're produced.\n"
        "\n"
        "BENCHMARK MODES:\n"
        "  --cml   Cumulative budget (default). ALL calls for an algorithm together\n"
        "          must fit within --time. Lower n, but bounded total run time.\n"
        "  --sng   Per-call budget. Only a single call's own duration is checked\n"
        "          against --time. Reaches much higher n, but total search time\n"
        "          is unbounded -- it keeps going until one call is too slow.\n"
        "\n"
        "OPTIONS:\n"
        "  --time <seconds>   Time budget (default: 5.0 for --cml, 1.0 for --sng)\n"
        "  --algo <list>      Comma-separated algorithm names (default: all)\n"
        "  --out <file>       Output CSV path for --benchmark (default: benchmark.csv)\n"
        "  --quiet            Suppress progress output (used by FibinachoUI)\n"
        "  --help, -h         Show this message\n"
        "\n"
        "AVAILABLE ALGORITHMS:\n";
    for (const auto &algo : ALL_ALGOS)
    {
        std::cout << "  " << algo.name << "\n";
    }
    std::cout <<
        "\n"
        "EXAMPLES:\n"
        "  Fibinacho 50\n"
        "  Fibinacho 50 --algo linear,matmul_fastexp\n"
        "  Fibinacho --benchmark\n"
        "  Fibinacho --benchmark --sng --time 0.5 --algo naive\n"
        "  Fibinacho --benchmark --cml --out results.csv\n";
}

int main(int argc, char *argv[])
{
    bool benchmark_mode = false;
    bool quiet = false;
    BenchMode bench_mode = BenchMode::Cumulative; // default per spec
    bool time_budget_set = false;
    double time_budget = 1.0;
    std::string algo_arg;
    std::string index_arg = "30";
    std::string out_path = "benchmark.csv";

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h")
        {
            print_help();
            return 0;
        }
        else if (arg == "--benchmark")
        {
            benchmark_mode = true;
        }
        else if (arg == "--quiet")
        {
            quiet = true;
        }
        else if (arg == "--cml")
        {
            bench_mode = BenchMode::Cumulative;
        }
        else if (arg == "--sng")
        {
            bench_mode = BenchMode::PerCall;
        }
        else if (arg == "--time" && i + 1 < argc)
        {
            time_budget = std::stod(argv[++i]);
            time_budget_set = true;
        }
        else if (arg == "--algo" && i + 1 < argc)
        {
            algo_arg = argv[++i];
        }
        else if (arg == "--out" && i + 1 < argc)
        {
            out_path = argv[++i];
        }
        else if (!arg.empty() && arg[0] != '-')
        {
            index_arg = arg;
        }
        else
        {
            std::cerr << "Unrecognized option: " << arg << " (use --help to see available options)\n";
        }
    }

    std::vector<Algo> algos = algo_arg.empty() ? ALL_ALGOS : select_algos(algo_arg);
    if (algos.empty())
    {
        std::cerr << "No valid algorithms selected.\n";
        return 1;
    }

    // per-mode default budgets, only applied if --time was not explicitly given
    if (!time_budget_set && benchmark_mode)
    {
        time_budget = (bench_mode == BenchMode::Cumulative) ? 5.0 : 1.0;
    }

    if (benchmark_mode)
    {
        if (!quiet)
        {
            run_benchmark(sec_t(time_budget), algos, out_path, bench_mode);
        }
        else
        {
            // quiet mode: suppress per-algorithm progress lines, just run
            struct NullWriter : BenchmarkWriter
            {
                using BenchmarkWriter::BenchmarkWriter;
            };
            BenchmarkWriter writer(out_path);
            for (const auto &algo : algos)
            {
                if (bench_mode == BenchMode::Cumulative)
                    benchmark_algo_cml(algo, sec_t(time_budget), writer);
                else
                    benchmark_algo_sng(algo, sec_t(time_budget), writer);
            }
            writer.close();
        }
    }
    else
    {
        number n(index_arg);
        run_single(n, algos, sec_t(time_budget * 1.5));
    }

    return 0;
}