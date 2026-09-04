// Python bindings for libxaucore.
//
// This closes the loop on the plan's second architectural principle: Python
// never reimplements anything the C++ core does, it calls into it. Optuna, the
// purged-CV splitter and the DSR/PBO statistics all live in Python; every
// backtest they evaluate runs through this module and is therefore the same
// engine, the same fill model and the same costs the live system will use.
//
// On performance. Strategy is a PER-BAR interface, not per-tick, and that is
// what makes a Python strategy viable at all: a decade of M15 bars is ~350k
// calls, so the GIL round trip costs a fraction of a second. A per-tick Python
// callback over 300M ticks would be hopeless, and the interface deliberately
// does not offer one. The engine releases the GIL for the duration of a run, so
// a C++ strategy leaves other Python threads free; the trampoline below
// reacquires it only when a Python override actually has to run.

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "xau/bar.hpp"
#include "xau/baselines.hpp"
#include "xau/engine.hpp"
#include "xau/tick_store.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace py = pybind11;
using namespace xau;

namespace {

// Flat rows for numpy. Trade carries a `const char*` reason that numpy has no
// column type for, so the array form drops it; the reason is still reachable
// through on_trade_closed for strategies that care.
struct TradeRow {
    std::int64_t entry_ts;
    std::int64_t exit_ts;
    std::int32_t side;
    std::int32_t exit_reason;
    double       lots;
    std::int32_t entry_pts;
    std::int32_t exit_pts;
    std::int32_t sl_pts;
    std::int32_t tp_pts;
    std::int32_t mfe_pts;
    std::int32_t mae_pts;
    double       gross_usd;
    double       commission_usd;
    double       swap_usd;
    double       net_usd;
    double       balance_after;
};

struct EquityRow {
    std::int64_t ts_us;
    double       equity;
    double       balance;
};

py::array trades_to_numpy(const std::vector<Trade>& trades) {
    py::array_t<TradeRow> out(static_cast<py::ssize_t>(trades.size()));
    auto* r = static_cast<TradeRow*>(out.request().ptr);
    for (std::size_t i = 0; i < trades.size(); ++i) {
        const Trade& t = trades[i];
        r[i] = TradeRow{t.entry_ts,
                        t.exit_ts,
                        static_cast<std::int32_t>(t.side),
                        static_cast<std::int32_t>(t.exit_reason),
                        t.lots,
                        t.entry_pts,
                        t.exit_pts,
                        t.sl_pts,
                        t.tp_pts,
                        t.mfe_pts,
                        t.mae_pts,
                        t.gross_usd,
                        t.commission_usd,
                        t.swap_usd,
                        t.net_usd,
                        t.balance_after};
    }
    return std::move(out);
}

py::array equity_to_numpy(const std::vector<EquityPoint>& eq) {
    py::array_t<EquityRow> out(static_cast<py::ssize_t>(eq.size()));
    auto* r = static_cast<EquityRow*>(out.request().ptr);
    for (std::size_t i = 0; i < eq.size(); ++i) {
        r[i] = EquityRow{eq[i].ts_us, eq[i].equity, eq[i].balance};
    }
    return std::move(out);
}

// Lets Python subclass Strategy.
//
// name() and warmup_bars() are noexcept in the C++ interface, and a Python
// exception crossing a noexcept boundary calls std::terminate. So neither is
// dispatched to Python: both are plain values supplied once at construction.
// The trade is deliberate — a research strategy gains nothing from computing
// its own name, and it costs a crash if it tries.
class PyStrategy : public Strategy {
public:
    explicit PyStrategy(std::string name = "PyStrategy", std::size_t warmup = 0)
        : name_(std::move(name)), warmup_(warmup) {}

    [[nodiscard]] const char* name() const noexcept override { return name_.c_str(); }
    [[nodiscard]] std::size_t warmup_bars() const noexcept override { return warmup_; }

