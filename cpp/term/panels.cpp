#include "app.hpp"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <exception>
#include <vector>

namespace xauterm {

extern const ImVec4 kBull;
extern const ImVec4 kBear;
extern const ImVec4 kAccent;
extern const ImVec4 kMuted;

namespace {

constexpr double kUsPerSec = 1'000'000.0;

// ImPlot's time axis works in Unix seconds; the store works in microseconds.
// The conversion lives here and nowhere else.
constexpr double to_sec(TimeUs us) { return static_cast<double>(us) / kUsPerSec; }
constexpr TimeUs to_us(double sec) { return static_cast<TimeUs>(sec * kUsPerSec); }

ImU32 col32(const ImVec4& c, float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
}

void text_muted(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

// Label above, value below — the shape a status strip wants.
void stat(const char* label, const char* value, const ImVec4* value_col = nullptr) {
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (value_col) ImGui::PushStyleColor(ImGuiCol_Text, *value_col);
    ImGui::TextUnformatted(value);
    if (value_col) ImGui::PopStyleColor();
    ImGui::EndGroup();
}

int utc_hour(TimeUs us) {
    const std::time_t t = static_cast<std::time_t>(us / 1'000'000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm.tm_hour;
}

const char* session_name(int hour_utc) {
    if (hour_utc >= 7 && hour_utc < 12) return "London";
    if (hour_utc >= 12 && hour_utc < 17) return "London/NY overlap";
    if (hour_utc >= 17 && hour_utc < 21) return "New York";
    if (hour_utc >= 21 && hour_utc < 23) return "Rollover";
    return "Asia";
}

}  // namespace

std::string fmt_time(TimeUs us) {
    const std::time_t t = static_cast<std::time_t>(us / 1'000'000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

std::string fmt_duration(TimeUs us) {
    char buf[48];
    const double s = static_cast<double>(us) / 1e6;
    if (s < 90) std::snprintf(buf, sizeof(buf), "%.0f s", s);
    else if (s < 5400) std::snprintf(buf, sizeof(buf), "%.1f min", s / 60);
    else if (s < 172800) std::snprintf(buf, sizeof(buf), "%.1f h", s / 3600);
    else std::snprintf(buf, sizeof(buf), "%.1f days", s / 86400);
    return buf;
}

// ---------------------------------------------------------------------------
// data
// ---------------------------------------------------------------------------

void open_store(AppState& app) {
    app.store.reset();
    app.load_error.clear();
    app.bars = BarSeries{};
    app.x_initialised = false;
    try {
        app.store = TickStore::open(app.store_dir, app.symbol);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "opened %s: %zu months, %llu ticks",
                      app.store_dir.c_str(), app.store->file_count(),
                      static_cast<unsigned long long>(app.store->total_ticks()));
        app.log.info(buf);
        app.needs_rebuild = true;
    } catch (const std::exception& e) {
        app.load_error = e.what();
        app.log.error(std::string("open failed: ") + e.what());
    }
}

void rebuild_bars(AppState& app) {
    if (!app.store) return;
    const auto t0 = std::chrono::steady_clock::now();

    // Builds the whole span in one pass. At Phase 0 volumes (a few million
    // ticks) this is well under a second. For a decade of real ticks it will
    // need an incremental builder that appends rather than rebuilds — that
    // belongs with the engine in Phase 1, not here.
    app.bars = BarSeries::build(*app.store, app.tf, app.store->first_ts(),
                                app.store->last_ts() + 1);

    app.build_ms = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
    app.needs_rebuild = false;

    char buf[192];
    std::snprintf(buf, sizeof(buf), "built %zu %s bars in %.0f ms", app.bars.size(),
                  timeframe_name(app.tf), app.build_ms);
    app.log.info(buf);

    if (!app.bars.empty() && !app.x_initialised) {
        // Open on the most recent ~200 bars, the way a chart should.
        const auto all = app.bars.all_for_display();
        const std::size_t n = std::min<std::size_t>(200, all.size());
        app.x_max = to_sec(all.back().close_time_us(app.tf));
        app.x_min = to_sec(all[all.size() - n].open_time_us);
        app.x_initialised = true;
    }
}

// ---------------------------------------------------------------------------
// chart
// ---------------------------------------------------------------------------

namespace {

struct Visible {
    std::size_t first = 0;
    std::size_t last = 0;  // exclusive
    [[nodiscard]] std::size_t count() const { return last - first; }
};

Visible visible_range(std::span<const Bar> bars, double x_min, double x_max) {
    const TimeUs lo = to_us(x_min);
    const TimeUs hi = to_us(x_max);
    const auto cmp = [](const Bar& b, TimeUs v) { return b.open_time_us < v; };

    auto a = std::lower_bound(bars.begin(), bars.end(), lo, cmp);
    auto b = std::lower_bound(bars.begin(), bars.end(), hi, cmp);
    Visible v;
    // One bar of slack each side so partially-visible candles still draw.
    v.first = static_cast<std::size_t>(a - bars.begin());
    if (v.first > 0) --v.first;
    v.last = std::min(bars.size(), static_cast<std::size_t>(b - bars.begin()) + 1);
    if (v.last < v.first) v.last = v.first;
    return v;
}

void draw_session_bands(double x_min, double x_max) {
    // Only when zoomed in enough to be readable; a year of bands is a smear.
    if (x_max - x_min > 8 * 86400.0) return;

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const ImPlotRect lim = ImPlot::GetPlotLimits();

    const TimeUs day = 86400LL * 1'000'000;
    TimeUs t = (to_us(x_min) / day) * day;
    for (; to_sec(t) < x_max; t += day) {
        struct Band { int from, to; ImVec4 col; };
        const Band bands[] = {
            {7, 12, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.045f)},
            {12, 17, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.085f)},
        };
        for (const Band& b : bands) {
            const double a = to_sec(t) + b.from * 3600.0;
            const double z = to_sec(t) + b.to * 3600.0;
            if (z < x_min || a > x_max) continue;
            const ImVec2 p0 = ImPlot::PlotToPixels(std::max(a, lim.X.Min), lim.Y.Max);
            const ImVec2 p1 = ImPlot::PlotToPixels(std::min(z, lim.X.Max), lim.Y.Min);
            dl->AddRectFilled(p0, p1, col32(b.col));
        }
    }
}

void draw_candles(const AppState& app, std::span<const Bar> bars, const Visible& vis) {
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const double slot = static_cast<double>(timeframe_us(app.tf)) / kUsPerSec;
    const double half = slot * 0.34;  // 68% of the slot, leaving a gap between candles

    // Sub-pixel candles are noise. Real terminals switch to a line here, so do we.
    const ImVec2 a = ImPlot::PlotToPixels(0.0, 0.0);
    const ImVec2 b = ImPlot::PlotToPixels(slot, 0.0);
    const bool too_dense = std::fabs(b.x - a.x) < 3.0f;

    ImPlot::PushPlotClipRect();

    if (too_dense) {
        ImVec2 prev{};
        bool have_prev = false;
        for (std::size_t i = vis.first; i < vis.last; ++i) {
            const Bar& bar = bars[i];
            const ImVec2 p = ImPlot::PlotToPixels(
                to_sec(bar.open_time_us) + slot * 0.5, app.to_usd(bar.close));
            if (have_prev) dl->AddLine(prev, p, col32(kAccent, 0.9f), 1.2f);
            prev = p;
            have_prev = true;
        }
    } else {
        for (std::size_t i = vis.first; i < vis.last; ++i) {
            const Bar& bar = bars[i];
            const double x = to_sec(bar.open_time_us) + slot * 0.5;
            const bool up = bar.close >= bar.open;
            const ImU32 c = col32(up ? kBull : kBear);

            const ImVec2 wick_hi = ImPlot::PlotToPixels(x, app.to_usd(bar.high));
            const ImVec2 wick_lo = ImPlot::PlotToPixels(x, app.to_usd(bar.low));
            dl->AddLine(ImVec2(wick_hi.x, wick_hi.y), ImVec2(wick_lo.x, wick_lo.y), c, 1.0f);

            const ImVec2 o = ImPlot::PlotToPixels(x - half, app.to_usd(bar.open));
            const ImVec2 cl = ImPlot::PlotToPixels(x + half, app.to_usd(bar.close));
            const float y0 = std::min(o.y, cl.y);
            const float y1 = std::max(o.y, cl.y);
            if (y1 - y0 < 1.0f) {
                dl->AddLine(ImVec2(o.x, y0), ImVec2(cl.x, y0), c, 1.2f);  // doji
            } else {
                dl->AddRectFilled(ImVec2(o.x, y0), ImVec2(cl.x, y1), c);
            }
        }
    }

    ImPlot::PopPlotClipRect();
}

// OHLC readout for the bar under the cursor.
void draw_hover_readout(const AppState& app, std::span<const Bar> bars, const Visible& vis) {
    if (!ImPlot::IsPlotHovered() || vis.count() == 0) return;

    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
    const TimeUs at = to_us(mouse.x);
    const auto cmp = [](const Bar& b, TimeUs v) { return b.open_time_us < v; };
    auto it = std::lower_bound(bars.begin() + static_cast<std::ptrdiff_t>(vis.first),
                               bars.begin() + static_cast<std::ptrdiff_t>(vis.last), at, cmp);
    if (it != bars.begin()) --it;
    if (it == bars.end()) return;

    const Bar& b = *it;
    const bool up = b.close >= b.open;
    ImGui::BeginTooltip();
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(fmt_time(b.open_time_us).c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, up ? kBull : kBear);
    ImGui::Text("O %.3f   H %.3f", app.to_usd(b.open), app.to_usd(b.high));
    ImGui::Text("L %.3f   C %.3f", app.to_usd(b.low), app.to_usd(b.close));
    ImGui::PopStyleColor();
    ImGui::Separator();
    text_muted("range   %.3f USD", app.to_usd(b.range_pts()));
    text_muted("ticks   %u", b.ticks);
    text_muted("spread  %.3f avg / %.3f max", app.to_usd(static_cast<Points>(b.spread_mean_pts)),
               app.to_usd(static_cast<Points>(b.spread_max_pts)));
    text_muted("session %s", session_name(utc_hour(b.open_time_us)));
    ImGui::EndTooltip();
}

}  // namespace

void panel_chart(AppState& app) {
    if (!ImGui::Begin("Chart", &app.p_chart)) {
        ImGui::End();
        return;
    }

    if (!app.store) {
        ImGui::TextUnformatted("No tick store loaded.");
        ImGui::Spacing();
        if (!app.load_error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kBear);
            ImGui::TextWrapped("%s", app.load_error.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }
        text_muted("Generate synthetic ticks, then reopen:");
        ImGui::TextUnformatted("  cd python");
        ImGui::TextUnformatted("  python -m xau_ingest.synth --months 3 --out ../data/ticks/XAUUSD");
        ImGui::Spacing();
        if (ImGui::Button("Reopen store")) open_store(app);
        ImGui::End();
        return;
    }

    // ---- toolbar ---------------------------------------------------------
    for (int i = 0; i < static_cast<int>(Timeframe::COUNT); ++i) {
        const auto tf = static_cast<Timeframe>(i);
        if (i) ImGui::SameLine();
        const bool active = (tf == app.tf);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        if (active) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.09f, 0.10f, 1.0f));
        if (ImGui::Button(timeframe_name(tf))) {
            if (app.tf != tf) {
                app.tf = tf;
                app.needs_rebuild = true;
            }
        }
        if (active) ImGui::PopStyleColor(2);
    }
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(12, 0));
    ImGui::SameLine();
    ImGui::Checkbox("Sessions", &app.show_sessions);
    ImGui::SameLine();
    ImGui::Checkbox("Volume", &app.show_volume);
    ImGui::SameLine();
    ImGui::Checkbox("Spread", &app.show_spread);
    ImGui::SameLine();
    if (ImGui::Button("Fit all") && !app.bars.empty()) {
        const auto all = app.bars.all_for_display();
        app.x_min = to_sec(all.front().open_time_us);
        app.x_max = to_sec(all.back().close_time_us(app.tf));
    }
    ImGui::SameLine();
    text_muted("|  %zu bars, built in %.0f ms", app.bars.size(), app.build_ms);

    if (app.bars.empty()) {
        ImGui::TextUnformatted("No bars in range.");
        ImGui::End();
        return;
    }

    const std::span<const Bar> bars = app.bars.all_for_display();
    const Visible vis = visible_range(bars, app.x_min, app.x_max);

    // Auto-fit Y to what is on screen, the way a price chart should behave.
    double y_lo = 0.0, y_hi = 0.0;
    if (vis.count() > 0) {
        Points lo = bars[vis.first].low;
        Points hi = bars[vis.first].high;
        for (std::size_t i = vis.first; i < vis.last; ++i) {
            lo = std::min(lo, bars[i].low);
            hi = std::max(hi, bars[i].high);
        }
        y_lo = app.to_usd(lo);
        y_hi = app.to_usd(hi);
        const double pad = std::max((y_hi - y_lo) * 0.06, 0.05);
        y_lo -= pad;
        y_hi += pad;
    }

    const float avail = ImGui::GetContentRegionAvail().y;
    const float sub_h = 78.0f;
    const int   subs = (app.show_volume ? 1 : 0) + (app.show_spread ? 1 : 0);
    const float price_h = std::max(120.0f, avail - static_cast<float>(subs) * (sub_h + 4.0f));

    // ---- price -----------------------------------------------------------
    if (ImPlot::BeginPlot("##price", ImVec2(-1, price_h),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoLegend |
                              ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoLabel,
                          ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_Opposite);
        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
        ImPlot::SetupAxisLinks(ImAxis_X1, &app.x_min, &app.x_max);
        if (vis.count() > 0) {
            ImPlot::SetupAxisLimits(ImAxis_Y1, y_lo, y_hi, ImPlotCond_Always);
        }
        ImPlot::SetupFinish();

        if (app.show_sessions) draw_session_bands(app.x_min, app.x_max);
        draw_candles(app, bars, vis);
        draw_hover_readout(app, bars, vis);

        ImPlot::EndPlot();
    }

    // ---- volume ----------------------------------------------------------
    if (app.show_volume) {
        if (ImPlot::BeginPlot("##volume", ImVec2(-1, sub_h),
                              ImPlotFlags_NoTitle | ImPlotFlags_NoLegend |
                                  ImPlotFlags_NoMouseText)) {
            ImPlot::SetupAxes(nullptr, nullptr,
                              ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickLabels,
                              ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_Opposite);
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
            ImPlot::SetupAxisLinks(ImAxis_X1, &app.x_min, &app.x_max);

            std::uint32_t vmax = 1;
            for (std::size_t i = vis.first; i < vis.last; ++i)
                vmax = std::max(vmax, bars[i].ticks);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, vmax * 1.12, ImPlotCond_Always);
            ImPlot::SetupFinish();

            ImDrawList* dl = ImPlot::GetPlotDrawList();
            const double slot = static_cast<double>(timeframe_us(app.tf)) / kUsPerSec;
            const double half = slot * 0.34;
            ImPlot::PushPlotClipRect();
            for (std::size_t i = vis.first; i < vis.last; ++i) {
                const Bar& b = bars[i];
                const double x = to_sec(b.open_time_us) + slot * 0.5;
                const ImVec2 p0 = ImPlot::PlotToPixels(x - half, 0.0);
                const ImVec2 p1 = ImPlot::PlotToPixels(x + half, b.ticks);
                dl->AddRectFilled(ImVec2(p0.x, std::min(p0.y, p1.y)),
                                  ImVec2(p1.x, std::max(p0.y, p1.y)),
                                  col32(b.close >= b.open ? kBull : kBear, 0.45f));
            }
            ImPlot::PopPlotClipRect();
            ImPlot::EndPlot();
        }
    }

    // ---- spread ----------------------------------------------------------
    // Given its own row rather than an overlay: on gold the spread is a
    // first-class part of whether an edge survives, not chart decoration.
    if (app.show_spread) {
        if (ImPlot::BeginPlot("##spread", ImVec2(-1, sub_h),
                              ImPlotFlags_NoTitle | ImPlotFlags_NoLegend)) {
            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoLabel,
                              ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_Opposite);
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
            ImPlot::SetupAxisLinks(ImAxis_X1, &app.x_min, &app.x_max);
            ImPlot::SetupAxisFormat(ImAxis_Y1, "$%.2f");
            ImPlot::SetupFinish();

            static std::vector<double> xs, mean_s, max_s;
            xs.clear();
            mean_s.clear();
            max_s.clear();
            xs.reserve(vis.count());
            mean_s.reserve(vis.count());
            max_s.reserve(vis.count());
            const double slot = static_cast<double>(timeframe_us(app.tf)) / kUsPerSec;
            for (std::size_t i = vis.first; i < vis.last; ++i) {
                xs.push_back(to_sec(bars[i].open_time_us) + slot * 0.5);
                mean_s.push_back(app.to_usd(static_cast<Points>(bars[i].spread_mean_pts)));
                max_s.push_back(app.to_usd(static_cast<Points>(bars[i].spread_max_pts)));
            }
            if (!xs.empty()) {
                ImPlot::SetNextLineStyle(ImVec4(kBear.x, kBear.y, kBear.z, 0.55f), 1.0f);
                ImPlot::PlotLine("max", xs.data(), max_s.data(), static_cast<int>(xs.size()));
                ImPlot::SetNextLineStyle(kAccent, 1.4f);
                ImPlot::PlotLine("mean", xs.data(), mean_s.data(), static_cast<int>(xs.size()));
            }
            ImPlot::EndPlot();
        }
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// market
// ---------------------------------------------------------------------------

