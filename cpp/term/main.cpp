// xauterm — Win32 + DirectX 11 host.
//
// Platform boilerplate only. Everything that matters lives in panels.cpp and
// the core library; this file exists to open a window and pump frames.
//
//   xauterm.exe [store_dir] [symbol]

#include "app.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "implot.h"

#include <d3d11.h>
#include <shellapi.h>
#include <tchar.h>

#include <cctype>
#include <cstdlib>
#include <exception>
#include <span>
#include <string>
#include <vector>

// Forward-declared in the backend header's implementation.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace {

ID3D11Device*           g_device = nullptr;
ID3D11DeviceContext*    g_context = nullptr;
IDXGISwapChain*         g_swapchain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT                    g_resize_w = 0;
UINT                    g_resize_h = 0;

void create_rtv() {
    ID3D11Texture2D* back = nullptr;
    g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void release_rtv() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL       got{};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
        &g_swapchain, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        // No hardware device (RDP, a VM without GPU passthrough). WARP is slower
        // but correct, and a trading terminal that will not open at all is worse
        // than one that redraws in software.
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swapchain, &g_device, &got,
                                           &g_context);
    }
    if (FAILED(hr)) return false;

    create_rtv();
    return true;
}

void destroy_device() {
    release_rtv();
    if (g_swapchain) { g_swapchain->Release(); g_swapchain = nullptr; }
    if (g_context)   { g_context->Release();   g_context = nullptr; }
    if (g_device)    { g_device->Release();    g_device = nullptr; }
}

// Command-line arguments, as UTF-8.
//
// NOT __argv. A WIN32 executable with wWinMain links wWinMainCRTStartup, which
// populates __wargv and leaves __argv null — so `__argv[1]` is a null
// dereference, and the crash happens before anything is drawn. It builds and
// links perfectly, and CI cannot catch it because CI never launches a window.
std::vector<std::string> command_line_args() {
    std::vector<std::string> out;
    int                      wargc = 0;
    LPWSTR*                  wargv = ::CommandLineToArgvW(::GetCommandLineW(), &wargc);
    if (wargv == nullptr) return out;

    for (int i = 0; i < wargc; ++i) {
        const int n =
            ::WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            std::string s(static_cast<std::size_t>(n - 1), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), n, nullptr, nullptr);
            out.push_back(std::move(s));
        } else {
            out.emplace_back();
        }
    }
    ::LocalFree(wargv);
    return out;
}

bool starts_with_ci(const std::string& s, const std::string& prefix) {
    if (prefix.empty() || prefix.size() > s.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

struct Options {
    std::string    store_dir = "data/ticks/XAUUSD";
    std::string    symbol = "XAUUSD";
    std::string    strategy;
    double         lots = 0.10;
    xau::Timeframe tf = xau::Timeframe::M15;
    bool           run = false;
};

// xauterm [store_dir] [symbol] [--tf M15] [--strategy Name] [--lots X] [--run]
//
// --run exists so the terminal can be launched into a finished backtest rather
// than an empty one. That makes it demonstrable and scriptable, and it is the
// only way to see the trade panels without clicking.
Options parse_options(const std::vector<std::string>& a) {
    Options o;
    int     positional = 0;
    for (std::size_t i = 1; i < a.size(); ++i) {
        const std::string& s = a[i];
        if (s == "--run") {
            o.run = true;
        } else if (s == "--strategy" && i + 1 < a.size()) {
            o.strategy = a[++i];
        } else if (s == "--lots" && i + 1 < a.size()) {
            o.lots = std::atof(a[++i].c_str());
        } else if (s == "--tf" && i + 1 < a.size()) {
            const std::string want = a[++i];
            for (int k = 0; k < static_cast<int>(xau::Timeframe::COUNT); ++k) {
                const auto cand = static_cast<xau::Timeframe>(k);
                if (want == xau::timeframe_name(cand)) o.tf = cand;
            }
        } else if (!s.empty() && s[0] != '-') {
            if (positional == 0) o.store_dir = s;
            else if (positional == 1) o.symbol = s;
            ++positional;
        }
    }
    return o;
}

LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;

    switch (msg) {
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) return 0;
            g_resize_w = static_cast<UINT>(LOWORD(lp));
            g_resize_h = static_cast<UINT>(HIWORD(lp));
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;  // swallow alt-menu
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"xauterm";
    ::RegisterClassExW(&wc);

    // Fit the monitor's work area rather than a hardcoded size. Opening larger
    // than the screen is a bug on its own, and it also distorts the default
    // dock layout: the nodes get sized for a viewport that never exists, and
    // the side panel ends up a few pixels wide once the window is clamped.
    RECT work{};
    if (!::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work = RECT{0, 0, 1280, 800};
    }
    const LONG avail_w = work.right - work.left;
    const LONG avail_h = work.bottom - work.top;
    const int  win_w = static_cast<int>(avail_w < 1680 ? avail_w : 1680);
    const int  win_h = static_cast<int>(avail_h < 980 ? avail_h : 980);

    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"xauterm  -  XAUUSD", WS_OVERLAPPEDWINDOW,
                                static_cast<int>(work.left), static_cast<int>(work.top), win_w,
                                win_h, nullptr, nullptr, inst, nullptr);
    if (!create_device(hwnd)) {
        destroy_device();
        ::UnregisterClassW(wc.lpszClassName, inst);
        ::MessageBoxW(nullptr, L"Could not create a Direct3D 11 device.", L"xauterm",
                      MB_OK | MB_ICONERROR);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = "xauterm.ini";  // remembers the docking layout between runs

    xauterm::apply_theme();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    xauterm::AppState app;
    const Options     opt = parse_options(command_line_args());
    app.store_dir = opt.store_dir;
    app.symbol = opt.symbol;
    app.tf = opt.tf;
    app.lots = opt.lots;

    if (!opt.strategy.empty()) {
        // Prefix match, case-insensitively: one registry entry is called
        // "RandomEntry (null)", which is awkward to type on a command line.
        const std::span<const xau::BaselineEntry> reg = xau::baseline_registry();
        for (int i = 0; i < static_cast<int>(reg.size()); ++i) {
            if (starts_with_ci(reg[static_cast<std::size_t>(i)].name, opt.strategy)) {
                app.strategy_index = i;
                break;
            }
        }
    }

    app.log.info("xauterm starting");
    xauterm::open_store(app);
    if (opt.run) xauterm::start_backtest(app);

    const float clear[4] = {0.063f, 0.070f, 0.082f, 1.0f};
    bool        running = true;

    while (running) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0u, 0u, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (g_resize_w != 0 && g_resize_h != 0) {
            release_rtv();
            g_swapchain->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            create_rtv();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        try {
            xauterm::draw_ui(app);
        } catch (const std::exception& e) {
            // A panel throwing must not take the window down mid-frame; the
            // frame still has to be ended or ImGui asserts on the next one.
            app.log.error(std::string("panel exception: ") + e.what());
        }
        if (app.should_quit) running = false;

        ImGui::Render();
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_swapchain->Present(1, 0);  // vsync
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    destroy_device();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, inst);
    return 0;
}
