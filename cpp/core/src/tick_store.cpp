#include "xau/tick_store.hpp"

#include <cstring>
#include <sstream>
#include <stdexcept>

namespace xau {
namespace {

[[noreturn]] void bad(const std::filesystem::path& p, const std::string& why) {
    throw std::runtime_error("TickFile " + p.string() + ": " + why);
}

bool all_digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(),
                                     [](char c) { return c >= '0' && c <= '9'; });
}

int to_int(std::string_view s) {
    int v = 0;
    for (char c : s) v = v * 10 + (c - '0');
    return v;
}

}  // namespace

bool parse_tick_filename(std::string_view filename, std::string_view symbol,
                         int& year, int& month) {
    // <SYMBOL>-YYYY-MM.bin
    constexpr std::string_view kExt = ".bin";
    if (filename.size() < symbol.size() + 1 + 4 + 1 + 2 + kExt.size()) return false;
    if (filename.substr(filename.size() - kExt.size()) != kExt) return false;
    if (filename.substr(0, symbol.size()) != symbol) return false;
    if (filename[symbol.size()] != '-') return false;

    const std::string_view rest =
        filename.substr(symbol.size() + 1, filename.size() - symbol.size() - 1 - kExt.size());
    if (rest.size() != 7 || rest[4] != '-') return false;

    const std::string_view y = rest.substr(0, 4);
    const std::string_view m = rest.substr(5, 2);
    if (!all_digits(y) || !all_digits(m)) return false;

    year  = to_int(y);
    month = to_int(m);
    return month >= 1 && month <= 12 && year >= 1970 && year <= 2999;
}

// ---------------------------------------------------------------------------
// TickFile
// ---------------------------------------------------------------------------

TickFile::TickFile(const std::filesystem::path& path) : map_(path), path_(path) {
    if (map_.size() < HEADER_BYTES) bad(path, "shorter than a header");

    std::memcpy(&hdr_, map_.data(), sizeof(FileHeader));

    if (std::memcmp(hdr_.magic, MAGIC, sizeof(MAGIC)) != 0) bad(path, "bad magic");
    if (hdr_.version != FORMAT_VERSION) {
        bad(path, "format version " + std::to_string(hdr_.version) + ", expected " +
                      std::to_string(FORMAT_VERSION));
    }
    if (hdr_.tick_size != sizeof(Tick)) {
        bad(path, "tick_size " + std::to_string(hdr_.tick_size) + ", expected " +
                      std::to_string(sizeof(Tick)));
    }
    if (hdr_.point_den == 0) bad(path, "point_den is zero");

    const std::size_t payload = map_.size() - HEADER_BYTES;
    if (payload % sizeof(Tick) != 0) bad(path, "payload is not a whole number of ticks");

    const std::uint64_t n = static_cast<std::uint64_t>(payload / sizeof(Tick));
    if (n != hdr_.count) {
        bad(path, "header says " + std::to_string(hdr_.count) + " ticks, file holds " +
                      std::to_string(n));
    }

    if (n > 0) {
        // Safe: the header is 64 bytes and Tick is 8-aligned, so this address
        // is correctly aligned for Tick.
        const auto* first = reinterpret_cast<const Tick*>(map_.data() + HEADER_BYTES);
        ticks_ = std::span<const Tick>(first, static_cast<std::size_t>(n));

        if (ticks_.front().ts_us != hdr_.first_ts_us) bad(path, "first_ts_us disagrees with data");
        if (ticks_.back().ts_us != hdr_.last_ts_us)   bad(path, "last_ts_us disagrees with data");
    }

    map_.advise_sequential();
}

