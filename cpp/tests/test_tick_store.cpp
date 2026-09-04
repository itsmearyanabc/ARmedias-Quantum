#include "harness.hpp"

#include "xau/tick_store.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace xau;

namespace {

// A scratch directory that removes itself. Tests must not leave litter in the
// user's temp folder, and must not collide when run in parallel.
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        path_ = fs::temp_directory_path() /
                ("xau_test_" + std::to_string(rd()) + "_" + std::to_string(rd()));
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

struct WriteOpts {
    bool          corrupt_magic   = false;
    bool          truncate_a_tick = false;   // chop the last tick, leave count alone
    bool          bad_version     = false;
    std::uint64_t override_count  = 0;       // 0 = use the real count
};

void write_tick_file(const fs::path& path, std::string_view symbol,
                     const std::vector<Tick>& ticks, WriteOpts opts = {}) {
    FileHeader h{};
    std::memcpy(h.magic, MAGIC, sizeof(MAGIC));
    if (opts.corrupt_magic) h.magic[0] = 'Z';
    h.version     = opts.bad_version ? FORMAT_VERSION + 1 : FORMAT_VERSION;
    h.tick_size   = static_cast<std::uint32_t>(sizeof(Tick));
    h.point_num   = XAUUSD_POINT_NUM;
    h.point_den   = XAUUSD_POINT_DEN;
    h.first_ts_us = ticks.empty() ? 0 : ticks.front().ts_us;
    h.last_ts_us  = ticks.empty() ? 0 : ticks.back().ts_us;
    h.count       = opts.override_count ? opts.override_count : ticks.size();
    std::memset(h.symbol, 0, sizeof(h.symbol));
    std::memcpy(h.symbol, symbol.data(),
                std::min(symbol.size(), sizeof(h.symbol)));

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(&h), sizeof(h));

    std::size_t n = ticks.size();
    if (opts.truncate_a_tick && n > 0) --n;
    if (n > 0) {
        out.write(reinterpret_cast<const char*>(ticks.data()),
                  static_cast<std::streamsize>(n * sizeof(Tick)));
    }
}

// Ticks one second apart starting at ts0, price walking upward by 1 point.
std::vector<Tick> make_ticks(TimeUs ts0, int count, Points bid0 = 2'650'000) {
    std::vector<Tick> v;
    v.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        Tick t{};
        t.ts_us      = ts0 + static_cast<TimeUs>(i) * 1'000'000;
        t.bid_pts    = bid0 + i;
        t.spread_pts = 250;                       // $0.25
        t.flags      = TF_BID | TF_ASK | TF_SYNTHETIC;
        v.push_back(t);
    }
    return v;
}

constexpr TimeUs kJan2020 = 1'577'836'800'000'000LL;   // 2020-01-01T00:00:00Z
constexpr TimeUs kFeb2020 = 1'580'515'200'000'000LL;   // 2020-02-01T00:00:00Z

}  // namespace

// ---------------------------------------------------------------------------

XAU_TEST(tick_layout_is_pinned) {
    // The Python writer depends on exactly this. If these change, bump
    // FORMAT_VERSION and update python/xau_ingest/tickfmt.py in the same commit.
    CHECK_EQ(sizeof(Tick), std::size_t{16});
    CHECK_EQ(sizeof(FileHeader), std::size_t{64});
    CHECK_EQ(alignof(Tick), std::size_t{8});

    Tick t{};
    t.bid_pts    = 2'650'000;
    t.spread_pts = 300;
    CHECK_EQ(t.ask_pts(), Points{2'650'300});
    CHECK_EQ(t.mid_half_pts(), std::int64_t{5'300'300});
}

XAU_TEST(filename_parsing) {
    int y = 0, m = 0;

    CHECK(parse_tick_filename("XAUUSD-2020-01.bin", "XAUUSD", y, m));
    CHECK_EQ(y, 2020);
    CHECK_EQ(m, 1);

    CHECK(parse_tick_filename("XAUUSD-1999-12.bin", "XAUUSD", y, m));
    CHECK_EQ(y, 1999);
    CHECK_EQ(m, 12);

    CHECK(!parse_tick_filename("XAGUSD-2020-01.bin", "XAUUSD", y, m));  // other symbol
    CHECK(!parse_tick_filename("XAUUSD-2020-13.bin", "XAUUSD", y, m));  // month 13
    CHECK(!parse_tick_filename("XAUUSD-2020-00.bin", "XAUUSD", y, m));  // month 0
    CHECK(!parse_tick_filename("XAUUSD-2020-1.bin", "XAUUSD", y, m));   // unpadded
    CHECK(!parse_tick_filename("XAUUSD-2020-01.csv", "XAUUSD", y, m));  // wrong ext
    CHECK(!parse_tick_filename("XAUUSD-20aa-01.bin", "XAUUSD", y, m));  // non-digit
    CHECK(!parse_tick_filename("XAUUSD.bin", "XAUUSD", y, m));          // no date
    CHECK(!parse_tick_filename("", "XAUUSD", y, m));

    // A longer symbol whose prefix matches must not be accepted.
    CHECK(!parse_tick_filename("XAUUSDm-2020-01.bin", "XAUUSD", y, m));
}

