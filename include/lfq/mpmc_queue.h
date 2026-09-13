#pragma once

#include <atomic>
#include <cstddef>

#include "cell.h"
#include "inplace_array.h"

namespace lfq {

namespace detail {
// Typical x86/ARM cache line size, used to keep head_/tail_ on separate
// cache lines and avoid false sharing between producer and consumer threads.
inline static constexpr size_t CACHELINE_SIZE = 64;   
} // namespace detail

// Bounded multi-producer / multi-consumer queue.
//
// Capacity must be a power of two (so index wraparound can use a bitmask
// instead of a modulo). This is a skeleton: try_push/try_pop are declared
// but not yet implemented. See the TODO in the private section for the
// intended (Vyukov-style) algorithm shape.
template <typename ValueT, size_t capacity, typename IndexT = uint32_t>
class mpmc_queue {
    using value_type = ValueT;
    using index_type = IndexT;
    using cell_type = cell<value_type, index_type>;
    using array_type = inplace_array<cell_type, capacity>;

    static_assert(capacity >= 2, "Capacity must be at least 2");
    static_assert((capacity & (capacity - 1)) == 0, "Capacity must be a power of two");

public:
    mpmc_queue() {
        for (size_t i = 0; i < capacity; ++i) {
            buffer_[i].set_seq(static_cast<index_type>(i << 1));
        }
    }

    mpmc_queue(const mpmc_queue&) = delete;
    mpmc_queue& operator=(const mpmc_queue&) = delete;
    mpmc_queue(mpmc_queue&&) = delete;
    mpmc_queue& operator=(mpmc_queue&&) = delete;

    [[using gnu: hot]] bool push(value_type value) noexcept {
        while (true) {
            index_type tail = tail_.load();
            index_type seq = buffer_[tail].get_seq();

            if (seq == static_cast<index_type>(tail << 1)) { // happy path: cell is empty
                cell_type expected{static_cast<index_type>(tail << 1)};
                cell_type desired{value, static_cast<index_type>((tail << 1) | 1U)};
                if (buffer_[tail].compare_exchange(expected, desired)) {
                    tail_.compare_exchange_strong(tail, tail + 1); // Consider using compare_exchange_weak since we're already in retry loop
                    return true;
                }
            } else if (seq == static_cast<index_type>((tail << 1) | 1U) ||
                       seq == static_cast<index_type>((tail + buffer_.size()) << 1)) { // cell is full OR someone has already popped this cell (cell is ready for next lap)
                    tail_.compare_exchange_strong(tail, tail + 1); // Consider using compare_exchange_weak since we're already in retry loop
            } else if (static_cast<index_type>(seq + (buffer_.size() << 1)) == static_cast<index_type>((tail << 1) | 1U)) {
                return false; // queue is full
            }
        }
    }

private:
    alignas(detail::CACHELINE_SIZE) std::atomic<index_type> tail_{0};
    alignas(detail::CACHELINE_SIZE) std::atomic<index_type> head_{0};
    array_type buffer_;
};

} // namespace lfq
