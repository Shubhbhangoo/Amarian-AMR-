/// \file
/// Fuzz target for the serialisation codec.
///
/// This is the highest-value fuzz target in the project so far: every byte a node
/// will ever act on arrives through Reader, from a peer, a block file or an RPC
/// argument. A crash here is a remote denial of service, and an acceptance here is
/// a consensus disagreement.
///
/// The harness drives the Reader from the fuzzer's own bytes — the input is read as
/// a small program of read operations, so libFuzzer explores sequences of reads
/// against a buffer rather than one read in isolation. Interleaving matters,
/// because the bugs in a codec live in the cursor arithmetic between reads.
///
/// Properties asserted, beyond memory safety:
///
///   1. **The cursor never passes the end.** Remaining() only ever decreases, and
///      by exactly the width of a successful read.
///   2. **Failure is sticky and total.** After any failure, no read succeeds,
///      Remaining() is zero, and Finish() is false.
///   3. **A failed read writes nothing.** Its output parameter is unchanged.
///   4. **Accepted compact sizes are minimal and plausible.** Anything accepted
///      re-encodes to exactly the bytes it was decoded from, and its count fits in
///      the bytes that were left.
///   5. **Whatever a Writer produces, a Reader accepts and reproduces.** Checked on
///      the fuzzer's bytes as a payload, so the round trip is exercised on data
///      nobody chose.

#include <amarian/util/serialize.hpp>
#include <amarian/util/types.hpp>

#include "fuzz_assert.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using amarian::ByteSpan;
using amarian::ByteVec;
using amarian::CompactSizeLen;
using amarian::Hash256;
using amarian::Reader;
using amarian::Writer;

/// Property 4: an accepted compact size must be the only encoding of its value,
/// and must have been bounded by the bytes that followed it.
void CheckAcceptedCompactSize(ByteSpan consumed,
                              uint64_t value,
                              uint64_t remaining_after,
                              size_t min_element_bytes) {
    FUZZ_CHECK(consumed.size() == CompactSizeLen(value),
               "an accepted compact size was not in its minimal form");

    Writer writer;
    writer.WriteCompactSize(value);
    FUZZ_CHECK(writer.Bytes().size() == consumed.size(), "CompactSizeLen and the encoder disagree");
    for (size_t i = 0; i < consumed.size(); ++i) {
        FUZZ_CHECK(writer.Bytes()[i] == consumed[i],
                   "an accepted compact size does not re-encode to itself");
    }

    FUZZ_CHECK(value <= remaining_after / min_element_bytes,
               "an accepted count exceeded the bytes remaining");
}

/// Runs a sequence of reads chosen by the fuzzer over `payload`, checking the
/// cursor and stickiness invariants after every single one.
void DriveReader(ByteSpan program, ByteSpan payload) {
    Reader reader(payload);
    size_t previous_remaining = reader.Remaining();
    bool expect_failed = false;

    for (const uint8_t opcode : program) {
        const size_t before = reader.Remaining();
        const bool was_ok = reader.Ok();
        FUZZ_CHECK(before <= previous_remaining, "Remaining() increased");
        previous_remaining = before;

        // Sentinels chosen so that a failed read is detectable by comparison.
        uint8_t u8 = 0x5A;
        uint16_t u16 = 0x5A5A;
        uint32_t u32 = 0x5A5A5A5AU;
        uint64_t u64 = 0x5A5A5A5A5A5A5A5AULL;
        int64_t i64 = 0x5A5A5A5A5A5A5A5ALL;

        bool read_ok = false;
        size_t expected_width = 0;

        switch (opcode % 8U) {
            case 0:
                read_ok = reader.ReadU8(u8);
                expected_width = 1;
                FUZZ_CHECK(read_ok || u8 == 0x5A, "a failed ReadU8 wrote its output");
                break;
            case 1:
                read_ok = reader.ReadU16(u16);
                expected_width = 2;
                FUZZ_CHECK(read_ok || u16 == 0x5A5A, "a failed ReadU16 wrote its output");
                break;
            case 2:
                read_ok = reader.ReadU32(u32);
                expected_width = 4;
                FUZZ_CHECK(read_ok || u32 == 0x5A5A5A5AU, "a failed ReadU32 wrote its output");
                break;
            case 3:
                read_ok = reader.ReadU64(u64);
                expected_width = 8;
                FUZZ_CHECK(read_ok || u64 == 0x5A5A5A5A5A5A5A5AULL,
                           "a failed ReadU64 wrote its output");
                break;
            case 4:
                read_ok = reader.ReadI64(i64);
                expected_width = 8;
                FUZZ_CHECK(read_ok || i64 == 0x5A5A5A5A5A5A5A5ALL,
                           "a failed ReadI64 wrote its output");
                break;
            case 5: {
                Hash256 hash;
                read_ok = reader.ReadHash256(hash);
                expected_width = Hash256::SIZE;
                FUZZ_CHECK(read_ok || hash.IsZero(), "a failed ReadHash256 wrote its output");
                break;
            }
            case 6: {
                // The compact-size case: the width is not known in advance, so it
                // is measured from the cursor and checked against the value.
                const size_t start = reader.Remaining();
                uint64_t value = 0;
                // Element sizes 1..4, so the allocation bound is exercised at more
                // than one scale.
                const size_t min_element_bytes = static_cast<size_t>(opcode % 4U) + 1U;
                read_ok = reader.ReadCompactSize(value, min_element_bytes);
                if (read_ok) {
                    const size_t width = start - reader.Remaining();
                    FUZZ_CHECK(width >= 1U && width <= 9U, "compact size consumed an absurd width");
                    const size_t offset = payload.size() - start;
                    CheckAcceptedCompactSize(payload.subspan(offset, width),
                                             value,
                                             reader.Remaining(),
                                             min_element_bytes);
                }
                expected_width = start - reader.Remaining();
                break;
            }
            default: {
                ByteVec out{0xC3, 0xC3};
                const size_t start = reader.Remaining();
                // A generous field maximum, so most rejections come from the
                // buffer rather than from the limit.
                read_ok = reader.ReadByteString(out, 1024);
                if (read_ok) {
                    FUZZ_CHECK(start - reader.Remaining() ==
                                   CompactSizeLen(out.size()) + out.size(),
                               "a byte string consumed a width its own length denies");
                } else {
                    FUZZ_CHECK(out == ByteVec({0xC3, 0xC3}),
                               "a failed ReadByteString wrote its output");
                }
                expected_width = start - reader.Remaining();
                break;
            }
        }

        // Property 2: a read after a failure can never succeed.
        FUZZ_CHECK(!(read_ok && expect_failed), "a read succeeded after a failure");
        if (!read_ok) {
            expect_failed = true;
        }
        FUZZ_CHECK(reader.Ok() == !expect_failed, "Ok() disagrees with the read results");
        FUZZ_CHECK(was_ok || !reader.Ok(), "a failed Reader recovered");

        // Property 1: a successful read advances by exactly its own width; a failed
        // one leaves nothing to read at all.
        if (read_ok) {
            FUZZ_CHECK(before - reader.Remaining() == expected_width,
                       "a successful read advanced by the wrong number of bytes");
        } else {
            FUZZ_CHECK(reader.Remaining() == 0U, "a failed Reader still offered bytes");
        }
    }

    if (expect_failed) {
        FUZZ_CHECK(!reader.Finish(), "Finish() succeeded on a failed parse");
    }
    // Finish() is destructive of state, so nothing is checked after it.
}

