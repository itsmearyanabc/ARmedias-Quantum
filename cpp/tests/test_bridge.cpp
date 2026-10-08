// Bridge tests: the guards, the order lifecycle, persistence, and parity.
//
// Every guard test describes a way the bridge could lose real money quietly.
// The governing property is FAIL CLOSED: whenever the bridge cannot establish
// that trading is safe, the correct output is FLATTEN_AND_HALT or, for a
// condition worth waiting out, a refused entry -- never a trade.
//
// The parity test at the end is the one the rest depend on: it drives this ABI
// tick by tick through a simulated broker and requires the trades to match
// BacktestEngine's on the same ticks, exactly. Without it, "the thing that
// trades is the thing that was tested" is a sentence, not a property.

#include "fixture.hpp"
#include "harness.hpp"

#include "xau_bridge.h"

#include "xau/engine.hpp"
#include "xau/registry.hpp"
#include "xau/session.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr int64_t kT0ms = 1'704'067'200'000LL;   // 2024-01-01T00:00:00Z, a Monday
constexpr int64_t kMinMs = 60'000;
constexpr int64_t kDayMs = 86'400'000;

xau_limits default_limits() {
    xau_limits l{};
    l.max_daily_loss_frac = 0.04;
    l.max_drawdown_frac = 0.08;
    l.initial_balance = 0.0;
    l.max_spread = 1.00;
    l.max_lots = 0.10;
    l.max_open_positions = 1;
    l.max_quote_age_ms = 10'000;
    l.pending_timeout_ms = 30'000;
    return l;
}

void set_path(char (&dst)[260], const std::string& p) {
    std::memset(dst, 0, sizeof(dst));
    std::strncpy(dst, p.c_str(), sizeof(dst) - 1);
}

xau_market healthy_market(int64_t t_ms = kT0ms + 10 * kMinMs) {
    xau_market m{};
    m.tick_time_ms = t_ms;
    m.now_ms = t_ms;
    m.bid = 2650.00;
    m.ask = 2650.30;
    m.equity = 10'000.0;
    m.balance = 10'000.0;
    m.server_day = 20240101;
    m.session_open = 1;
    return m;
}

xau_strategy_config gold_config(const char* strategy, int32_t tf, double lots) {
    xau_strategy_config k{};
    std::strncpy(k.strategy, strategy, sizeof(k.strategy) - 1);
    k.timeframe = tf;
    k.fixed_lots = lots;
    k.contract_size = 100.0;
    k.volume_min = 0.01;
    k.volume_max = 100.0;
    k.volume_step = 0.01;
    k.point_den = 1000;
    return k;
}

struct Ctx {
    void* p = nullptr;
    explicit Ctx(const xau_limits& l) : p(xau_create("XAUUSD", XAU_BRIDGE_ABI_VERSION, &l)) {}
    ~Ctx() { xau_destroy(p); }
    Ctx(const Ctx&) = delete;
    Ctx& operator=(const Ctx&) = delete;
};

xau_decision tick(void* ctx, const xau_market& m) {
    xau_decision d{};
    CHECK_EQ(xau_on_tick(ctx, &m, &d), XAU_OK);
    return d;
}

std::string message(void* ctx) {
    char buf[256];
    xau_last_message(ctx, buf, sizeof(buf));
    return buf;
}

// Arm BuyAndHold on M1: it asks to enter on the first bar to close, which is
// the tick that crosses the first minute boundary. A strategy with a known,
// immediate entry is what lets the gating around entries be tested precisely.
void arm_buy_and_hold(void* ctx, double lots = 0.05) {
    const xau_strategy_config k = gold_config("BuyAndHold", XAU_TF_M1, lots);
    REQUIRE(xau_arm(ctx, &k) == XAU_OK);
}

}  // namespace

// ---------------------------------------------------------------------------
// the contract
// ---------------------------------------------------------------------------

XAU_TEST(abi_version_and_struct_sizes_are_exposed_and_enforced) {
    CHECK_EQ(xau_abi_version(), XAU_BRIDGE_ABI_VERSION);
    CHECK_EQ(XAU_BRIDGE_ABI_VERSION, 3);
    // The EA compares these against its own sizeof. Every size is a multiple
    // of 8 so MQL5's 1-byte packing and C's natural alignment agree.
    CHECK_EQ(xau_struct_size(0), 128);
    CHECK_EQ(xau_struct_size(1), 112);
    CHECK_EQ(xau_struct_size(2), 576);
    CHECK_EQ(xau_struct_size(3), 128);
    CHECK_EQ(xau_struct_size(4), 56);
    CHECK_EQ(xau_struct_size(5), 0);

    // A version mismatch must REFUSE, not adapt.
    const xau_limits l = default_limits();
    CHECK(xau_create("XAUUSD", XAU_BRIDGE_ABI_VERSION + 1, &l) == nullptr);
    CHECK(xau_create("XAUUSD", 1, &l) == nullptr);
    CHECK(xau_create(nullptr, XAU_BRIDGE_ABI_VERSION, &l) == nullptr);
}

XAU_TEST(a_null_or_wild_context_reports_halted_and_flattens) {
    CHECK_EQ(xau_is_halted(nullptr), 1);
    xau_decision     d{};
    const xau_market m = healthy_market();
    CHECK_EQ(xau_on_tick(nullptr, &m, &d), XAU_ERR_BAD_CONTEXT);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(xau_on_timer(nullptr, kT0ms, &d), XAU_ERR_BAD_CONTEXT);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
}

XAU_TEST(unarmed_the_bridge_runs_its_guards_and_trades_nothing) {
    Ctx c(default_limits());
    REQUIRE(c.p != nullptr);
    for (int i = 0; i < 5; ++i) {
        const xau_decision d = tick(c.p, healthy_market(kT0ms + i * kMinMs));
        CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));
    }
    CHECK_EQ(xau_is_halted(c.p), 0);
}

// ---------------------------------------------------------------------------
// guards that halt
// ---------------------------------------------------------------------------

XAU_TEST(daily_loss_measures_from_the_first_equity_of_the_broker_day) {
    Ctx c(default_limits());
    xau_market m = healthy_market();
    tick(c.p, m);                         // anchors the day at 10,000

    m.tick_time_ms = m.now_ms = m.tick_time_ms + 1000;
    m.equity = 9'610.0;                   // -3.9%: inside
    CHECK_EQ(tick(c.p, m).action, static_cast<int32_t>(XAU_ACTION_NONE));

    m.tick_time_ms = m.now_ms = m.tick_time_ms + 1000;
    m.equity = 9'600.0;                   // -4.0%: the limit
    const xau_decision d = tick(c.p, m);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_DAILY_LOSS));

    // Sticky: recovering equity does not re-arm anything.
    m.equity = 10'050.0;
    CHECK_EQ(tick(c.p, m).action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(xau_is_halted(c.p), 1);
}

