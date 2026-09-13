#pragma once

#include <cstddef>
#include <array>

namespace lfq {

template<typename T, size_t N>
class inplace_array {
    static_assert(N > 0 && (N & (N - 1)) == 0, "N must be a power of two and greater than zero");

public:
    explicit inplace_array(size_t = N) : data_() {}
    ~inplace_array() noexcept = default;
    T& operator[](size_t index) noexcept { return data_[index & (N - 1)]; }
    const T& operator[](size_t index) const noexcept { return data_[index & (N - 1)]; }
    [[nodiscard]] constexpr size_t size() const noexcept { return N; }
    [[nodiscard]] constexpr size_t capacity() const noexcept { return N; }
    [[nodiscard]] constexpr size_t index_mask() const noexcept { return N - 1; }

private:
    std::array<T, N> data_;

};
} // namespace lfq