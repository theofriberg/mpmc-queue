add_executable(lfq_tests
    inplace_array_test.cc
    cell_test.cc
    mpmc_queue_test.cc
)
target_link_libraries(lfq_tests PRIVATE lfq GTest::gtest_main Threads::Threads)
target_compile_options(lfq_tests PRIVATE -Wall -Wextra -Wpedantic)

include(GoogleTest)
gtest_discover_tests(lfq_tests)
