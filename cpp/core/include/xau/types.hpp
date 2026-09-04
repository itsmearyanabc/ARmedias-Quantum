// Core value types and the on-disk tick format.
//
// This header is the single source of truth for the binary layout. The Python
// writer in python/xau_ingest/tickfmt.py mirrors it exactly; if you change a
// field here, change it there and bump FORMAT_VERSION.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace xau {

// Prices are integer points, never floats.
//
// 1 point = 0.001 USD for XAUUSD (three decimals, matching Dukascopy and
// three-digit MT5 brokers). Integer prices make replay deterministic: a
// comparison against a stop or a barrier is exact, so the same build cannot
// disagree with itself, and two builds cannot disagree with each other.
// int32 spans +/- 2,147,483 USD at this scale — ample headroom for gold.
using Points = std::int32_t;

// Microseconds since the Unix epoch, always UTC.
//
// Broker server time (typically GMT+2/+3, and it shifts with US DST) is
// converted at the edges and never stored. See docs/PLAN.md section 4.
using TimeUs = std::int64_t;

inline constexpr std::uint32_t FORMAT_VERSION = 1;
inline constexpr std::int32_t  XAUUSD_POINT_NUM = 1;
inline constexpr std::int32_t  XAUUSD_POINT_DEN = 1000;   // 1 pt = 1/1000 USD

enum TickFlag : std::uint16_t {
    TF_BID      = 1u << 0,   // bid updated (MT5 TICK_FLAG_BID)
    TF_ASK      = 1u << 1,
    TF_LAST     = 1u << 2,
    TF_VOLUME   = 1u << 3,
    TF_SPREAD_SAT = 1u << 4, // spread exceeded uint16 range and was clamped
    TF_SYNTHETIC  = 1u << 5, // generated, not observed (synthetic test data)
};

struct Tick {
    TimeUs        ts_us;
    Points        bid_pts;
    std::uint16_t spread_pts;   // ask = bid + spread; clamped, see TF_SPREAD_SAT
    std::uint16_t flags;

    [[nodiscard]] constexpr Points ask_pts() const noexcept {
        return bid_pts + static_cast<Points>(spread_pts);
    }
    // Mid in half-points, to stay integral. Callers that need a real mid
    // divide by two themselves and accept the rounding explicitly.
    [[nodiscard]] constexpr std::int64_t mid_half_pts() const noexcept {
        return static_cast<std::int64_t>(bid_pts) * 2 + static_cast<std::int64_t>(spread_pts);
    }
};

// Natural alignment already yields exactly 16 bytes with no padding, so we get
// a packed layout without #pragma pack — and therefore without unaligned loads
// in the replay hot path. These asserts pin the ABI shared with Python.
static_assert(sizeof(Tick) == 16, "Tick must be 16 bytes");
static_assert(alignof(Tick) == 8, "Tick must be 8-byte aligned");
static_assert(std::is_trivially_copyable_v<Tick>);
static_assert(offsetof(Tick, ts_us)      == 0);
static_assert(offsetof(Tick, bid_pts)    == 8);
static_assert(offsetof(Tick, spread_pts) == 12);
static_assert(offsetof(Tick, flags)      == 14);

// 64-byte file header. Ticks begin immediately after it, so the first tick sits
// at offset 64 — 8-aligned, as Tick requires.
struct FileHeader {
    char          magic[8];      // "XAUTICK1", not NUL-terminated
    std::uint32_t version;
    std::uint32_t tick_size;     // sizeof(Tick), checked on open
    std::int32_t  point_num;     // 1 point = point_num / point_den USD
    std::int32_t  point_den;
    TimeUs        first_ts_us;
    TimeUs        last_ts_us;
    std::uint64_t count;
    char          symbol[16];    // NUL-padded
};

static_assert(sizeof(FileHeader) == 64, "FileHeader must be 64 bytes");
static_assert(offsetof(FileHeader, version)     == 8);
static_assert(offsetof(FileHeader, tick_size)   == 12);
static_assert(offsetof(FileHeader, point_num)   == 16);
static_assert(offsetof(FileHeader, point_den)   == 20);
static_assert(offsetof(FileHeader, first_ts_us) == 24);
static_assert(offsetof(FileHeader, last_ts_us)  == 32);
static_assert(offsetof(FileHeader, count)       == 40);
static_assert(offsetof(FileHeader, symbol)      == 48);

inline constexpr char        MAGIC[8]      = {'X','A','U','T','I','C','K','1'};
inline constexpr std::size_t HEADER_BYTES  = sizeof(FileHeader);

// Convenience for reporting and for the plausibility checks in verify().
inline constexpr double points_to_usd(Points p, std::int32_t num, std::int32_t den) noexcept {
    return static_cast<double>(p) * static_cast<double>(num) / static_cast<double>(den);
}

}  // namespace xau