std::span<const Tick> TickFile::range(TimeUs from_us, TimeUs to_us) const noexcept {
    if (ticks_.empty() || from_us >= to_us) return {};
    if (hdr_.last_ts_us < from_us || hdr_.first_ts_us >= to_us) return {};

    const auto cmp = [](const Tick& t, TimeUs v) { return t.ts_us < v; };
    const auto begin = ticks_.begin();
    const auto lo = std::lower_bound(begin, ticks_.end(), from_us, cmp);
    const auto hi = std::lower_bound(lo, ticks_.end(), to_us, cmp);

    return ticks_.subspan(static_cast<std::size_t>(lo - begin),
                          static_cast<std::size_t>(hi - lo));
}

// ---------------------------------------------------------------------------
// TickStore
// ---------------------------------------------------------------------------

TickStore TickStore::open(const std::filesystem::path& dir, std::string_view symbol) {
    namespace fs = std::filesystem;
    if (!fs::is_directory(dir)) {
        throw std::runtime_error("TickStore: not a directory: " + dir.string());
    }

    TickStore store;
    store.symbol_.assign(symbol);

    std::vector<fs::path> paths;
    for (const fs::directory_entry& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        int y = 0, m = 0;
        if (parse_tick_filename(e.path().filename().string(), symbol, y, m)) {
            paths.push_back(e.path());
        }
    }
    if (paths.empty()) {
        throw std::runtime_error("TickStore: no " + std::string(symbol) +
                                 "-YYYY-MM.bin files in " + dir.string());
    }
    // Filenames are zero-padded, so lexicographic order is chronological order.
    std::sort(paths.begin(), paths.end());

    store.files_.reserve(paths.size());
    for (const fs::path& p : paths) store.files_.emplace_back(p);

    // Drop empty months so callers never have to special-case them, then
    // confirm the remainder really is ordered and non-overlapping.
    std::erase_if(store.files_, [](const TickFile& f) { return f.header().count == 0; });

    for (std::size_t i = 0; i < store.files_.size(); ++i) {
        const FileHeader& h = store.files_[i].header();
        store.total_ticks_ += h.count;
        if (i > 0) {
            const FileHeader& prev = store.files_[i - 1].header();
            if (h.first_ts_us < prev.last_ts_us) {
                std::ostringstream os;
                os << "TickStore: files overlap in time: "
                   << store.files_[i - 1].path().filename().string() << " ends at "
                   << prev.last_ts_us << " but "
                   << store.files_[i].path().filename().string() << " starts at "
                   << h.first_ts_us;
                throw std::runtime_error(os.str());
            }
        }
    }

    if (!store.files_.empty()) {
        store.first_ts_ = store.files_.front().header().first_ts_us;
        store.last_ts_  = store.files_.back().header().last_ts_us;
    }
    return store;
}

VerifyReport TickStore::verify(TimeUs gap_threshold_us,
                               Points min_plausible_pts,
                               Points max_plausible_pts) const {
    VerifyReport r;
    bool   first = true;
    TimeUs prev_ts = 0;

    for (const TickFile& f : files_) {
        for (const Tick& t : f.ticks()) {
            if (first) {
                r.min_bid = t.bid_pts;
                r.max_bid = t.bid_pts;
                first = false;
            } else {
                if (t.ts_us < prev_ts) {
                    ++r.non_monotonic;
                } else {
                    const TimeUs gap = t.ts_us - prev_ts;
                    if (gap > gap_threshold_us) {
                        ++r.gaps;
                        if (gap > r.largest_gap_us) {
                            r.largest_gap_us = gap;
                            r.largest_gap_at = prev_ts;
                        }
                    }
                }
                if (t.bid_pts < r.min_bid) r.min_bid = t.bid_pts;
                if (t.bid_pts > r.max_bid) r.max_bid = t.bid_pts;
            }

            if (t.bid_pts < min_plausible_pts || t.bid_pts > max_plausible_pts) {
                ++r.out_of_range;
            }
            if (t.spread_pts == 0) ++r.zero_spread;
            if (t.flags & TF_SPREAD_SAT) ++r.saturated_spread;

            prev_ts = t.ts_us;
            ++r.ticks;
        }
    }
    return r;
}

}  // namespace xau
