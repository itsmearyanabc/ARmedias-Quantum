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
    src/regime.cpp
    src/validation.cpp
    src/risk.cpp
    src/reconcile.cpp
    src/bar_assembler.cpp
    src/live.cpp
    src/bayes.cpp
)

add_library(xau::core ALIAS xaucore)

# Custom strategies, one file each in cpp/strategies/custom/ (see
# xau/custom.hpp). CONFIGURE_DEPENDS re-globs on every build, so a new file
# needs no manual cmake run; the generated list is rebuilt whenever any of
# them changes, which also refreshes the source hashes the lab counts trials by.
file(GLOB XAU_CUSTOM_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/cpp/strategies/custom/*.cpp)
set(_xau_custom_gen ${CMAKE_CURRENT_BINARY_DIR}/generated/custom_strategies.cpp)
string(REPLACE ";" "|" _xau_custom_arg "${XAU_CUSTOM_SOURCES}")
add_custom_command(
    OUTPUT ${_xau_custom_gen}
    COMMAND ${CMAKE_COMMAND} -DOUT=${_xau_custom_gen} -DSOURCES=${_xau_custom_arg}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/gen_custom.cmake
    DEPENDS ${XAU_CUSTOM_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/gen_custom.cmake
    COMMENT "Registering custom strategies"
    VERBATIM)
target_sources(xaucore PRIVATE ${XAU_CUSTOM_SOURCES} ${_xau_custom_gen})

target_include_directories(xaucore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# PRIVATE, not PUBLIC: the pybind11 module links xau::core but must not inherit
# /W4 /WX, because pybind11's own headers do not compile clean under it. Every
# first-party consumer (tests, benches, terminal) links xau_flags explicitly.
target_link_libraries(xaucore PRIVATE xau_flags)

set_target_properties(xaucore PROPERTIES
    POSITION_INDEPENDENT_CODE ON)   # it will be linked into the MT5 bridge DLL