void panel_market(AppState& app) {
    if (!ImGui::Begin("Market", &app.p_market)) {
        ImGui::End();
        return;
    }
    if (!app.store || app.bars.empty()) {
        text_muted("no data");
        ImGui::End();
        return;
    }

    const auto bars = app.bars.all_for_display();
    const Bar& last = bars.back();

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", app.to_usd(last.close));
    const bool up = last.close >= last.open;
    stat("LAST (bid)", buf, up ? &kBull : &kBear);

    ImGui::SameLine(0, 28);
    std::snprintf(buf, sizeof(buf), "%.3f", app.to_usd(static_cast<Points>(last.spread_mean_pts)));
    stat("SPREAD", buf);

    // The percentile matters more than the level: a spread that is normal for
    // 03:00 UTC is a blowout at 13:00, and the entry gate keys off the rank.
    ImGui::SameLine(0, 28);
    {
        std::size_t below = 0, total = 0;
        const std::size_t n = std::min<std::size_t>(bars.size(), 2000);
        for (std::size_t i = bars.size() - n; i < bars.size(); ++i, ++total)
            if (bars[i].spread_mean_pts <= last.spread_mean_pts) ++below;
        const double pct = total ? 100.0 * static_cast<double>(below) / static_cast<double>(total) : 0.0;
        std::snprintf(buf, sizeof(buf), "%.0f%%", pct);
        const ImVec4 c = pct > 90 ? kBear : (pct > 70 ? kAccent : kBull);
        stat("SPREAD PCTILE", buf, &c);
    }

    ImGui::SameLine(0, 28);
    const int h = utc_hour(last.open_time_us);
    stat("SESSION", session_name(h));

    ImGui::SameLine(0, 28);
    std::snprintf(buf, sizeof(buf), "%02d:00 UTC", h);
    stat("BAR TIME", buf);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    text_muted("last bar   %s  (%s)", fmt_time(last.open_time_us).c_str(),
               timeframe_name(app.tf));
    text_muted("range      %.3f USD over %u ticks", app.to_usd(last.range_pts()), last.ticks);
    text_muted("closed     %s", app.bars.forming().has_value() ? "no - still forming" : "yes");

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped(
        "Live quotes, the news countdown and the prop drawdown gauges arrive with "
        "the MT5 bridge in Phase 7. Everything above is read from the tick store.");
    ImGui::PopStyleColor();

    ImGui::End();
}

