// UTC calendar arithmetic and gold's trading sessions.
//
// Everything here works in UTC. Broker server time is GMT+2/+3 and shifts with
// US DST; it is converted at the edges and never reaches this file. See
// docs/PLAN.md section 4 for why that boundary matters.
//
// The session boundaries below are the ones that matter for gold specifically:
// activity is dead in late Asia and violent at the London/NY overlap, and the
// spread-to-range ratio differs by a factor of several between them. A strategy
// that ignores which session it is trading is trading a different instrument at
// 03:00 than at 14:00.
#pragma once

#include "xau/types.hpp"

#include <cstdint>

namespace xau {

inline constexpr TimeUs kUsPerSecond = 1'000'000;
inline constexpr TimeUs kUsPerMinute = 60 * kUsPerSecond;
inline constexpr TimeUs kUsPerHour   = 60 * kUsPerMinute;
inline constexpr TimeUs kUsPerDay    = 24 * kUsPerHour;

// Floor division, so timestamps before 1970 stay aligned instead of truncating
// toward zero and landing a day out.
constexpr TimeUs floor_div(TimeUs a, TimeUs b) noexcept {
    TimeUs q = a / b;
    if (a < 0 && q * b != a) --q;
    return q;
}

constexpr TimeUs day_start(TimeUs us) noexcept { return floor_div(us, kUsPerDay) * kUsPerDay; }

constexpr int utc_hour(TimeUs us) noexcept {
    return static_cast<int>(floor_div(us - day_start(us), kUsPerHour));
}

// ISO weekday: 1 = Monday .. 7 = Sunday. 1970-01-01 was a Thursday.
constexpr int utc_weekday(TimeUs us) noexcept {
    long long w = (floor_div(us, kUsPerDay) + 3) % 7;   // 0 = Monday
    if (w < 0) w += 7;
    return static_cast<int>(w) + 1;
}

enum class Session : std::uint8_t { Asia, London, Overlap, NewYork, Rollover };

constexpr Session session_at(TimeUs us) noexcept {
    const int h = utc_hour(us);
    if (h >= 7 && h < 12) return Session::London;
    if (h >= 12 && h < 17) return Session::Overlap;   // London/NY, the busiest
    if (h >= 17 && h < 21) return Session::NewYork;
    if (h >= 21 && h < 23) return Session::Rollover;  // thin, spreads blow out
    return Session::Asia;                             // 23:00-07:00
}

constexpr const char* session_name(Session s) noexcept {
    switch (s) {
        case Session::Asia:     return "asia";
        case Session::London:   return "london";
        case Session::Overlap:  return "overlap";
        case Session::NewYork:  return "newyork";
        case Session::Rollover: return "rollover";
    }
    return "?";
}

// Half-open [from_hour, to_hour) in UTC. Wraps past midnight when from > to,
// which the Asia session needs.
constexpr bool in_hours(TimeUs us, int from_hour, int to_hour) noexcept {
    const int h = utc_hour(us);
    return (from_hour <= to_hour) ? (h >= from_hour && h < to_hour)
                                  : (h >= from_hour || h < to_hour);
}

// Identifies the *session* a timestamp belongs to for a window that starts at
// `start_hour` and may wrap midnight. Asia opening 23:00 Monday and continuing
// to 07:00 Tuesday is one session, and both halves return the same key.
constexpr TimeUs session_day(TimeUs us, int start_hour) noexcept {
    return day_start(us - static_cast<TimeUs>(start_hour) * kUsPerHour);
}

// Gold trades Sunday 22:00 UTC through Friday 21:00 UTC.
constexpr bool market_open(TimeUs us) noexcept {
    const int wd = utc_weekday(us);
    const int h = utc_hour(us);
    if (wd == 6) return false;              // Saturday
    if (wd == 7) return h >= 22;            // Sunday evening open
    if (wd == 5) return h < 21;             // Friday close
    return true;
}

}  // namespace xau
