#include "csv_reader.h"
#include "runner.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifndef FIBINACHO_BIN
#define FIBINACHO_BIN "./Fibinacho"
#endif

// ---------------------------------------------------------------------------
// Native file-open dialog (best-effort, no hard dependency)
// Returns an empty string if the user cancels or no dialog tool is found.
// ---------------------------------------------------------------------------

static std::string browse_for_csv()
{
#ifdef _WIN32
    // PowerShell one-liner: opens a Win32 file-picker and prints the path
    const char *cmd =
        "powershell -NoProfile -Command \""
        "[System.Reflection.Assembly]::LoadWithPartialName('System.Windows.Forms') | Out-Null;"
        "$d = New-Object System.Windows.Forms.OpenFileDialog;"
        "$d.Filter = 'CSV files (*.csv)|*.csv|All files (*.*)|*.*';"
        "if ($d.ShowDialog() -eq 'OK') { Write-Host $d.FileName }\" 2>NUL";
#elif defined(__APPLE__)
    const char *cmd =
        "osascript -e 'tell application \"System Events\" to activate'"
        " -e 'POSIX path of (choose file with prompt \"Select benchmark CSV\" of type {\"csv\", \"public.comma-separated-values-text\"})' 2>/dev/null";
#else
    // Linux: try zenity, then kdialog, give up gracefully if neither is found
    const char *cmd =
        "zenity --file-selection --title='Select benchmark CSV' --file-filter='CSV files | *.csv' 2>/dev/null"
        " || kdialog --getopenfilename . '*.csv' 2>/dev/null";
#endif

    FILE *pipe = popen(cmd, "r");
    if (!pipe) return {};

    char buf[1024] = {};
    std::string result;
    while (fgets(buf, sizeof(buf), pipe))
        result += buf;
    pclose(pipe);

    // strip trailing newline / carriage-return
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();

    return result;
}

// ---------------------------------------------------------------------------
// Per-tab state
// ---------------------------------------------------------------------------

struct Tab
{
    std::string label;           // e.g. "cml 5.0s"
    ui::BenchmarkData data;

    // animation state
    size_t      reveal_idx  = 0; // how many samples revealed so far (per series)
    bool        done        = false;
    double      anim_accum  = 0.0; // accumulated wall time for pacing
    static constexpr double SAMPLES_PER_SEC = 50.0;

    // per-series visibility toggle
    std::vector<bool> visible;

    // true after we've done one full-data fit so user zoom is preserved after
    bool fitted = false;

    void init_visibility()
    {
        visible.assign(data.series.size(), true);
    }

    // advance animation by dt seconds, returns true if anything changed
    bool advance(double dt)
    {
        if (done) return false;

        anim_accum += dt;
        const auto steps = static_cast<size_t>(anim_accum * SAMPLES_PER_SEC);
        if (steps == 0) return false;
        anim_accum -= static_cast<double>(steps) / SAMPLES_PER_SEC;

        // find max samples across all series
        size_t max_samples = 0;
        for (const auto &s : data.series)
            max_samples = std::max(max_samples, s.samples.size());

        size_t old_idx = reveal_idx;
        reveal_idx = std::min(reveal_idx + steps, max_samples);
        if (reveal_idx >= max_samples)
        {
            reveal_idx = max_samples;
            done = true;
        }
        return reveal_idx != old_idx;
    }
};

// ---------------------------------------------------------------------------
// Run panel state (shared across tabs)
// ---------------------------------------------------------------------------

struct RunPanel
{
    bool open = false;

    // config widgets
    int  mode_idx     = 0;     // 0=cml, 1=sng
    float time_budget = 1.0f;
    int  step_pct     = 38;    // index growth % per step (1=dense, 38=default, 100=coarse)
    bool  algo_sel[5] = { true, true, true, true, true };
    char  out_path[512] = "benchmark.csv";

    // runtime state
    bool running  = false;
    int  exit_code = 0;
    bool finished  = false;
    std::vector<std::string> log_lines;
    std::mutex               log_mutex;

    static constexpr const char *ALGO_NAMES[5] = {
        "naive", "linear", "matmul_simple", "matmul_fastexp", "field_ext"
    };

