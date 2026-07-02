#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

// ---------------------------------------------------------------------------
// Fast decimal digit count estimate.
//
// number::str(true) does a full double-dabble binary->decimal conversion,
// which is O(bits * decimal_digits) -- effectively O(n^2) in digit count.
// For benchmark logging we only need an approximate digit count (it's just
// metadata in the CSV), so instead we compute it directly from the bit
// length in O(digit_count): decimal_digits ~= bit_length * log10(2).
// This is what was silently dominating field_ext/matmul_fastexp's measured
// time at large n -- the "algorithm" time included an O(n^2) string
// conversion just to count digits for the CSV.
// ---------------------------------------------------------------------------

std::size_t estimate_decimal_digits(const number &x)
{
    if (x.value.empty())
        return 1;

    using digit_t = number::int_t;
    constexpr std::size_t digit_bits = sizeof(digit_t) * 8;

    std::size_t bit_length = (x.value.size() - 1) * digit_bits;
    digit_t top = x.value.back();
    while (top)
    {
        ++bit_length;
        top >>= 1;
    }

    // decimal_digits = floor(bit_length * log10(2)) + 1
    static constexpr double LOG10_2 = 0.30102999566398119521;
    return static_cast<std::size_t>(bit_length * LOG10_2) + 1;
}

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
// shared timeout helper
// ---------------------------------------------------------------------------

bool run_with_timeout(number (*fn)(number), const number& n, const sec_t cap, sec_t &runtime, number &result)
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
// single-value mode
// ---------------------------------------------------------------------------

void run_single(const number& n, const std::vector<Algo> &algos, const sec_t cap)
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
// index stepping
// step_pct is the percentage to grow n by each iteration (e.g. 1 = +1%).
// Below SMALL_SEARCH_LIMIT we always step by +1 regardless.
// step_cap, if non-zero, caps the absolute step size so sampling never gets
// too sparse at large n -- without it, at n in the millions even a 1% step
// is a jump of tens of thousands. step_cap == 0 means no cap (default):
// pure percentage growth, same behaviour as before this option existed.
// Uses long double approximation -- off by at most 1, fine for index selection.
// ---------------------------------------------------------------------------

static constexpr std::uint64_t SMALL_SEARCH_LIMIT = 200;

static number next_n(const number &n, const int step_pct, const std::uint64_t step_cap)
{
    if (n < SMALL_SEARCH_LIMIT)
        return n + 1;
    const auto nd = static_cast<long double>(n);
    auto delta = static_cast<std::uint64_t>(nd * step_pct / 100.0L);
    if (delta < 1) delta = 1;
    if (step_cap > 0 && delta > step_cap) delta = step_cap;
    return n + delta;
}

// ---------------------------------------------------------------------------
// benchmark mode: --cml (cumulative budget)
// ---------------------------------------------------------------------------

void benchmark_algo_cml(const Algo &algo, const sec_t budget, BenchmarkWriter &writer, const int step_pct, const std::uint64_t step_cap)
{
    number n = 0;
    auto algo_start = std::chrono::steady_clock::now();

    std::cout << "Benchmarking " << algo.name << " (cumulative budget, step=" << step_pct
               << "%, cap=" << (step_cap > 0 ? std::to_string(step_cap) : "none") << ")...\n";

    number best_n = 0;
    sec_t total_elapsed(0);
    size_t sample_count = 0;

    while (true)
    {
        number result = algo.fn(n);
        sec_t now_elapsed = std::chrono::steady_clock::now() - algo_start;

        writer.write_sample(algo.name, n.str(true), now_elapsed.count(), estimate_decimal_digits(result));
        ++sample_count;

        best_n = n;
        total_elapsed = now_elapsed;

        if (now_elapsed >= budget)
            break;

        n = next_n(n, step_pct, step_cap);
    }

    std::cout << "  reached n=" << best_n.str() << " in " << total_elapsed.count()
               << "s (cumulative, " << sample_count << " samples)\n";
}

// ---------------------------------------------------------------------------
// benchmark mode: --sng (per-call budget)
// ---------------------------------------------------------------------------

void benchmark_algo_sng(const Algo &algo, const sec_t budget, BenchmarkWriter &writer, const int step_pct, const std::uint64_t step_cap)
{
    number n = 0;
    number best_n = 0;
    sec_t best_time(0);
    size_t sample_count = 0;

    std::cout << "Benchmarking " << algo.name << " (per-call budget, step=" << step_pct
               << "%, cap=" << (step_cap > 0 ? std::to_string(step_cap) : "none") << ")...\n";

    while (true)
    {
        auto call_start = std::chrono::steady_clock::now();
        number result = algo.fn(n);
        sec_t call_time = std::chrono::steady_clock::now() - call_start;

        if (call_time > budget)
        {
            // log the over-budget call so the graph shows where the wall is
            writer.write_sample(algo.name, n.str(true), call_time.count(), estimate_decimal_digits(result));
            ++sample_count;
            best_n = n;
            best_time = call_time;
            break;
        }

        writer.write_sample(algo.name, n.str(true), call_time.count(), estimate_decimal_digits(result));
        ++sample_count;

        best_n = n;
        best_time = call_time;

        n = next_n(n, step_pct, step_cap);
    }

    std::cout << "  reached n=" << best_n.str() << " (last call took " << best_time.count()
               << "s, " << sample_count << " samples)\n";
}

enum class BenchMode { Cumulative, PerCall };

