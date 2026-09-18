#include "lfq/mpmc_queue.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
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

TEST(MpmcQueuePushIndexTest, FirstPushReturnsIndexZero) {
    lfq::mpmc_queue<int, 8> q;
    std::uint32_t index = 999;
    EXPECT_TRUE(q.push(42, index));
    EXPECT_EQ(index, 0U);
}

TEST(MpmcQueuePushIndexTest, SequentialPushesReturnIncreasingIndices) {
    lfq::mpmc_queue<int, 8> q;
    for (std::uint32_t i = 0; i < 5; ++i) {
        std::uint32_t index = 999;
        ASSERT_TRUE(q.push(static_cast<int>(i), index));
        EXPECT_EQ(index, i);
    }
}

TEST(MpmcQueuePushIndexTest, IndexUntouchedWhenQueueIsFull) {
    constexpr std::size_t kCapacity = 8;
    lfq::mpmc_queue<int, kCapacity> q;
    std::uint32_t index = 999;
    for (std::size_t i = 0; i < kCapacity; ++i) {
        ASSERT_TRUE(q.push(static_cast<int>(i), index));
    }

    index = 999;
    EXPECT_FALSE(q.push(-1, index));
    EXPECT_EQ(index, 999U);  // untouched on failure
}

// The index returned to the caller is the ring's raw, ever-increasing tail
// counter (the value the algorithm CAS-checks against a cell's seq), not the
// physical slot number -- it keeps climbing past kCapacity on every lap
// instead of wrapping back into [0, kCapacity). Physical slot is index %
// kCapacity if a caller needs it.
TEST(MpmcQueuePushIndexTest, IndexKeepsIncreasingAcrossMultipleLapsAroundTheRing) {
    constexpr std::size_t kCapacity = 4;
    lfq::mpmc_queue<int, kCapacity> q;

    for (std::uint32_t lap = 0; lap < 5; ++lap) {
        for (std::size_t i = 0; i < kCapacity; ++i) {
            std::uint32_t index = 999;
            ASSERT_TRUE(q.push(static_cast<int>(i), index)) << "lap " << lap << " push " << i;
            EXPECT_EQ(index, static_cast<std::uint32_t>(lap * kCapacity + i)) << "lap " << lap << " push " << i;
        }
        for (std::size_t i = 0; i < kCapacity; ++i) {
            int value = -1;
            ASSERT_TRUE(q.pop(value));
        }
    }
}

// Regression-style test mirroring ConcurrentPushesSucceedExactlyCapacityTimes:
// with many producers racing to CAS the same cells, every index handed back
// on a successful push must be globally unique -- a duplicate would mean two
// producers were told they both won the same cell.
TEST(MpmcQueuePushIndexTest, ConcurrentPushesReturnGloballyUniqueIndices) {
    constexpr std::size_t kCapacity = 64;
    constexpr int kProducers = 8;
    constexpr int kAttemptsPerProducer = 2000;

    lfq::mpmc_queue<int, kCapacity> q;
    std::mutex mtx;
    std::unordered_set<std::uint32_t> seen_indices;

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kAttemptsPerProducer; ++i) {
                std::uint32_t index = 0;
                if (q.push(p * kAttemptsPerProducer + i, index)) {
                    std::lock_guard<std::mutex> lock(mtx);
                    EXPECT_TRUE(seen_indices.insert(index).second) << "duplicate index " << index;
                }
            }
        });
    }
    for (auto& t : producers) {
        t.join();
    }

    EXPECT_EQ(seen_indices.size(), kCapacity);
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

TEST(MpmcQueuePopIndexTest, PopFromEmptyQueueLeavesIndexUntouched) {
    lfq::mpmc_queue<int, 8> q;
    int value = -1;
    std::uint32_t index = 999;
    EXPECT_FALSE(q.pop(value, index));
    EXPECT_EQ(index, 999U);  // untouched on failure
}

TEST(MpmcQueuePopIndexTest, PushThenPopReturnsMatchingIndex) {
    lfq::mpmc_queue<int, 8> q;
    std::uint32_t push_index = 999;
    ASSERT_TRUE(q.push(42, push_index));

    int value = -1;
    std::uint32_t pop_index = 999;
    EXPECT_TRUE(q.pop(value, pop_index));
    EXPECT_EQ(value, 42);
    EXPECT_EQ(pop_index, push_index);
}