    [[nodiscard]] ui::RunConfig make_config() const
    {
        ui::RunConfig cfg;
        cfg.mode        = (mode_idx == 0) ? ui::BenchMode::Cumulative : ui::BenchMode::PerCall;
        cfg.time_budget = static_cast<double>(time_budget);
        cfg.step_pct    = step_pct;
        cfg.out_path    = out_path;
        for (int i = 0; i < 5; ++i)
            if (algo_sel[i]) cfg.algos.emplace_back(ALGO_NAMES[i]);
        return cfg;
    }

    void push_line(const std::string &line)
    {
        std::lock_guard<std::mutex> lk(log_mutex);
        log_lines.push_back(line);
        if (log_lines.size() > 200)
            log_lines.erase(log_lines.begin());
    }
};

// ---------------------------------------------------------------------------
// Color palette (matches 5 algorithms)
// ---------------------------------------------------------------------------

static const ImVec4 SERIES_COLORS[5] = {
    { 0.95f, 0.35f, 0.35f, 1.0f }, // naive       - red
    { 0.35f, 0.75f, 0.40f, 1.0f }, // linear      - green
    { 0.35f, 0.55f, 0.95f, 1.0f }, // matmul_s    - blue
    { 0.95f, 0.75f, 0.20f, 1.0f }, // matmul_f    - yellow
    { 0.80f, 0.40f, 0.95f, 1.0f }, // field_ext   - purple
};

static ImVec4 series_color(const std::string &name)
{
    // stable color by name so same algo always gets same color across tabs
    static const char *names[5] = {
        "naive", "linear", "matmul_simple", "matmul_fastexp", "field_ext"
    };
    for (int i = 0; i < 5; ++i)
        if (name == names[i]) return SERIES_COLORS[i];
    // fallback: hash
    size_t h = std::hash<std::string>{}(name);
    return { ((h >> 16) & 0xff) / 255.f,
             ((h >>  8) & 0xff) / 255.f,
             ( h        & 0xff) / 255.f, 1.f };
}

// ---------------------------------------------------------------------------
// Draw run panel
// ---------------------------------------------------------------------------