// ---------------------------------------------------------------------------
// store
// ---------------------------------------------------------------------------

void panel_store(AppState& app) {
    if (!ImGui::Begin("Store", &app.p_store)) {
        ImGui::End();
        return;
    }

    ImGui::SetNextItemWidth(240);
    char dir[512];
    std::snprintf(dir, sizeof(dir), "%s", app.store_dir.c_str());
    if (ImGui::InputText("directory", dir, sizeof(dir))) app.store_dir = dir;
    ImGui::SameLine();
    if (ImGui::Button("Open")) open_store(app);

    if (!app.store) {
        ImGui::Spacing();
        if (!app.load_error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kBear);
            ImGui::TextWrapped("%s", app.load_error.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::End();
        return;
    }

    ImGui::Spacing();
    const TickStore& s = *app.store;
    text_muted("%zu months, %llu ticks, %.2f GB", s.file_count(),
               static_cast<unsigned long long>(s.total_ticks()),
               static_cast<double>(s.total_ticks()) * 16.0 / 1e9);
    text_muted("%s  ..  %s", fmt_time(s.first_ts()).c_str(), fmt_time(s.last_ts()).c_str());

    ImGui::Spacing();
    if (ImGui::BeginTable("files", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("file");
        ImGui::TableSetupColumn("ticks");
        ImGui::TableSetupColumn("from");
        ImGui::TableSetupColumn("to");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (const TickFile& f : s.files()) {
            const FileHeader& hh = f.header();
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(f.path().filename().string().c_str(), false,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                // Jump the chart to this month.
                app.x_min = to_sec(hh.first_ts_us);
                app.x_max = to_sec(hh.last_ts_us);
            }
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(hh.count));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(fmt_time(hh.first_ts_us).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(fmt_time(hh.last_ts_us).c_str());
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// log
// ---------------------------------------------------------------------------

void panel_log(AppState& app) {
    if (!ImGui::Begin("Log", &app.p_log)) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("Clear")) app.log.clear();
    ImGui::Separator();

    ImGui::BeginChild("scroll", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (const LogLine& l : app.log.lines()) {
        const ImVec4* c = nullptr;
        if (l.level == LogLevel::Warn) c = &kAccent;
        else if (l.level == LogLevel::Error) c = &kBear;
        if (c) ImGui::PushStyleColor(ImGuiCol_Text, *c);
        ImGui::TextUnformatted(l.text.c_str());
        if (c) ImGui::PopStyleColor();
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::End();
}

// ---------------------------------------------------------------------------
// shell
// ---------------------------------------------------------------------------

void draw_ui(AppState& app) {
    // Signature is (dockspace_id, viewport, flags, window_class) as of the
    // pinned imgui v1.90.9-docking. Passing 0 lets ImGui derive the id.
    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Reopen store")) open_store(app);
            if (ImGui::MenuItem("Rebuild bars")) app.needs_rebuild = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) app.should_quit = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Chart", nullptr, &app.p_chart);
            ImGui::MenuItem("Market", nullptr, &app.p_market);
            ImGui::MenuItem("Store", nullptr, &app.p_store);
            ImGui::MenuItem("Log", nullptr, &app.p_log);
            ImGui::Separator();
            ImGui::MenuItem("ImGui metrics", nullptr, &app.p_metrics);
            ImGui::EndMenu();
        }

        // Panels that need the Phase 1 engine. Shown greyed rather than hidden,
        // so the shape of the finished terminal is visible and the gap is
        // explicit instead of quietly missing.
        if (ImGui::BeginMenu("Trade")) {
            ImGui::BeginDisabled();
            ImGui::MenuItem("Blotter");
            ImGui::MenuItem("Equity & drawdown");
            ImGui::MenuItem("Backtest runner");
            ImGui::MenuItem("Optimizer");
            ImGui::MenuItem("Risk gauges");
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
            ImGui::TextUnformatted("  requires the Phase 1 engine");
            ImGui::PopStyleColor();
            ImGui::EndMenu();
        }

        const std::string right = app.store
                                      ? app.symbol + "  |  " + timeframe_name(app.tf)
                                      : std::string("no store");
        const float w = ImGui::CalcTextSize(right.c_str()).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - w - 16);
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextUnformatted(right.c_str());
        ImGui::PopStyleColor();
        ImGui::EndMainMenuBar();
    }

    if (app.needs_rebuild) rebuild_bars(app);

    if (app.p_chart) panel_chart(app);
    if (app.p_market) panel_market(app);
    if (app.p_store) panel_store(app);
    if (app.p_log) panel_log(app);
    if (app.p_metrics) ImGui::ShowMetricsWindow(&app.p_metrics);
}

}  // namespace xauterm
