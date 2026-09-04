// Shared formatting for VerifyReport, used by the bench and (later) by the
// ingest CLI so both print the gate evidence in the same shape.
#pragma once

#include "xau/tick_store.hpp"

#include <cstdio>

namespace xau {

inline void print_verify(const VerifyReport& r, std::int32_t point_num,
                         std::int32_t point_den) {
    std::printf("ticks            %llu\n", static_cast<unsigned long long>(r.ticks));
    std::printf("non-monotonic    %llu%s\n",
                static_cast<unsigned long long>(r.non_monotonic),
                r.non_monotonic ? "   <-- CORRUPT" : "");
    std::printf("out of range     %llu%s\n", static_cast<unsigned long long>(r.out_of_range),
                r.out_of_range ? "   <-- check the decode scale" : "");
    std::printf("zero spread      %llu\n", static_cast<unsigned long long>(r.zero_spread));
    std::printf("clamped spread   %llu\n",
                static_cast<unsigned long long>(r.saturated_spread));
    std::printf("gaps over thresh %llu  (largest %.1f s)\n",
                static_cast<unsigned long long>(r.gaps),
                static_cast<double>(r.largest_gap_us) / 1e6);
    std::printf("bid range        %.3f .. %.3f USD\n", points_to_usd(r.min_bid, point_num, point_den),
                points_to_usd(r.max_bid, point_num, point_den));
    std::printf("verdict          %s\n", r.ok() ? "OK" : "FAILED");
}

}  // namespace xau
