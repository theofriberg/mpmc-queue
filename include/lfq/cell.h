#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace lfq {

__extension__ using uint128_t = unsigned __int128;

template<size_t S> struct unit_value;
template<> struct unit_value<8> { using type = uint64_t; };
template<> struct unit_value<16> { using type = uint128_t; };

inline static constexpr bool CELL_USE_BUILTIN_16B =
#if defined(__GNUC__) && !defined(__clang__)
    true
#else
    false
#endif
;

namespace detail {
template<typename ValueT, typename IndexT>
struct alignas(8) helper_cell {
    ValueT d_;
    IndexT i_;
};
} // namespace detail

template<typename ValueT, typename IndexT>
using cell_value_t = typename unit_value<sizeof(detail::helper_cell<ValueT, IndexT>)>::type;

template<typename ValueT, typename IndexT>
class alignas(alignof(cell_value_t<ValueT, IndexT>)) cell {
    using value_type = ValueT;
    using index_type = IndexT;
    using cell_as_value = cell_value_t<value_type, index_type>;

    union cell_union {
        cell_as_value value_;
        struct cell_data {
            value_type data_;
            index_type seq_;
        } x_;
        cell_union() { value_ = 0; }
    } u_;


public:
    cell() noexcept { clear(); }
    explicit cell(index_type seq) noexcept { 
        clear();
        u_.x_.seq_ = seq; 
    }
    explicit cell(const value_type data, index_type seq) noexcept {
        clear();
        u_.x_.data_ = data; 
        u_.x_.seq_ = seq; 
    }
    explicit cell(cell_as_value value) {
        u_.value_ = value;
    }
    ~cell() noexcept = default;

    void clear() noexcept { u_.value_ = 0; }

    void set_seq(index_type seq) noexcept { 
        clear();
        u_.x_.seq_ = seq; 
    }
    void set(value_type data, index_type seq) noexcept {
        clear();
        u_.x_.data_ = data; 
        u_.x_.seq_ = seq; 
    }

    cell_as_value value() const noexcept { return u_.value_; }
    value_type get_data() const noexcept { return u_.x_.data_; }
    index_type get_seq() const noexcept { return u_.x_.seq_; }

    bool is_empty() const noexcept { return !(u_.x_.seq_ & 1U); }
    bool is_full() const noexcept { return (u_.x_.seq_ & 1U); }

    cell& operator=(cell_as_value value) noexcept {
        u_.value_ = value;
        return *this;
    }

    [[using gnu: hot]] cell_as_value load() const noexcept { 
        if constexpr (sizeof(cell_as_value) == 16 && CELL_USE_BUILTIN_16B) {
            return __sync_val_compare_and_swap(this->u_.value_, 0, 0);
        } else {
            return reinterpret_cast<const std::atomic<cell_as_value>*>(this)->load();
        }
    }

    [[using gnu: hot]] bool compare_exchange(cell expected, cell desired) noexcept {
        if constexpr (sizeof(cell_as_value) == 16 && CELL_USE_BUILTIN_16B) {
            return __sync_bool_compare_and_swap(&this->u_.value_, expected.u_.value_, desired.u_.value_);
        } else {
            return reinterpret_cast<std::atomic<cell_as_value>*>(this)->compare_exchange_strong(expected.u_.value_, desired.u_.value_);
        }
    }

};
} // namespace lfq