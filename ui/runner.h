#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace ui
{

    enum class BenchMode { Cumulative, PerCall };

    struct RunConfig
    {
        BenchMode mode        = BenchMode::Cumulative;
        double    time_budget = 1.0;
        int       step_pct    = 38;  // index growth % per step (1=dense .. 100=coarse)
        std::vector<std::string> algos; // empty = all
        std::string out_path  = "benchmark.csv";
    };

    // Runs Fibinacho in a background thread.
    // - on_line: called (from background thread) for each stdout line
    // - on_done: called (from background thread) with exit code when finished
    // Returns immediately; check is_running() to poll.
    class Runner
    {
    public:
        explicit Runner(std::string binary_path);
        ~Runner();

        // no copy
        Runner(const Runner &) = delete;
        Runner &operator=(const Runner &) = delete;

        void start(const RunConfig &cfg,
                   const std::function<void(std::string)>& on_line,
                   const std::function<void(int)>&         on_done);

        [[nodiscard]] bool is_running() const { return running_.load(); }
        void wait();

        static std::string build_args(const RunConfig &cfg);

    private:
        std::string binary_path_;
        std::atomic<bool> running_ { false };
        std::thread thread_;
    };

} // namespace ui