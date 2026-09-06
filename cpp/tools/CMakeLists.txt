# Reporting tools. Not gates and not benchmarks: they answer questions about a
# store, and their exit codes reflect operational failure rather than verdicts.
add_executable(run_baselines run_baselines.cpp)
target_link_libraries(run_baselines PRIVATE xau::core xau_flags)

add_executable(build_dataset build_dataset.cpp)
target_link_libraries(build_dataset PRIVATE xau::core xau_flags)

add_executable(regime_report regime_report.cpp)
target_link_libraries(regime_report PRIVATE xau::core xau_flags)