    [[nodiscard]] Decision on_bar(const BarContext& c) override {
        PYBIND11_OVERRIDE_PURE(Decision, Strategy, on_bar, c);
    }
    void on_start(const SymbolSpec& s) override { PYBIND11_OVERRIDE(void, Strategy, on_start, s); }
    void on_trade_closed(const Trade& t) override {
        PYBIND11_OVERRIDE(void, Strategy, on_trade_closed, t);
    }
    void on_finish() override { PYBIND11_OVERRIDE(void, Strategy, on_finish); }

private:
    std::string name_;
    std::size_t warmup_;
};

// The last `n` closed-bar values, as a numpy array. Copies only the tail, so a
// strategy computing an indicator each bar stays linear rather than quadratic.
template <class Field>
py::array_t<std::int32_t> recent(const BarContext& c, std::size_t n, Field field) {
    n = std::min(n, c.history.size());
    py::array_t<std::int32_t> out(static_cast<py::ssize_t>(n));
    auto* p = static_cast<std::int32_t*>(out.request().ptr);
    const std::size_t start = c.history.size() - n;
    for (std::size_t i = 0; i < n; ++i) p[i] = field(c.history[start + i]);
    return out;
}

}  // namespace

PYBIND11_MODULE(xaucore, m) {
    m.doc() =
        "Bindings for libxaucore: the same backtest engine, fill model and costs "
        "the live system runs. See docs/PLAN.md sections 2 and 3.";
    m.attr("__version__") = "0.1.0";

    PYBIND11_NUMPY_DTYPE(TradeRow, entry_ts, exit_ts, side, exit_reason, lots, entry_pts,
                         exit_pts, sl_pts, tp_pts, mfe_pts, mae_pts, gross_usd, commission_usd,
                         swap_usd, net_usd, balance_after);
    PYBIND11_NUMPY_DTYPE(EquityRow, ts_us, equity, balance);

    // ---- enums ----------------------------------------------------------
    py::enum_<Side>(m, "Side")
        .value("NONE", Side::None)
        .value("LONG", Side::Long)
        .value("SHORT", Side::Short);

    py::enum_<ExitReason>(m, "ExitReason")
        .value("OPEN", ExitReason::Open)
        .value("STOP_LOSS", ExitReason::StopLoss)
        .value("TAKE_PROFIT", ExitReason::TakeProfit)
        .value("STRATEGY_CLOSE", ExitReason::StrategyClose)
        .value("END_OF_DATA", ExitReason::EndOfData);

    py::enum_<Timeframe>(m, "Timeframe")
        .value("M1", Timeframe::M1)
        .value("M5", Timeframe::M5)
        .value("M15", Timeframe::M15)
        .value("M30", Timeframe::M30)
        .value("H1", Timeframe::H1)
        .value("H4", Timeframe::H4)
        .value("D1", Timeframe::D1);

    m.def("timeframe_us", &timeframe_us, py::arg("tf"),
          "Duration of one bar, in microseconds.");
    m.def("timeframe_name", &timeframe_name, py::arg("tf"));
    m.def("bar_open_for", &bar_open_for, py::arg("ts_us"), py::arg("tf"),
          "Opening instant of the bar containing ts_us.");

    // ---- market data ----------------------------------------------------
    py::class_<Bar>(m, "Bar")
        .def_readonly("open_time_us", &Bar::open_time_us)
        .def_readonly("open", &Bar::open)
        .def_readonly("high", &Bar::high)
        .def_readonly("low", &Bar::low)
        .def_readonly("close", &Bar::close)
        .def_readonly("ticks", &Bar::ticks)
        .def_readonly("spread_mean_pts", &Bar::spread_mean_pts)
        .def_readonly("spread_max_pts", &Bar::spread_max_pts)
        .def("range_pts", &Bar::range_pts)
        .def("close_time_us", &Bar::close_time_us, py::arg("tf"))
        .def("__repr__", [](const Bar& b) {
            return "<Bar t=" + std::to_string(b.open_time_us) + " o=" +
                   std::to_string(b.open) + " h=" + std::to_string(b.high) + " l=" +
                   std::to_string(b.low) + " c=" + std::to_string(b.close) + ">";
        });

    py::class_<TickStore>(m, "TickStore")
        .def_static("open", &TickStore::open, py::arg("directory"), py::arg("symbol"),
                    "Open a directory of <SYMBOL>-YYYY-MM.bin files.")
        .def_property_readonly("symbol", &TickStore::symbol)
        .def_property_readonly("file_count", &TickStore::file_count)
        .def_property_readonly("total_ticks", &TickStore::total_ticks)
        .def_property_readonly("first_ts", &TickStore::first_ts)
        .def_property_readonly("last_ts", &TickStore::last_ts);

    // ---- contract and costs ---------------------------------------------
    py::class_<SymbolSpec>(m, "SymbolSpec")
        .def(py::init<>())
        .def_static("xauusd_default", &SymbolSpec::xauusd_default)
        .def_readwrite("name", &SymbolSpec::name)
        .def_readwrite("point_num", &SymbolSpec::point_num)
        .def_readwrite("point_den", &SymbolSpec::point_den)
        .def_readwrite("contract_size", &SymbolSpec::contract_size)
        .def_readwrite("volume_min", &SymbolSpec::volume_min)
        .def_readwrite("volume_max", &SymbolSpec::volume_max)
        .def_readwrite("volume_step", &SymbolSpec::volume_step)
        .def_readwrite("stops_level_pts", &SymbolSpec::stops_level_pts)
        .def_readwrite("freeze_level_pts", &SymbolSpec::freeze_level_pts)
        .def_readwrite("swap_long_pts", &SymbolSpec::swap_long_pts)
        .def_readwrite("swap_short_pts", &SymbolSpec::swap_short_pts)
        .def_readwrite("triple_swap_weekday", &SymbolSpec::triple_swap_weekday)
        .def("usd_per_point_per_lot", &SymbolSpec::usd_per_point_per_lot)
        .def("price_usd", &SymbolSpec::price_usd, py::arg("points"))
        .def("points_from_usd", &SymbolSpec::points_from_usd, py::arg("usd"))
        .def("pnl_usd", &SymbolSpec::pnl_usd, py::arg("move_pts"), py::arg("lots"))
        .def("round_lots", &SymbolSpec::round_lots, py::arg("lots"));

    py::class_<CostModel>(m, "CostModel")
        .def(py::init<>())
        .def_readwrite("slip_base_pts", &CostModel::slip_base_pts)
        .def_readwrite("slip_vol_coef", &CostModel::slip_vol_coef)
        .def_readwrite("slip_max_pts", &CostModel::slip_max_pts)
        .def_readwrite("latency_us", &CostModel::latency_us)
        .def_readwrite("commission_per_lot_round_usd", &CostModel::commission_per_lot_round_usd)
        .def_readwrite("spread_mult", &CostModel::spread_mult)
        .def_readwrite("slippage_mult", &CostModel::slippage_mult)
        .def("stressed", &CostModel::stressed, py::arg("factor") = 2.0,
             "A copy with the spread and slippage multipliers scaled. Section 8 "
             "requires a strategy to survive factor=2.");

    py::class_<RiskConfig> risk(m, "RiskConfig");
    py::enum_<RiskConfig::Mode>(risk, "Mode")
        .value("FIXED_LOTS", RiskConfig::Mode::FixedLots)
        .value("FIXED_FRACTIONAL", RiskConfig::Mode::FixedFractional);
    risk.def(py::init<>())
        .def_readwrite("mode", &RiskConfig::mode)
        .def_readwrite("fixed_lots", &RiskConfig::fixed_lots)
        .def_readwrite("risk_fraction", &RiskConfig::risk_fraction)
        .def_readwrite("max_lots", &RiskConfig::max_lots);

    m.def("size_position", &size_position, py::arg("risk"), py::arg("spec"), py::arg("equity"),
          py::arg("sl_dist_pts"),
          "Lots for a trade risking risk_fraction of equity over sl_dist_pts. "
          "Returns 0.0 when the result is below the broker minimum, which the "
          "caller must treat as 'no trade'.");

    // ---- decisions and results ------------------------------------------
    py::class_<Decision> decision(m, "Decision");
    py::enum_<Decision::Kind>(decision, "Kind")
        .value("HOLD", Decision::Kind::Hold)
        .value("ENTER", Decision::Kind::Enter)
        .value("CLOSE", Decision::Kind::Close);
    decision.def(py::init<>())
        .def_readwrite("kind", &Decision::kind)
        .def_readwrite("side", &Decision::side)
        .def_readwrite("sl_dist_pts", &Decision::sl_dist_pts)
        .def_readwrite("tp_dist_pts", &Decision::tp_dist_pts)
        .def_readwrite("lots", &Decision::lots)
        .def_property_readonly("reason", [](const Decision& d) { return std::string(d.reason); })
        .def_static("hold", &Decision::hold)
        .def_static(
            "enter",
            [](Side side, Points sl, Points tp, double lots) {
                // The C++ Decision holds a `const char*` reason that must
                // outlive the call, so a Python-supplied string cannot be
                // stored here. Python decisions are tagged with a literal.
                Decision d = Decision::enter(side, sl, tp, "python");
                d.lots = lots;
                return d;
            },
            py::arg("side"), py::arg("sl_dist_pts") = 0, py::arg("tp_dist_pts") = 0,
            py::arg("lots") = 0.0)
        .def_static("close", []() { return Decision::close("python"); });

    py::class_<Position>(m, "Position")
        .def_readonly("side", &Position::side)
        .def_readonly("lots", &Position::lots)
        .def_readonly("entry_pts", &Position::entry_pts)
        .def_readonly("entry_ts", &Position::entry_ts)
        .def_readonly("sl_pts", &Position::sl_pts)
        .def_readonly("tp_pts", &Position::tp_pts)
        .def_readonly("swap_usd", &Position::swap_usd)
        .def_readonly("mfe_pts", &Position::mfe_pts)
        .def_readonly("mae_pts", &Position::mae_pts)
        .def("is_open", &Position::is_open);

    py::class_<Trade>(m, "Trade")
        .def_readonly("entry_ts", &Trade::entry_ts)
        .def_readonly("exit_ts", &Trade::exit_ts)
        .def_readonly("side", &Trade::side)
        .def_readonly("lots", &Trade::lots)
        .def_readonly("entry_pts", &Trade::entry_pts)
        .def_readonly("exit_pts", &Trade::exit_pts)
        .def_readonly("sl_pts", &Trade::sl_pts)
        .def_readonly("tp_pts", &Trade::tp_pts)
        .def_readonly("mfe_pts", &Trade::mfe_pts)
        .def_readonly("mae_pts", &Trade::mae_pts)
        .def_readonly("gross_usd", &Trade::gross_usd)
        .def_readonly("commission_usd", &Trade::commission_usd)
        .def_readonly("swap_usd", &Trade::swap_usd)
        .def_readonly("net_usd", &Trade::net_usd)
        .def_readonly("balance_after", &Trade::balance_after)
        .def_readonly("exit_reason", &Trade::exit_reason)
        .def_property_readonly("entry_reason",
                               [](const Trade& t) { return std::string(t.entry_reason); })
        .def("won", &Trade::won)
        .def("duration_us", &Trade::duration_us);

    // ---- strategy -------------------------------------------------------
    py::class_<BarContext>(m, "BarContext")
        .def_property_readonly("bar", &BarContext::bar, py::return_value_policy::copy)
        .def_property_readonly("history_size",
                               [](const BarContext& c) { return c.history.size(); })
        .def_property_readonly("position", &BarContext::position,
                               py::return_value_policy::copy)
        .def_readonly("equity", &BarContext::equity)
        .def_readonly("balance", &BarContext::balance)
        .def_readonly("now_us", &BarContext::now_us)
        .def_readonly("tf", &BarContext::tf)
        .def_property_readonly("spec", &BarContext::spec, py::return_value_policy::copy)
        .def("ago", &BarContext::ago, py::arg("n"), py::return_value_policy::copy,
             "ago(0) is the bar that just closed; ago(1) the one before it.")
        .def("has", &BarContext::has, py::arg("lookback"))
        .def("recent_closes", [](const BarContext& c,
                                 std::size_t n) { return recent(c, n, [](const Bar& b) {
                 return b.close;
             }); }, py::arg("n"))
        .def("recent_highs", [](const BarContext& c,
                                std::size_t n) { return recent(c, n, [](const Bar& b) {
                 return b.high;
             }); }, py::arg("n"))
        .def("recent_lows", [](const BarContext& c,
                               std::size_t n) { return recent(c, n, [](const Bar& b) {
                 return b.low;
             }); }, py::arg("n"));

    py::class_<Strategy, PyStrategy>(m, "Strategy")
        .def(py::init<std::string, std::size_t>(), py::arg("name") = "PyStrategy",
             py::arg("warmup_bars") = 0,
             "Subclass and override on_bar(ctx) -> Decision.\n\n"
             "The BarContext is only valid for the duration of the call: it "
             "refers to engine-owned memory, so do not store it. Copy out what "
             "you need instead.\n\n"
             "name and warmup_bars are fixed at construction rather than "
             "dispatched to Python, because both are noexcept in the C++ "
             "interface and an exception crossing that boundary would abort.")
        .def("name", &Strategy::name)
        .def("warmup_bars", &Strategy::warmup_bars)
        .def("on_bar", &Strategy::on_bar, py::arg("ctx"))
        .def("on_start", &Strategy::on_start, py::arg("spec"))
        .def("on_trade_closed", &Strategy::on_trade_closed, py::arg("trade"))
        .def("on_finish", &Strategy::on_finish);

    py::class_<BuyAndHold, Strategy>(m, "BuyAndHold")
        .def(py::init<double, Side>(), py::arg("lots") = 0.01, py::arg("side") = Side::Long);

    py::class_<RandomEntry> rnd(m, "RandomEntry");
    py::class_<RandomEntry::Config>(rnd, "Config")
        .def(py::init<>())
        .def_readwrite("entry_prob", &RandomEntry::Config::entry_prob)
        .def_readwrite("hold_bars", &RandomEntry::Config::hold_bars)
        .def_readwrite("lots", &RandomEntry::Config::lots)
        .def_readwrite("seed", &RandomEntry::Config::seed);
    // Declared after Config so the nested type exists; base registered here.
    rnd.def(py::init<>()).def(py::init<const RandomEntry::Config&>(), py::arg("config"));

    // ---- engine ---------------------------------------------------------
    py::class_<BacktestConfig>(m, "BacktestConfig")
        .def(py::init<>())
        .def_readwrite("spec", &BacktestConfig::spec)
        .def_readwrite("costs", &BacktestConfig::costs)
        .def_readwrite("risk", &BacktestConfig::risk)
        .def_readwrite("tf", &BacktestConfig::tf)
        .def_readwrite("initial_balance", &BacktestConfig::initial_balance)
        .def_readwrite("from_us", &BacktestConfig::from_us)
        .def_readwrite("to_us", &BacktestConfig::to_us)
        .def_readwrite("close_at_end", &BacktestConfig::close_at_end)
        .def_readwrite("swap_hour_utc", &BacktestConfig::swap_hour_utc)
        .def_readwrite("apply_swap", &BacktestConfig::apply_swap)
        .def_readwrite("max_history_bars", &BacktestConfig::max_history_bars);

    py::class_<BacktestStats>(m, "BacktestStats")
        .def_readonly("ticks", &BacktestStats::ticks)
        .def_readonly("bars", &BacktestStats::bars)
        .def_readonly("signals", &BacktestStats::signals)
        .def_readonly("rejected_stop_too_close", &BacktestStats::rejected_stop_too_close)
        .def_readonly("rejected_stop_inside_spread",
                      &BacktestStats::rejected_stop_inside_spread)
        .def_readonly("rejected_volume", &BacktestStats::rejected_volume)
        .def_readonly("rejected_in_position", &BacktestStats::rejected_in_position)
        .def_readonly("swap_charges", &BacktestStats::swap_charges)
        .def_readonly("wall_seconds", &BacktestStats::wall_seconds)
        .def("rejected_total", &BacktestStats::rejected_total);

    py::class_<Metrics>(m, "Metrics")
        .def_readonly("trades", &Metrics::trades)
        .def_readonly("wins", &Metrics::wins)
        .def_readonly("losses", &Metrics::losses)
        .def_readonly("win_rate", &Metrics::win_rate)
        .def_readonly("gross_profit", &Metrics::gross_profit)
        .def_readonly("gross_loss", &Metrics::gross_loss)
        .def_readonly("net_profit", &Metrics::net_profit)
        .def_readonly("profit_factor", &Metrics::profit_factor)
        .def_readonly("expectancy_usd", &Metrics::expectancy_usd)
        .def_readonly("avg_win", &Metrics::avg_win)
        .def_readonly("avg_loss", &Metrics::avg_loss)
        .def_readonly("total_commission", &Metrics::total_commission)
        .def_readonly("total_swap", &Metrics::total_swap)
        .def_readonly("max_drawdown_usd", &Metrics::max_drawdown_usd)
        .def_readonly("max_drawdown_pct", &Metrics::max_drawdown_pct)
        .def_readonly("return_pct", &Metrics::return_pct)
        .def_readonly("sharpe", &Metrics::sharpe)
        .def("__str__", &Metrics::to_string);

    py::class_<BacktestResult>(m, "BacktestResult")
        .def_readonly("stats", &BacktestResult::stats)
        .def_readonly("metrics", &BacktestResult::metrics)
        .def_readonly("initial_balance", &BacktestResult::initial_balance)
        .def_readonly("final_balance", &BacktestResult::final_balance)
        .def_readonly("strategy_name", &BacktestResult::strategy_name)
        .def_property_readonly(
            "trades", [](const BacktestResult& r) { return trades_to_numpy(r.trades); },
            "Closed trades as a numpy structured array. The per-trade entry "
            "reason is a string and has no numpy column; reach it through "
            "on_trade_closed if you need it.")
        .def_property_readonly(
            "equity", [](const BacktestResult& r) { return equity_to_numpy(r.equity); },
            "Equity sampled at each bar close, as a numpy structured array.")
        .def("trade_objects",
             [](const BacktestResult& r) { return r.trades; })
        .def("summary", &BacktestResult::summary);

    py::class_<BacktestEngine>(m, "BacktestEngine")
        // keep_alive<1,2>: the engine holds a reference to the store, so the
        // store must outlive it. Without this, Python could collect the store
        // and leave the engine reading freed memory.
        .def(py::init<const TickStore&, BacktestConfig>(), py::arg("store"), py::arg("config"),
             py::keep_alive<1, 2>())
        .def("run", &BacktestEngine::run, py::arg("strategy"),
             // Released so a C++ strategy leaves other Python threads free
             // (Optuna running trials in parallel, for instance). A Python
             // strategy reacquires it inside the trampoline, per bar.
             py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("config", &BacktestEngine::config,
                               py::return_value_policy::copy);
}
