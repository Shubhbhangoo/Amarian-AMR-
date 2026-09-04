#include <amarian/util/overflow.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>

namespace amarian {
namespace {

TEST(CheckedAdd, DetectsUnsignedOverflow) {
    constexpr uint64_t max = std::numeric_limits<uint64_t>::max();
    EXPECT_EQ(CheckedAdd<uint64_t>(1, 2), std::optional<uint64_t>(3));
    EXPECT_EQ(CheckedAdd<uint64_t>(max, 0), std::optional<uint64_t>(max));
    EXPECT_FALSE(CheckedAdd<uint64_t>(max, 1).has_value());
    EXPECT_FALSE(CheckedAdd<uint64_t>(max, max).has_value());
}

TEST(CheckedAdd, DetectsSignedOverflowBothDirections) {
    constexpr int64_t max = std::numeric_limits<int64_t>::max();
    constexpr int64_t min = std::numeric_limits<int64_t>::min();
    EXPECT_FALSE(CheckedAdd<int64_t>(max, 1).has_value());
    EXPECT_FALSE(CheckedAdd<int64_t>(min, -1).has_value());
    EXPECT_EQ(CheckedAdd<int64_t>(max, -1), std::optional<int64_t>(max - 1));
}

TEST(CheckedSub, DetectsUnsignedUnderflow) {
    EXPECT_EQ(CheckedSub<uint64_t>(5, 3), std::optional<uint64_t>(2));
    EXPECT_EQ(CheckedSub<uint64_t>(0, 0), std::optional<uint64_t>(0));
    EXPECT_FALSE(CheckedSub<uint64_t>(0, 1).has_value());
    EXPECT_FALSE(CheckedSub<uint64_t>(3, 5).has_value());
}

TEST(CheckedMul, DetectsOverflow) {
    constexpr uint64_t max = std::numeric_limits<uint64_t>::max();
    EXPECT_EQ(CheckedMul<uint64_t>(1'000'000, 1'000'000),
              std::optional<uint64_t>(1'000'000'000'000));
    EXPECT_FALSE(CheckedMul<uint64_t>(max, 2).has_value());
    EXPECT_EQ(CheckedMul<uint64_t>(max, 0), std::optional<uint64_t>(0));
    EXPECT_EQ(CheckedMul<uint64_t>(max, 1), std::optional<uint64_t>(max));
}

TEST(TryAccumulate, LeavesAccumulatorUntouchedOnOverflow) {
    constexpr uint64_t max = std::numeric_limits<uint64_t>::max();
    uint64_t acc = max - 1;
    EXPECT_TRUE(TryAccumulate<uint64_t>(acc, 1));
    EXPECT_EQ(acc, max);
    EXPECT_FALSE(TryAccumulate<uint64_t>(acc, 1));
    EXPECT_EQ(acc, max);
}

TEST(TryNarrow, RejectsOutOfRangeAndNegative) {
    EXPECT_EQ(TryNarrow<uint8_t>(255), std::optional<uint8_t>(255));
    EXPECT_FALSE(TryNarrow<uint8_t>(256).has_value());
    EXPECT_FALSE(TryNarrow<uint32_t>(int64_t{-1}).has_value());
    EXPECT_EQ(TryNarrow<int32_t>(int64_t{-1}), std::optional<int32_t>(-1));
    EXPECT_FALSE(TryNarrow<int32_t>(int64_t{2'147'483'648}).has_value());
    EXPECT_EQ(TryNarrow<uint64_t>(int64_t{0}), std::optional<uint64_t>(0));
}

// These are the properties the consensus code relies on; assert them at compile time
// so a refactor cannot quietly weaken them.
static_assert(CheckedAdd<uint64_t>(std::numeric_limits<uint64_t>::max(), 1) == std::nullopt);
static_assert(CheckedSub<uint64_t>(0, 1) == std::nullopt);
static_assert(CheckedMul<uint64_t>(uint64_t{1} << 32U, uint64_t{1} << 32U) == std::nullopt);
static_assert(TryNarrow<uint32_t>(int64_t{-1}) == std::nullopt);

}  // namespace
}  // namespace amarian
