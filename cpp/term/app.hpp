// Terminal state and panel declarations.
//
// Research mode and Live mode are deliberately the same binary and the same
// panels. Looking at backtest and live through different lenses is how you fail
// to notice they disagree (docs/PLAN.md section 10).
#pragma once

#include "xau/bar.hpp"
#include "xau/tick_store.hpp"

#include <deque>
#include <optional>
#include <string>

namespace xauterm {

using namespace xau;

enum class LogLevel { Info, Warn, Error };

struct LogLine {
    LogLevel    level;
    std::string text;
};

// Small ring buffer. The Phase 7 journal is the durable record; this is just
// what the operator sees.
class Log {
public:
    void add(LogLevel lvl, std::string text) {
        lines_.push_back({lvl, std::move(text)});
        while (lines_.size() > kMax) lines_.pop_front();
    }
    void info(std::string s) { add(LogLevel::Info, std::move(s)); }
    void warn(std::string s) { add(LogLevel::Warn, std::move(s)); }
    void error(std::string s) { add(LogLevel::Error, std::move(s)); }

    [[nodiscard]] const std::deque<LogLine>& lines() const noexcept { return lines_; }
    void clear() { lines_.clear(); }

private:
    static constexpr std::size_t kMax = 2000;
    std::deque<LogLine> lines_;
};

struct AppState {
    // data
    std::string               store_dir = "data/ticks/XAUUSD";
    std::string               symbol = "XAUUSD";
    std::optional<TickStore>  store;
    std::string               load_error;

    // chart
    Timeframe tf = Timeframe::M15;
    BarSeries bars;
    double    build_ms = 0.0;
    bool      needs_rebuild = true;

    // Linked X range, in Unix seconds — ImPlot's time axis works in seconds,
    // our store works in microseconds. The conversion lives at this boundary
    // and nowhere else.
    double x_min = 0.0;
    double x_max = 0.0;
    bool   x_initialised = false;

    // view options
    bool show_sessions = true;
    bool show_spread   = true;
    bool show_volume   = true;

    // panels
    bool p_chart = true;
    bool p_market = true;
    bool p_store = true;
    bool p_log = true;
    bool p_metrics = false;

    bool should_quit = false;

    Log log;

    [[nodiscard]] double point_usd() const noexcept {
        if (!store || store->files().empty()) return 1.0 / XAUUSD_POINT_DEN;
        const FileHeader& h = store->files().front().header();
        return static_cast<double>(h.point_num) / static_cast<double>(h.point_den);
    }
    [[nodiscard]] double to_usd(Points p) const noexcept {
        return static_cast<double>(p) * point_usd();
    }
};

// app lifecycle
void apply_theme();
void open_store(AppState&);
void rebuild_bars(AppState&);
void draw_ui(AppState&);

// panels
void panel_chart(AppState&);
void panel_market(AppState&);
void panel_store(AppState&);
void panel_log(AppState&);

// helpers shared across panels
std::string fmt_time(TimeUs us);
std::string fmt_duration(TimeUs us);

}  // namespace xauterm
