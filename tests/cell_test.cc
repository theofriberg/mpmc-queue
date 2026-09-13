#include "lfq/cell.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace {

using SmallCell = lfq::cell<std::int32_t, std::uint32_t>;

TEST(CellTest, DefaultConstructedCellIsEmpty) {
    SmallCell c;
    EXPECT_TRUE(c.is_empty());
    EXPECT_FALSE(c.is_full());
}

TEST(CellTest, SetMarksCellFull) {
    SmallCell c;
    c.set(42, 1);  // odd sequence number => full, per is_full()/is_empty()
    EXPECT_TRUE(c.is_full());
    EXPECT_FALSE(c.is_empty());
    EXPECT_EQ(c.get_data(), 42);
    EXPECT_EQ(c.get_seq(), 1u);
}

TEST(CellTest, SetSeqClearsPreviousData) {
    SmallCell c;
    c.set(42, 1);
    c.set_seq(2);
    EXPECT_EQ(c.get_seq(), 2u);
    EXPECT_EQ(c.get_data(), 0);  // set_seq() clears the union before writing seq
}

TEST(CellTest, ClearResetsToEmpty) {
    SmallCell c(7, 3);
    c.clear();
    EXPECT_TRUE(c.is_empty());
    EXPECT_EQ(c.value(), 0u);
}

TEST(CellTest, ValueRoundTripsThroughRawAssignment) {
    SmallCell a(7, 5);
    SmallCell b;
    b = a.value();
    EXPECT_EQ(b.get_data(), a.get_data());
    EXPECT_EQ(b.get_seq(), a.get_seq());
}

TEST(CellTest, LoadReturnsCurrentRawValue) {
    SmallCell c(9, 1);
    EXPECT_EQ(c.load(), c.value());
}

TEST(CellTest, CompareExchangeSucceedsWhenExpectedMatches) {
    SmallCell c(1, 1);
    SmallCell expected(1, 1);
    SmallCell desired(2, 3);
    EXPECT_TRUE(c.compare_exchange(expected, desired));
    EXPECT_EQ(c.get_data(), 2);
    EXPECT_EQ(c.get_seq(), 3u);
}

TEST(CellTest, CompareExchangeFailsWhenExpectedDoesNotMatch) {
    SmallCell c(1, 1);
    SmallCell wrong_expected(9, 9);
    SmallCell desired(2, 3);
    EXPECT_FALSE(c.compare_exchange(wrong_expected, desired));
    EXPECT_EQ(c.get_data(), 1);
    EXPECT_EQ(c.get_seq(), 1u);
}

}  // namespace
