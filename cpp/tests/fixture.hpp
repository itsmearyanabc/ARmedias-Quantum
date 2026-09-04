// Shared test fixtures: a self-cleaning temp directory and a tick-file writer.
#pragma once

#include "xau/tick_store.hpp"
#include "xau/types.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string_view>
#include <vector>

namespace fixture {

namespace fs = std::filesystem;

class TempDir {
public:
    TempDir() {
        std::random_device rd;
        path_ = fs::temp_directory_path() /
                ("xau_t_" + std::to_string(rd()) + "_" + std::to_string(rd()));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

inline xau::Tick tick(xau::TimeUs ts, xau::Points bid, std::uint16_t spread = 200) {
    xau::Tick t{};
    t.ts_us = ts;
    t.bid_pts = bid;
    t.spread_pts = spread;
    t.flags = xau::TF_BID | xau::TF_ASK;
    return t;
}

inline void write_ticks(const fs::path& path, std::string_view symbol,
                        const std::vector<xau::Tick>& ticks) {
    xau::FileHeader h{};
    std::memcpy(h.magic, xau::MAGIC, sizeof(xau::MAGIC));
    h.version     = xau::FORMAT_VERSION;
    h.tick_size   = static_cast<std::uint32_t>(sizeof(xau::Tick));
    h.point_num   = xau::XAUUSD_POINT_NUM;
    h.point_den   = xau::XAUUSD_POINT_DEN;
    h.first_ts_us = ticks.empty() ? 0 : ticks.front().ts_us;
    h.last_ts_us  = ticks.empty() ? 0 : ticks.back().ts_us;
    h.count       = ticks.size();
    std::memcpy(h.symbol, symbol.data(), std::min(symbol.size(), sizeof(h.symbol)));

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(&h), sizeof(h));
    if (!ticks.empty()) {
        out.write(reinterpret_cast<const char*>(ticks.data()),
                  static_cast<std::streamsize>(ticks.size() * sizeof(xau::Tick)));
    }
}

// Writes one month file and opens a store over it.
inline xau::TickStore make_store(const TempDir& dir, const std::vector<xau::Tick>& ticks,
                                 std::string_view symbol = "XAUUSD") {
    write_ticks(dir.path() / (std::string(symbol) + "-2020-01.bin"), symbol, ticks);
    return xau::TickStore::open(dir.path(), symbol);
}

// 2020-01-01T00:00:00Z. Divisible by every timeframe up to D1, so bars land on
// exact boundaries and the arithmetic in a test stays checkable by hand.
inline constexpr xau::TimeUs kT0 = 1'577'836'800'000'000LL;
inline constexpr xau::TimeUs kMinute = 60'000'000LL;

}  // namespace fixture
