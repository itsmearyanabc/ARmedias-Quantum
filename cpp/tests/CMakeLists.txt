add_executable(xau_tests
    harness.cpp
    test_tick_store.cpp
    test_bar.cpp
    test_engine.cpp
    test_rules.cpp
    test_leakage.cpp
    test_validation.cpp
)

target_include_directories(xau_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xau_tests PRIVATE xau::core xau_flags)

add_test(NAME xau_tests COMMAND xau_tests)
