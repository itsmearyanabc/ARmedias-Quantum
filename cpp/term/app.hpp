// Terminal state and panel declarations.
//
// Research mode and Live mode are deliberately the same binary and the same
// panels. Looking at backtest and live through different lenses is how you fail
// to notice they disagree (docs/PLAN.md section 10).
#pragma once

#include "xau/bar.hpp"
#include "xau/engine.hpp"
#include "xau/registry.hpp"
#include "xau/tick_store.hpp"

#include <deque>
#include <future>
#include <optional>
#include <string>
#include <vector>

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

// Carried back from the worker thread. An exception must not cross a thread
// boundary into the render loop, so failure comes back as a value.
struct BacktestOutcome {
    bool           ok = false;
    BacktestResult result;
    std::string    error;
};

struct AppState {
    // data
    std::string              store_dir = "data/ticks/XAUUSD";
    std::string              symbol = "XAUUSD";
    std::optional<TickStore> store;
    std::string              load_error;

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
    bool show_spread = true;
    bool show_volume = true;
    bool show_trades = true;
    bool show_sl_tp = true;

    // ---- backtest ------------------------------------------------------
    int    strategy_index = 0;
    double lots = 0.10;
    double initial_balance = 10'000.0;
    double commission = 7.0;
    double spread_mult = 1.0;      // section 8 requires surviving 2x
    double slippage_mult = 1.0;
    bool   apply_swap = false;

    std::optional<BacktestResult> bt;
    std::string                   bt_error;
    std::future<BacktestOutcome>  bt_future;
    bool                          bt_running = false;

    // Index into bt->trades, or -1. Drives the chart highlight and the jump.
    int selected_trade = -1;

    // Equity and underwater series, flattened once when a result arrives
    // rather than rebuilt per frame. A decade of M15 is ~350k points, and
    // rebuilding that at 60 fps is 20M+ operations a second for a curve that
    // has not changed.
    std::vector<double> eq_x;
    std::vector<double> eq_y;
    std::vector<double> dd_y;   // drawdown from peak, percent, negative
    double              eqx_min = 0.0;   // equity/underwater shared X range
    double              eqx_max = 0.0;

    // panels
    bool p_chart = true;
    bool p_market = true;
    bool p_store = true;
    bool p_log = true;
    bool p_runner = true;
    bool p_equity = true;
    bool p_blotter = true;
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

// backtest, run off the render thread
void start_backtest(AppState&);
void poll_backtest(AppState&);
void select_trade(AppState&, int index, bool jump_chart);

// panels
void panel_chart(AppState&);
void panel_market(AppState&);
void panel_store(AppState&);
void panel_log(AppState&);
void panel_runner(AppState&);
void panel_equity(AppState&);
void panel_blotter(AppState&);

// helpers shared across panels
std::string fmt_time(TimeUs us);
std::string fmt_duration(TimeUs us);

}  // namespace xauterm
