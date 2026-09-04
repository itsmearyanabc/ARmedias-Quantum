# Reporting tools. Not gates and not benchmarks: they answer questions about a
# store, and their exit codes reflect operational failure rather than verdicts.
add_executable(run_baselines run_baselines.cpp)
target_link_libraries(run_baselines PRIVATE xau::core xau_flags)