static void draw_run_panel(RunPanel &panel, ui::Runner &runner,
                            std::vector<Tab> &tabs,
                            const std::function<void(Tab)>& on_new_tab)
{
    if (!panel.open) return;

    ImGui::SetNextWindowSize({ 480, 420 }, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Run Benchmark", &panel.open))
    {
        ImGui::End();
        return;
    }

    bool busy = panel.running;

    ImGui::SeparatorText("Mode");
    ImGui::BeginDisabled(busy);
    ImGui::RadioButton("Cumulative (--cml)", &panel.mode_idx, 0); ImGui::SameLine();
    ImGui::RadioButton("Per-call   (--sng)", &panel.mode_idx, 1);

    ImGui::SeparatorText("Time budget (seconds)");
    ImGui::SliderFloat("##budget", &panel.time_budget, 0.1f, 30.0f, "%.1f s");

    ImGui::SeparatorText("Index step (% growth per sample)");
    ImGui::SliderInt("##step", &panel.step_pct, 1, 100, "%d%%");
    ImGui::SameLine();
    ImGui::TextDisabled(panel.step_pct <= 5 ? "(dense)" : panel.step_pct >= 50 ? "(coarse)" : "(normal)");

    ImGui::SeparatorText("Algorithms");
    for (int i = 0; i < 5; ++i)
    {
        ImGui::PushStyleColor(ImGuiCol_CheckMark, series_color(RunPanel::ALGO_NAMES[i]));
        ImGui::Checkbox(RunPanel::ALGO_NAMES[i], &panel.algo_sel[i]);
        ImGui::PopStyleColor();
        if (i < 4) ImGui::SameLine();
    }

    ImGui::SeparatorText("Output file");
    ImGui::InputText("##outpath", panel.out_path, sizeof(panel.out_path));
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (busy)
    {
        ImGui::TextDisabled("Running...");
        ImGui::SameLine();
        // simple spinner
        static const char *frames[] = { "|", "/", "-", "\\" };
        ImGui::Text("%s", frames[static_cast<int>(ImGui::GetTime() * 6) % 4]);
    }
    else
    {
        if (ImGui::Button("Run", { 80, 0 }))
        {
            panel.running  = true;
            panel.finished = false;
            panel.log_lines.clear();

            auto cfg = panel.make_config();

            runner.start(
                cfg,
                [&panel](const std::string& line) { panel.push_line(line); },
                [&panel](int code)
                {
                    panel.running   = false;
                    panel.finished  = true;
                    panel.exit_code = code;
                }
            );
        }
    }

    // log output
    ImGui::SeparatorText("Output");
    ImGui::BeginChild("##log", { 0, 120 }, true, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lk(panel.log_mutex);
        for (const auto &line : panel.log_lines)
            ImGui::TextUnformatted(line.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    // when run finishes, auto-load the CSV into a new tab
    if (panel.finished)
    {
        panel.finished = false;
        if (panel.exit_code == 0)
        {
            auto bdata = ui::load_csv(panel.out_path);
            if (bdata.ok())
            {
                Tab t;
                t.label = std::string(panel.mode_idx == 0 ? "cml" : "sng")
                          + " " + std::to_string(panel.time_budget).substr(0, 4) + "s";
                t.data  = std::move(bdata);
                t.init_visibility();
                on_new_tab(std::move(t));
            }
            else
            {
                panel.push_line("[UI] Failed to load CSV: " + bdata.error);
            }
        }
        else
        {
            panel.push_line("[UI] Binary exited with code " + std::to_string(panel.exit_code));
        }
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Draw one tab's graph
// ---------------------------------------------------------------------------

static void draw_tab_graph(Tab &tab)
{
    // legend toggles
    for (size_t i = 0; i < tab.data.series.size(); ++i)
    {
        const auto &s = tab.data.series[i];
        ImVec4 col = series_color(s.name);
        if (i > 0) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button,        tab.visible[i] ? col : ImVec4(0.3f,0.3f,0.3f,1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  col);
        if (ImGui::SmallButton(s.name.c_str()))
            tab.visible[i] = !tab.visible[i];
        ImGui::PopStyleColor(3);
    }

    if (!tab.done)
        ImGui::TextDisabled("  animating...");
    else
        ImGui::TextDisabled("  complete");

    ImVec2 plot_size = { ImGui::GetContentRegionAvail().x,
                         ImGui::GetContentRegionAvail().y - 4 };

    if (ImPlot::BeginPlot("##graph", plot_size, ImPlotFlags_NoTitle))
    {
        // While animating: AutoFit both axes every frame so the view tracks
        // the data as it reveals. Once done: switch to None so the user can
        // freely pan and zoom without the view snapping back.
        ImPlotAxisFlags x_flags = tab.done ? ImPlotAxisFlags_None : ImPlotAxisFlags_AutoFit;
        ImPlotAxisFlags y_flags = tab.done ? ImPlotAxisFlags_None : ImPlotAxisFlags_AutoFit;
        ImPlot::SetupAxes("n (Fibonacci index)", "time (s)", x_flags, y_flags);

        // On the single frame where animation finishes, do one final Always-fit
        // so the frozen view matches the completed data before handing off to user.
        if (tab.done && !tab.fitted)
        {
            double xmax = 1, ymax = 1;
            for (const auto &ser : tab.data.series)
                for (const auto &s : ser.samples)
                {
                    xmax = std::max(xmax, s.n);
                    ymax = std::max(ymax, s.time_seconds);
                }
            ImPlot::SetupAxesLimits(0, xmax * 1.05, 0, ymax * 1.1, ImGuiCond_Always);
            tab.fitted = true;
        }

        for (size_t si = 0; si < tab.data.series.size(); ++si)
        {
            if (!tab.visible[si]) continue;

            const auto &series = tab.data.series[si];
            size_t count = std::min(tab.reveal_idx, series.samples.size());
            if (count == 0) continue;

            // build contiguous x/y arrays for ImPlot
            std::vector<double> xs(count), ys(count);
            for (size_t i = 0; i < count; ++i)
            {
                xs[i] = series.samples[i].n;
                ys[i] = series.samples[i].time_seconds;
            }

            ImVec4 col = series_color(series.name);
            ImPlot::SetNextLineStyle(col, 2.0f);
            ImPlot::PlotLine(series.name.c_str(),
                             xs.data(), ys.data(),
                             static_cast<int>(count));

            // dot at the current tip -- HideNextItem() suppresses the
            // duplicate legend entry that PlotScatter would otherwise add
            if (count > 0)
            {
                ImPlot::HideNextItem(true, ImGuiCond_Always);
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 5.f, col, 1.f, col);
                ImPlot::PlotScatter((series.name + "##tip").c_str(),
                                    &xs[count-1], &ys[count-1], 1);
            }
        }

        ImPlot::EndPlot();
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    if (!glfwInit()) return 1;

    const char *glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow *window = glfwCreateWindow(1100, 700, "Fibinacho UI", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.FontGlobalScale = 1.4f;   // bump rendered text ~40% larger
    ImGui::StyleColorsDark();

    // widen widgets to match the larger text
    ImGuiStyle &style = ImGui::GetStyle();
    style.FramePadding     = { 8.f, 5.f };
    style.ItemSpacing      = { 10.f, 6.f };
    style.ScrollbarSize    = 18.f;
    style.GrabMinSize      = 14.f;
    style.WindowPadding    = { 12.f, 10.f };
    style.TabRounding      = 4.f;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // -----------------------------------------------------------------------

    ui::Runner runner(FIBINACHO_BIN);
    RunPanel   run_panel;
    std::vector<Tab> tabs;

    // file dialog state (simple path input for now)
    char load_path_buf[512] = "benchmark.csv";
    bool show_load_error    = false;
    std::string load_error_msg;

    auto prev_time = std::chrono::steady_clock::now();

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - prev_time).count();
        prev_time = now;

        // advance all animating tabs
        for (auto &tab : tabs)
            tab.advance(dt);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // main window fills the GLFW window
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##main", nullptr,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoMove       |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        // toolbar
        if (ImGui::Button("Run"))   run_panel.open = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(260);
        ImGui::InputText("##loadpath", load_path_buf, sizeof(load_path_buf));
        ImGui::SameLine();
        if (ImGui::Button("Browse..."))
        {
            std::string picked = browse_for_csv();
            if (!picked.empty())
            {
                // copy into the fixed buffer (truncate safely)
                std::strncpy(load_path_buf, picked.c_str(), sizeof(load_path_buf) - 1);
                load_path_buf[sizeof(load_path_buf) - 1] = '\0';
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Load CSV"))
        {
            auto bdata = ui::load_csv(load_path_buf);
            if (bdata.ok())
            {
                show_load_error = false;
                Tab t;
                t.label = "csv";
                // try to infer a short label from filename
                std::string p = load_path_buf;
                size_t slash = p.find_last_of("/\\");
                t.label = (slash == std::string::npos) ? p : p.substr(slash + 1);
                t.data  = std::move(bdata);
                t.init_visibility();
                tabs.push_back(std::move(t));
            }
            else
            {
                show_load_error  = true;
                load_error_msg   = bdata.error;
            }
        }

        if (show_load_error)
        {
            ImGui::SameLine();
            ImGui::TextColored({ 1.f, 0.4f, 0.4f, 1.f }, "%s", load_error_msg.c_str());
        }

        ImGui::Separator();

        // tab bar
        if (tabs.empty())
        {
            ImGui::TextDisabled("No data loaded. Use 'Load CSV' or 'Run' to get started.");
        }
        else if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_AutoSelectNewTabs |
                                              ImGuiTabBarFlags_TabListPopupButton))
        {
            size_t to_close = SIZE_MAX;
            for (size_t i = 0; i < tabs.size(); ++i)
            {
                bool open = true;
                std::string tab_label = tabs[i].label + "##" + std::to_string(i);
                if (ImGui::BeginTabItem(tab_label.c_str(), &open))
                {
                    draw_tab_graph(tabs[i]);
                    ImGui::EndTabItem();
                }
                if (!open) to_close = i;
            }
            if (to_close != SIZE_MAX)
                tabs.erase(tabs.begin() + to_close);

            ImGui::EndTabBar();
        }

        ImGui::End(); // main window

        // run panel (floating)
        draw_run_panel(run_panel, runner, tabs, [&tabs](Tab t)
        {
            tabs.push_back(std::move(t));
        });

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.12f, 0.12f, 0.12f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}