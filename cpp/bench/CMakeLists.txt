add_executable(bench_tick_store bench_tick_store.cpp)
target_link_libraries(bench_tick_store PRIVATE xau::core xau_flags)

# Benchmarks are meaningless in a Debug build; make that hard to get wrong.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "bench: Debug build — throughput numbers will not be meaningful")
endif()
