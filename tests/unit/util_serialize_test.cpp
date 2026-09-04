#include <amarian/util/serialize.hpp>
#include <amarian/util/types.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>

namespace amarian {
namespace {

TEST(Serialize, CompactSizeLenMatchesTheEncoding) {
    // The length function and the encoder must agree, or every size and weight
    // computed without serialising is wrong.
    const uint64_t values[] = {0,
                               1,
                               0xFCU,
                               0xFDU,
                               0xFFFFU,
                               0x10000U,
                               0xFFFFFFFFU,
                               0x100000000U,
                               std::numeric_limits<uint64_t>::max()};
    for (const uint64_t value : values) {
        Writer writer;
        writer.WriteCompactSize(value);
        EXPECT_EQ(writer.Size(), CompactSizeLen(value)) << "value " << value;
    }
}

TEST(Serialize, CompactSizeLenIsConstexpr) {
    static_assert(CompactSizeLen(0) == 1);
    static_assert(CompactSizeLen(0xFCU) == 1);
    static_assert(CompactSizeLen(0xFDU) == 3);
    static_assert(CompactSizeLen(0xFFFFU) == 3);
    static_assert(CompactSizeLen(0x10000U) == 5);
    static_assert(CompactSizeLen(0xFFFFFFFFU) == 5);
    static_assert(CompactSizeLen(0x100000000U) == 9);
}

TEST(Serialize, IntegersAreLittleEndianAndFixedWidth) {
    Writer writer;
    writer.WriteU8(0x01);
    writer.WriteU16(0x0302);
    writer.WriteU32(0x07060504);
    writer.WriteU64(0x0F0E0D0C0B0A0908ULL);
    const ByteVec expected{
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
    EXPECT_EQ(writer.Bytes(), expected);
    EXPECT_EQ(writer.Size(), 15U);
}

TEST(Serialize, IntegersRoundTrip) {
    Writer writer;
    writer.WriteU8(0xAB);
    writer.WriteU16(0xBEEF);
    writer.WriteU32(0xDEADBEEFU);
    writer.WriteU64(0x0123456789ABCDEFULL);
    const ByteVec bytes = writer.Take();

    Reader reader(bytes);
    uint8_t a = 0;
    uint16_t b = 0;
    uint32_t c = 0;
    uint64_t d = 0;
    ASSERT_TRUE(reader.ReadU8(a));
    ASSERT_TRUE(reader.ReadU16(b));
    ASSERT_TRUE(reader.ReadU32(c));
    ASSERT_TRUE(reader.ReadU64(d));
    EXPECT_TRUE(reader.Finish());
    EXPECT_EQ(a, 0xAB);
    EXPECT_EQ(b, 0xBEEF);
    EXPECT_EQ(c, 0xDEADBEEFU);
    EXPECT_EQ(d, 0x0123456789ABCDEFULL);
}

TEST(Serialize, SignedValuesRoundTripIncludingNegativeAndExtremes) {
    // i64 carries amounts and timestamps. Amounts are non-negative by rule, but
    // the codec must not be the thing enforcing that, and a timestamp before the
    // epoch has to survive the trip so the range check is the only gate.
    const int64_t values[] = {0,
                              1,
                              -1,
                              std::numeric_limits<int64_t>::min(),
                              std::numeric_limits<int64_t>::max(),
                              83'999'999'932'170'000LL};
    for (const int64_t value : values) {
        Writer writer;
        writer.WriteI64(value);
        ASSERT_EQ(writer.Size(), 8U);
        const ByteVec bytes = writer.Take();

        Reader reader(bytes);
        int64_t out = 0;
        ASSERT_TRUE(reader.ReadI64(out));
        EXPECT_TRUE(reader.Finish());
        EXPECT_EQ(out, value);
    }
}

TEST(Serialize, NegativeOneIsAllOnes) {
    Writer writer;
    writer.WriteI64(-1);
    EXPECT_EQ(writer.Bytes(), ByteVec(8, 0xFF));
}

TEST(Serialize, Hash256UsesInternalByteOrder) {
    ByteVec raw(Hash256::SIZE);
    for (size_t i = 0; i < raw.size(); ++i) {
        raw[i] = static_cast<uint8_t>(i + 1);
    }
    const Hash256 hash = Hash256::FromBytes(raw);

    Writer writer;
    writer.WriteHash256(hash);
    // Serialised order is the order the hash function produced, not the reversed
    // order an explorer displays. Getting this backwards produces a node that
    // agrees with the network and disagrees with every explorer, or the reverse.
    EXPECT_EQ(writer.Bytes(), raw);

    const ByteVec bytes = writer.Take();
    Reader reader(bytes);
    Hash256 decoded;
    ASSERT_TRUE(reader.ReadHash256(decoded));
    EXPECT_TRUE(reader.Finish());
    EXPECT_EQ(decoded, hash);
}

// --- compact size: canonicality ---------------------------------------------

TEST(CompactSize, UsesTheShortestFormAtEveryBoundary) {
    struct Case {
        uint64_t value;
        ByteVec encoded;
    };

    const Case cases[] = {
        {0, {0x00}},
        {1, {0x01}},
        {0xFCU, {0xFC}},
        {0xFDU, {0xFD, 0xFD, 0x00}},
        {0xFFFFU, {0xFD, 0xFF, 0xFF}},
        {0x10000U, {0xFE, 0x00, 0x00, 0x01, 0x00}},
        {0xFFFFFFFFU, {0xFE, 0xFF, 0xFF, 0xFF, 0xFF}},
        {0x100000000ULL, {0xFF, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00}},
    };
    for (const Case& c : cases) {
        Writer writer;
        writer.WriteCompactSize(c.value);
        EXPECT_EQ(writer.Bytes(), c.encoded) << "value " << c.value;
    }
}

TEST(CompactSize, RejectsNonMinimalEncodings) {
    // Each of these encodes a value that had a shorter representation. Accepting
    // them would give one transaction more than one serialisation, and therefore
    // more than one txid.
    const ByteVec non_minimal[] = {
        {0xFD, 0x00, 0x00},                                      // 0 in three bytes
        {0xFD, 0x01, 0x00},                                      // 1 in three bytes
        {0xFD, 0xFC, 0x00},                                      // 0xFC in three bytes
        {0xFE, 0x00, 0x00, 0x00, 0x00},                          // 0 in five bytes
        {0xFE, 0xFF, 0xFF, 0x00, 0x00},                          // 0xFFFF in five
        {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // 0 in nine bytes
        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00},  // 0xFFFFFFFF in nine
    };
    for (const ByteVec& bytes : non_minimal) {
        Reader reader(bytes);
        uint64_t value = 0;
        EXPECT_FALSE(reader.ReadCompactSize(value, 1)) << "accepted a non-minimal encoding";
        EXPECT_FALSE(reader.Ok());
    }
}

TEST(CompactSize, AcceptsTheMinimalFormOfEachBoundaryValue) {
    // The mirror of the test above: the smallest legal encoding of a value that
    // needs the wider form must be accepted, or the boundary check is off by one.
    const ByteVec minimal[] = {
        {0xFD, 0xFD, 0x00},              // 0xFD, needs three bytes
        {0xFE, 0x00, 0x00, 0x01, 0x00},  // 0x10000, needs five
    };
    const size_t expected[] = {0xFDU, 0x10000U};
    for (size_t i = 0; i < std::size(minimal); ++i) {
        // Padded so the value is plausible against the bytes remaining.
        ByteVec bytes(minimal[i].size() + expected[i], 0x00);
        std::copy(minimal[i].begin(), minimal[i].end(), bytes.begin());
        Reader reader(bytes);
        uint64_t value = 0;
        ASSERT_TRUE(reader.ReadCompactSize(value, 1)) << "rejected a minimal encoding";
        EXPECT_EQ(value, expected[i]);
    }
}

// --- compact size: the allocation bound --------------------------------------

TEST(CompactSize, RejectsACountLargerThanTheBytesRemaining) {
    // Nine bytes claiming 2^64 elements. Without this bound a loop reading one
    // element at a time runs 2^64 times before it discovers the buffer is empty.
    const ByteVec bytes{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    Reader reader(bytes);
    uint64_t value = 0;
    EXPECT_FALSE(reader.ReadCompactSize(value, 1));
    EXPECT_FALSE(reader.Ok());
}

TEST(CompactSize, BoundScalesWithElementSize) {
    // Ten payload bytes after the prefix. Three elements fit at three bytes each;
    // four do not.
    ByteVec bytes(11, 0x00);
    bytes[0] = 0x03;
    {
        Reader reader(bytes);
        uint64_t value = 0;
        EXPECT_TRUE(reader.ReadCompactSize(value, 3));
        EXPECT_EQ(value, 3U);
    }
    bytes[0] = 0x04;
    {
        Reader reader(bytes);
        uint64_t value = 0;
        EXPECT_FALSE(reader.ReadCompactSize(value, 3));
        EXPECT_FALSE(reader.Ok());
    }
}

TEST(CompactSize, ZeroIsAlwaysPlausible) {
    const ByteVec bytes{0x00};
    Reader reader(bytes);
    uint64_t value = 1;
    ASSERT_TRUE(reader.ReadCompactSize(value, 1000));
    EXPECT_EQ(value, 0U);
    EXPECT_TRUE(reader.Finish());
}

TEST(CompactSize, ZeroElementSizeIsRejectedRatherThanWideningTheBound) {
    const ByteVec bytes{0x01, 0x00};
    Reader reader(bytes);
    uint64_t value = 0;
    EXPECT_FALSE(reader.ReadCompactSize(value, 0));
    EXPECT_FALSE(reader.Ok());
}

TEST(CompactSize, TruncatedEncodingsFail) {
    const ByteVec truncated[] = {
        {},
        {0xFD},
        {0xFD, 0x00},
        {0xFE, 0x00, 0x00},
        {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    };
    for (const ByteVec& bytes : truncated) {
        Reader reader(bytes);
        uint64_t value = 0;
        EXPECT_FALSE(reader.ReadCompactSize(value, 1));
        EXPECT_FALSE(reader.Ok());
    }
}

// --- length-prefixed byte strings --------------------------------------------

TEST(ByteString, RoundTrips) {
    const ByteVec payload{0xDE, 0xAD, 0xBE, 0xEF};
    Writer writer;
    writer.WriteByteString(payload);
    EXPECT_EQ(writer.Size(), 1U + payload.size());
    const ByteVec bytes = writer.Take();

    Reader reader(bytes);
    ByteVec decoded;
    ASSERT_TRUE(reader.ReadByteString(decoded, 64));
    EXPECT_TRUE(reader.Finish());
    EXPECT_EQ(decoded, payload);
}

TEST(ByteString, EmptyIsAValidValueNotAnError) {
    Writer writer;
    writer.WriteByteString(ByteVec{});
    EXPECT_EQ(writer.Bytes(), ByteVec{0x00});
    const ByteVec bytes = writer.Take();

    Reader reader(bytes);
    ByteVec decoded{0xFF};
    ASSERT_TRUE(reader.ReadByteString(decoded, 64));
    EXPECT_TRUE(reader.Finish());
    EXPECT_TRUE(decoded.empty());
}

TEST(ByteString, RejectsLengthAboveTheFieldMaximum) {
    // Four bytes present and available, but the field's own limit is three. The
    // limit is the protocol's, not the buffer's, so a well-formed-looking string
    // still has to be refused.
    const ByteVec bytes{0x04, 0x01, 0x02, 0x03, 0x04};
    Reader reader(bytes);
    ByteVec decoded;
    EXPECT_FALSE(reader.ReadByteString(decoded, 3));
    EXPECT_FALSE(reader.Ok());
}

TEST(ByteString, RejectsLengthBeyondTheBuffer) {
    // Claims 64 bytes, provides 2. The maximum permits it; the bytes do not.
    const ByteVec bytes{0x40, 0x01, 0x02};
    Reader reader(bytes);
    ByteVec decoded;
    EXPECT_FALSE(reader.ReadByteString(decoded, 1024));
    EXPECT_FALSE(reader.Ok());
}

TEST(ByteString, LeavesTheOutputUntouchedOnFailure) {
    const ByteVec bytes{0x40, 0x01, 0x02};
    Reader reader(bytes);
    ByteVec decoded{0xAA, 0xBB};
    EXPECT_FALSE(reader.ReadByteString(decoded, 1024));
    EXPECT_EQ(decoded, ByteVec({0xAA, 0xBB}));
}

TEST(ByteString, ReadsTwoInSequence) {
    Writer writer;
    writer.WriteByteString(ByteVec{0x01, 0x02});
    writer.WriteByteString(ByteVec{0x03});
    const ByteVec bytes = writer.Take();

    Reader reader(bytes);
    ByteVec first;
    ByteVec second;
    ASSERT_TRUE(reader.ReadByteString(first, 16));
    ASSERT_TRUE(reader.ReadByteString(second, 16));
    EXPECT_TRUE(reader.Finish());
    EXPECT_EQ(first, ByteVec({0x01, 0x02}));
    EXPECT_EQ(second, ByteVec({0x03}));
}

// --- Reader state: bounds, stickiness, exact consumption ---------------------

TEST(Reader, ReadsNothingWhenAFieldDoesNotFit) {
    const ByteVec bytes{0x01, 0x02, 0x03};
    Reader reader(bytes);
    uint32_t value = 0xFFFFFFFFU;
    EXPECT_FALSE(reader.ReadU32(value));
    EXPECT_EQ(value, 0xFFFFFFFFU) << "a failed read must not write its output";
    EXPECT_FALSE(reader.Ok());
    EXPECT_EQ(reader.Remaining(), 0U) << "a failed Reader must offer no bytes";
}

TEST(Reader, FailureIsSticky) {
    // Two bytes, so the u32 fails; then a u8 that would have fitted must also
    // fail. This is the property that makes checking Finish() once safe.
    const ByteVec bytes{0x01, 0x02};
    Reader reader(bytes);
    uint32_t wide = 0;
    EXPECT_FALSE(reader.ReadU32(wide));
    uint8_t narrow = 0x7F;
    EXPECT_FALSE(reader.ReadU8(narrow));
    EXPECT_EQ(narrow, 0x7F);
    EXPECT_FALSE(reader.Finish());
}

TEST(Reader, ExplicitFailIsIndistinguishableFromAParseFailure) {
    const ByteVec bytes{0x01, 0x02};
    Reader reader(bytes);
    uint8_t value = 0;
    ASSERT_TRUE(reader.ReadU8(value));
    reader.Fail();
    EXPECT_FALSE(reader.Ok());
    EXPECT_FALSE(reader.ReadU8(value));
    EXPECT_FALSE(reader.Finish());
}

TEST(Reader, TrailingBytesFailTheParse) {
    const ByteVec bytes{0x01, 0x02};
    Reader reader(bytes);
    uint8_t value = 0;
    ASSERT_TRUE(reader.ReadU8(value));
    EXPECT_TRUE(reader.Ok()) << "an unfinished parse is not yet a failed one";
    EXPECT_FALSE(reader.Finish()) << "one byte was left over";
    EXPECT_FALSE(reader.Ok()) << "Finish must record the failure, not just report it";
}

TEST(Reader, EmptyInputIsAtEndAndFinishes) {
    const ByteVec bytes;
    Reader reader(bytes);
    EXPECT_TRUE(reader.AtEnd());
    EXPECT_TRUE(reader.Ok());
    EXPECT_TRUE(reader.Finish());
}

TEST(Reader, ZeroLengthReadsSucceedWithoutConsuming) {
    const ByteVec bytes{0x01};
    Reader reader(bytes);
    ByteSpan span{};
    ASSERT_TRUE(reader.ReadRawSpan(0, span));
    EXPECT_TRUE(span.empty());
    ASSERT_TRUE(reader.ReadBytes(MutableByteSpan{}));
    EXPECT_EQ(reader.Remaining(), 1U);
}

TEST(Reader, RawSpanBorrowsTheUnderlyingBytes) {
    const ByteVec bytes{0x01, 0x02, 0x03, 0x04};
    Reader reader(bytes);
    ByteSpan span{};
    ASSERT_TRUE(reader.ReadRawSpan(3, span));
    ASSERT_EQ(span.size(), 3U);
    EXPECT_EQ(span.data(), bytes.data()) << "ReadRawSpan must not copy";
    EXPECT_EQ(span[2], 0x03);
    EXPECT_EQ(reader.Remaining(), 1U);
}

TEST(Reader, ReadBytesFillsExactlyAndFailsWholesale) {
    const ByteVec bytes{0x01, 0x02, 0x03};
    Reader reader(bytes);
    ByteVec out(4, 0xEE);
    EXPECT_FALSE(reader.ReadBytes(MutableByteSpan(out)));
    EXPECT_EQ(out, ByteVec(4, 0xEE)) << "a partial fill would leave a half-read field";
    EXPECT_FALSE(reader.Ok());
}

TEST(Writer, TakeMovesTheBufferOut) {
    Writer writer;
    writer.WriteU32(0x04030201U);
    const ByteVec taken = writer.Take();
    EXPECT_EQ(taken, ByteVec({0x01, 0x02, 0x03, 0x04}));
    EXPECT_EQ(writer.Size(), 0U);
}

}  // namespace
}  // namespace amarian
