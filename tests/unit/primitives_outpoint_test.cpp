#include <amarian/primitives/outpoint.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace amarian {
namespace {

Hash256 SequentialHash() {
    ByteVec bytes(Hash256::SIZE);
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>(i);
    }
    return Hash256::FromBytes(bytes);
}

TEST(OutPoint, SerializesAsInternalOrderHashThenLittleEndianIndex) {
    const OutPoint outpoint{.txid = SequentialHash(), .index = 0x07060504U};
    Writer writer;
    outpoint.Serialize(writer);

    ASSERT_EQ(writer.Size(), Hash256::SIZE + 4U);
    for (size_t i = 0; i < Hash256::SIZE; ++i) {
        EXPECT_EQ(writer.Bytes()[i], i);
    }
    EXPECT_EQ(writer.Bytes()[Hash256::SIZE + 0U], 0x04);
    EXPECT_EQ(writer.Bytes()[Hash256::SIZE + 1U], 0x05);
    EXPECT_EQ(writer.Bytes()[Hash256::SIZE + 2U], 0x06);
    EXPECT_EQ(writer.Bytes()[Hash256::SIZE + 3U], 0x07);
}

TEST(OutPoint, RoundTripsExactly) {
    const OutPoint expected{.txid = SequentialHash(), .index = 42};
    Writer writer;
    expected.Serialize(writer);

    Reader reader(writer.Bytes());
    OutPoint actual;
    ASSERT_TRUE(OutPoint::Deserialize(reader, actual));
    EXPECT_TRUE(reader.Finish());
    EXPECT_EQ(actual, expected);
}

TEST(OutPoint, TruncatedInputFailsWithoutPartiallyChangingTheOutput) {
    const ByteVec bytes(Hash256::SIZE + 3U, 0xAA);
    Reader reader(bytes);
    OutPoint out{.txid = SequentialHash(), .index = 123};
    const OutPoint original = out;

    EXPECT_FALSE(OutPoint::Deserialize(reader, out));
    EXPECT_EQ(out, original);
    EXPECT_FALSE(reader.Ok());
}

}  // namespace
}  // namespace amarian
