cmake_minimum_required(VERSION 3.25)

project(xau
    VERSION 0.1.0
    DESCRIPTION "XAUUSD algorithmic trading terminal"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

option(XAU_BUILD_TESTS "Build unit tests"  ON)
option(XAU_BUILD_BENCH "Build benchmarks"  ON)
option(XAU_WERROR      "Treat warnings as errors" OFF)

# ---------------------------------------------------------------------------
# Determinism.  The backtester and the live engine run the same compiled code,
# so a result that changes with compiler flags is a correctness bug, not a
# rounding detail.  See docs/PLAN.md section 10.
#   - no fast-math: reassociation would let the optimiser change trade outcomes
#   - no FMA contraction: a fused multiply-add rounds differently to two ops,
#     which is enough to flip a barrier comparison
# ---------------------------------------------------------------------------
add_library(xau_flags INTERFACE)

if(MSVC)
    target_compile_options(xau_flags INTERFACE
        /W4 /permissive- /utf-8 /Zc:__cplusplus /fp:precise /EHsc
        $<$<BOOL:${XAU_WERROR}>:/WX>)
    target_compile_definitions(xau_flags INTERFACE
        NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(xau_flags INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        -ffp-contract=off
        $<$<BOOL:${XAU_WERROR}>:-Werror>)
endif()

option(XAU_BUILD_TERM   "Build the xauterm terminal (Windows only)" ON)
option(XAU_BUILD_PYTHON "Build the xaucore Python extension module" ON)

add_subdirectory(cpp/core)

if(XAU_BUILD_TERM)
    add_subdirectory(cpp/term)
endif()

if(XAU_BUILD_PYTHON)
    add_subdirectory(cpp/bridge/pybind)
endif()

if(XAU_BUILD_TESTS)
    enable_testing()
    add_subdirectory(cpp/tests)
endif()

if(XAU_BUILD_BENCH)
    add_subdirectory(cpp/bench)
endif()

message(STATUS "xau ${PROJECT_VERSION} | ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} | tests=${XAU_BUILD_TESTS} bench=${XAU_BUILD_BENCH}")
