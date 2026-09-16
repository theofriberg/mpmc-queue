#include <cstdio>

#include "lfq/mpmc_queue.h"

int main() {
    lfq::mpmc_queue<int, 1024> queue;
    (void)queue;

    std::printf("lfq::mpmc_queue<int, 1024> constructed successfully.\n");
    return 0;
}
