#include "lfq/mpmc_queue.h"

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

TEST(MpmcQueuePushTest, FirstPushSucceeds) {
    lfq::mpmc_queue<int, 8> q;
    EXPECT_TRUE(q.push(42));
}

TEST(MpmcQueuePushTest, FillsToCapacityThenReportsFull) {
    constexpr std::size_t kCapacity = 8;
    lfq::mpmc_queue<int, kCapacity> q;

    for (std::size_t i = 0; i < kCapacity; ++i) {
        EXPECT_TRUE(q.push(static_cast<int>(i))) << "push #" << i << " should have succeeded";
    }

    // The queue is now full; nothing can be pushed until something is popped
    // (pop isn't implemented yet, so it must stay full).
    EXPECT_FALSE(q.push(999));
    EXPECT_FALSE(q.push(1000));
}

TEST(MpmcQueuePushTest, MinimumCapacityOfTwoWorks) {
    lfq::mpmc_queue<int, 2> q;
    EXPECT_TRUE(q.push(1));
    EXPECT_TRUE(q.push(2));
    EXPECT_FALSE(q.push(3));
}

// Regression test for a bug where a producer with a stale (raced) view of
// tail_ would report the queue full even though most slots were empty.
// With many producers hammering a small queue, exactly `capacity` pushes
// must succeed in total -- no more (would mean overflow/corruption) and
// no fewer (would mean spurious full reports like the fixed bug).
TEST(MpmcQueuePushTest, ConcurrentPushesSucceedExactlyCapacityTimes) {
    constexpr std::size_t kCapacity = 64;
    constexpr int kProducers = 8;
    constexpr int kAttemptsPerProducer = 2000; // >> kCapacity across all producers

    lfq::mpmc_queue<int, kCapacity> q;
    std::atomic<int> success_count{0};

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kAttemptsPerProducer; ++i) {
                if (q.push(p * kAttemptsPerProducer + i)) {
                    success_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& t : producers) {
        t.join();
    }

    EXPECT_EQ(success_count.load(), static_cast<int>(kCapacity));
}

}  // namespace
