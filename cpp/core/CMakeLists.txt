add_library(xaucore STATIC
    src/mapped_file.cpp
    src/tick_store.cpp
    src/bar.cpp
    src/engine.cpp
    src/baselines.cpp
    src/rules.cpp
    src/walk_forward.cpp
    src/registry.cpp
    src/features.cpp
    src/labels.cpp
    src/zoo.cpp
)

add_library(xau::core ALIAS xaucore)

target_include_directories(xaucore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# PRIVATE, not PUBLIC: the pybind11 module links xau::core but must not inherit
# /W4 /WX, because pybind11's own headers do not compile clean under it. Every
# first-party consumer (tests, benches, terminal) links xau_flags explicitly.
target_link_libraries(xaucore PRIVATE xau_flags)

set_target_properties(xaucore PROPERTIES
    POSITION_INDEPENDENT_CODE ON)   # it will be linked into the MT5 bridge DLL
