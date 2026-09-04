#include <amarian/primitives/lock.hpp>

#include <gtest/gtest.h>

#include <cstdint>

namespace amarian {
namespace {

TEST(Lock, SerializesVersionThenLengthPrefixedProgram) {
    const Lock lock{.version = 1, .program = {0xAA, 0xBB, 0xCC}};
    Writer writer;
    lock.Serialize(writer);
    EXPECT_EQ(writer.Bytes(), ByteVec({0x01, 0x03, 0xAA, 0xBB, 0xCC}));
}

TEST(Lock, UnknownVersionsRoundTripWithoutInterpretation) {
    const Lock expected{.version = 99, .program = {0x00, 0x01}};
    Writer writer;
    expected.Serialize(writer);
    Reader reader(writer.Bytes());
    Lock actual;
    ASSERT_TRUE(Lock::Deserialize(reader, actual, 16));
    EXPECT_TRUE(reader.Finish());
    EXPECT_EQ(actual, expected);
}

TEST(Lock, RejectsProgramAboveTheEnclosingBoundWithoutChangingOutput) {
    const ByteVec bytes{0x01, 0x03, 0xAA, 0xBB, 0xCC};
    Reader reader(bytes);
    Lock actual{.version = 7, .program = {0x55}};
    const Lock original = actual;
    EXPECT_FALSE(Lock::Deserialize(reader, actual, 2));
    EXPECT_FALSE(reader.Ok());
    EXPECT_EQ(actual, original);
}

}  // namespace
}  // namespace amarian
