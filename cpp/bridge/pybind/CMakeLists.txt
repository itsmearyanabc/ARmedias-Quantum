# xaucore — the Python extension module.
#
# Optional and self-disabling: a machine without Python development headers
# still builds the engine, the tests, the benches and the terminal. Only the
# research bindings go missing, and CMake says so rather than failing.

find_package(Python 3.9 COMPONENTS Interpreter Development.Module QUIET)

if(NOT Python_FOUND OR NOT TARGET Python::Module)
    message(STATUS "xaucore python module: skipped (no Python Development.Module found)")
    return()
endif()

include(FetchContent)
FetchContent_Declare(pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG        v2.13.6
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(pybind11)

pybind11_add_module(xaucore_py MODULE module.cpp)

# ARCHIVE too, not just LIBRARY/RUNTIME. A .pyd exports PyInit_xaucore, so MSVC
# emits an import library beside it — which would land in the shared lib/ output
# directory as xaucore.lib and collide with the static core library of the same
# name (LNK1149). Keeping all of the module's outputs together avoids it.
set_target_properties(xaucore_py PROPERTIES
    OUTPUT_NAME              xaucore
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/python"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/python"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/python")

# Deliberately NOT xau_flags: pybind11's headers do not compile clean under
# /W4 /WX, and failing our build over someone else's warnings helps nobody.
target_link_libraries(xaucore_py PRIVATE xau::core)

message(STATUS "xaucore python module: ${Python_VERSION} -> ${CMAKE_BINARY_DIR}/python")