// head_ and tail_ are both raw, ever-increasing counters starting at 0, and
// the queue is strictly FIFO, so the k-th successful pop must be handed back
// the exact same index the k-th successful push was handed -- not just the
// same value.
TEST(MpmcQueuePopIndexTest, PopsReturnIndicesMatchingOriginalPushIndices) {
    lfq::mpmc_queue<int, 8> q;
    std::vector<std::uint32_t> push_indices;
    for (int i = 0; i < 5; ++i) {
        std::uint32_t index = 999;
        ASSERT_TRUE(q.push(i, index));
        push_indices.push_back(index);
    }

    for (int i = 0; i < 5; ++i) {
        int value = -1;
        std::uint32_t index = 999;
        EXPECT_TRUE(q.pop(value, index));
        EXPECT_EQ(value, i);
        EXPECT_EQ(index, push_indices[static_cast<std::size_t>(i)]);
    }
}

TEST(MpmcQueuePopIndexTest, IndexKeepsIncreasingAcrossMultipleLapsAroundTheRing) {
    constexpr std::size_t kCapacity = 4;
    lfq::mpmc_queue<int, kCapacity> q;

    for (std::uint32_t lap = 0; lap < 5; ++lap) {
        for (std::size_t i = 0; i < kCapacity; ++i) {
            ASSERT_TRUE(q.push(static_cast<int>(i)));
        }
        for (std::size_t i = 0; i < kCapacity; ++i) {
            int value = -1;
            std::uint32_t index = 999;
            ASSERT_TRUE(q.pop(value, index)) << "lap " << lap << " pop " << i;
            EXPECT_EQ(index, static_cast<std::uint32_t>(lap * kCapacity + i)) << "lap " << lap << " pop " << i;
        }
    }
}

