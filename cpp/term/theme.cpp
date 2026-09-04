// Terminal styling.
//
// Instrument-panel dark, not ImGui's default blue. Colour carries meaning here:
// brass is structure and emphasis, green and red are reserved for P&L direction
// and risk state, and nothing else is allowed to use them. When the risk gauges
// arrive in Phase 7 they have to read across the room, and that only works if
// green and red mean exactly one thing everywhere else.

#include "app.hpp"

#include "imgui.h"
#include "implot.h"

namespace xauterm {
namespace {

constexpr ImVec4 rgb(int r, int g, int b, float a = 1.0f) {
    return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                  static_cast<float>(b) / 255.0f, a);
}

}  // namespace

// Shared with the chart so candles and text come from the same palette.
//
// `extern` is load-bearing: a namespace-scope `const` has internal linkage in
// C++, so without it panels.cpp would compile against these and then fail to
// link against them.
extern const ImVec4 kBull   = rgb(107, 174, 133);
extern const ImVec4 kBear   = rgb(217, 123, 120);
extern const ImVec4 kAccent = rgb(213, 164, 79);
extern const ImVec4 kMuted  = rgb(148, 142, 132);

namespace {
const ImVec4 kGrid = rgb(42, 46, 53);  // theme-local
}

void apply_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();

    s.WindowRounding    = 3.0f;
    s.ChildRounding     = 3.0f;
    s.FrameRounding     = 3.0f;
    s.PopupRounding     = 3.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding      = 3.0f;
    s.TabRounding       = 3.0f;

    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize  = 0.0f;
    s.WindowPadding    = ImVec2(10, 8);
    s.FramePadding     = ImVec2(8, 4);
    s.ItemSpacing      = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.CellPadding      = ImVec2(8, 4);
    s.ScrollbarSize    = 11.0f;
    s.GrabMinSize      = 9.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                  = rgb(231, 228, 222);
    c[ImGuiCol_TextDisabled]          = kMuted;
    c[ImGuiCol_WindowBg]              = rgb(20, 22, 26);
    c[ImGuiCol_ChildBg]               = rgb(20, 22, 26);
    c[ImGuiCol_PopupBg]               = rgb(26, 29, 34);
    c[ImGuiCol_Border]                = rgb(42, 46, 53);
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]               = rgb(32, 36, 42);
    c[ImGuiCol_FrameBgHovered]        = rgb(42, 47, 55);
    c[ImGuiCol_FrameBgActive]         = rgb(52, 58, 68);
    c[ImGuiCol_TitleBg]               = rgb(16, 18, 21);
    c[ImGuiCol_TitleBgActive]         = rgb(26, 29, 34);
    c[ImGuiCol_TitleBgCollapsed]      = rgb(16, 18, 21);
    c[ImGuiCol_MenuBarBg]             = rgb(16, 18, 21);
    c[ImGuiCol_ScrollbarBg]           = rgb(20, 22, 26);
    c[ImGuiCol_ScrollbarGrab]         = rgb(52, 58, 68);
    c[ImGuiCol_ScrollbarGrabHovered]  = rgb(70, 78, 90);
    c[ImGuiCol_ScrollbarGrabActive]   = kAccent;
    c[ImGuiCol_CheckMark]             = kAccent;
    c[ImGuiCol_SliderGrab]            = kAccent;
    c[ImGuiCol_SliderGrabActive]      = rgb(232, 188, 110);
    c[ImGuiCol_Button]                = rgb(38, 42, 49);
    c[ImGuiCol_ButtonHovered]         = rgb(52, 58, 68);
    c[ImGuiCol_ButtonActive]          = rgb(70, 78, 90);
    c[ImGuiCol_Header]                = rgb(38, 42, 49);
    c[ImGuiCol_HeaderHovered]         = rgb(52, 58, 68);
    c[ImGuiCol_HeaderActive]          = rgb(62, 70, 82);
    c[ImGuiCol_Separator]             = rgb(42, 46, 53);
    c[ImGuiCol_SeparatorHovered]      = kAccent;
    c[ImGuiCol_SeparatorActive]       = kAccent;
    c[ImGuiCol_ResizeGrip]            = rgb(42, 46, 53);
    c[ImGuiCol_ResizeGripHovered]     = kAccent;
    c[ImGuiCol_ResizeGripActive]      = kAccent;
    c[ImGuiCol_Tab]                   = rgb(24, 27, 32);
    c[ImGuiCol_TabHovered]            = rgb(52, 58, 68);
    c[ImGuiCol_TabActive]             = rgb(38, 42, 49);
    c[ImGuiCol_TabUnfocused]          = rgb(20, 22, 26);
    c[ImGuiCol_TabUnfocusedActive]    = rgb(30, 34, 40);
    c[ImGuiCol_DockingPreview]        = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f);
    c[ImGuiCol_DockingEmptyBg]        = rgb(16, 18, 21);
    c[ImGuiCol_TableHeaderBg]         = rgb(26, 29, 34);
    c[ImGuiCol_TableBorderStrong]     = rgb(52, 58, 68);
    c[ImGuiCol_TableBorderLight]      = rgb(36, 40, 46);
    c[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(1, 1, 1, 0.02f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.30f);
    c[ImGuiCol_NavHighlight]          = kAccent;

    // ---- ImPlot ----------------------------------------------------------
    ImPlotStyle& p = ImPlot::GetStyle();
    ImPlot::StyleColorsDark();

    p.LineWeight       = 1.2f;
    p.MarkerSize       = 3.0f;
    p.PlotPadding      = ImVec2(8, 6);
    p.LabelPadding     = ImVec2(4, 4);
    p.LegendPadding    = ImVec2(6, 6);
    p.MousePosPadding  = ImVec2(8, 8);
    p.PlotBorderSize   = 0.0f;
    p.MinorAlpha       = 0.20f;
    p.MajorGridSize    = ImVec2(1, 1);
    p.MinorGridSize    = ImVec2(1, 1);

    ImVec4* pc = p.Colors;
    pc[ImPlotCol_FrameBg]      = rgb(20, 22, 26);
    pc[ImPlotCol_PlotBg]       = rgb(17, 19, 23);
    pc[ImPlotCol_PlotBorder]   = ImVec4(0, 0, 0, 0);
    pc[ImPlotCol_LegendBg]     = ImVec4(0.10f, 0.11f, 0.13f, 0.92f);
    pc[ImPlotCol_LegendBorder] = kGrid;
    pc[ImPlotCol_LegendText]   = rgb(231, 228, 222);
    pc[ImPlotCol_TitleText]    = rgb(231, 228, 222);
    pc[ImPlotCol_InlayText]    = kMuted;
    pc[ImPlotCol_AxisText]     = kMuted;
    pc[ImPlotCol_AxisGrid]     = kGrid;
    pc[ImPlotCol_AxisTick]     = kGrid;
    pc[ImPlotCol_Crosshairs]   = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.55f);
    pc[ImPlotCol_Selection]    = kAccent;
}

}  // namespace xauterm