XAU_TEST(a_new_broker_day_resets_the_daily_anchor) {
    Ctx c(default_limits());
    xau_market m = healthy_market();
    tick(c.p, m);
    m.equity = 9'700.0;                   // -3% today
    tick(c.p, m);

    m.server_day = 20240102;              // tomorrow anchors at 9,700
    m.tick_time_ms = m.now_ms = m.tick_time_ms + kDayMs;
    tick(c.p, m);
    m.equity = 9'400.0;                   // -3.1% from 9,700: inside
    CHECK_EQ(tick(c.p, m).action, static_cast<int32_t>(XAU_ACTION_NONE));
    CHECK_EQ(xau_is_halted(c.p), 0);
}

XAU_TEST(drawdown_measures_from_the_peak_or_from_initial_balance) {
    {
        xau_limits l = default_limits();
        l.max_daily_loss_frac = 0.50;     // isolate the drawdown guard
        Ctx        c(l);
        xau_market m = healthy_market();
        m.equity = 12'000.0;
        tick(c.p, m);                     // peak
        m.equity = 11'000.0;              // -8.3% from the peak
        const xau_decision d = tick(c.p, m);
        CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
        CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_MAX_DRAWDOWN));
    }
    {
        // Funded-trader rules: the floor is fixed off the starting balance and
        // does not trail the peak.
        xau_limits l = default_limits();
        l.max_daily_loss_frac = 0.50;
        l.max_drawdown_frac = 0.10;
        l.initial_balance = 10'000.0;
        Ctx        c(l);
        xau_market m = healthy_market();
        m.equity = 12'000.0;
        tick(c.p, m);
        m.equity = 9'100.0;               // -24% from peak, -9% from initial
        CHECK_EQ(tick(c.p, m).action, static_cast<int32_t>(XAU_ACTION_NONE));
        m.equity = 9'000.0;               // -10% from initial
        CHECK_EQ(tick(c.p, m).halt_reason, static_cast<int32_t>(XAU_HALT_MAX_DRAWDOWN));
    }
}

XAU_TEST(a_wide_spread_never_bypasses_the_loss_limits) {
    // The v1 bridge returned at the spread check, BEFORE the daily-loss and
    // drawdown checks: during exactly the news spikes that blow spreads out,
    // the loss limits were not looked at.
    Ctx        c(default_limits());
    xau_market m = healthy_market();
    tick(c.p, m);
    m.equity = 9'500.0;                   // -5% on the day
    m.ask = m.bid + 3.00;                 // and a 3 USD spread
    const xau_decision d = tick(c.p, m);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_DAILY_LOSS));
}

XAU_TEST(an_implausible_quote_halts) {
    Ctx        c(default_limits());
    xau_market m = healthy_market();
    m.ask = m.bid - 1.0;                  // inverted book
    const xau_decision d = tick(c.p, m);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_STALE_QUOTES));
}

XAU_TEST(a_foreign_position_or_too_many_of_ours_halts) {
    {
        Ctx        c(default_limits());
        xau_market m = healthy_market();
        m.foreign_positions = 1;          // a manual trade on the symbol
        m.gross_lots = 0.01;
        CHECK_EQ(tick(c.p, m).halt_reason, static_cast<int32_t>(XAU_HALT_RECONCILE_DRIFT));
    }
    {
        Ctx        c(default_limits());
        xau_market m = healthy_market();
        m.own_positions = 2;              // limit is 1
        m.pos_side = 1;
        m.pos_lots = 0.05;
        m.gross_lots = 0.10;
        CHECK_EQ(tick(c.p, m).halt_reason, static_cast<int32_t>(XAU_HALT_RECONCILE_DRIFT));
    }
}

XAU_TEST(lot_exposure_is_gross_not_net) {
    // A long and a short of 0.06 net to zero and are still 0.12 lots of
    // exposure. v1 measured net and saw nothing.
    xau_limits l = default_limits();
    l.max_open_positions = 2;
    Ctx        c(l);
    xau_market m = healthy_market();
    m.own_positions = 2;
    m.gross_lots = 0.12;
    const xau_decision d = tick(c.p, m);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_RECONCILE_DRIFT));
}

XAU_TEST(zero_limits_become_conservative_defaults_not_no_limits) {
    xau_limits l{};
    Ctx        c(l);
    REQUIRE(c.p != nullptr);
    xau_market m = healthy_market();
    tick(c.p, m);
    m.equity = 9'000.0;                   // -10% on the day
    CHECK_EQ(tick(c.p, m).action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
}

XAU_TEST(halt_is_sticky_and_resume_is_explicit) {
    Ctx c(default_limits());
    CHECK_EQ(xau_halt(c.p, XAU_HALT_MANUAL), XAU_OK);
    for (int i = 0; i < 10; ++i) {
        CHECK_EQ(tick(c.p, healthy_market(kT0ms + i * 1000)).action,
                 static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    }
    CHECK_EQ(xau_is_halted(c.p), 1);
    CHECK_EQ(xau_resume(c.p), XAU_OK);
    CHECK_EQ(xau_is_halted(c.p), 0);
}

XAU_TEST(the_reason_buffer_is_always_null_terminated) {
    Ctx        c(default_limits());
    xau_market m = healthy_market();
    tick(c.p, m);
    m.equity = 9'000.0;
    xau_decision d;
    std::memset(&d, 'X', sizeof(d));
    CHECK_EQ(xau_on_tick(c.p, &m, &d), XAU_OK);
    bool terminated = false;
    for (char ch : d.reason) terminated = terminated || ch == '\0';
    CHECK(terminated);
    char buf[8];
    CHECK_EQ(xau_last_message(c.p, buf, sizeof(buf)), XAU_OK);
    CHECK_EQ(buf[sizeof(buf) - 1], '\0');
    char sbuf[16];
    CHECK_EQ(xau_status_text(c.p, sbuf, sizeof(sbuf)), XAU_OK);
    CHECK_EQ(sbuf[sizeof(sbuf) - 1], '\0');
}

// ---------------------------------------------------------------------------
// persistence: what a restart of MetaTrader must not undo
// ---------------------------------------------------------------------------

XAU_TEST(a_halt_survives_a_restart) {
    fixture::TempDir dir;
    xau_limits       l = default_limits();
    set_path(l.state_file, (dir.path() / "state.txt").string());
    {
        Ctx        c(l);
        xau_market m = healthy_market();
        tick(c.p, m);
        m.equity = 9'500.0;
        CHECK_EQ(tick(c.p, m).halt_reason, static_cast<int32_t>(XAU_HALT_DAILY_LOSS));
    }
    {
        // v1 kept the halt in memory: re-attaching the EA came back armed.
        Ctx c(l);
        CHECK_EQ(xau_is_halted(c.p), 1);
        const xau_decision d = tick(c.p, healthy_market());
        CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
        CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_DAILY_LOSS));
        CHECK_EQ(xau_resume(c.p), XAU_OK);
    }
    {
        Ctx c(l);                         // and the resume persisted too
        CHECK_EQ(xau_is_halted(c.p), 0);
    }
}

