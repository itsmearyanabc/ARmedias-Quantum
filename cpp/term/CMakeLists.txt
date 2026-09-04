# xauterm — the trading terminal.
#
# Win32 + DirectX 11. Windows-only on purpose: the MT5 terminal this eventually
# talks to is Windows-only, and the Win32/DX11 backend needs nothing beyond the
# Windows SDK — no GLFW, no vcpkg, no extra install step.
#
# ImGui and ImPlot are vendored via FetchContent rather than vcpkg. They are
# designed to be compiled directly into the consumer, the versions must match
# each other, and this keeps the prerequisites at "MSVC and CMake".

if(NOT WIN32)
    message(STATUS "xauterm: skipped (Win32/DX11 backend, Windows only)")
    return()
endif()

include(FetchContent)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.9-docking
    GIT_SHALLOW    TRUE)

FetchContent_Declare(implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        v0.16
    GIT_SHALLOW    TRUE)

# Neither ships a CMakeLists, so this only downloads; we define the targets.
FetchContent_MakeAvailable(imgui implot)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler)

add_library(implot STATIC
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
)
target_include_directories(implot PUBLIC ${implot_SOURCE_DIR})
target_link_libraries(implot PUBLIC imgui)

# Third-party code does not get our warning flags; /W4 /WX on someone else's
# source is just noise we would have to suppress anyway.
if(MSVC)
    target_compile_options(imgui  PRIVATE /W0)
    target_compile_options(implot PRIVATE /W0)
endif()

add_executable(xauterm WIN32
    main.cpp
    theme.cpp
    panels.cpp
)

target_include_directories(xauterm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xauterm PRIVATE xau::core xau_flags imgui implot)

set_target_properties(xauterm PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
