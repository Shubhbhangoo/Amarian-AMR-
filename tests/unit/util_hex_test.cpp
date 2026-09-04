#include <amarian/util/hex.hpp>
#include <amarian/util/types.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace amarian {
namespace {

TEST(Hex, EncodesLowercaseWithoutSeparators) {
    const ByteVec bytes{0x00, 0x0f, 0x10, 0xff, 0xab};
    EXPECT_EQ(ToHex(bytes), "000f10ffab");
}

TEST(Hex, EncodesEmptyAsEmpty) {
    EXPECT_EQ(ToHex(ByteVec{}), "");
}

TEST(Hex, RoundTrips) {
    const ByteVec bytes{0xde, 0xad, 0xbe, 0xef, 0x00, 0x01};
    const auto decoded = FromHex(ToHex(bytes));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, bytes);
}

TEST(Hex, AcceptsUppercase) {
    const auto decoded = FromHex("DEADbeef");
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, ByteVec({0xde, 0xad, 0xbe, 0xef}));
}

TEST(Hex, RejectsOddLength) {
    EXPECT_FALSE(FromHex("abc").has_value());
    EXPECT_FALSE(FromHex("0").has_value());
}

TEST(Hex, RejectsNonHexCharacters) {
    EXPECT_FALSE(FromHex("00zz").has_value());
    EXPECT_FALSE(FromHex("0x00").has_value());
    EXPECT_FALSE(FromHex("00 11").has_value());
    EXPECT_FALSE(FromHex("00\n").has_value());
    EXPECT_FALSE(FromHex("00-11").has_value());
}

TEST(Hex, ExactLengthIsEnforced) {
    EXPECT_TRUE(FromHexExact("0011", 2).has_value());
    EXPECT_FALSE(FromHexExact("0011", 3).has_value());
    EXPECT_FALSE(FromHexExact("0011", 1).has_value());
}

TEST(Hash256, DefaultIsZero) {
    const Hash256 h;
    EXPECT_TRUE(h.IsZero());
    EXPECT_EQ(h.ToHexInternal(), std::string(64, '0'));
}

TEST(Hash256, DisplayHexIsByteReversed) {
    // Internal order 0x01 0x02 ... 0x20 displays as 0x20 ... 0x02 0x01.
    ByteVec bytes(Hash256::SIZE);
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>(i + 1);
    }
    const Hash256 h = Hash256::FromBytes(bytes);
    EXPECT_EQ(h.ToHexInternal(), "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20");
    EXPECT_EQ(h.ToHex(), "201f1e1d1c1b1a191817161514131211100f0e0d0c0b0a090807060504030201");
}

TEST(Hash256, HexRoundTripsInBothOrders) {
    ByteVec bytes(Hash256::SIZE, 0xAB);
    bytes[0] = 0x01;
    bytes[31] = 0xFE;
    const Hash256 h = Hash256::FromBytes(bytes);

    const auto internal = Hash256FromHexInternal(h.ToHexInternal());
    ASSERT_TRUE(internal.has_value());
    EXPECT_EQ(*internal, h);

    const auto display = Hash256FromHex(h.ToHex());
    ASSERT_TRUE(display.has_value());
    EXPECT_EQ(*display, h);
}

TEST(Hash256, RejectsWrongLengthHex) {
    EXPECT_FALSE(Hash256FromHex(std::string(62, '0')).has_value());
    EXPECT_FALSE(Hash256FromHex(std::string(66, '0')).has_value());
}

TEST(Hash256, OrdersLexicographicallyOnInternalBytes) {
    ByteVec a(Hash256::SIZE, 0x00);
    ByteVec b(Hash256::SIZE, 0x00);
    b[0] = 0x01;
    EXPECT_LT(Hash256::FromBytes(a), Hash256::FromBytes(b));
    EXPECT_NE(Hash256::FromBytes(a), Hash256::FromBytes(b));
}

TEST(ConstantTimeEqual, MatchesSemanticsOfMemcmp) {
    const ByteVec a{1, 2, 3, 4};
    const ByteVec b{1, 2, 3, 4};
    const ByteVec c{1, 2, 3, 5};
    const ByteVec shorter{1, 2, 3};
    EXPECT_TRUE(ConstantTimeEqual(a, b));
    EXPECT_FALSE(ConstantTimeEqual(a, c));
    EXPECT_FALSE(ConstantTimeEqual(a, shorter));
    EXPECT_TRUE(ConstantTimeEqual(ByteVec{}, ByteVec{}));
}

}  // namespace
}  // namespace amarian
