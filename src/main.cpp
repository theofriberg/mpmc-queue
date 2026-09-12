#include <cstdio>

#include "lfq/mpmc_queue.hpp"

int main() {
    lfq::MPMCQueue<int, 1024> queue;
    (void)queue;

    std::printf("lfq::MPMCQueue<int, 1024> constructed successfully.\n");
    std::printf("try_push/try_pop are not implemented yet -- see include/lfq/mpmc_queue.hpp.\n");
    return 0;
}
