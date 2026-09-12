#pragma once

#include <atomic>
#include <cstddef>

namespace lfq {

namespace detail {
// Typical x86/ARM cache line size, used to keep head_/tail_ on separate
// cache lines and avoid false sharing between producer and consumer threads.
constexpr size_t kCacheLineSize = 64;
} // namespace detail

// Bounded multi-producer / multi-consumer queue.
//
// Capacity must be a power of two (so index wraparound can use a bitmask
// instead of a modulo). This is a skeleton: try_push/try_pop are declared
// but not yet implemented. See the TODO in the private section for the
// intended (Vyukov-style) algorithm shape.
template <typename T, size_t Capacity>
class MPMCQueue {
    static_assert(Capacity >= 2, "Capacity must be at least 2");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    MPMCQueue() {
        for (size_t i = 0; i < Capacity; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    MPMCQueue(const MPMCQueue&) = delete;
    MPMCQueue& operator=(const MPMCQueue&) = delete;
    MPMCQueue(MPMCQueue&&) = delete;
    MPMCQueue& operator=(MPMCQueue&&) = delete;

    // Attempts to enqueue `value`. Returns false if the queue is full.
    bool try_push(const T& value);
    bool try_push(T&& value);

    // Attempts to dequeue into `out`. Returns false if the queue is empty.
    bool try_pop(T& out);

private:
    struct Cell {
        std::atomic<size_t> sequence;
        T data;
    };

    // TODO: implement try_push/try_pop.
    //
    // Rough shape of the classic bounded MPMC ring buffer (Dmitry Vyukov's
    // algorithm, which the erez-strauss repo you're using as a reference
    // is also based on):
    //
    //   try_push(value):
    //     loop:
    //       pos  = tail_.load(memory_order_relaxed)
    //       cell = &buffer_[pos & mask_]
    //       seq  = cell->sequence.load(memory_order_acquire)
    //       diff = (intptr_t)seq - (intptr_t)pos
    //       if diff == 0:
    //         if tail_.compare_exchange_weak(pos, pos + 1, memory_order_relaxed):
    //           break  // claimed the slot
    //       else if diff < 0:
    //         return false  // queue full
    //       else:
    //         retry (another producer claimed it first)
    //     cell->data = std::forward<...>(value)
    //     cell->sequence.store(pos + 1, memory_order_release)
    //     return true
    //
    //   try_pop(out): symmetric, operating on head_ and checking
    //     diff = seq - (pos + 1), publishing cell->sequence = pos + mask_ + 1
    //     on success.
    //
    // Work out the memory ordering and the full/empty conditions yourself —
    // that's the point of this exercise.

    alignas(detail::kCacheLineSize) std::atomic<size_t> tail_{0};
    alignas(detail::kCacheLineSize) std::atomic<size_t> head_{0};

    static constexpr size_t mask_ = Capacity - 1;
    Cell buffer_[Capacity];
};

} // namespace lfq