void run_benchmark(const sec_t budget, const std::vector<Algo> &algos, const std::string &out_path,
                   const BenchMode mode, const int step_pct, std::uint64_t step_cap)
{
    BenchmarkWriter writer(out_path);

    for (const auto &algo : algos)
    {
        if (mode == BenchMode::Cumulative)
            benchmark_algo_cml(algo, budget, writer, step_pct, step_cap);
        else
            benchmark_algo_sng(algo, budget, writer, step_pct, step_cap);
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
            std::cerr << "Unknown algorithm: " << name << " (skipping)\n";
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
        "          The call that finally exceeds --time IS logged as the last\n"
        "          sample (so the graph shows exactly where the wall was hit),\n"
        "          then the run stops.\n"
        "\n"
        "OPTIONS:\n"
        "  --time <seconds>   Time budget (default: 5.0 for --cml, 1.0 for --sng)\n"
        "  --step <percent>   Index growth per step, 1-100 (default: 38).\n"
        "                     Lower = denser samples, but many more total calls\n"
        "                     means a much longer sustained run -- on real\n"
        "                     hardware this can be more exposed to thermal\n"
        "                     throttling, which may cap --sng's reach lower\n"
        "                     than a quick high-step run would.\n"
        "  --step-cap <n>     Optional hard cap on the absolute step size,\n"
        "                     1-100000 (default: none -- pure percentage growth).\n"
        "                     Without a cap, a percentage step becomes a huge\n"
        "                     jump once n is large (e.g. 1% of 4,000,000 is\n"
        "                     40,000); a cap keeps the tail of the graph dense.\n"
        "                     Lower values mean denser tails but longer runs --\n"
        "                     every sample past the point where the cap kicks\n"
        "                     in is a full recomputation, so a low cap can take\n"
        "                     a long time once n reaches into the millions.\n"
        "  --algo <list>      Comma-separated algorithm names (default: all)\n"
        "  --out <file>       Output CSV path for --benchmark (default: benchmark.csv)\n"
        "  --quiet            Suppress progress output (used by FibinachoUI)\n"
        "  --help, -h         Show this message\n"
        "\n"
        "AVAILABLE ALGORITHMS:\n";
    for (const auto &algo : ALL_ALGOS)
        std::cout << "  " << algo.name << "\n";
    std::cout <<
        "\n"
        "EXAMPLES:\n"
        "  Fibinacho 50\n"
        "  Fibinacho 50 --algo linear,matmul_fastexp\n"
        "  Fibinacho --benchmark\n"
        "  Fibinacho --benchmark --sng --time 0.5 --algo naive\n"
        "  Fibinacho --benchmark --cml --out results.csv\n"
        "  Fibinacho --benchmark --sng --step 1\n"
        "  Fibinacho --benchmark --sng --step-cap 50000\n";
}

int main(const int argc, char *argv[])
{
    bool benchmark_mode = false;
    bool quiet = false;
    BenchMode bench_mode = BenchMode::Cumulative;
    bool time_budget_set = false;
    double time_budget = 1.0;
    int step_pct = 38; // default: ~37.5% approximated as 38%
    std::uint64_t step_cap = 0; // 0 = no cap (default); otherwise clamped to [1, 100000]
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
        else if (arg == "--benchmark")  { benchmark_mode = true; }
        else if (arg == "--quiet")      { quiet = true; }
        else if (arg == "--cml")        { bench_mode = BenchMode::Cumulative; }
        else if (arg == "--sng")        { bench_mode = BenchMode::PerCall; }
        else if (arg == "--time" && i + 1 < argc)
        {
            time_budget = std::stod(argv[++i]);
            time_budget_set = true;
        }
        else if (arg == "--step" && i + 1 < argc)
        {
            step_pct = std::clamp(std::stoi(argv[++i]), 1, 100);
        }
        else if (arg == "--step-cap" && i + 1 < argc)
        {
            long long parsed = std::stoll(argv[++i]);
            step_cap = static_cast<std::uint64_t>(std::clamp(parsed, 1LL, 100000LL));
        }
        else if (arg == "--algo" && i + 1 < argc) { algo_arg = argv[++i]; }
        else if (arg == "--out"  && i + 1 < argc) { out_path = argv[++i]; }
        else if (!arg.empty() && arg[0] != '-')
        {
            // Positional argument: must be a plain non-negative integer (the
            // Fibonacci index). number(std::string) does not validate its
            // input -- non-digit characters silently corrupt its internal
            // state instead of erroring, which can crash later. Check here
            // instead of letting bad input reach the constructor.
            const bool all_digits = !arg.empty() &&
                std::ranges::all_of(arg, [](const unsigned char c) { return std::isdigit(c); });

            if (!all_digits)
            {
                bool is_algo_name = std::ranges::any_of(ALL_ALGOS, [&](const Algo &a) { return arg == a.name; });

                if (is_algo_name)
                {
                    std::cerr << "'" << arg << "' is an algorithm name, not an index -- "
                                 "select it with --algo " << arg << " instead.\n";
                }
                else
                {
                    std::cerr << "Invalid index '" << arg << "': expected a non-negative integer "
                                 "(use --help to see available options)\n";
                }
                return 1;
            }
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

    if (!time_budget_set && benchmark_mode)
        time_budget = (bench_mode == BenchMode::Cumulative) ? 5.0 : 1.0;

    if (benchmark_mode)
    {
        if (!quiet)
        {
            run_benchmark(sec_t(time_budget), algos, out_path, bench_mode, step_pct, step_cap);
        }
        else
        {
            BenchmarkWriter writer(out_path);
            for (const auto &algo : algos)
            {
                if (bench_mode == BenchMode::Cumulative)
                    benchmark_algo_cml(algo, sec_t(time_budget), writer, step_pct, step_cap);
                else
                    benchmark_algo_sng(algo, sec_t(time_budget), writer, step_pct, step_cap);
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