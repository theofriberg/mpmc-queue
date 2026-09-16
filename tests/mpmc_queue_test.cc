#include "lfq/mpmc_queue.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
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
