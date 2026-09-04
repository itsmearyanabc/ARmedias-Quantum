// Memory-mapped tick storage.
//
// A TickStore is a directory of per-month files for one symbol, named
// <SYMBOL>-YYYY-MM.bin. Files are mapped eagerly at open, which costs almost
// nothing: mmap is lazy at the OS level, so pages fault in only when a backtest
// actually touches that month. That keeps the API const-correct and the hot
// path a flat walk over contiguous memory.
#pragma once

#include "xau/mapped_file.hpp"
#include "xau/types.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xau {

// Result of walking every tick and checking it. This is what the Phase 0 gate
// reports against: "10 years of ticks, gap- and outlier-checked".
struct VerifyReport {
    std::uint64_t ticks            = 0;
    std::uint64_t non_monotonic    = 0;   // ts went backwards — corrupt
    std::uint64_t out_of_range     = 0;   // price outside plausible bounds
    std::uint64_t zero_spread      = 0;   // suspicious, not fatal
    std::uint64_t saturated_spread = 0;   // TF_SPREAD_SAT set by the writer
    std::uint64_t gaps             = 0;   // quiet stretches over the threshold
    TimeUs        largest_gap_us   = 0;
    TimeUs        largest_gap_at   = 0;   // ts_us at which it started
    Points        min_bid          = 0;
    Points        max_bid          = 0;

    [[nodiscard]] bool ok() const noexcept {
        return non_monotonic == 0 && out_of_range == 0;
    }
};

class TickFile {
public:
    TickFile() = default;
    // Throws std::runtime_error if the file is missing, truncated, or not in
    // this format at this version.
    explicit TickFile(const std::filesystem::path& path);

    TickFile(TickFile&&) noexcept = default;
    TickFile& operator=(TickFile&&) noexcept = default;

    [[nodiscard]] const FileHeader& header() const noexcept { return hdr_; }
    [[nodiscard]] std::span<const Tick> ticks() const noexcept { return ticks_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    // Half-open [from_us, to_us). Empty span if the file does not overlap.
    [[nodiscard]] std::span<const Tick> range(TimeUs from_us, TimeUs to_us) const noexcept;

private:
    MappedFile            map_;
    FileHeader            hdr_{};
    std::span<const Tick> ticks_;
    std::filesystem::path path_;
};

class TickStore {
public:
    // Opens every <symbol>-YYYY-MM.bin in dir, sorted by time.
    // Throws if the directory is missing or any file fails validation — a
    // partially-readable store is a bug we want to hear about immediately, not
    // a degraded mode we silently backtest on.
    static TickStore open(const std::filesystem::path& dir, std::string_view symbol);

    // Move-only, and the copy has to be deleted *explicitly*.
    //
    // std::vector<T> advertises itself as copy-constructible whether or not T
    // is; the copy only fails when it is instantiated. So a TickStore holding
    // vector<TickFile> satisfies std::is_copy_constructible even though TickFile
    // is move-only, and any generic code that trusts that trait — pybind11's
    // class registration, for one — emits copy machinery that then refuses to
    // compile. Saying it here makes the trait tell the truth.
    TickStore() = default;
    TickStore(const TickStore&) = delete;
    TickStore& operator=(const TickStore&) = delete;
    TickStore(TickStore&&) noexcept = default;
    TickStore& operator=(TickStore&&) noexcept = default;

    [[nodiscard]] std::size_t   file_count()  const noexcept { return files_.size(); }
    [[nodiscard]] std::uint64_t total_ticks() const noexcept { return total_ticks_; }
    [[nodiscard]] TimeUs        first_ts()    const noexcept { return first_ts_; }
    [[nodiscard]] TimeUs        last_ts()     const noexcept { return last_ts_; }
    [[nodiscard]] const std::string& symbol() const noexcept { return symbol_; }
    [[nodiscard]] const std::vector<TickFile>& files() const noexcept { return files_; }

    // Visit the ticks in [from_us, to_us) as contiguous spans, in order.
    // The callback returns false to stop early.
    //
    // Chunked rather than per-tick on purpose: the inner loop stays a flat walk
    // over an array with no indirection, which is what gets us to the >= 20M
    // ticks/s the Phase 0 gate demands.
    template <class F>
    void for_each_chunk(TimeUs from_us, TimeUs to_us, F&& fn) const {
        for (const TickFile& f : files_) {
            const FileHeader& h = f.header();
            if (h.count == 0) continue;
            if (h.last_ts_us < from_us) continue;
            if (h.first_ts_us >= to_us) break;      // files_ is time-sorted
            const std::span<const Tick> s = f.range(from_us, to_us);
            if (!s.empty() && !fn(s)) return;
        }
    }

    // Walk everything, checking monotonicity, plausibility and gaps.
    // gap_threshold_us: report quiet stretches longer than this. Weekends are
    // expected, so a sane weekday threshold is a few minutes.
    [[nodiscard]] VerifyReport verify(TimeUs gap_threshold_us,
                                      Points min_plausible_pts,
                                      Points max_plausible_pts) const;

private:
    std::vector<TickFile> files_;
    std::string           symbol_;
    std::uint64_t         total_ticks_ = 0;
    TimeUs                first_ts_    = 0;
    TimeUs                last_ts_     = 0;
};

// Parses "<SYMBOL>-YYYY-MM.bin". Returns false if the name does not match or
// the symbol differs. Exposed for testing.
bool parse_tick_filename(std::string_view filename, std::string_view symbol,
                         int& year, int& month);

}  // namespace xau
