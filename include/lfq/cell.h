#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace lfq {

__extension__ using uint128_t = unsigned __int128;

template<size_t S> struct uint_of_size;
template<> struct uint_of_size<8> { using type = uint64_t; };
template<> struct uint_of_size<16> { using type = uint128_t; };

inline static constexpr bool CELL_USE_BUILTIN_16B =
#if defined(__GNUC__) && !defined(__clang__)
    true
#else
    false
#endif
;

namespace detail {
// Never instantiated for its data -- exists only so sizeof() can measure
// the natural combined size of a {ValueT, IndexT} pair before picking a
// packed word type for it.
template<typename ValueT, typename IndexT>
struct alignas(8) size_probe {
    ValueT d_;
    IndexT i_;
};
} // namespace detail

template<typename ValueT, typename IndexT>
using cell_word_t = typename uint_of_size<sizeof(detail::size_probe<ValueT, IndexT>)>::type;

template<typename ValueT, typename IndexT>
class alignas(alignof(cell_word_t<ValueT, IndexT>)) cell {
    using value_type = ValueT;
    using index_type = IndexT;
    using word_type = cell_word_t<value_type, index_type>;

    union storage {
        word_type word_;
        struct fields {
            value_type data_;
            index_type seq_;
        } fields_;
        storage() { word_ = 0; }
    } storage_;

    // Decodes an already-loaded word into its {data, seq} view. `raw` is a
    // local value (a snapshot from load()), so this itself never touches
    // shared memory.
    static typename storage::fields decode(word_type raw) noexcept {
        storage tmp;
        tmp.word_ = raw;
        return tmp.fields_;
    }

public:
    cell() noexcept { clear(); }
    explicit cell(index_type seq) noexcept {
        clear();
        storage_.fields_.seq_ = seq;
    }
    explicit cell(const value_type data, index_type seq) noexcept {
        clear();
        storage_.fields_.data_ = data;
        storage_.fields_.seq_ = seq;
    }
    explicit cell(word_type value) {
        storage_.word_ = value;
    }
    ~cell() noexcept = default;

    void clear() noexcept { storage_.word_ = 0; }

    void set_seq(index_type seq) noexcept {
        clear();
        storage_.fields_.seq_ = seq;
    }
    void set(value_type data, index_type seq) noexcept {
        clear();
        storage_.fields_.data_ = data;
        storage_.fields_.seq_ = seq;
    }

    // These decode a word_type already returned by load() (an atomic read
    // of the shared cell), never the live storage_ directly -- storage_ is
    // read/written non-atomically elsewhere (constructors, clear/set,
    // operator=), which is only safe on a cell not yet visible to other
    // threads. Reading storage_ directly here would race with a concurrent
    // compare_exchange()/load() on the same cell from another thread.
    word_type value() const noexcept { return load(); }
    value_type get_data() const noexcept { return decode(load()).data_; }
    index_type get_seq() const noexcept { return decode(load()).seq_; }

    bool is_empty() const noexcept { return !(get_seq() & 1U); }
    bool is_full() const noexcept { return (get_seq() & 1U); }

    cell& operator=(word_type value) noexcept {
        storage_.word_ = value;
        return *this;
    }

    [[using gnu: hot]] word_type load() const noexcept {
        if constexpr (sizeof(word_type) == 16 && CELL_USE_BUILTIN_16B) {
            return __sync_val_compare_and_swap(this->storage_.word_, 0, 0);
        } else {
            return reinterpret_cast<const std::atomic<word_type>*>(this)->load();
        }
    }

    [[using gnu: hot]] bool compare_exchange(cell expected, cell desired) noexcept {
        if constexpr (sizeof(word_type) == 16 && CELL_USE_BUILTIN_16B) {
            return __sync_bool_compare_and_swap(&this->storage_.word_, expected.storage_.word_, desired.storage_.word_);
        } else {
            return reinterpret_cast<std::atomic<word_type>*>(this)->compare_exchange_strong(expected.storage_.word_, desired.storage_.word_);
        }
    }

};
} // namespace lfq
