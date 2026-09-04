add_library(xaucore STATIC
    src/mapped_file.cpp
    src/tick_store.cpp
    src/bar.cpp
)

add_library(xau::core ALIAS xaucore)

target_include_directories(xaucore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(xaucore PUBLIC xau_flags)

set_target_properties(xaucore PROPERTIES
    POSITION_INDEPENDENT_CODE ON)   # it will be linked into the MT5 bridge DLL
