#include <cstdio>

#include "lfq/mpmc_queue.h"

int main() {
    lfq::mpmc_queue<int, 1024> queue;
    (void)queue;

    std::printf("lfq::mpmc_queue<int, 1024> constructed successfully.\n");
    std::printf("try_push/try_pop are not implemented yet -- see include/lfq/mpmc_queue.h.\n");
    return 0;
}