XAU_TEST(roundtrip_single_file) {
    TempDir dir;
    const auto ticks = make_ticks(kJan2020, 1000);
    const auto p     = dir.path() / "XAUUSD-2020-01.bin";
    write_tick_file(p, "XAUUSD", ticks);

    TickFile f(p);
    CHECK_EQ(f.header().count, std::uint64_t{1000});
    CHECK_EQ(f.header().point_den, XAUUSD_POINT_DEN);
    CHECK_EQ(f.ticks().size(), std::size_t{1000});
    CHECK_EQ(f.ticks().front().ts_us, kJan2020);
    CHECK_EQ(f.ticks().front().bid_pts, Points{2'650'000});
    CHECK_EQ(f.ticks().back().bid_pts, Points{2'650'999});
    CHECK_EQ(std::string(f.header().symbol), std::string("XAUUSD"));
}

XAU_TEST(range_queries) {
    TempDir dir;
    const auto ticks = make_ticks(kJan2020, 1000);
    const auto p     = dir.path() / "XAUUSD-2020-01.bin";
    write_tick_file(p, "XAUUSD", ticks);
    TickFile f(p);

    // Half-open: [t0, t0+10s) is exactly ten ticks.
    CHECK_EQ(f.range(kJan2020, kJan2020 + 10'000'000).size(), std::size_t{10});

    // Entirely before / entirely after the file.
    CHECK(f.range(0, kJan2020).empty());
    CHECK(f.range(kJan2020 + 10'000'000'000LL, kJan2020 + 20'000'000'000LL).empty());

    // Inverted and degenerate ranges are empty, not undefined.
    CHECK(f.range(kJan2020 + 5'000'000, kJan2020).empty());
    CHECK(f.range(kJan2020, kJan2020).empty());

    // A range starting mid-file lands on the right tick.
    const auto mid = f.range(kJan2020 + 500'000'000, kJan2020 + 502'000'000);
    CHECK_EQ(mid.size(), std::size_t{2});
    if (mid.size() == 2) CHECK_EQ(mid.front().bid_pts, Points{2'650'500});

    // Fully covering range returns everything.
    CHECK_EQ(f.range(0, kJan2020 + 999'999'999'999LL).size(), std::size_t{1000});
}

XAU_TEST(rejects_corrupt_files) {
    TempDir dir;
    const auto ticks = make_ticks(kJan2020, 100);

    const auto bad_magic = dir.path() / "XAUUSD-2020-01.bin";
    write_tick_file(bad_magic, "XAUUSD", ticks, WriteOpts{.corrupt_magic = true});
    CHECK_THROWS(TickFile{bad_magic});

    const auto bad_ver = dir.path() / "XAUUSD-2020-02.bin";
    write_tick_file(bad_ver, "XAUUSD", ticks, WriteOpts{.bad_version = true});
    CHECK_THROWS(TickFile{bad_ver});

    // Header claims more ticks than the payload holds — the classic symptom of
    // an ingest that died mid-write. Must be loud, not silently short.
    const auto truncated = dir.path() / "XAUUSD-2020-03.bin";
    write_tick_file(truncated, "XAUUSD", ticks, WriteOpts{.truncate_a_tick = true});
    CHECK_THROWS(TickFile{truncated});

    const auto missing = dir.path() / "XAUUSD-2020-04.bin";
    CHECK_THROWS(TickFile{missing});
}

XAU_TEST(store_spans_months_in_order) {
    TempDir dir;
    write_tick_file(dir.path() / "XAUUSD-2020-01.bin", "XAUUSD", make_ticks(kJan2020, 500));
    write_tick_file(dir.path() / "XAUUSD-2020-02.bin", "XAUUSD",
                    make_ticks(kFeb2020, 300, 2'700'000));
    // An unrelated symbol in the same directory must be ignored.
    write_tick_file(dir.path() / "XAGUSD-2020-01.bin", "XAGUSD", make_ticks(kJan2020, 9));

    const TickStore s = TickStore::open(dir.path(), "XAUUSD");
    CHECK_EQ(s.file_count(), std::size_t{2});
    CHECK_EQ(s.total_ticks(), std::uint64_t{800});
    CHECK_EQ(s.first_ts(), kJan2020);

    // Chunks arrive in time order and cover every tick exactly once.
    std::uint64_t seen = 0;
    TimeUs        prev = 0;
    bool          ordered = true;
    s.for_each_chunk(0, kFeb2020 * 2, [&](std::span<const Tick> c) {
        for (const Tick& t : c) {
            if (t.ts_us < prev) ordered = false;
            prev = t.ts_us;
            ++seen;
        }
        return true;
    });
    CHECK_EQ(seen, std::uint64_t{800});
    CHECK(ordered);

    // Early exit really stops the walk.
    std::uint64_t chunks = 0;
    s.for_each_chunk(0, kFeb2020 * 2, [&](std::span<const Tick>) {
        ++chunks;
        return false;
    });
    CHECK_EQ(chunks, std::uint64_t{1});

    // A window inside January alone must not touch February.
    std::uint64_t jan_only = 0;
    s.for_each_chunk(kJan2020, kJan2020 + 10'000'000, [&](std::span<const Tick> c) {
        jan_only += c.size();
        return true;
    });
    CHECK_EQ(jan_only, std::uint64_t{10});
}

XAU_TEST(store_drops_empty_months) {
    TempDir dir;
    write_tick_file(dir.path() / "XAUUSD-2020-01.bin", "XAUUSD", make_ticks(kJan2020, 50));
    write_tick_file(dir.path() / "XAUUSD-2020-02.bin", "XAUUSD", {});  // holiday month

    const TickStore s = TickStore::open(dir.path(), "XAUUSD");
    CHECK_EQ(s.file_count(), std::size_t{1});
    CHECK_EQ(s.total_ticks(), std::uint64_t{50});
}

XAU_TEST(store_rejects_missing_dir_and_empty_dir) {
    TempDir dir;
    CHECK_THROWS((void)TickStore::open(dir.path() / "nope", "XAUUSD"));
    CHECK_THROWS((void)TickStore::open(dir.path(), "XAUUSD"));  // no matching files
}

XAU_TEST(verify_reports_clean_data) {
    TempDir dir;
    write_tick_file(dir.path() / "XAUUSD-2020-01.bin", "XAUUSD", make_ticks(kJan2020, 1000));
    const TickStore s = TickStore::open(dir.path(), "XAUUSD");

    // Plausible band: $200 to $20,000 in points.
    const VerifyReport r = s.verify(/*gap*/ 60'000'000, 200'000, 20'000'000);
    CHECK(r.ok());
    CHECK_EQ(r.ticks, std::uint64_t{1000});
    CHECK_EQ(r.non_monotonic, std::uint64_t{0});
    CHECK_EQ(r.out_of_range, std::uint64_t{0});
    CHECK_EQ(r.gaps, std::uint64_t{0});      // 1s spacing, 60s threshold
    CHECK_EQ(r.min_bid, Points{2'650'000});
    CHECK_EQ(r.max_bid, Points{2'650'999});
}

XAU_TEST(verify_catches_outliers_and_gaps) {
    TempDir dir;
    auto ticks = make_ticks(kJan2020, 100);
    ticks[50].bid_pts = 99;                        // absurd price — decode scale bug
    ticks[70].spread_pts = 0;                      // suspicious
    for (std::size_t i = 80; i < ticks.size(); ++i) {
        ticks[i].ts_us += 300'000'000;             // a 5-minute hole
    }
    write_tick_file(dir.path() / "XAUUSD-2020-01.bin", "XAUUSD", ticks);

    const TickStore s = TickStore::open(dir.path(), "XAUUSD");
    const VerifyReport r = s.verify(60'000'000, 200'000, 20'000'000);

    CHECK(!r.ok());                                // out_of_range makes it not ok
    CHECK_EQ(r.out_of_range, std::uint64_t{1});
    CHECK_EQ(r.zero_spread, std::uint64_t{1});
    CHECK_EQ(r.gaps, std::uint64_t{1});
    CHECK(r.largest_gap_us >= 300'000'000);
    CHECK_EQ(r.non_monotonic, std::uint64_t{0});   // shifting forward stays ordered
}
