# Reporting tools. Not gates and not benchmarks: they answer questions about a
# store, and their exit codes reflect operational failure rather than verdicts.
add_executable(run_baselines run_baselines.cpp)
target_link_libraries(run_baselines PRIVATE xau::core xau_flags)

add_executable(build_dataset build_dataset.cpp)
target_link_libraries(build_dataset PRIVATE xau::core xau_flags)

add_executable(regime_report regime_report.cpp)
target_link_libraries(regime_report PRIVATE xau::core xau_flags)

add_executable(validate validate.cpp)
target_link_libraries(validate PRIVATE xau::core xau_flags)

add_executable(evaluate evaluate.cpp)
target_link_libraries(evaluate PRIVATE xau::core xau_flags)

# The strategy lab: ranks every registered strategy (custom ones included)
# honestly, and evolves the champion on live results. Uses threads.
find_package(Threads REQUIRED)
add_executable(lab lab.cpp)
if(WIN32)
    # Shipped beside the MT5 DLL and run on the trading PC, so it carries its
    # own C runtime too (see cpp/mt5/CMakeLists.txt).
    target_link_libraries(lab PRIVATE xaucore_static_crt xau_flags Threads::Threads)
    set_target_properties(lab PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_link_libraries(lab PRIVATE xau::core xau_flags Threads::Threads)
endif()