XAU_TEST(the_daily_anchor_survives_a_mid_day_restart) {
    fixture::TempDir dir;
    xau_limits       l = default_limits();
    set_path(l.state_file, (dir.path() / "state.txt").string());
    xau_market m = healthy_market();
    {
        Ctx c(l);
        tick(c.p, m);                     // anchor 10,000
        m.equity = 9'800.0;               // -2%
        tick(c.p, m);
    }
    {
        // Same broker day. v1 re-anchored at whatever equity it restarted on,
        // handing back a fresh 4% after having lost 2.
        Ctx c(l);
        m.tick_time_ms = m.now_ms = m.tick_time_ms + 60'000;
        m.equity = 9'590.0;               // -4.1% from the persisted 10,000
        const xau_decision d = tick(c.p, m);
        CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
        CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_DAILY_LOSS));
    }
}

XAU_TEST(an_unreadable_state_file_starts_halted) {
    fixture::TempDir dir;
    const auto       path = dir.path() / "state.txt";
    xau_limits       l = default_limits();
    set_path(l.state_file, path.string());
    {
        Ctx c(l);                         // no file yet: a first run, clean
        CHECK_EQ(xau_is_halted(c.p), 0);
    }
    for (const char* body : {"garbage\n", "xau_bridge_state 1\nhalted=0\n",
                             "xau_bridge_state 1\nhalted=x\nhalt_reason=0\nhalt_message=\nday=1\n"
                             "day_start_equity=1\npeak_equity=1\n"}) {
        std::ofstream(path, std::ios::trunc) << body;
        Ctx c(l);
        // It might have said "halted". Not knowing is treated as if it did.
        CHECK_EQ(xau_is_halted(c.p), 1);
        CHECK_EQ(tick(c.p, healthy_market()).halt_reason,
                 static_cast<int32_t>(XAU_HALT_STATE_FILE));
    }
}

XAU_TEST(two_live_contexts_cannot_share_a_state_file) {
    // Two EAs with the same symbol and magic would overwrite each other's
    // halts and anchors. The second is refused while the first lives.
    fixture::TempDir dir;
    xau_limits       l = default_limits();
    set_path(l.state_file, (dir.path() / "state.txt").string());
    {
        Ctx a(l);
        REQUIRE(a.p != nullptr);
        Ctx b(l);
        CHECK(b.p == nullptr);
        xau_limits other = l;
        set_path(other.state_file, (dir.path() / "other.txt").string());
        Ctx d(other);
        CHECK(d.p != nullptr);
    }
    Ctx again(l);                         // released by the first one's destroy
    CHECK(again.p != nullptr);
}

XAU_TEST(a_failed_state_write_is_retried_until_it_lands) {
    fixture::TempDir dir;
    const auto       sub = dir.path() / "missing";
    const auto       path = sub / "state.txt";
    xau_limits       l = default_limits();
    set_path(l.state_file, path.string());
    {
        Ctx        c(l);
        xau_market m = healthy_market();
        tick(c.p, m);
        m.equity = 9'500.0;               // daily loss: halts, and the write fails
        tick(c.p, m);
        CHECK_EQ(xau_is_halted(c.p), 1);
        CHECK(!std::filesystem::exists(path));
        char buf[1024];
        xau_status_text(c.p, buf, sizeof(buf));
        CHECK(std::string(buf).find("NOT persisted") != std::string::npos);

        std::filesystem::create_directories(sub);   // the disk comes back
        xau_decision d{};
        CHECK_EQ(xau_on_timer(c.p, m.now_ms + 1'500, &d), XAU_OK);
        CHECK(std::filesystem::exists(path));
    }
    Ctx c(l);                             // v2 never retried: this came back armed
    CHECK_EQ(xau_is_halted(c.p), 1);
}

XAU_TEST(resume_after_an_unreadable_state_file_rereads_it) {
    fixture::TempDir dir;
    const auto       path = dir.path() / "state.txt";
    xau_limits       l = default_limits();
    set_path(l.state_file, path.string());
    std::ofstream(path, std::ios::trunc) << "garbage\n";
    const auto read_all = [&] {
        std::ifstream f(path);
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    };

    Ctx c(l);
    REQUIRE(xau_is_halted(c.p) == 1);
    // Still unreadable: refused. And neither a manual halt nor a resume
    // attempt overwrites the file holding the anchors.
    CHECK_EQ(xau_halt(c.p, XAU_HALT_MANUAL), XAU_OK);
    CHECK_EQ(xau_resume(c.p), XAU_ERR_REFUSED);
    CHECK_EQ(read_all(), std::string("garbage\n"));

    // Repaired by the operator: its anchors are adopted, not zeroed. v2
    // resumed on zero anchors, handing back the day's spent allowance.
    std::ofstream(path, std::ios::trunc)
        << "xau_bridge_state 1\nhalted=0\nhalt_reason=0\nhalt_message=\nday=20240101\n"
           "day_start_equity=10000\npeak_equity=10000\n";
    CHECK_EQ(xau_resume(c.p), XAU_OK);
    xau_market m = healthy_market();
    m.equity = 9'590.0;                   // -4.1% from the repaired anchor
    CHECK_EQ(tick(c.p, m).halt_reason, static_cast<int32_t>(XAU_HALT_DAILY_LOSS));
}

XAU_TEST(a_deleted_state_file_resumes_with_fresh_anchors) {
    fixture::TempDir dir;
    const auto       path = dir.path() / "state.txt";
    xau_limits       l = default_limits();
    set_path(l.state_file, path.string());
    std::ofstream(path, std::ios::trunc) << "garbage\n";
    Ctx c(l);
    REQUIRE(xau_is_halted(c.p) == 1);
    std::filesystem::remove(path);
    CHECK_EQ(xau_resume(c.p), XAU_OK);
    CHECK_EQ(xau_is_halted(c.p), 0);
    CHECK(std::filesystem::exists(path));
}

XAU_TEST(state_and_kill_paths_are_utf8) {
    // MQL5\Files under a user name like "René". Read as the ANSI code page,
    // these bytes name a different file on Windows and the kill switch never
    // fires. (On Linux paths are bytes and this passes either way; the
    // Windows CI run is the one that tests it.)
    fixture::TempDir dir;
    const std::string state_u8 = "\xc3\xa9tat.txt", kill_u8 = "ARR\xc3\x8aT";
    xau_limits        l = default_limits();
    const std::u8string d8 = (dir.path() / "").u8string();   // with its separator
    const std::string   base(reinterpret_cast<const char*>(d8.data()), d8.size());
    set_path(l.state_file, base + state_u8);
    set_path(l.kill_file, base + kill_u8);
    const auto state = dir.path() / std::u8string(u8"état.txt");
    const auto kill = dir.path() / std::u8string(u8"ARRÊT");

    Ctx c(l);
    REQUIRE(c.p != nullptr);
    CHECK_EQ(xau_halt(c.p, XAU_HALT_MANUAL), XAU_OK);
    CHECK(std::filesystem::exists(state));
    CHECK_EQ(xau_resume(c.p), XAU_OK);
    std::ofstream(kill) << "stop";
    xau_decision d{};
    CHECK_EQ(xau_on_timer(c.p, kT0ms, &d), XAU_OK);
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_KILL_FILE));
}