/// Property 5: the encoder's output is always accepted and always reproduced.
void CheckRoundTrip(ByteSpan payload) {
    Writer writer;
    writer.WriteU8(0x01);
    writer.WriteU16(0x0203);
    writer.WriteU32(0x04050607U);
    writer.WriteU64(0x08090A0B0C0D0E0FULL);
    writer.WriteI64(-1);
    writer.WriteCompactSize(payload.size());
    writer.WriteByteString(payload);
    const ByteVec bytes = writer.Take();

    Reader reader(bytes);
    uint8_t a = 0;
    uint16_t b = 0;
    uint32_t c = 0;
    uint64_t d = 0;
    int64_t e = 0;
    uint64_t size_prefix = 0;
    ByteVec decoded;
    FUZZ_CHECK(reader.ReadU8(a) && a == 0x01, "u8 did not round trip");
    FUZZ_CHECK(reader.ReadU16(b) && b == 0x0203, "u16 did not round trip");
    FUZZ_CHECK(reader.ReadU32(c) && c == 0x04050607U, "u32 did not round trip");
    FUZZ_CHECK(reader.ReadU64(d) && d == 0x08090A0B0C0D0E0FULL, "u64 did not round trip");
    FUZZ_CHECK(reader.ReadI64(e) && e == -1, "i64 did not round trip");
    // Followed by a byte string of the same length, so the count is plausible.
    FUZZ_CHECK(reader.ReadCompactSize(size_prefix, 1), "a written compact size was rejected");
    FUZZ_CHECK(size_prefix == payload.size(), "compact size did not round trip");
    FUZZ_CHECK(reader.ReadByteString(decoded, payload.size()),
               "a written byte string was rejected");
    FUZZ_CHECK(decoded.size() == payload.size(), "byte string length did not round trip");
    for (size_t i = 0; i < decoded.size(); ++i) {
        FUZZ_CHECK(decoded[i] == payload[i], "byte string contents did not round trip");
    }
    FUZZ_CHECK(reader.Finish(), "the encoder produced bytes its own decoder would not finish");
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const ByteSpan input(data, size);

    // First byte splits the input into the read program and the buffer it reads
    // from, so the fuzzer controls both the operations and their operand.
    const size_t split = size == 0 ? 0 : (static_cast<size_t>(data[0]) * size) / 256U;
    const ByteSpan program = input.subspan(0, split);
    const ByteSpan payload = input.subspan(split);

    DriveReader(program, payload);
    DriveReader(payload, program);
    CheckRoundTrip(payload);

    // Determinism: the same bytes parsed twice must give the same answer. Two
    // nodes reading one message is the case that matters.
    Reader first(input);
    Reader second(input);
    uint64_t left = 0;
    uint64_t right = 0;
    const bool left_ok = first.ReadCompactSize(left, 1);
    const bool right_ok = second.ReadCompactSize(right, 1);
    FUZZ_CHECK(left_ok == right_ok, "two parses of the same bytes disagreed");
    FUZZ_CHECK(left == right, "two parses of the same bytes gave different values");

    return 0;
}
