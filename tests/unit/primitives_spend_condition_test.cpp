#include <amarian/primitives/spend_condition.hpp>
#include <amarian/primitives/witness.hpp>

#include <gtest/gtest.h>

#include <cstdint>

namespace amarian {
namespace {

TEST(PublicKey, SerializesSchemeLittleEndianThenLengthPrefixedBytes) {
    const PublicKey key{.scheme = 0x0201, .bytes = {0xAA, 0xBB, 0xCC}};
    Writer writer;
    key.Serialize(writer);
    EXPECT_EQ(writer.Bytes(), ByteVec({0x01, 0x02, 0x03, 0xAA, 0xBB, 0xCC}));
}

TEST(PublicKey, RoundTripsAndEnforcesTheKeySizeBound) {
    const PublicKey expected{.scheme = 1, .bytes = {0x00, 0x01, 0x02, 0x03}};
    Writer writer;
    expected.Serialize(writer);
    Reader reader(writer.Bytes());
    PublicKey actual;
    ASSERT_TRUE(PublicKey::Deserialize(reader, actual, 16));
    EXPECT_TRUE(reader.Finish());
    EXPECT_EQ(actual, expected);

    const ByteVec too_long{0x01, 0x00, 0x05, 0x01, 0x02, 0x03, 0x04, 0x05};
    Reader rejecting(too_long);
    EXPECT_FALSE(PublicKey::Deserialize(rejecting, actual, 4));
    EXPECT_FALSE(rejecting.Ok());
}

TEST(Signature, SharesTheSchemeThenLengthPrefixShape) {
    const Signature expected{.scheme = 1, .bytes = {0x11, 0x22}};
    Writer writer;
    expected.Serialize(writer);
    EXPECT_EQ(writer.Bytes(), ByteVec({0x01, 0x00, 0x02, 0x11, 0x22}));

    Reader reader(writer.Bytes());
    Signature actual;
    ASSERT_TRUE(Signature::Deserialize(reader, actual, 16));
    EXPECT_TRUE(reader.Finish());
    EXPECT_EQ(actual, expected);
}

TEST(Signature, EnforcesTheSignatureSizeBoundWithoutChangingOutput) {
    const ByteVec bytes{0x01, 0x00, 0x03, 0x11, 0x22, 0x33};
    Reader reader(bytes);
    Signature actual{.scheme = 9, .bytes = {0x99}};
    const Signature original = actual;
    EXPECT_FALSE(Signature::Deserialize(reader, actual, 2));
    EXPECT_FALSE(reader.Ok());
    EXPECT_EQ(actual, original);
}

TEST(SpendCondition, SerializesFieldsThenCountThenKeysCanonically) {
    const SpendCondition condition{
        .version = 1, .threshold = 1, .keys = {{.scheme = 1, .bytes = {0xAA, 0xBB}}}};
    Writer writer;
    condition.Serialize(writer);
    EXPECT_EQ(writer.Bytes(), ByteVec({0x01, 0x01, 0x01, 0x01, 0x00, 0x02, 0xAA, 0xBB}));
}

TEST(SpendCondition, RoundTripsUnknownVersionsAndSchemes) {
    const SpendCondition expected{
        .version = 77,
        .threshold = 2,
        .keys = {{.scheme = 99, .bytes = {0x01}}, {.scheme = 100, .bytes = {0x02, 0x03}}}};
    Writer writer;
    expected.Serialize(writer);
    Reader reader(writer.Bytes());
    SpendCondition actual;
    ASSERT_TRUE(SpendCondition::Deserialize(reader, actual, 16, 64));
    EXPECT_TRUE(reader.Finish());
    EXPECT_EQ(actual, expected);
}

TEST(SpendCondition, BoundsKeyCountAndPreservesOutputOnFailure) {
    // Claims two keys where one is the maximum; the decode must fail wholesale.
    const ByteVec bytes{0x01, 0x01, 0x02, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00};
    Reader reader(bytes);
    SpendCondition actual{.version = 9, .threshold = 9, .keys = {{.scheme = 9, .bytes = {9}}}};
    const SpendCondition original = actual;
    EXPECT_FALSE(SpendCondition::Deserialize(reader, actual, 1, 64));
    EXPECT_FALSE(reader.Ok());
    EXPECT_EQ(actual, original);
}

TEST(SpendCondition, RejectsATruncatedKeyWithoutChangingOutput) {
    const ByteVec bytes{0x01, 0x01, 0x01, 0x01, 0x00, 0x04, 0xAA, 0xBB};
    Reader reader(bytes);
    SpendCondition actual{.version = 1, .threshold = 1, .keys = {{.scheme = 1, .bytes = {0xCC}}}};
    const SpendCondition original = actual;
    EXPECT_FALSE(SpendCondition::Deserialize(reader, actual, 16, 64));
    EXPECT_FALSE(reader.Ok());
    EXPECT_EQ(actual, original);
}

TEST(Witness, SerializesConditionThenCountThenSignatures) {
    const Witness witness{
        .condition = {.version = 1,
                      .threshold = 2,
                      .keys = {{.scheme = 1, .bytes = {0xAA}}, {.scheme = 1, .bytes = {0xBB}}}},
        .signatures = {{.scheme = 1, .bytes = {0x01}}, {.scheme = 1, .bytes = {0x02}}}};
    Writer writer;
    witness.Serialize(writer);
    EXPECT_EQ(writer.Bytes(),
              ByteVec({0x01, 0x02, 0x02, 0x01, 0x00, 0x01, 0xAA, 0x01, 0x00, 0x01, 0xBB,  //
                       0x02, 0x01, 0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x02}));
}

TEST(Witness, RoundTripsExactly) {
    const Witness expected{.condition = {.version = 1,
                                         .threshold = 2,
                                         .keys = {{.scheme = 1, .bytes = {0xAA}},
                                                  {.scheme = 2, .bytes = {0xBB, 0xCC}}}},
                           .signatures = {{.scheme = 1, .bytes = {0x01, 0x02, 0x03}}}};
    Writer writer;
    expected.Serialize(writer);
    Reader reader(writer.Bytes());
    Witness actual;
    ASSERT_TRUE(Witness::Deserialize(reader, actual, 16, 64, 8, 64));
    EXPECT_TRUE(reader.Finish());
    EXPECT_EQ(actual, expected);
}

TEST(Witness, BoundsTheSignatureCountWithoutChangingOutput) {
    // Two signatures claimed; the per-witness maximum is one.
    Writer writer;
    SpendCondition{.version = 1, .threshold = 2, .keys = {{.scheme = 1, .bytes = {0xAA}}}}
        .Serialize(writer);
    writer.WriteCompactSize(2);
    Signature{.scheme = 1, .bytes = {0x01}}.Serialize(writer);
    Signature{.scheme = 1, .bytes = {0x02}}.Serialize(writer);

    Reader reader(writer.Bytes());
    Witness actual{.condition = {.version = 9, .threshold = 9, .keys = {}}, .signatures = {}};
    const Witness original = actual;
    EXPECT_FALSE(Witness::Deserialize(reader, actual, 16, 64, 1, 64));
    EXPECT_FALSE(reader.Ok());
    EXPECT_EQ(actual, original);
}

TEST(Witness, TruncatedConditionFailsWholesale) {
    const ByteVec bytes{0x01, 0x01, 0x01, 0x01, 0x00};  // condition cut mid-key
    Reader reader(bytes);
    Witness actual;
    EXPECT_FALSE(Witness::Deserialize(reader, actual, 16, 64, 8, 64));
    EXPECT_FALSE(reader.Ok());
}

}  // namespace
}  // namespace amarian