// ---------------------------------------------------------------------------
// the kill file and the timer
// ---------------------------------------------------------------------------

XAU_TEST(the_kill_file_halts_from_the_timer_without_any_tick) {
    fixture::TempDir dir;
    const auto       kill = dir.path() / "STOP";
    xau_limits       l = default_limits();
    set_path(l.kill_file, kill.string());
    Ctx c(l);

    xau_decision d{};
    CHECK_EQ(xau_on_timer(c.p, kT0ms, &d), XAU_OK);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));

    std::ofstream(kill) << "stop";
    // The market can be closed or the feed frozen; the operator's stop must
    // still land. v1 only looked on a tick.
    CHECK_EQ(xau_on_timer(c.p, kT0ms + 1500, &d), XAU_OK);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_KILL_FILE));

    // Deleting the file does not resume; resuming while it exists is refused.
    CHECK_EQ(xau_resume(c.p), XAU_ERR_REFUSED);
    std::filesystem::remove(kill);
    CHECK_EQ(xau_resume(c.p), XAU_OK);
}

XAU_TEST(a_kill_file_present_at_start_up_starts_halted) {
    fixture::TempDir dir;
    const auto       kill = dir.path() / "STOP";
    std::ofstream(kill) << "stop";
    xau_limits l = default_limits();
    set_path(l.kill_file, kill.string());
    Ctx c(l);
    CHECK_EQ(xau_is_halted(c.p), 1);
}

XAU_TEST(a_kill_file_in_a_missing_folder_starts_halted_and_blocks_resume) {
    // A typo in the folder, or a path the DLL decodes differently from the
    // EA: the operator's stop would never be seen. Not a reason to trade.
    fixture::TempDir dir;
    const auto       folder = dir.path() / "nope";
    xau_limits       l = default_limits();
    set_path(l.kill_file, (folder / "STOP").string());
    Ctx c(l);
    REQUIRE(c.p != nullptr);
    CHECK_EQ(xau_is_halted(c.p), 1);
    CHECK_EQ(tick(c.p, healthy_market()).halt_reason, static_cast<int32_t>(XAU_HALT_KILL_FILE));
    CHECK_EQ(xau_resume(c.p), XAU_ERR_REFUSED);
    std::filesystem::create_directories(folder);
    CHECK_EQ(xau_resume(c.p), XAU_OK);
}

// ---------------------------------------------------------------------------
// entries, and what refuses them
// ---------------------------------------------------------------------------

