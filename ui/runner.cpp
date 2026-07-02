#include "runner.h"

#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
  #define POPEN  _popen
  #define PCLOSE _pclose
#else
  #define POPEN  popen
  #define PCLOSE pclose
#endif

namespace ui
{

std::string Runner::build_args(const RunConfig &cfg)
{
    std::ostringstream ss;

    ss << " --benchmark";
    ss << (cfg.mode == BenchMode::Cumulative ? " --cml" : " --sng");
    ss << " --time " << cfg.time_budget;
    ss << " --step " << cfg.step_pct;
    ss << " --quiet";
    ss << " --out \"" << cfg.out_path << "\"";

    if (!cfg.algos.empty())
    {
        ss << " --algo ";
        for (size_t i = 0; i < cfg.algos.size(); ++i)
        {
            if (i) ss << ',';
            ss << cfg.algos[i];
        }
    }

    return ss.str();
}

Runner::Runner(std::string binary_path)
    : binary_path_(std::move(binary_path))
{}

Runner::~Runner()
{
    wait();
}

void Runner::start(const RunConfig &cfg,
                   const std::function<void(std::string)>& on_line,
                   const std::function<void(int)>&         on_done)
{
    if (running_.load())
    {
        return; // already running, ignore
    }

    if (thread_.joinable())
    {
        thread_.join();
    }

    running_.store(true);

    thread_ = std::thread([this, cfg, on_line, on_done]
    {
        // Convert backslashes to forward slashes -- cmd.exe handles both,
        // but mixing quotes and backslashes in popen strings is fragile.
        std::string bin = binary_path_;
        for (auto &c : bin) if (c == '\\') c = '/';

#ifdef _WIN32
        // Windows cmd.exe quirk: when the executable path is quoted,
        // the entire command must also be wrapped in an outer pair of quotes.
        std::string cmd = "\"\"" + bin + "\"" + build_args(cfg) + "\" 2>&1";
#else
        std::string cmd = "\"" + bin + "\"" + build_args(cfg) + " 2>&1";
#endif

        FILE *pipe = POPEN(cmd.c_str(), "r");
        int exit_code = -1;

        if (pipe)
        {
            char buf[512];
            while (fgets(buf, sizeof(buf), pipe))
            {
                std::string line(buf);
                // strip trailing newline
                if (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                    line.pop_back();
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                on_line(std::move(line));
            }
            exit_code = PCLOSE(pipe);
#ifndef _WIN32
            if (WIFEXITED(exit_code))
                exit_code = WEXITSTATUS(exit_code);
#endif
        }

        running_.store(false);
        on_done(exit_code);
    });
}

void Runner::wait()
{
    if (thread_.joinable())
    {
        thread_.join();
    }
}

} // namespace ui