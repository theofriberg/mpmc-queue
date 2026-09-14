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

TEST(MpmcQueuePopTest, PopFromEmptyQueueFails) {
    lfq::mpmc_queue<int, 8> q;
    int value = -1;
    EXPECT_FALSE(q.pop(value));
    EXPECT_EQ(value, -1);  // untouched on failure
}

TEST(MpmcQueuePopTest, PushThenPopRoundTrips) {
    lfq::mpmc_queue<int, 8> q;
    ASSERT_TRUE(q.push(42));

    int value = -1;
    EXPECT_TRUE(q.pop(value));
    EXPECT_EQ(value, 42);
    EXPECT_FALSE(q.pop(value));  // nothing left
}

TEST(MpmcQueuePopTest, PopsInFifoOrder) {
    lfq::mpmc_queue<int, 8> q;
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(q.push(i));
    }

    for (int i = 0; i < 5; ++i) {
        int value = -1;
        EXPECT_TRUE(q.pop(value));
        EXPECT_EQ(value, i);
    }
    int value = -1;
    EXPECT_FALSE(q.pop(value));
}

// Drives the ring buffer through several full laps: fill to capacity, drain
// to empty, refill, repeat. Exercises the "recycled cell" seq transitions
// (cell::set_seq((head + capacity) << 1)) that a single fill-once test can't
// reach.
TEST(MpmcQueuePopTest, SurvivesMultipleLapsAroundTheRing) {
    constexpr std::size_t kCapacity = 4;
    lfq::mpmc_queue<int, kCapacity> q;

    for (int lap = 0; lap < 5; ++lap) {
        for (std::size_t i = 0; i < kCapacity; ++i) {
            ASSERT_TRUE(q.push(lap * 100 + static_cast<int>(i))) << "lap " << lap << " push " << i;
        }
        EXPECT_FALSE(q.push(-1)) << "lap " << lap << " should be full";

        for (std::size_t i = 0; i < kCapacity; ++i) {
            int value = -1;
            ASSERT_TRUE(q.pop(value)) << "lap " << lap << " pop " << i;
            EXPECT_EQ(value, lap * 100 + static_cast<int>(i));
        }
        int value = -1;
        EXPECT_FALSE(q.pop(value)) << "lap " << lap << " should be empty";
    }
}

// Regression test mirroring ConcurrentPushesSucceedExactlyCapacityTimes: many
// producers and many consumers hammer a small queue concurrently. Every
// value a producer manages to push must be popped by exactly one consumer,
// with no value lost, duplicated, or corrupted -- this would catch both a
// stale-head livelock/spurious-empty bug and a torn/racy snapshot read.
TEST(MpmcQueuePopTest, ConcurrentPushPopMovesEveryValueExactlyOnce) {
    constexpr std::size_t kCapacity = 64;
    constexpr int kProducers = 8;
    constexpr int kConsumers = 8;
    constexpr int kAttemptsPerProducer = 2000;
    constexpr int kTotalPushed = kProducers * kAttemptsPerProducer;

    lfq::mpmc_queue<int, kCapacity> q;
    std::atomic<int> push_success_count{0};
    std::atomic<int> pop_success_count{0};
    std::vector<std::atomic<int>> seen(kTotalPushed);
    for (auto& s : seen) {
        s.store(0, std::memory_order_relaxed);
    }

    std::vector<std::thread> threads;
    threads.reserve(kProducers + kConsumers);

    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p] {
            for (int i = 0; i < kAttemptsPerProducer; ++i) {
                while (!q.push(p * kAttemptsPerProducer + i)) {
                    std::this_thread::yield();
                }
                push_success_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (int c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&] {
            while (pop_success_count.load(std::memory_order_relaxed) < kTotalPushed) {
                int value = -1;
                if (q.pop(value)) {
                    ASSERT_GE(value, 0);
                    ASSERT_LT(value, kTotalPushed);
                    EXPECT_EQ(seen[static_cast<std::size_t>(value)].fetch_add(1, std::memory_order_relaxed), 0)
                        << "value " << value << " popped more than once";
                    pop_success_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(push_success_count.load(), kTotalPushed);
    EXPECT_EQ(pop_success_count.load(), kTotalPushed);
    for (int i = 0; i < kTotalPushed; ++i) {
        EXPECT_EQ(seen[static_cast<std::size_t>(i)].load(), 1) << "value " << i << " was never popped";
    }
}

}  // namespace
