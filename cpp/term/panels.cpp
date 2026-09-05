#include "app.hpp"

#include "xau/session.hpp"

#include "imgui.h"
#include "imgui_internal.h"   // DockBuilder, for the default layout
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

// Session classification comes from the core (session.hpp) so the chart and the
// strategies can never disagree about which session a bar is in. Only the
// display names live here.
const char* session_label(TimeUs us) {
    switch (session_at(us)) {
        case Session::London:   return "London";
        case Session::Overlap:  return "London/NY overlap";
        case Session::NewYork:  return "New York";
        case Session::Rollover: return "Rollover";
        case Session::Asia:     return "Asia";
    }
    return "?";
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
    text_muted("session %s", session_label(b.open_time_us));
    ImGui::EndTooltip();
}

// Trades drawn onto the price chart. This is the Phase 2 gate: being able to
// look at an individual trade in context is what catches the bugs no summary
// statistic reveals.
//
// Culled to the visible window. A decade of a daily strategy is a few thousand
// trades, and drawing them all every frame would cost more than the candles.
void draw_trade_markers(const AppState& app) {
    if (!app.show_trades || !app.bt || app.bt->trades.empty()) return;

    ImDrawList*      dl = ImPlot::GetPlotDrawList();
    const ImPlotRect lim = ImPlot::GetPlotLimits();
    ImPlot::PushPlotClipRect();

    const std::vector<Trade>& trades = app.bt->trades;
    for (std::size_t i = 0; i < trades.size(); ++i) {
        const Trade& t = trades[i];
        const double te = to_sec(t.entry_ts);
        const double tx = to_sec(t.exit_ts);
        if (tx < lim.X.Min || te > lim.X.Max) continue;

        const bool  sel = (static_cast<int>(i) == app.selected_trade);
        const ImU32 c = col32(t.net_usd > 0.0 ? kBull : kBear, sel ? 1.0f : 0.8f);

        const ImVec2 pe = ImPlot::PlotToPixels(te, app.to_usd(t.entry_pts));
        const ImVec2 px = ImPlot::PlotToPixels(tx, app.to_usd(t.exit_pts));

        dl->AddLine(pe, px, c, sel ? 2.5f : 1.3f);

        // The entry marker points the way the trade was taken.
        const float s = sel ? 7.0f : 5.0f;
        if (t.side == Side::Long) {
            dl->AddTriangleFilled(ImVec2(pe.x, pe.y - s), ImVec2(pe.x - s, pe.y + s),
                                  ImVec2(pe.x + s, pe.y + s), c);
        } else {
            dl->AddTriangleFilled(ImVec2(pe.x, pe.y + s), ImVec2(pe.x - s, pe.y - s),
                                  ImVec2(pe.x + s, pe.y - s), c);
        }
        dl->AddRectFilled(ImVec2(px.x - 3.0f, px.y - 3.0f), ImVec2(px.x + 3.0f, px.y + 3.0f), c);

        // Stop and target for the selected trade only, drawn across that
        // trade's own duration rather than the whole chart, because that is
        // when they were actually live.
        if (sel && app.show_sl_tp) {
            if (t.sl_pts != 0) {
                dl->AddLine(ImPlot::PlotToPixels(te, app.to_usd(t.sl_pts)),
                            ImPlot::PlotToPixels(tx, app.to_usd(t.sl_pts)),
                            col32(kBear, 0.7f), 1.2f);
            }
            if (t.tp_pts != 0) {
                dl->AddLine(ImPlot::PlotToPixels(te, app.to_usd(t.tp_pts)),
                            ImPlot::PlotToPixels(tx, app.to_usd(t.tp_pts)),
                            col32(kBull, 0.7f), 1.2f);
            }
            dl->AddCircle(pe, s + 4.0f, col32(kAccent), 0, 2.0f);
        }
    }
    ImPlot::PopPlotClipRect();
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
    ImGui::BeginDisabled(!app.bt.has_value());
    ImGui::Checkbox("Trades", &app.show_trades);
    ImGui::EndDisabled();
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
        draw_trade_markers(app);
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
    stat("SESSION", session_label(last.open_time_us));

    ImGui::SameLine(0, 28);
    std::snprintf(buf, sizeof(buf), "%02d:00 UTC", utc_hour(last.open_time_us));
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
    // A running backtest holds a pointer into this store; swapping it out from
    // under that thread would be a use-after-free.
    ImGui::BeginDisabled(app.bt_running);
    if (ImGui::Button("Open")) open_store(app);
    ImGui::EndDisabled();

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
// backtest, driven off the render thread
// ---------------------------------------------------------------------------

void start_backtest(AppState& app) {
    if (!app.store || app.bt_running) return;

    const std::span<const BaselineEntry> reg = baseline_registry();
    if (app.strategy_index < 0 || app.strategy_index >= static_cast<int>(reg.size())) return;
    const BaselineEntry& entry = reg[static_cast<std::size_t>(app.strategy_index)];

    BacktestConfig cfg;
    cfg.spec = SymbolSpec::xauusd_default();
    cfg.tf = app.tf;
    cfg.initial_balance = app.initial_balance;
    cfg.apply_swap = app.apply_swap;
    cfg.costs.slip_base_pts = 15.0;
    cfg.costs.slip_vol_coef = 0.05;
    cfg.costs.latency_us = 150'000;
    cfg.costs.commission_per_lot_round_usd = app.commission;
    cfg.costs.spread_mult = app.spread_mult;
    cfg.costs.slippage_mult = app.slippage_mult;

    // The worker holds a bare pointer to the store, so reopening it mid-run
    // would leave that thread reading freed memory. Both the File menu and the
    // Store panel disable reopening while bt_running.
    const TickStore* store = &*app.store;
    StrategyFactory  make = factory_for(entry, app.lots);

    app.bt_error.clear();
    app.selected_trade = -1;
    app.bt_running = true;
    app.log.info(std::string("backtest: ") + entry.name + " started");

    // A decade is a few seconds of work. Doing it inline would freeze the
    // window, and an unresponsive terminal is the kind of thing you forgive
    // exactly once.
    app.bt_future = std::async(std::launch::async, [store, cfg, make]() -> BacktestOutcome {
        BacktestOutcome out;
        try {
            std::unique_ptr<Strategy> s = make();
            out.result = BacktestEngine(*store, cfg).run(*s);
            out.ok = true;
        } catch (const std::exception& e) {
            out.error = e.what();
        } catch (...) {
            out.error = "unknown error";
        }
        return out;
    });
}

void poll_backtest(AppState& app) {
    if (!app.bt_running || !app.bt_future.valid()) return;
    if (app.bt_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;

    BacktestOutcome out = app.bt_future.get();
    app.bt_running = false;

    if (!out.ok) {
        app.bt.reset();
        app.eq_x.clear();
        app.eq_y.clear();
        app.dd_y.clear();
        app.bt_error = out.error;
        app.log.error("backtest failed: " + out.error);
        return;
    }

    app.bt = std::move(out.result);

    // Flatten the equity series once, here. A decade of M15 is ~350k points,
    // and rebuilding that inside the panel would redo it sixty times a second
    // for a curve that has not changed.
    app.eq_x.clear();
    app.eq_y.clear();
    app.dd_y.clear();
    const std::vector<EquityPoint>& eq = app.bt->equity;
    app.eq_x.reserve(eq.size());
    app.eq_y.reserve(eq.size());
    app.dd_y.reserve(eq.size());
    double peak = app.bt->initial_balance;
    for (const EquityPoint& p : eq) {
        peak = std::max(peak, p.equity);
        app.eq_x.push_back(to_sec(p.ts_us));
        app.eq_y.push_back(p.equity);
        app.dd_y.push_back(peak > 0.0 ? -100.0 * (peak - p.equity) / peak : 0.0);
    }
    app.eqx_min = app.eq_x.empty() ? 0.0 : app.eq_x.front();
    app.eqx_max = app.eq_x.empty() ? 1.0 : app.eq_x.back();

    char buf[224];
    std::snprintf(buf, sizeof(buf),
                  "backtest: %zu trades, net %.2f USD, PF %.3f, maxDD %.2f%% in %.2f s",
                  app.bt->metrics.trades, app.bt->metrics.net_profit,
                  app.bt->metrics.profit_factor, app.bt->metrics.max_drawdown_pct,
                  app.bt->stats.wall_seconds);
    app.log.info(buf);
    if (app.bt->stats.rejected_total() > 0) {
        std::snprintf(buf, sizeof(buf),
                      "  %llu orders rejected (stop too close %llu, inside spread %llu, "
                      "volume %llu, already in position %llu)",
                      static_cast<unsigned long long>(app.bt->stats.rejected_total()),
                      static_cast<unsigned long long>(app.bt->stats.rejected_stop_too_close),
                      static_cast<unsigned long long>(app.bt->stats.rejected_stop_inside_spread),
                      static_cast<unsigned long long>(app.bt->stats.rejected_volume),
                      static_cast<unsigned long long>(app.bt->stats.rejected_in_position));
        app.log.warn(buf);
    }
}

void select_trade(AppState& app, int index, bool jump_chart) {
    if (!app.bt || app.bt->trades.empty()) {
        app.selected_trade = -1;
        return;
    }
    const int n = static_cast<int>(app.bt->trades.size());
    app.selected_trade = std::clamp(index, 0, n - 1);
    if (!jump_chart) return;

    // Frame the trade with context either side rather than filling the view
    // with it. What led into a trade is most of why you are looking at it.
    const Trade& t = app.bt->trades[static_cast<std::size_t>(app.selected_trade)];
    const double a = to_sec(t.entry_ts);
    const double b = to_sec(t.exit_ts);
    const double bar_seconds = static_cast<double>(timeframe_us(app.tf)) / kUsPerSec;
    const double pad = std::max((b - a) * 1.5, bar_seconds * 20.0);
    app.x_min = a - pad;
    app.x_max = b + pad;
}

// ---------------------------------------------------------------------------
// backtest runner
// ---------------------------------------------------------------------------

void panel_runner(AppState& app) {
    if (!ImGui::Begin("Backtest", &app.p_runner)) {
        ImGui::End();
        return;
    }

    const std::span<const BaselineEntry> reg = baseline_registry();
    if (app.strategy_index >= static_cast<int>(reg.size())) app.strategy_index = 0;
    const BaselineEntry& cur = reg[static_cast<std::size_t>(app.strategy_index)];

    ImGui::SetNextItemWidth(240);
    if (ImGui::BeginCombo("strategy", cur.name)) {
        for (int i = 0; i < static_cast<int>(reg.size()); ++i) {
            const BaselineEntry& e = reg[static_cast<std::size_t>(i)];
            const bool           chosen = (i == app.strategy_index);
            if (ImGui::Selectable(e.name, chosen)) app.strategy_index = i;
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(380.0f);
                ImGui::TextUnformatted(e.description);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            if (chosen) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(cur.description);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    // One control per row. An InputDouble carries its step buttons and puts its
    // label to the RIGHT, so two side by side overflow a docked side panel and
    // the labels get clipped.
    ImGui::Spacing();
    ImGui::SetNextItemWidth(96);
    ImGui::InputDouble("lots", &app.lots, 0.01, 0.10, "%.2f");
    ImGui::SetNextItemWidth(96);
    ImGui::InputDouble("balance", &app.initial_balance, 1000.0, 5000.0, "%.0f");
    ImGui::SetNextItemWidth(96);
    ImGui::InputDouble("commission", &app.commission, 1.0, 5.0, "%.2f");
    ImGui::Checkbox("apply swap", &app.apply_swap);

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("cost stress - must survive 2x (PLAN section 8)");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(80);
    ImGui::InputDouble("spread x", &app.spread_mult, 0.25, 1.0, "%.2f");
    ImGui::SetNextItemWidth(80);
    ImGui::InputDouble("slip x", &app.slippage_mult, 0.25, 1.0, "%.2f");
    if (ImGui::Button("1x")) {
        app.spread_mult = 1.0;
        app.slippage_mult = 1.0;
    }
    ImGui::SameLine();
    if (ImGui::Button("2x")) {
        app.spread_mult = 2.0;
        app.slippage_mult = 2.0;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::BeginDisabled(app.bt_running || !app.store);
    if (ImGui::Button("Run backtest", ImVec2(150, 28))) start_backtest(app);
    ImGui::EndDisabled();
    if (app.bt_running) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::TextUnformatted("running...");
        ImGui::PopStyleColor();
    } else if (!app.store) {
        ImGui::SameLine();
        text_muted("no store loaded");
    }

    if (!app.bt_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kBear);
        ImGui::TextWrapped("%s", app.bt_error.c_str());
        ImGui::PopStyleColor();
    }

    if (app.bt) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextUnformatted(app.bt->summary().c_str());
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// equity and drawdown
// ---------------------------------------------------------------------------

void panel_equity(AppState& app) {
    if (!ImGui::Begin("Equity", &app.p_equity)) {
        ImGui::End();
        return;
    }
    if (!app.bt || app.eq_x.empty()) {
        text_muted("run a backtest to see the equity curve");
        ImGui::End();
        return;
    }

    const Metrics& m = app.bt->metrics;
    char           buf[64];

    std::snprintf(buf, sizeof(buf), "%.2f", m.net_profit);
    stat("NET USD", buf, m.net_profit >= 0.0 ? &kBull : &kBear);
    ImGui::SameLine(0, 24);
    std::snprintf(buf, sizeof(buf), "%.3f", m.profit_factor);
    stat("PROFIT FACTOR", buf);
    ImGui::SameLine(0, 24);
    std::snprintf(buf, sizeof(buf), "-%.2f%%", m.max_drawdown_pct);
    stat("MAX DD", buf, &kBear);
    ImGui::SameLine(0, 24);
    std::snprintf(buf, sizeof(buf), "%.1f%%", m.win_rate * 100.0);
    stat("WIN RATE", buf);
    ImGui::SameLine(0, 24);
    std::snprintf(buf, sizeof(buf), "%.4f", m.expectancy_usd);
    stat("EXPECTANCY", buf, m.expectancy_usd >= 0.0 ? &kBull : &kBear);

    ImGui::Spacing();

    const int   n = static_cast<int>(app.eq_x.size());
    const float avail = ImGui::GetContentRegionAvail().y;
    const float dd_h = std::max(60.0f, avail * 0.30f);
    const float eq_h = std::max(80.0f, avail - dd_h - 6.0f);

    if (ImPlot::BeginPlot("##equity", ImVec2(-1, eq_h),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoLabel,
                          ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_Opposite);
        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
        ImPlot::SetupAxisLinks(ImAxis_X1, &app.eqx_min, &app.eqx_max);
        ImPlot::SetupAxisFormat(ImAxis_Y1, "$%.0f");
        ImPlot::SetupFinish();

        // The starting balance, so profit and loss are separated by eye rather
        // than by reading the axis.
        const double bx[2] = {app.eq_x.front(), app.eq_x.back()};
        const double by[2] = {app.bt->initial_balance, app.bt->initial_balance};
        ImPlot::SetNextLineStyle(ImVec4(kMuted.x, kMuted.y, kMuted.z, 0.55f), 1.0f);
        ImPlot::PlotLine("start", bx, by, 2);

        ImPlot::SetNextLineStyle(kAccent, 1.6f);
        ImPlot::PlotLine("equity", app.eq_x.data(), app.eq_y.data(), n);

        // The selected trade, so blotter, chart and curve all agree about which
        // one you are looking at.
        if (app.selected_trade >= 0) {
            const Trade& t = app.bt->trades[static_cast<std::size_t>(app.selected_trade)];
            const double mx[1] = {to_sec(t.exit_ts)};
            const double my[1] = {t.balance_after};
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 5.0f, kAccent, 2.0f, kAccent);
            ImPlot::PlotScatter("##sel", mx, my, 1);
        }
        ImPlot::EndPlot();
    }

    if (ImPlot::BeginPlot("##underwater", ImVec2(-1, dd_h),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoLabel,
                          ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_Opposite);
        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
        ImPlot::SetupAxisLinks(ImAxis_X1, &app.eqx_min, &app.eqx_max);
        ImPlot::SetupAxisFormat(ImAxis_Y1, "%.1f%%");
        ImPlot::SetupFinish();
        ImPlot::SetNextFillStyle(kBear, 0.35f);
        ImPlot::PlotShaded("underwater", app.eq_x.data(), app.dd_y.data(), n, 0.0);
        ImPlot::SetNextLineStyle(kBear, 1.1f);
        ImPlot::PlotLine("underwater", app.eq_x.data(), app.dd_y.data(), n);
        ImPlot::EndPlot();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// blotter
// ---------------------------------------------------------------------------

void panel_blotter(AppState& app) {
    if (!ImGui::Begin("Trades", &app.p_blotter)) {
        ImGui::End();
        return;
    }
    if (!app.bt) {
        text_muted("run a backtest to populate the blotter");
        ImGui::End();
        return;
    }

    const std::vector<Trade>& tr = app.bt->trades;
    const SymbolSpec          spec = SymbolSpec::xauusd_default();

    // Rejections are shown next to the trade count, never hidden: a strategy
    // that is mostly rejected looks profitable on the trades that survived.
    text_muted("%zu trades from %llu signals, %llu rejected", tr.size(),
               static_cast<unsigned long long>(app.bt->stats.signals),
               static_cast<unsigned long long>(app.bt->stats.rejected_total()));
    ImGui::SameLine();
    text_muted("   [ and ] step through");

    ImGui::Spacing();
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingStretchProp |
                                      ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("blotter", 9, flags)) {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("side");
        ImGui::TableSetupColumn("entry time");
        ImGui::TableSetupColumn("in");
        ImGui::TableSetupColumn("out");
        ImGui::TableSetupColumn("net USD");
        ImGui::TableSetupColumn("R");
        ImGui::TableSetupColumn("MAE / MFE");
        ImGui::TableSetupColumn("exit");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // Clipped: a decade of a daily strategy is thousands of rows and only
        // a screenful is ever visible.
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(tr.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const Trade& t = tr[static_cast<std::size_t>(i)];
                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableNextColumn();
                char lbl[16];
                std::snprintf(lbl, sizeof(lbl), "%d", i + 1);
                if (ImGui::Selectable(lbl, i == app.selected_trade,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    select_trade(app, i, true);
                }

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(t.side == Side::Long ? "long" : "short");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(fmt_time(t.entry_ts).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", app.to_usd(t.entry_pts));
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", app.to_usd(t.exit_pts));

                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, t.net_usd > 0.0 ? kBull : kBear);
                ImGui::Text("%.2f", t.net_usd);
                ImGui::PopStyleColor();

                // R multiple: profit over what was actually at risk. Undefined
                // without a stop, and saying so beats printing a number.
                ImGui::TableNextColumn();
                if (t.sl_pts != 0) {
                    const Points risk_pts = t.entry_pts > t.sl_pts ? t.entry_pts - t.sl_pts
                                                                   : t.sl_pts - t.entry_pts;
                    const double risk = spec.pnl_usd(risk_pts, t.lots);
                    if (risk > 0.0) {
                        ImGui::Text("%.2f", t.net_usd / risk);
                    } else {
                        ImGui::TextUnformatted("-");
                    }
                } else {
                    ImGui::TextUnformatted("-");
                }

                ImGui::TableNextColumn();
                ImGui::Text("%.2f / %.2f", app.to_usd(t.mae_pts), app.to_usd(t.mfe_pts));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(exit_reason_name(t.exit_reason));

                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// shell
// ---------------------------------------------------------------------------

void draw_ui(AppState& app) {
    // An explicit host window rather than DockSpaceOverViewport, so the default
    // layout can be built BEFORE the dockspace is submitted. Building it
    // afterwards - removing and re-adding a node the same frame it was already
    // submitted - left the split ratios ignored and the side panel a few pixels
    // wide, which looks exactly like a broken window.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##dockhost", nullptr,
                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar(3);

    const ImGuiID dock_id = ImGui::GetID("xauterm_dock");

    // Built once, and only when there is no saved layout. A restored
    // xauterm.ini always wins: its node is already split, so we leave it alone.
    static bool layout_checked = false;
    if (!layout_checked) {
        layout_checked = true;
        const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dock_id);
        if (node == nullptr || node->IsLeafNode()) {
            ImGui::DockBuilderRemoveNode(dock_id);
            ImGui::DockBuilderAddNode(dock_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dock_id, vp->WorkSize);

            // Both outputs captured explicitly; relying on the return value
            // alone put the side panel on the wrong side.
            ImGuiID right = 0, centre = 0, bottom = 0, right_bottom = 0, right_top = 0;
            ImGui::DockBuilderSplitNode(dock_id, ImGuiDir_Right, 0.26f, &right, &centre);
            ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.32f, &bottom, &centre);
            ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, &right_bottom, &right_top);

            // Price dominates; trades sit under it; controls and equity to the
            // side. Chart and blotter are what you actually read.
            ImGui::DockBuilderDockWindow("Chart", centre);
            ImGui::DockBuilderDockWindow("Trades", bottom);
            ImGui::DockBuilderDockWindow("Log", bottom);
            ImGui::DockBuilderDockWindow("Market", bottom);
            ImGui::DockBuilderDockWindow("Backtest", right_top);
            ImGui::DockBuilderDockWindow("Store", right_top);
            ImGui::DockBuilderDockWindow("Equity", right_bottom);
            ImGui::DockBuilderFinish(dock_id);
        }
    }

    ImGui::DockSpace(dock_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();

    // Pick up a finished run before anything draws, so every panel in this
    // frame sees the same result.
    poll_backtest(app);

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            // Reopening would free the store a running backtest is reading.
            ImGui::BeginDisabled(app.bt_running);
            if (ImGui::MenuItem("Reopen store")) open_store(app);
            ImGui::EndDisabled();
            if (ImGui::MenuItem("Rebuild bars")) app.needs_rebuild = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) app.should_quit = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Chart", nullptr, &app.p_chart);
            ImGui::MenuItem("Backtest", nullptr, &app.p_runner);
            ImGui::MenuItem("Equity", nullptr, &app.p_equity);
            ImGui::MenuItem("Trades", nullptr, &app.p_blotter);
            ImGui::Separator();
            ImGui::MenuItem("Market", nullptr, &app.p_market);
            ImGui::MenuItem("Store", nullptr, &app.p_store);
            ImGui::MenuItem("Log", nullptr, &app.p_log);
            ImGui::Separator();
            ImGui::MenuItem("ImGui metrics", nullptr, &app.p_metrics);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Trade")) {
            const bool have = app.bt && !app.bt->trades.empty();
            ImGui::BeginDisabled(!have);
            if (ImGui::MenuItem("First")) select_trade(app, 0, true);
            if (ImGui::MenuItem("Previous", "[")) select_trade(app, app.selected_trade - 1, true);
            if (ImGui::MenuItem("Next", "]")) select_trade(app, app.selected_trade + 1, true);
            if (ImGui::MenuItem("Last")) {
                select_trade(app, have ? static_cast<int>(app.bt->trades.size()) - 1 : 0, true);
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::MenuItem("Show markers", nullptr, &app.show_trades);
            ImGui::MenuItem("Show stop / target", nullptr, &app.show_sl_tp);
            ImGui::Separator();
            // Still greyed, and still labelled, so the remaining gap stays
            // visible rather than quietly absent.
            ImGui::BeginDisabled();
            ImGui::MenuItem("Optimizer");
            ImGui::MenuItem("Risk gauges");
            ImGui::EndDisabled();
            ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
            ImGui::TextUnformatted("  optimizer needs Phase 6, gauges Phase 7");
            ImGui::PopStyleColor();
            ImGui::EndMenu();
        }

        std::string right = app.store ? app.symbol + "  |  " + timeframe_name(app.tf)
                                      : std::string("no store");
        if (app.bt) {
            char extra[64];
            std::snprintf(extra, sizeof(extra), "  |  %zu trades", app.bt->metrics.trades);
            right += extra;
        }
        const float w = ImGui::CalcTextSize(right.c_str()).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - w - 16);
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextUnformatted(right.c_str());
        ImGui::PopStyleColor();
        ImGui::EndMainMenuBar();
    }

    // Stepping trade by trade with [ and ] is the Phase 2 gate in practice.
    // Suppressed while a text field has focus, or typing a lot size would
    // scroll the chart.
    if (app.bt && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true)) {
            select_trade(app, app.selected_trade + 1, true);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true)) {
            select_trade(app, app.selected_trade - 1, true);
        }
    }

    if (app.needs_rebuild) rebuild_bars(app);

    if (app.p_chart) panel_chart(app);
    if (app.p_runner) panel_runner(app);
    if (app.p_equity) panel_equity(app);
    if (app.p_blotter) panel_blotter(app);
    if (app.p_market) panel_market(app);
    if (app.p_store) panel_store(app);
    if (app.p_log) panel_log(app);
    if (app.p_metrics) ImGui::ShowMetricsWindow(&app.p_metrics);
}

}  // namespace xauterm