// Regression-style test mirroring ConcurrentPushPopMovesEveryValueExactlyOnce:
// many consumers race to CAS the same cells, so every index handed back on a
// successful pop must be globally unique -- a duplicate would mean two
// consumers were told they both drained the same cell.
TEST(MpmcQueuePopIndexTest, ConcurrentPopsReturnGloballyUniqueIndices) {
    constexpr std::size_t kCapacity = 64;
    constexpr int kProducers = 8;
    constexpr int kConsumers = 8;
    constexpr int kAttemptsPerProducer = 2000;
    constexpr int kTotalPushed = kProducers * kAttemptsPerProducer;

    lfq::mpmc_queue<int, kCapacity> q;
    std::atomic<int> pop_success_count{0};
    std::mutex mtx;
    std::unordered_set<std::uint32_t> seen_indices;

    std::vector<std::thread> threads;
    threads.reserve(kProducers + kConsumers);

    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p] {
            for (int i = 0; i < kAttemptsPerProducer; ++i) {
                while (!q.push(p * kAttemptsPerProducer + i)) {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (int c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&] {
            while (pop_success_count.load(std::memory_order_relaxed) < kTotalPushed) {
                int value = -1;
                std::uint32_t index = 0;
                if (q.pop(value, index)) {
                    {
                        std::lock_guard<std::mutex> lock(mtx);
                        EXPECT_TRUE(seen_indices.insert(index).second) << "duplicate index " << index;
                    }
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

    EXPECT_EQ(seen_indices.size(), static_cast<std::size_t>(kTotalPushed));
}

TEST(MpmcQueuePeekTest, PeekOnEmptyQueueFails) {
    lfq::mpmc_queue<int, 8> q;
    int value = -1;
    EXPECT_FALSE(q.peek(value));
    EXPECT_EQ(value, -1);  // untouched on failure
}

TEST(MpmcQueuePeekTest, PeekReturnsFrontValueWithoutRemovingIt) {
    lfq::mpmc_queue<int, 8> q;
    ASSERT_TRUE(q.push(42));

    int value = -1;
    EXPECT_TRUE(q.peek(value));
    EXPECT_EQ(value, 42);

    // peek() must not have consumed the value -- it's still there to pop.
    value = -1;
    EXPECT_TRUE(q.pop(value));
    EXPECT_EQ(value, 42);
    EXPECT_FALSE(q.pop(value));
}

TEST(MpmcQueuePeekTest, RepeatedPeeksReturnSameFrontValue) {
    lfq::mpmc_queue<int, 8> q;
    ASSERT_TRUE(q.push(7));
    ASSERT_TRUE(q.push(8));

    int value = -1;
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(q.peek(value));
        EXPECT_EQ(value, 7);  // still the front -- nothing consumed it
    }
}

TEST(MpmcQueuePeekTest, PeekTracksFrontAcrossPops) {
    lfq::mpmc_queue<int, 8> q;
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(q.push(i));
    }

    for (int i = 0; i < 3; ++i) {
        int value = -1;
        EXPECT_TRUE(q.peek(value));
        EXPECT_EQ(value, i);

        int popped = -1;
        EXPECT_TRUE(q.pop(popped));
        EXPECT_EQ(popped, i);
    }
    int value = -1;
    EXPECT_FALSE(q.peek(value));
}

// Exercises the "recycled cell" branch of peek() (a cell already popped by
// another consumer, seq == (head + capacity) << 1), which push()/pop() also
// hit but peek() must handle without ever CAS-ing the cell itself.
TEST(MpmcQueuePeekTest, PeekSurvivesMultipleLapsAroundTheRing) {
    constexpr std::size_t kCapacity = 4;
    lfq::mpmc_queue<int, kCapacity> q;

    for (int lap = 0; lap < 5; ++lap) {
        for (std::size_t i = 0; i < kCapacity; ++i) {
            ASSERT_TRUE(q.push(lap * 100 + static_cast<int>(i))) << "lap " << lap << " push " << i;
        }
        for (std::size_t i = 0; i < kCapacity; ++i) {
            int peeked = -1;
            EXPECT_TRUE(q.peek(peeked)) << "lap " << lap << " peek " << i;
            EXPECT_EQ(peeked, lap * 100 + static_cast<int>(i));

            int popped = -1;
            ASSERT_TRUE(q.pop(popped)) << "lap " << lap << " pop " << i;
            EXPECT_EQ(popped, lap * 100 + static_cast<int>(i));
        }
        int value = -1;
        EXPECT_FALSE(q.peek(value)) << "lap " << lap << " should be empty";
    }
}

// Concurrent producers and consumers, with every consumer peek()-ing
// (without side effects on the popped count) immediately before pop()-ing.
// If peek() ever advanced past a live, unconsumed element, or corrupted
// state shared with pop()/push() under contention, this would show up as a
// wrong total or a duplicate/lost value -- same invariant as
// ConcurrentPushPopMovesEveryValueExactlyOnce, with peek() added to the mix.
TEST(MpmcQueuePeekTest, ConcurrentPeekAndPopDoNotLoseOrDuplicateValues) {
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
                int peeked = -1;
                q.peek(peeked);  // result ignored -- just exercising it concurrently

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

TEST(MpmcQueuePeekIndexTest, PeekOnEmptyQueueLeavesIndexUntouched) {
    lfq::mpmc_queue<int, 8> q;
    int value = -1;
    std::uint32_t index = 999;
    EXPECT_FALSE(q.peek(value, index));
    EXPECT_EQ(index, 999U);  // untouched on failure
}

TEST(MpmcQueuePeekIndexTest, PeekReturnsSameIndexAsSubsequentPop) {
    lfq::mpmc_queue<int, 8> q;
    std::uint32_t push_index = 999;
    ASSERT_TRUE(q.push(42, push_index));

    int peeked = -1;
    std::uint32_t peek_index = 999;
    EXPECT_TRUE(q.peek(peeked, peek_index));
    EXPECT_EQ(peek_index, push_index);

    int popped = -1;
    std::uint32_t pop_index = 999;
    EXPECT_TRUE(q.pop(popped, pop_index));
    EXPECT_EQ(pop_index, peek_index);
}

TEST(MpmcQueuePeekIndexTest, RepeatedPeeksReturnSameIndex) {
    lfq::mpmc_queue<int, 8> q;
    ASSERT_TRUE(q.push(7));
    ASSERT_TRUE(q.push(8));

    int value = -1;
    std::uint32_t index = 999;
    EXPECT_TRUE(q.peek(value, index));
    for (int i = 0; i < 3; ++i) {
        int repeat_value = -1;
        std::uint32_t repeat_index = 999;
        EXPECT_TRUE(q.peek(repeat_value, repeat_index));
        EXPECT_EQ(repeat_value, value);
        EXPECT_EQ(repeat_index, index);
    }
}

TEST(MpmcQueuePeekIndexTest, PeekIndexTracksFrontAcrossPops) {
    lfq::mpmc_queue<int, 8> q;
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(q.push(i));
    }

    for (int i = 0; i < 3; ++i) {
        int peeked = -1;
        std::uint32_t peek_index = 999;
        EXPECT_TRUE(q.peek(peeked, peek_index));

        int popped = -1;
        std::uint32_t pop_index = 999;
        EXPECT_TRUE(q.pop(popped, pop_index));
        EXPECT_EQ(pop_index, peek_index);
    }
}

TEST(MpmcQueueEmptyTest, DefaultConstructedQueueIsEmpty) {
    lfq::mpmc_queue<int, 8> q;
    EXPECT_TRUE(q.empty());
}

TEST(MpmcQueueEmptyTest, NotEmptyAfterPush) {
    lfq::mpmc_queue<int, 8> q;
    ASSERT_TRUE(q.push(1));
    EXPECT_FALSE(q.empty());
}

TEST(MpmcQueueEmptyTest, EmptyAgainAfterPushThenPop) {
    lfq::mpmc_queue<int, 8> q;
    ASSERT_TRUE(q.push(1));

    int value = -1;
    ASSERT_TRUE(q.pop(value));
    EXPECT_TRUE(q.empty());
}

TEST(MpmcQueueEmptyTest, NotEmptyUntilAllElementsArePopped) {
    lfq::mpmc_queue<int, 8> q;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.push(i));
    }

    for (int i = 0; i < 4; ++i) {
        EXPECT_FALSE(q.empty()) << "should still report non-empty before pop " << i;
        int value = -1;
        ASSERT_TRUE(q.pop(value));
    }
    EXPECT_TRUE(q.empty());
}

TEST(MpmcQueueEmptyTest, TracksEmptinessAcrossMultipleLapsAroundTheRing) {
    constexpr std::size_t kCapacity = 4;
    lfq::mpmc_queue<int, kCapacity> q;

    for (int lap = 0; lap < 5; ++lap) {
        EXPECT_TRUE(q.empty()) << "lap " << lap << " should start empty";
        for (std::size_t i = 0; i < kCapacity; ++i) {
            ASSERT_TRUE(q.push(lap * 100 + static_cast<int>(i)));
            EXPECT_FALSE(q.empty()) << "lap " << lap << " push " << i;
        }
        for (std::size_t i = 0; i < kCapacity; ++i) {
            int value = -1;
            ASSERT_TRUE(q.pop(value));
        }
    }
    EXPECT_TRUE(q.empty());
}

// Concurrent producers and consumers, mirroring the other Concurrent* tests:
// only the final state (after every thread has joined) is checked, since
// empty() is inherently a racy, instant-in-time snapshot while other threads
// are actively pushing/popping -- there is no promise it's accurate mid-flight.
TEST(MpmcQueueEmptyTest, ReportsEmptyOnceAllConcurrentWorkIsDrained) {
    constexpr std::size_t kCapacity = 64;
    constexpr int kProducers = 8;
    constexpr int kConsumers = 8;
    constexpr int kAttemptsPerProducer = 2000;
    constexpr int kTotalPushed = kProducers * kAttemptsPerProducer;

    lfq::mpmc_queue<int, kCapacity> q;
    std::atomic<int> pop_success_count{0};

    std::vector<std::thread> threads;
    threads.reserve(kProducers + kConsumers);

    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&] {
            for (int i = 0; i < kAttemptsPerProducer; ++i) {
                while (!q.push(i)) {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (int c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&] {
            while (pop_success_count.load(std::memory_order_relaxed) < kTotalPushed) {
                int value = -1;
                if (q.pop(value)) {
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

    EXPECT_TRUE(q.empty());
}

// lazy_push/lazy_pop defer the tail_/head_ advancement CAS: a successful
// push()/pop() that claims/releases a cell doesn't also bump the shared
// index right away, leaving it to be noticed and bumped by a later call
// (see the "cell is full OR someone has already popped this cell" /
// "recycled" branches in push()/pop()). This should be entirely an internal
// performance detail -- externally-observable behavior (FIFO order,
// fullness/emptiness, exactly-once delivery) must be identical regardless
// of which combination of the two flags is set. The following tests run
// the same checks used above for the default (eager) queue against all four
// lazy_push/lazy_pop combinations via a typed test suite.
template <typename QueueT>
class MpmcQueueLazyTest : public ::testing::Test {};

using LazyQueueVariants = ::testing::Types<lfq::mpmc_queue<int, 4, std::uint32_t, false, false>,
                                            lfq::mpmc_queue<int, 4, std::uint32_t, true, false>,
                                            lfq::mpmc_queue<int, 4, std::uint32_t, false, true>,
                                            lfq::mpmc_queue<int, 4, std::uint32_t, true, true>>;

struct LazyQueueVariantNames {
    template <typename T>
    static std::string GetName(int i) {
        switch (i) {
            case 0: return "EagerPushEagerPop";
            case 1: return "LazyPushEagerPop";
            case 2: return "EagerPushLazyPop";
            case 3: return "LazyPushLazyPop";
            default: return std::to_string(i);
        }
    }
};

TYPED_TEST_SUITE(MpmcQueueLazyTest, LazyQueueVariants, LazyQueueVariantNames);

TYPED_TEST(MpmcQueueLazyTest, PushThenPopRoundTrips) {
    TypeParam q;
    ASSERT_TRUE(q.push(42));

    int value = -1;
    EXPECT_TRUE(q.pop(value));
    EXPECT_EQ(value, 42);
    EXPECT_FALSE(q.pop(value));  // nothing left
}

TYPED_TEST(MpmcQueueLazyTest, PopsInFifoOrder) {
    TypeParam q;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.push(i));
    }

    for (int i = 0; i < 4; ++i) {
        int value = -1;
        EXPECT_TRUE(q.pop(value));
        EXPECT_EQ(value, i);
    }
    int value = -1;
    EXPECT_FALSE(q.pop(value));
}

TYPED_TEST(MpmcQueueLazyTest, FillsToCapacityThenReportsFull) {
    TypeParam q;
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(q.push(i)) << "push #" << i << " should have succeeded";
    }
    // A deferred tail_ bump (lazy_push) must not make the fullness check see
    // a stale, not-yet-advanced tail_ as "still room here".
    EXPECT_FALSE(q.push(999));
}

// empty() has no equivalent of pop()/peek()'s "recycled cell" catch-up
// branch -- it never nudges a stale head_ forward. With lazy_pop, every
// successful pop() leaves head_ pointing at the *previous* round instead of
// the one it just claimed (the next call's own catch-up step is what
// belatedly corrects it -- see the pop() review). If nothing calls
// pop()/peek() again after the last element is drained, head_ never gets
// that final nudge, so a queue that is genuinely, permanently empty can
// still read as non-empty. This drains serially with no further pop()/peek()
// calls afterwards, so it isn't racy -- it's a deterministic check of
// whatever state pop() leaves head_ in.
TYPED_TEST(MpmcQueueLazyTest, EmptyReportsTrueAfterFullyDraining) {
    constexpr int kCapacity = 4;
    TypeParam q;

    for (int i = 0; i < kCapacity; ++i) {
        ASSERT_TRUE(q.push(i));
    }
    for (int i = 0; i < kCapacity; ++i) {
        int value = -1;
        ASSERT_TRUE(q.pop(value));
    }

    EXPECT_TRUE(q.empty());
}

// Drives the ring through several full laps of push/peek/pop, the same way
// the non-lazy MpmcQueuePopTest.SurvivesMultipleLapsAroundTheRing and
// MpmcQueuePeekTest.PeekSurvivesMultipleLapsAroundTheRing do. This is the
// case most likely to expose a lazy-specific bug: each lap forces some call
// to notice a stale tail_/head_ left behind by the previous lap's deferred
// bump and catch it up before wraparound reuses that slot.
TYPED_TEST(MpmcQueueLazyTest, SurvivesMultipleLapsAroundTheRing) {
    constexpr int kCapacity = 4;
    TypeParam q;

    for (int lap = 0; lap < 5; ++lap) {
        for (int i = 0; i < kCapacity; ++i) {
            ASSERT_TRUE(q.push(lap * 100 + i)) << "lap " << lap << " push " << i;
        }
        EXPECT_FALSE(q.push(-1)) << "lap " << lap << " should be full";

        for (int i = 0; i < kCapacity; ++i) {
            int peeked = -1;
            EXPECT_TRUE(q.peek(peeked)) << "lap " << lap << " peek " << i;
            EXPECT_EQ(peeked, lap * 100 + i);

            int popped = -1;
            ASSERT_TRUE(q.pop(popped)) << "lap " << lap << " pop " << i;
            EXPECT_EQ(popped, lap * 100 + i);
        }
        int value = -1;
        EXPECT_FALSE(q.pop(value)) << "lap " << lap << " should be empty";
    }
}

// Regression test mirroring ConcurrentPushPopMovesEveryValueExactlyOnce for
// each lazy_push/lazy_pop combination: many producers and consumers hammer
// a small queue concurrently, and every pushed value must be popped by
// exactly one consumer. This is what would catch a lazy-specific
// concurrency bug (e.g. a permanently-stale tail_/head_ under contention)
// that a single-threaded test can't reach.
TYPED_TEST(MpmcQueueLazyTest, ConcurrentPushPopMovesEveryValueExactlyOnce) {
    constexpr int kProducers = 8;
    constexpr int kConsumers = 8;
    constexpr int kAttemptsPerProducer = 1000;
    constexpr int kTotalPushed = kProducers * kAttemptsPerProducer;

    TypeParam q;
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
