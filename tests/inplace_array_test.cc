#include "lfq/inplace_array.h"

#include <cstddef>

#include <gtest/gtest.h>

namespace {

TEST(InplaceArrayTest, SizeAndCapacityMatchTemplateParameter) {
    lfq::inplace_array<int, 8> arr;
    EXPECT_EQ(arr.size(), 8u);
    EXPECT_EQ(arr.capacity(), 8u);
}

TEST(InplaceArrayTest, IndexMaskIsCapacityMinusOne) {
    lfq::inplace_array<int, 8> arr;
    EXPECT_EQ(arr.index_mask(), 7u);
}

TEST(InplaceArrayTest, DefaultConstructedElementsAreZeroInitialized) {
    lfq::inplace_array<int, 4> arr;
    for (std::size_t i = 0; i < arr.size(); ++i) {
        EXPECT_EQ(arr[i], 0);
    }
}

TEST(InplaceArrayTest, WriteThenReadSameIndex) {
    lfq::inplace_array<int, 8> arr;
    arr[3] = 42;
    EXPECT_EQ(arr[3], 42);
}

TEST(InplaceArrayTest, IndexWrapsAroundModuloCapacity) {
    lfq::inplace_array<int, 8> arr;
    arr[2] = 99;
    // 2 + 8 == 10 must map to the same underlying slot as index 2.
    EXPECT_EQ(arr[10], 99);

    arr[10] = 7;
    EXPECT_EQ(arr[2], 7);
}

TEST(InplaceArrayTest, ConstOperatorIndexAlsoWraps) {
    lfq::inplace_array<int, 4> arr;
    arr[1] = 5;
    const auto& const_arr = arr;
    EXPECT_EQ(const_arr[5], 5); // 5 & 3 == 1
}

}  // namespace