XAU_TEST(an_armed_strategy_enters_on_the_bar_close_with_its_size) {
    Ctx c(default_limits());
    arm_buy_and_hold(c.p, 0.05);
    CHECK_EQ(tick(c.p, healthy_market(kT0ms + 5'000)).action, static_cast<int32_t>(XAU_ACTION_NONE));
    const xau_decision d = tick(c.p, healthy_market(kT0ms + kMinMs + 5'000));   // closes minute 0
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_BUY));
    CHECK_NEAR(d.lots, 0.05, 1e-12);
    CHECK_EQ(d.sl_price, 0.0);            // BuyAndHold carries no stop
}

XAU_TEST(the_lot_ceiling_cuts_an_oversized_decision) {
    Ctx c(default_limits());              // max_lots 0.10
    arm_buy_and_hold(c.p, 5.0);
    tick(c.p, healthy_market(kT0ms + 5'000));
    const xau_decision d = tick(c.p, healthy_market(kT0ms + kMinMs + 5'000));
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_BUY));
    CHECK_NEAR(d.lots, 0.10, 1e-12);
}

XAU_TEST(a_wide_spread_refuses_the_entry_without_halting) {
    Ctx c(default_limits());
    arm_buy_and_hold(c.p);
    tick(c.p, healthy_market(kT0ms + 5'000));
    xau_market m = healthy_market(kT0ms + kMinMs + 5'000);
    m.ask = m.bid + 3.00;
    const xau_decision d = tick(c.p, m);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_SPREAD_BLOWOUT));
    CHECK_EQ(xau_is_halted(c.p), 0);
}

XAU_TEST(a_stale_quote_refuses_the_entry_only_while_the_session_is_open) {
    {
        Ctx c(default_limits());          // max age 10 s
        arm_buy_and_hold(c.p);
        tick(c.p, healthy_market(kT0ms + 5'000));
        xau_market m = healthy_market(kT0ms + kMinMs + 5'000);
        m.now_ms = m.tick_time_ms + 15'000;   // the quote is 15 s old
        const xau_decision d = tick(c.p, m);
        // v1 computed this age and threw it away.
        CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));
        CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_STALE_QUOTES));
        CHECK_EQ(xau_is_halted(c.p), 0);
    }
    {
        // The daily break: no quotes is normal when nothing trades.
        Ctx c(default_limits());
        arm_buy_and_hold(c.p);
        tick(c.p, healthy_market(kT0ms + 5'000));
        xau_market m = healthy_market(kT0ms + kMinMs + 5'000);
        m.now_ms = m.tick_time_ms + 15'000;
        m.session_open = 0;
        CHECK_EQ(tick(c.p, m).action, static_cast<int32_t>(XAU_ACTION_BUY));
    }
}

// ---------------------------------------------------------------------------
// one decision, one order
// ---------------------------------------------------------------------------

XAU_TEST(nothing_new_is_sent_until_the_order_is_confirmed) {
    Ctx c(default_limits());
    arm_buy_and_hold(c.p);
    tick(c.p, healthy_market(kT0ms + 5'000));
    REQUIRE(tick(c.p, healthy_market(kT0ms + kMinMs + 5'000)).action == XAU_ACTION_BUY);

    // No result reported yet (and inside the 30 s timeout): nothing goes out.
    xau_decision d = tick(c.p, healthy_market(kT0ms + kMinMs + 20'000));
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));
    CHECK(std::string(d.reason).find("awaiting") != std::string::npos);

    CHECK_EQ(xau_order_result(c.p, 1, 10009, 2650.30, 0.05), XAU_OK);
    // Filled; the position appears at the broker.
    xau_market m = healthy_market(kT0ms + kMinMs + 25'000);
    m.own_positions = 1;
    m.pos_side = 1;
    m.pos_lots = 0.05;
    m.gross_lots = 0.05;
    m.pos_open_price = 2650.30;
    m.pos_open_time_ms = kT0ms + kMinMs + 5'000;
    d = tick(c.p, m);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));
    CHECK_EQ(xau_is_halted(c.p), 0);
    CHECK(message(c.p).find("open at the broker") != std::string::npos);

    // A result with no order outstanding is refused, not applied.
    CHECK_EQ(xau_order_result(c.p, 1, 10009, 2650.30, 0.05), XAU_ERR_REFUSED);
}

XAU_TEST(an_order_whose_result_never_comes_back_halts) {
    Ctx c(default_limits());              // 30 s timeout
    arm_buy_and_hold(c.p);
    tick(c.p, healthy_market(kT0ms + 5'000));
    REQUIRE(tick(c.p, healthy_market(kT0ms + kMinMs + 5'000)).action == XAU_ACTION_BUY);

    xau_decision d{};
    CHECK_EQ(xau_on_timer(c.p, kT0ms + kMinMs + 20'000, &d), XAU_OK);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));
    CHECK_EQ(xau_on_timer(c.p, kT0ms + kMinMs + 40'000, &d), XAU_OK);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_RECONCILE_DRIFT));
}

XAU_TEST(a_failed_order_is_not_retried) {
    Ctx c(default_limits());
    arm_buy_and_hold(c.p);
    tick(c.p, healthy_market(kT0ms + 5'000));
    REQUIRE(tick(c.p, healthy_market(kT0ms + kMinMs + 5'000)).action == XAU_ACTION_BUY);
    CHECK_EQ(xau_order_result(c.p, 0, 10004, 0.0, 0.0), XAU_OK);   // requote
    for (int i = 2; i < 6; ++i) {
        CHECK_EQ(tick(c.p, healthy_market(kT0ms + i * kMinMs + 5'000)).action,
                 static_cast<int32_t>(XAU_ACTION_NONE));
    }
    CHECK_EQ(xau_is_halted(c.p), 0);
}

XAU_TEST(a_fill_closed_before_it_was_seen_open_is_not_a_drift) {
    Ctx c(default_limits());
    arm_buy_and_hold(c.p);
    tick(c.p, healthy_market(kT0ms + 5'000));
    REQUIRE(tick(c.p, healthy_market(kT0ms + kMinMs + 5'000)).action == XAU_ACTION_BUY);
    CHECK_EQ(xau_order_result(c.p, 1, 10009, 2650.30, 0.05), XAU_OK);
    // A tight stop took it out before the next quote: never seen open.
    const xau_decision d = tick(c.p, healthy_market(kT0ms + kMinMs + 40'000));
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));
    CHECK_EQ(xau_is_halted(c.p), 0);
}

XAU_TEST(a_placed_order_is_waited_for_and_a_working_one_halts_at_the_timeout) {
    // TRADE_RETCODE_PLACED: accepted, not yet filled. The EA reports it as ok
    // and the order shows in own_orders until it fills or is cancelled.
    const auto send_and_place = [](void* ctx) {
        arm_buy_and_hold(ctx);
        tick(ctx, healthy_market(kT0ms + 5'000));
        REQUIRE(tick(ctx, healthy_market(kT0ms + kMinMs + 5'000)).action == XAU_ACTION_BUY);
        CHECK_EQ(xau_order_result(ctx, 1, 10008, 0.0, 0.05), XAU_OK);
    };
    {
        Ctx c(default_limits());
        send_and_place(c.p);
        xau_market m = healthy_market(kT0ms + kMinMs + 20'000);
        m.own_orders = 1;
        CHECK_EQ(tick(c.p, m).action, static_cast<int32_t>(XAU_ACTION_NONE));
        m = healthy_market(kT0ms + kMinMs + 40'000);   // past the 30 s timeout
        m.own_orders = 1;
        const xau_decision d = tick(c.p, m);
        CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
        CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_RECONCILE_DRIFT));
    }
    {
        // Cancelled by the broker: nothing open, nothing working. Understood.
        Ctx c(default_limits());
        send_and_place(c.p);
        const xau_decision d = tick(c.p, healthy_market(kT0ms + kMinMs + 40'000));
        CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));
        CHECK_EQ(xau_is_halted(c.p), 0);
    }
}

XAU_TEST(no_entry_while_an_order_of_ours_is_working) {
    Ctx c(default_limits());
    arm_buy_and_hold(c.p);
    tick(c.p, healthy_market(kT0ms + 5'000));
    xau_market m = healthy_market(kT0ms + kMinMs + 5'000);
    m.own_orders = 1;
    const xau_decision d = tick(c.p, m);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));
    CHECK(message(c.p).find("still working") != std::string::npos);
}

XAU_TEST(a_backward_clock_step_does_not_blind_the_kill_file) {
    fixture::TempDir dir;
    const auto       kill = dir.path() / "STOP";
    xau_limits       l = default_limits();
    set_path(l.kill_file, kill.string());
    Ctx          c(l);
    xau_decision d{};
    CHECK_EQ(xau_on_timer(c.p, kT0ms, &d), XAU_OK);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));
    std::ofstream(kill) << "stop";
    // The PC clock is corrected an hour back. v2 compared now - last < 1000,
    // which a negative difference always passes: no check for an hour.
    CHECK_EQ(xau_on_timer(c.p, kT0ms - 3'600'000, &d), XAU_OK);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_KILL_FILE));
}

// ---------------------------------------------------------------------------
// arming and warm-up
// ---------------------------------------------------------------------------

XAU_TEST(arming_refuses_what_it_cannot_trade_safely) {
    Ctx c(default_limits());
    xau_strategy_config k = gold_config("NoSuchStrategy", XAU_TF_H1, 0.01);
    CHECK_EQ(xau_arm(c.p, &k), XAU_ERR_BAD_ARGUMENT);
    k = gold_config("BuyAndHold", 9, 0.01);
    CHECK_EQ(xau_arm(c.p, &k), XAU_ERR_BAD_ARGUMENT);
    k = gold_config("BuyAndHold", XAU_TF_H1, 0.0);
    k.risk_per_trade = 0.10;              // 10% a trade
    CHECK_EQ(xau_arm(c.p, &k), XAU_ERR_BAD_ARGUMENT);
    k = gold_config("BuyAndHold", XAU_TF_H1, 0.01);
    k.contract_size = 0.0;
    CHECK_EQ(xau_arm(c.p, &k), XAU_ERR_BAD_ARGUMENT);

    // Still unarmed after the refusals: nothing trades.
    tick(c.p, healthy_market(kT0ms + 5'000));
    CHECK_EQ(tick(c.p, healthy_market(kT0ms + kMinMs + 5'000)).action,
             static_cast<int32_t>(XAU_ACTION_NONE));

    k = gold_config("BuyAndHold", XAU_TF_M1, 0.01);
    CHECK_EQ(xau_arm(c.p, &k), XAU_OK);
    CHECK_EQ(xau_arm(c.p, &k), XAU_ERR_REFUSED);   // no silent swap mid-session
}

XAU_TEST(warm_up_makes_a_twelve_month_lookback_trade_on_its_first_live_bar) {
    // TimeSeriesMomentum needs a year of daily closes. Without warm-up it
    // would sit silent for a year after every restart.
    Ctx c(default_limits());
    const xau_strategy_config k = gold_config("TimeSeriesMomentum", XAU_TF_D1, 0.01);
    REQUIRE(xau_arm(c.p, &k) == XAU_OK);

    // 400 days of a slow rise, weekdays only, one rate per hour standing in
    // for the minutes (the assembler only needs the bars to close).
    std::vector<xau_rate> rates;
    const int64_t         start = kT0ms - 400 * kDayMs;
    for (int64_t t = start; t < kT0ms; t += 60 * kMinMs) {
        if (!xau::market_open(static_cast<xau::TimeUs>(t) * 1000)) continue;
        const double px = 1800.0 + 0.02 * static_cast<double>((t - start) / (60 * kMinMs));
        xau_rate r{};
        r.time_ms = t;
        r.open = px;
        r.high = px + 1.0;
        r.low = px - 1.0;
        r.close = px + 0.5;
        r.spread = 0.30;
        r.tick_volume = 100;
        rates.push_back(r);
    }
    CHECK_EQ(xau_warmup(c.p, rates.data(), static_cast<int32_t>(rates.size())), XAU_OK);
    CHECK(message(c.p).find("warm-up") != std::string::npos);

    // First live quotes: the first one closes the last warm-up day, and the
    // year behind it is up.
    xau_market m = healthy_market(kT0ms + 5'000);
    m.bid = 1800.0 + 0.02 * 9600.0;
    m.ask = m.bid + 0.30;
    m.server_day = 20240101;
    const xau_decision d = tick(c.p, m);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_BUY));
    CHECK(d.sl_price > 0.0 && d.sl_price < m.ask);   // its 4-ATR disaster stop
}

// ---------------------------------------------------------------------------
// parity: live through this ABI == the backtest engine, trade for trade
// ---------------------------------------------------------------------------

namespace {

struct SimTrade {
    int64_t entry_ms = 0, exit_ms = 0;
    int     side = 0;
    int64_t entry_pts = 0, exit_pts = 0;
    char    why = '?';
};

// How the simulated broker misbehaves, for the exit tests. Default: never.
struct SimOpts {
    int  fail_closes = 0;      // the first N closes are rejected; the position stays
    int  ignore_closes = 0;    // the first N are reported done, yet the position stays
    bool timer = false;        // also run the 1 s timer between quotes, as the EA does
};

struct SimStats {
    int halts = 0;             // FLATTEN_AND_HALT decisions seen
    int last_halt_reason = 0;
    int close_sends = 0;       // CLOSE decisions seen
};

// A broker that fills the way the engine does with costs at zero: market
// orders at the touch, stops through gaps at the market, targets at their
// price. If the bridge and the engine disagree, this is where it shows.
std::vector<SimTrade> run_through_bridge(const std::vector<xau::Tick>& ticks, const char* strategy,
                                         int32_t tf, double lots, SimStats& st,
                                         const SimOpts& o = {}) {
    xau_limits l = default_limits();
    l.max_daily_loss_frac = 0.99;
    l.max_drawdown_frac = 0.99;
    l.max_spread = 1e9;
    l.max_lots = 1.0;
    void*                     ctx = xau_create("XAUUSD", XAU_BRIDGE_ABI_VERSION, &l);
    const xau_strategy_config k = gold_config(strategy, tf, lots);
    std::vector<SimTrade>     out;
    if (xau_arm(ctx, &k) != XAU_OK) {
        CHECK(false);                     // arming a registry strategy must work
        xau_destroy(ctx);
        return out;
    }

    SimTrade              open{};
    double                sl = 0.0, tp = 0.0;
    int                   fail_left = o.fail_closes, ignore_left = o.ignore_closes;
    int64_t               last_ms = 0;
    const auto            px = [](xau::Points p) { return static_cast<double>(p) / 1000.0; };
    const auto            pts = [](double v) { return static_cast<int64_t>(std::llround(v * 1000.0)); };
    const auto            norm = [](double v) { return std::round(v * 1000.0) / 1000.0; };   // NormalizeDouble

    for (const xau::Tick& t : ticks) {
        const int64_t ms = t.ts_us / 1000;
        const double  bid = px(t.bid_pts), ask = px(t.ask_pts());

        // 0) the timer fires each second between quotes (a few, over a gap)
        if (o.timer && last_ms != 0) {
            for (int64_t tm = last_ms + 1000; tm < ms && tm <= last_ms + 5000; tm += 1000) {
                xau_decision td{};
                xau_on_timer(ctx, tm, &td);
                if (td.action == XAU_ACTION_FLATTEN_AND_HALT) {
                    ++st.halts;
                    st.last_halt_reason = td.halt_reason;
                }
            }
        }
        last_ms = ms;

        // 1) the broker's resting stop and target
        if (open.side != 0) {
            const double close_px = open.side > 0 ? bid : ask;
            bool         hit = false;
            if (sl > 0.0 && (open.side > 0 ? close_px <= sl : close_px >= sl)) {
                open.exit_pts = pts(open.side > 0 ? std::min(close_px, sl) : std::max(close_px, sl));
                open.why = 's';
                hit = true;
            } else if (tp > 0.0 && (open.side > 0 ? close_px >= tp : close_px <= tp)) {
                open.exit_pts = pts(tp);
                open.why = 't';
                hit = true;
            }
            if (hit) {
                open.exit_ms = ms;
                out.push_back(open);
                open = SimTrade{};
            }
        }

        // 2) the bridge
        xau_market m{};
        m.tick_time_ms = m.now_ms = ms;
        m.bid = bid;
        m.ask = ask;
        m.equity = m.balance = 10'000.0;
        m.value_per_price_lot = 100.0;
        m.server_day = 0;                 // falls back to the UTC date
        if (open.side != 0) {
            m.own_positions = 1;
            m.pos_side = open.side;
            m.pos_lots = lots;
            m.gross_lots = lots;
            m.pos_open_price = px(static_cast<xau::Points>(open.entry_pts));
            m.pos_open_time_ms = open.entry_ms;
            m.pos_sl = sl;
            m.pos_tp = tp;
        }
        xau_decision d{};
        xau_on_tick(ctx, &m, &d);

        // 3) execute at this tick's prices, and confirm
        if (d.action == XAU_ACTION_BUY || d.action == XAU_ACTION_SELL) {
            open = SimTrade{};
            open.side = d.action == XAU_ACTION_BUY ? 1 : -1;
            open.entry_ms = ms;
            const double fill = open.side > 0 ? ask : bid;
            open.entry_pts = pts(fill);
            // As the EA does: stop and target re-anchored on the fill by
            // their distances. With no slippage that is the quoted price
            // exactly, and must be -- a hair off and a touch stops nothing.
            sl = d.sl_dist > 0.0 ? norm(fill - open.side * d.sl_dist) : 0.0;
            tp = d.tp_dist > 0.0 ? norm(fill + open.side * d.tp_dist) : 0.0;
            CHECK(sl == d.sl_price);
            CHECK(tp == d.tp_price);
            xau_order_result(ctx, 1, 10009, fill, d.lots);
        } else if (d.action == XAU_ACTION_CLOSE && open.side != 0) {
            ++st.close_sends;
            if (fail_left > 0) {
                --fail_left;
                xau_order_result(ctx, 0, 10006, 0.0, 0.0);   // rejected
            } else if (ignore_left > 0) {
                --ignore_left;
                xau_order_result(ctx, 1, 10009, 0.0, lots);  // "done", and still open
            } else {
                open.exit_ms = ms;
                open.exit_pts = pts(open.side > 0 ? bid : ask);
                open.why = 'c';
                out.push_back(open);
                open = SimTrade{};
                xau_order_result(ctx, 1, 10009, 0.0, lots);
            }
        } else if (d.action == XAU_ACTION_FLATTEN_AND_HALT) {
            ++st.halts;
            st.last_halt_reason = d.halt_reason;
        }
    }
    xau_destroy(ctx);
    return out;
}

std::vector<xau::Tick> parity_ticks(int days, std::uint64_t seed) {
    std::vector<xau::Tick> v;
    xau::Points            bid = 2'000'000;
    std::uint64_t          s = seed;
    const xau::TimeUs      t0 = static_cast<xau::TimeUs>(kT0ms) * 1000;
    for (xau::TimeUs t = t0; t < t0 + static_cast<xau::TimeUs>(days) * xau::kUsPerDay;
         t += 3'000'000) {                 // a quote every 3 s, whole milliseconds
        if (!xau::market_open(t)) continue;
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        bid += static_cast<xau::Points>((s >> 59) % 31) - 15;
        const int  h = xau::utc_hour(t);
        const auto spread = static_cast<std::uint16_t>((h >= 7 && h < 17) ? 220 : 380);
        v.push_back(fixture::tick(t, bid, spread));
    }
    return v;
}

}  // namespace

XAU_TEST(bridge_trades_exactly_what_the_backtest_engine_trades) {
    const auto       ticks = parity_ticks(40, 7);
    fixture::TempDir dir;
    const auto       store = fixture::make_store(dir, ticks);

    struct Case {
        const char*    name;
        xau::Timeframe tf;
    };
    std::size_t total_trades = 0;
    for (const Case cs : {Case{"LondonOpeningRange", xau::Timeframe::M15},
                          Case{"Rsi2Extreme", xau::Timeframe::M15},
                          Case{"BollingerReversion", xau::Timeframe::M15},
                          Case{"InsideBarBreak", xau::Timeframe::H1},
                          Case{"MomentumContinuation", xau::Timeframe::M15}}) {
        const xau::BaselineEntry* e = xau::find_baseline(cs.name);
        REQUIRE(e != nullptr);

        xau::BacktestConfig cfg;
        cfg.spec = xau::SymbolSpec::xauusd_default();
        cfg.tf = cs.tf;
        cfg.apply_swap = false;
        cfg.costs.slip_base_pts = 0.0;
        cfg.costs.slip_vol_coef = 0.0;
        cfg.costs.latency_us = 0;
        cfg.costs.commission_per_lot_round_usd = 0.0;
        auto        strat = e->make(0.10);
        const auto  bt = xau::BacktestEngine(store, cfg).run(*strat);

        SimStats   st;
        const auto live = run_through_bridge(ticks, cs.name, static_cast<int32_t>(cs.tf), 0.10, st);
        CHECK_EQ(st.halts, 0);

        std::vector<const xau::Trade*> want;
        for (const xau::Trade& t : bt.trades)
            if (t.exit_reason != xau::ExitReason::EndOfData) want.push_back(&t);

        CHECK_EQ(live.size(), want.size());
        const std::size_t n = std::min(live.size(), want.size());
        bool              same = true;
        for (std::size_t i = 0; i < n; ++i) {
            const xau::Trade& b = *want[i];
            const SimTrade&   s = live[i];
            const char        why = b.exit_reason == xau::ExitReason::StopLoss   ? 's'
                                    : b.exit_reason == xau::ExitReason::TakeProfit ? 't'
                                                                                   : 'c';
            same = same && s.entry_ms * 1000 == b.entry_ts && s.exit_ms * 1000 == b.exit_ts &&
                   s.side == (b.side == xau::Side::Long ? 1 : -1) && s.entry_pts == b.entry_pts &&
                   s.exit_pts == b.exit_pts && s.why == why;
        }
        if (!same) {
            for (std::size_t i = 0; i < n; ++i) {
                const xau::Trade& b = *want[i];
                const SimTrade&   q = live[i];
                if (q.entry_ms * 1000 != b.entry_ts || q.exit_ms * 1000 != b.exit_ts ||
                    q.entry_pts != b.entry_pts || q.exit_pts != b.exit_pts) {
                    std::fprintf(stderr, "    %s #%zu engine %lld..%lld %d->%d r%d | bridge %lld..%lld %lld->%lld %c\n",
                                 cs.name, i, (long long)b.entry_ts / 1000, (long long)b.exit_ts / 1000,
                                 b.entry_pts, b.exit_pts, (int)b.exit_reason, (long long)q.entry_ms,
                                 (long long)q.exit_ms, (long long)q.entry_pts, (long long)q.exit_pts, q.why);
                    break;
                }
            }
        }
        CHECK(same);
        total_trades += want.size();
    }
    // A parity test that compares two empty lists proves nothing.
    CHECK(total_trades > 50);
}

// ---------------------------------------------------------------------------
// exits the broker does not carry out
// ---------------------------------------------------------------------------

namespace {

// The first parity strategy with an exit of the strategy's own (not a stop or
// target): the only kind that sends CLOSE.
struct CloseCase {
    const char*           name = nullptr;
    int32_t               tf = 0;
    std::vector<SimTrade> base;
    std::size_t           first_c = 0;
};

CloseCase find_close_case(const std::vector<xau::Tick>& ticks) {
    for (const auto& [name, tf] : {std::pair{"MomentumContinuation", XAU_TF_M15},
                                   std::pair{"LondonOpeningRange", XAU_TF_M15},
                                   std::pair{"InsideBarBreak", XAU_TF_H1}}) {
        SimStats   st;
        CloseCase  cc;
        cc.name = name;
        cc.tf = tf;
        cc.base = run_through_bridge(ticks, name, tf, 0.10, st);
        for (std::size_t i = 0; i < cc.base.size(); ++i) {
            if (cc.base[i].why == 'c') {
                cc.first_c = i;
                return cc;
            }
        }
    }
    return {};
}

}  // namespace

XAU_TEST(a_rejected_close_is_retried_not_dropped) {
    const auto ticks = parity_ticks(40, 7);
    const auto cc = find_close_case(ticks);
    REQUIRE(cc.name != nullptr);
    const SimTrade& want = cc.base[cc.first_c];

    // v2 dropped it: the strategy's exit was gone and the position sat
    // unmanaged until its stop.
    SimOpts o;
    o.fail_closes = 2;
    SimStats   st;
    const auto live = run_through_bridge(ticks, cc.name, cc.tf, 0.10, st, o);
    CHECK_EQ(st.halts, 0);
    REQUIRE(live.size() > cc.first_c);
    const SimTrade& got = live[cc.first_c];
    CHECK_EQ(got.entry_ms, want.entry_ms);
    CHECK_EQ(got.why, 'c');
    // Two rejections, retried a second apart: out on the second quote after
    // the strategy's own exit (quotes are 3 s apart).
    CHECK_EQ(got.exit_ms, want.exit_ms + 6'000);
}

XAU_TEST(a_close_that_never_gets_through_halts_after_five_attempts) {
    const auto ticks = parity_ticks(40, 7);
    const auto cc = find_close_case(ticks);
    REQUIRE(cc.name != nullptr);
    SimOpts o;
    o.fail_closes = 1'000'000;
    SimStats st;
    run_through_bridge(ticks, cc.name, cc.tf, 0.10, st, o);
    CHECK_EQ(st.close_sends, 5);
    CHECK(st.halts > 0);
    CHECK_EQ(st.last_halt_reason, static_cast<int32_t>(XAU_HALT_RECONCILE_DRIFT));
}

XAU_TEST(a_close_reported_done_but_still_open_is_retried_and_the_timer_waits) {
    const auto ticks = parity_ticks(40, 7);
    const auto cc = find_close_case(ticks);
    REQUIRE(cc.name != nullptr);
    const SimTrade& want = cc.base[cc.first_c];

    // v2's timer halted on this at 30 s: an accepted close not yet reflected
    // in the book. Now the tick sees the position still open and re-sends.
    SimOpts o;
    o.ignore_closes = 1;
    o.timer = true;
    SimStats   st;
    const auto live = run_through_bridge(ticks, cc.name, cc.tf, 0.10, st, o);
    CHECK_EQ(st.halts, 0);
    REQUIRE(live.size() > cc.first_c);
    const SimTrade& got = live[cc.first_c];
    CHECK_EQ(got.why, 'c');
    CHECK(got.exit_ms > want.exit_ms + 30'000);   // the 30 s wait, then the retry
    CHECK(got.exit_ms <= want.exit_ms + 36'000);
}

// ---------------------------------------------------------------------------
// sizing in the account currency, and the broker's stop level
// ---------------------------------------------------------------------------

namespace {

struct Entry {
    xau_decision             d{};
    int64_t                  ms = 0;
    double                   bid = 0.0, ask = 0.0;
    std::vector<std::string> refusals;   // "ms message", every refused entry
};

// The first entry LondonOpeningRange makes on the parity ticks, risk-sized.
Entry first_entry(double value_per_price_lot, int32_t stops_level_pts) {
    const auto ticks = parity_ticks(40, 7);
    xau_limits l = default_limits();
    l.max_spread = 1e9;
    l.max_lots = 50.0;                    // out of the way: sizing is what is tested
    Ctx                 c(l);
    xau_strategy_config k = gold_config("LondonOpeningRange", XAU_TF_M15, 0.0);
    k.risk_per_trade = 0.01;
    k.stops_level_pts = stops_level_pts;
    Entry e;
    if (xau_arm(c.p, &k) != XAU_OK) {
        CHECK(false);
        return e;
    }
    for (const xau::Tick& t : ticks) {
        xau_market m{};
        m.tick_time_ms = m.now_ms = t.ts_us / 1000;
        m.bid = static_cast<double>(t.bid_pts) / 1000.0;
        m.ask = static_cast<double>(t.ask_pts()) / 1000.0;
        m.equity = m.balance = 10'000.0;
        m.value_per_price_lot = value_per_price_lot;
        xau_decision d{};
        xau_on_tick(c.p, &m, &d);
        if (d.action == XAU_ACTION_BUY || d.action == XAU_ACTION_SELL) {
            e.d = d;
            e.ms = m.now_ms;
            e.bid = m.bid;
            e.ask = m.ask;
            return e;
        }
        const std::string msg = message(c.p);
        if (msg.rfind("entry refused", 0) == 0 &&
            (e.refusals.empty() || e.refusals.back() != std::to_string(m.now_ms) + " " + msg))
            e.refusals.push_back(std::to_string(m.now_ms) + " " + msg);
    }
    return e;
}

}  // namespace

XAU_TEST(risk_sizing_uses_the_brokers_value_in_the_account_currency) {
    const Entry usd = first_entry(100.0, 0);    // USD account: 100 per 1.00 per lot
    REQUIRE(usd.d.action == XAU_ACTION_BUY || usd.d.action == XAU_ACTION_SELL);
    REQUIRE(usd.d.sl_dist > 0.0);
    // 1% of 10,000 lost at the stop.
    const double want = std::floor(100.0 / (usd.d.sl_dist * 100.0) * 100.0 + 1e-9) / 100.0;
    CHECK_NEAR(usd.d.lots, want, 1e-9);
    const double ref = usd.d.action == XAU_ACTION_BUY ? usd.ask : usd.bid;
    CHECK_NEAR(std::fabs(ref - usd.d.sl_price), usd.d.sl_dist, 1e-9);

    // An account where the same move is worth half as much (v2 sized every
    // non-USD account as if it were USD).
    const Entry half = first_entry(50.0, 0);
    REQUIRE(half.ms == usd.ms);
    const double want_half = std::floor(100.0 / (usd.d.sl_dist * 50.0) * 100.0 + 1e-9) / 100.0;
    CHECK_NEAR(half.d.lots, want_half, 1e-9);

    // No value from the broker: refused, never guessed.
    const Entry none = first_entry(0.0, 0);
    CHECK_EQ(none.d.action, static_cast<int32_t>(XAU_ACTION_NONE));
    REQUIRE(!none.refusals.empty());
    CHECK(none.refusals.front().find("tick value") != std::string::npos);
}

XAU_TEST(the_stop_level_is_measured_from_bid_and_ask_as_the_broker_does) {
    const Entry base = first_entry(100.0, 0);
    REQUIRE(base.d.sl_dist > 0.0);
    const auto dist = static_cast<int32_t>(std::llround(base.d.sl_dist * 1000.0));
    const auto spread = static_cast<int32_t>(std::llround((base.ask - base.bid) * 1000.0));
    REQUIRE(dist > spread + 1);

    // dist >= level: the engine's rule passes. dist < level + spread: the
    // broker would reject it (v2 sent it).
    const Entry tight = first_entry(100.0, dist - spread / 2);
    const std::string at = std::to_string(base.ms) + " ";
    bool refused_there = false;
    for (const auto& r : tight.refusals)
        refused_there = refused_there || (r.rfind(at, 0) == 0 && r.find("bid/ask") != std::string::npos);
    CHECK(refused_there);
    CHECK(tight.ms != base.ms);

    // dist == level + spread: allowed, at the same moment.
    const Entry ok = first_entry(100.0, dist - spread);
    CHECK_EQ(ok.ms, base.ms);
}
