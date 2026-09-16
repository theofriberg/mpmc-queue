add_executable(lfq_benchmarks
    mpmc_queue_benchmark.cc
)
target_link_libraries(lfq_benchmarks PRIVATE lfq benchmark::benchmark Threads::Threads)
target_compile_options(lfq_benchmarks PRIVATE -Wall -Wextra -Wpedantic)
