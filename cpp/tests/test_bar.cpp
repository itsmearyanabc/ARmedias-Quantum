#include "harness.hpp"

#include "xau/bar.hpp"
#include "xau/tick_store.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

namespace fs = std::filesystem;
using namespace xau;

namespace {

class TempDir {
public:
    TempDir() {
        std::random_device rd;
        path_ = fs::temp_directory_path() /
                ("xau_bar_" + std::to_string(rd()) + "_" + std::to_string(rd()));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void write_file(const fs::path& p, std::string_view symbol, const std::vector<Tick>& ticks) {
    FileHeader h{};
    std::memcpy(h.magic, MAGIC, sizeof(MAGIC));
    h.version     = FORMAT_VERSION;
    h.tick_size   = static_cast<std::uint32_t>(sizeof(Tick));
    h.point_num   = XAUUSD_POINT_NUM;
    h.point_den   = XAUUSD_POINT_DEN;
    h.first_ts_us = ticks.empty() ? 0 : ticks.front().ts_us;
    h.last_ts_us  = ticks.empty() ? 0 : ticks.back().ts_us;
    h.count       = ticks.size();
    std::memcpy(h.symbol, symbol.data(), std::min(symbol.size(), sizeof(h.symbol)));

    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(&h), sizeof(h));
    if (!ticks.empty()) {
        out.write(reinterpret_cast<const char*>(ticks.data()),
                  static_cast<std::streamsize>(ticks.size() * sizeof(Tick)));
    }
}

Tick mk(TimeUs ts, Points bid, std::uint16_t spread = 250) {
    Tick t{};
    t.ts_us      = ts;
    t.bid_pts    = bid;
    t.spread_pts = spread;
    t.flags      = TF_BID | TF_ASK;
    return t;
}

constexpr TimeUs kJan2020 = 1'577'836'800'000'000LL;  // 2020-01-01T00:00:00Z, an exact bar edge
constexpr TimeUs kMin = 60'000'000LL;

}  // namespace

XAU_TEST(timeframe_durations) {
    CHECK_EQ(timeframe_us(Timeframe::M1), kMin);
    CHECK_EQ(timeframe_us(Timeframe::M15), 15 * kMin);
    CHECK_EQ(timeframe_us(Timeframe::H1), 60 * kMin);
    CHECK_EQ(timeframe_us(Timeframe::H4), 240 * kMin);
    CHECK_EQ(timeframe_us(Timeframe::D1), 1440 * kMin);
    CHECK_EQ(std::string(timeframe_name(Timeframe::M15)), std::string("M15"));
}

XAU_TEST(bar_alignment) {
    // Exactly on a boundary stays put.
    CHECK_EQ(bar_open_for(kJan2020, Timeframe::M15), kJan2020);
    // One microsecond in still belongs to the same bar.
    CHECK_EQ(bar_open_for(kJan2020 + 1, Timeframe::M15), kJan2020);
    // Last microsecond of the bar.
    CHECK_EQ(bar_open_for(kJan2020 + 15 * kMin - 1, Timeframe::M15), kJan2020);
    // First microsecond of the next.
    CHECK_EQ(bar_open_for(kJan2020 + 15 * kMin, Timeframe::M15), kJan2020 + 15 * kMin);

    // Pre-epoch timestamps must floor, not truncate toward zero — otherwise
    // bars either side of 1970 would be misaligned by one.
    CHECK_EQ(bar_open_for(-1, Timeframe::M1), -kMin);
    CHECK_EQ(bar_open_for(-kMin, Timeframe::M1), -kMin);
    CHECK_EQ(bar_open_for(-kMin - 1, Timeframe::M1), -2 * kMin);
}

XAU_TEST(ohlc_is_correct) {
    TempDir dir;
    // One M15 bucket: open 2650.000, high 2651.000, low 2649.000, close 2650.500
    std::vector<Tick> ticks = {
        mk(kJan2020 + 0 * kMin, 2'650'000, 200),
        mk(kJan2020 + 1 * kMin, 2'651'000, 300),
        mk(kJan2020 + 2 * kMin, 2'649'000, 400),
        mk(kJan2020 + 3 * kMin, 2'650'500, 100),
    };
    write_file(dir.path() / "XAUUSD-2020-01.bin", "XAUUSD", ticks);
    const TickStore store = TickStore::open(dir.path(), "XAUUSD");

    const BarSeries s =
        BarSeries::build(store, Timeframe::M15, kJan2020, kJan2020 + 15 * kMin);
    REQUIRE(s.size() == 1);
    const Bar& b = s.all_for_display()[0];

    CHECK_EQ(b.open_time_us, kJan2020);
    CHECK_EQ(b.open, Points{2'650'000});
    CHECK_EQ(b.high, Points{2'651'000});
    CHECK_EQ(b.low, Points{2'649'000});
    CHECK_EQ(b.close, Points{2'650'500});
    CHECK_EQ(b.ticks, std::uint32_t{4});
    CHECK_EQ(b.range_pts(), Points{2000});
    CHECK_EQ(b.spread_max_pts, std::uint32_t{400});
    CHECK_EQ(b.spread_mean_pts, std::uint32_t{250});  // (200+300+400+100)/4
    CHECK_EQ(b.close_time_us(Timeframe::M15), kJan2020 + 15 * kMin);
}

XAU_TEST(buckets_split_on_boundaries) {
    TempDir dir;
    std::vector<Tick> ticks = {
        mk(kJan2020, 2'650'000),
        mk(kJan2020 + 15 * kMin - 1, 2'651'000),  // last us of bar 0
        mk(kJan2020 + 15 * kMin, 2'652'000),      // first us of bar 1
        mk(kJan2020 + 29 * kMin, 2'653'000),
    };
    write_file(dir.path() / "XAUUSD-2020-01.bin", "XAUUSD", ticks);
    const TickStore store = TickStore::open(dir.path(), "XAUUSD");

    const BarSeries s =
        BarSeries::build(store, Timeframe::M15, kJan2020, kJan2020 + 30 * kMin);
    REQUIRE(s.size() == 2);
    const auto bars = s.all_for_display();
    CHECK_EQ(bars[0].close, Points{2'651'000});
    CHECK_EQ(bars[1].open, Points{2'652'000});
    CHECK_EQ(bars[0].ticks, std::uint32_t{2});
    CHECK_EQ(bars[1].ticks, std::uint32_t{2});
}

XAU_TEST(empty_buckets_produce_no_bars) {
    TempDir dir;
    // A three-hour hole, as a weekend or a holiday would leave.
    std::vector<Tick> ticks = {
        mk(kJan2020, 2'650'000),
        mk(kJan2020 + 180 * kMin, 2'660'000),
    };
    write_file(dir.path() / "XAUUSD-2020-01.bin", "XAUUSD", ticks);
    const TickStore store = TickStore::open(dir.path(), "XAUUSD");

    const BarSeries s =
        BarSeries::build(store, Timeframe::M15, kJan2020, kJan2020 + 195 * kMin);
    // Two bars, not thirteen. A flat line across a closed market would be a lie,
    // and zero-range bars would poison every indicator built on this.
    CHECK_EQ(s.size(), std::size_t{2});
    CHECK_EQ(s.all_for_display()[1].open_time_us, kJan2020 + 180 * kMin);
}

XAU_TEST(forming_bar_is_separated_from_closed_ones) {
    TempDir dir;
    std::vector<Tick> ticks = {
        mk(kJan2020, 2'650'000),
        mk(kJan2020 + 16 * kMin, 2'651'000),  // in the second bucket
    };
    write_file(dir.path() / "XAUUSD-2020-01.bin", "XAUUSD", ticks);
    const TickStore store = TickStore::open(dir.path(), "XAUUSD");

    // Window ends mid-way through the second bucket: bar 0 is closed, bar 1 is not.
    const BarSeries s =
        BarSeries::build(store, Timeframe::M15, kJan2020, kJan2020 + 20 * kMin);
    CHECK_EQ(s.size(), std::size_t{2});
    CHECK_EQ(s.closed().size(), std::size_t{1});
    CHECK(s.forming().has_value());
    if (s.forming()) CHECK_EQ(s.forming()->open, Points{2'651'000});

    // Window ending exactly on a boundary closes everything: the bar covering
    // [15m, 30m) has fully elapsed at 30m.
    const BarSeries s2 =
        BarSeries::build(store, Timeframe::M15, kJan2020, kJan2020 + 30 * kMin);
    CHECK_EQ(s2.closed().size(), std::size_t{2});
    CHECK(!s2.forming().has_value());
}

XAU_TEST(bars_span_month_files) {
    TempDir dir;
    constexpr TimeUs kFeb2020 = 1'580'515'200'000'000LL;
    write_file(dir.path() / "XAUUSD-2020-01.bin", "XAUUSD",
               {mk(kJan2020, 2'650'000), mk(kJan2020 + kMin, 2'650'500)});
    write_file(dir.path() / "XAUUSD-2020-02.bin", "XAUUSD",
               {mk(kFeb2020, 2'700'000), mk(kFeb2020 + kMin, 2'700'500)});
    const TickStore store = TickStore::open(dir.path(), "XAUUSD");

    const BarSeries s = BarSeries::build(store, Timeframe::H1, kJan2020, kFeb2020 + 60 * kMin);
    CHECK_EQ(s.size(), std::size_t{2});
    CHECK_EQ(s.all_for_display()[0].open, Points{2'650'000});
    CHECK_EQ(s.all_for_display()[1].open, Points{2'700'000});
}

XAU_TEST(empty_and_degenerate_windows) {
    TempDir dir;
    write_file(dir.path() / "XAUUSD-2020-01.bin", "XAUUSD", {mk(kJan2020, 2'650'000)});
    const TickStore store = TickStore::open(dir.path(), "XAUUSD");

    CHECK(BarSeries::build(store, Timeframe::M15, kJan2020, kJan2020).empty());
    CHECK(BarSeries::build(store, Timeframe::M15, kJan2020 + kMin, kJan2020).empty());
    CHECK(BarSeries::build(store, Timeframe::M15, kJan2020 - 100 * kMin,
                           kJan2020 - 50 * kMin)
              .empty());
}
