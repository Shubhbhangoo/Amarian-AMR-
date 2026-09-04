#include <amarian/crypto/hash.hpp>
#include <amarian/primitives/transaction.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace amarian {
namespace {

/// Little-endian appenders, deliberately independent of the Writer under test:
/// the expected byte vectors below must not be produced by the codec they check.
void AppendU16LE(ByteVec& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void AppendU32LE(ByteVec& bytes, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        bytes.push_back(static_cast<uint8_t>(value));
        value >>= 8;
    }
}

void AppendU64LE(ByteVec& bytes, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<uint8_t>(value));
        value >>= 8;
    }
}

void AppendCompact(ByteVec& bytes, uint64_t value) {
    if (value < 0xFDU) {
        bytes.push_back(static_cast<uint8_t>(value));
    } else {
        FAIL() << "test helper only needs one-byte compact sizes";
    }
}

Hash256 SequentialHash() {
    ByteVec bytes(Hash256::SIZE);
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>(i);
    }
    return Hash256::FromBytes(bytes);
}

/// The transaction every wire test pins: one input, one output, one witness.
struct Sample {
    Transaction tx;
    ByteVec witnessless;
    ByteVec full;
};

Sample MakeSample() {
    Sample sample;

    sample.tx.version = 1;
    sample.tx.inputs.push_back(
        {.outpoint = {.txid = SequentialHash(), .index = 0x07060504U}, .sequence = 0xFFFFFFFFU});
    sample.tx.outputs.push_back({.amount = 25, .lock = {.version = 1, .program = {0x2A}}});
    sample.tx.locktime = 0;
    sample.tx.witnesses.push_back(
        {.condition = {.version = 1, .threshold = 1, .keys = {{.scheme = 1, .bytes = {0xAA}}}},
         .signatures = {{.scheme = 1, .bytes = {0xBB}}}});

    // version
    AppendU32LE(sample.witnessless, 1);
    // inputs: count, then txid, index, sequence
    AppendCompact(sample.witnessless, 1);
    for (size_t i = 0; i < Hash256::SIZE; ++i) {
        sample.witnessless.push_back(static_cast<uint8_t>(i));
    }
    AppendU32LE(sample.witnessless, 0x07060504U);
    AppendU32LE(sample.witnessless, 0xFFFFFFFFU);
    // outputs: count, then amount 25, then the lock
    AppendCompact(sample.witnessless, 1);
    AppendU64LE(sample.witnessless, 25);
    sample.witnessless.push_back(0x01);  // lock version
    AppendCompact(sample.witnessless, 1);
    sample.witnessless.push_back(0x2A);
    // locktime
    AppendU32LE(sample.witnessless, 0);

    // The witness section, appended for the full (wtxid) form.
    sample.full = sample.witnessless;
    AppendCompact(sample.full, 1);  // one witness
    sample.full.push_back(0x01);    // condition version
    sample.full.push_back(0x01);    // threshold
    AppendCompact(sample.full, 1);  // one key
    AppendU16LE(sample.full, 1);    // key scheme
    AppendCompact(sample.full, 1);  // key length
    sample.full.push_back(0xAA);
    AppendCompact(sample.full, 1);  // one signature
    AppendU16LE(sample.full, 1);    // signature scheme
    AppendCompact(sample.full, 1);  // signature length
    sample.full.push_back(0xBB);

    return sample;
}

constexpr TxLimits kLimits{.max_inputs = 16,
                           .max_outputs = 16,
                           .max_witnesses = 16,
                           .max_lock_program_size = 80,
                           .max_keys = 16,
                           .max_key_size = 80,
                           .max_signatures = 16,
                           .max_signature_size = 80};

TEST(TransactionPrimitive, SerializesTheCanonicalWireForm) {
    const Sample sample = MakeSample();
    Writer writer;
    sample.tx.Serialize(writer);
    EXPECT_EQ(writer.Bytes(), sample.full);
}

TEST(TransactionPrimitive, WitnesslessSerializationIsTheTxidPreimage) {
    const Sample sample = MakeSample();
    Writer writer;
    sample.tx.SerializeWithoutWitnesses(writer);
    EXPECT_EQ(writer.Bytes(), sample.witnessless);
}

TEST(TransactionPrimitive, RoundTripsExactly) {
    const Sample sample = MakeSample();
    Reader reader(sample.full);
    Transaction decoded;
    ASSERT_TRUE(Transaction::Deserialize(reader, decoded, kLimits));
    EXPECT_TRUE(reader.Finish());
    EXPECT_EQ(decoded, sample.tx);
}

TEST(TransactionPrimitive, TxidCoversEverythingButTheWitnesses) {
    const Sample sample = MakeSample();
    EXPECT_EQ(sample.tx.Txid(), DoubleSha256(sample.witnessless));
    EXPECT_EQ(sample.tx.Wtxid(), DoubleSha256(sample.full));
    EXPECT_NE(sample.tx.Txid(), sample.tx.Wtxid());
}

TEST(TransactionPrimitive, WitnessMalleabilityCannotChangeTheTxid) {
    Sample sample = MakeSample();
    const Hash256 txid = sample.tx.Txid();
    const Hash256 wtxid = sample.tx.Wtxid();

    // Same spend authorised by a different signature set: the txid is stable,
    // the wtxid is not. This is the property that makes witnesses segregable.
    sample.tx.witnesses[0].signatures[0].bytes[0] = 0xCC;
    EXPECT_EQ(sample.tx.Txid(), txid);
    EXPECT_NE(sample.tx.Wtxid(), wtxid);

    // The condition is revealed authorisation data and lives in the witness, so
    // the same holds for it: the txid is stable, the wtxid tracks every byte.
    sample.tx.witnesses[0].condition.keys[0].bytes[0] = 0xDD;
    EXPECT_EQ(sample.tx.Txid(), txid);
    EXPECT_NE(sample.tx.Wtxid(), wtxid);
}

TEST(TransactionPrimitive, AnOutputChangeChangesTheTxid) {
    Sample sample = MakeSample();
    const Hash256 txid = sample.tx.Txid();
    sample.tx.outputs[0].amount += 1;
    EXPECT_NE(sample.tx.Txid(), txid);
}

TEST(TransactionPrimitive, TheWitnessCountBelongsToTheWitnessSection) {
    // A transaction whose witness list is empty still serialises its
    // compact-size count on the wire, and that count is part of the witness
    // section: the txid preimage excludes it, the wtxid preimage includes it.
    // Removing the witnesses of the sample therefore leaves the txid unchanged
    // while changing the wtxid.
    Sample sample = MakeSample();
    const Hash256 txid = sample.tx.Txid();
    sample.tx.witnesses.clear();
    EXPECT_EQ(sample.tx.Txid(), txid);

    Writer writer;
    sample.tx.Serialize(writer);
    const ByteVec wire = writer.Bytes();

    Reader reader(wire);
    Transaction decoded;
    ASSERT_TRUE(Transaction::Deserialize(reader, decoded, kLimits));
    EXPECT_TRUE(reader.Finish());
    EXPECT_TRUE(decoded.witnesses.empty());

    EXPECT_EQ(decoded.Txid(), DoubleSha256(sample.witnessless));
    EXPECT_EQ(decoded.Wtxid(), DoubleSha256(wire));
    EXPECT_NE(decoded.Txid(), decoded.Wtxid());
}

TEST(TransactionPrimitive, EveryTruncationFailsWithoutChangingTheOutput) {
    const Sample sample = MakeSample();
    for (size_t cut = 0; cut < sample.full.size(); ++cut) {
        const ByteVec truncated(sample.full.begin(),
                                sample.full.begin() + static_cast<std::ptrdiff_t>(cut));
        Reader reader(truncated);
        Transaction decoded = sample.tx;  // must survive the parse untouched
        EXPECT_FALSE(Transaction::Deserialize(reader, decoded, kLimits))
            << "accepted a " << cut << "-byte prefix of a longer transaction";
        EXPECT_FALSE(reader.Ok());
        EXPECT_EQ(decoded, sample.tx);
    }
}

TEST(TransactionPrimitive, TrailingBytesAreRejectedByFinishNotByDeserialize) {
    const Sample sample = MakeSample();
    ByteVec padded = sample.full;
    padded.push_back(0x00);
    Reader reader(padded);
    Transaction decoded;
    ASSERT_TRUE(Transaction::Deserialize(reader, decoded, kLimits));
    EXPECT_FALSE(reader.Finish()) << "trailing bytes must fail the parse";
}

TEST(TransactionPrimitive, CountsAboveTheLimitsAreRejected) {
    // Two inputs claimed where the limit is one.
    const Sample sample = MakeSample();
    ByteVec bytes = sample.full;
    bytes[4] = 0x02;  // the inputs compact-size count

    Reader reader(bytes);
    Transaction decoded;
    EXPECT_FALSE(Transaction::Deserialize(reader, decoded, kLimits));
    EXPECT_FALSE(reader.Ok());

    // A count that would exhaust memory is rejected against the bytes remaining
    // rather than allocated from: 200 inputs need at least 8000 bytes.
    ByteVec greedy{0x01, 0x00, 0x00, 0x00, 0xC8};
    Reader greedy_reader(greedy);
    Transaction unused;
    EXPECT_FALSE(Transaction::Deserialize(greedy_reader, unused, kLimits));
    EXPECT_FALSE(greedy_reader.Ok());
}

TEST(TransactionPrimitive, BoundsTheSignatureCountPerWitness) {
    Sample sample = MakeSample();
    // Ask for two signatures; the witness limit is one.
    sample.tx.witnesses[0].signatures.push_back({.scheme = 1, .bytes = {0x01}});
    Writer writer;
    sample.tx.Serialize(writer);

    TxLimits tight = kLimits;
    tight.max_signatures = 1;
    Reader reader(writer.Bytes());
    Transaction decoded;
    EXPECT_FALSE(Transaction::Deserialize(reader, decoded, tight));
    EXPECT_FALSE(reader.Ok());
}

TEST(TransactionPrimitive, OutputAmountIsRangeCheckedAtTheWholeTransactionLevel) {
    Sample sample = MakeSample();
    sample.tx.outputs[0].amount = MAX_MONEY + 1;
    Writer writer;
    sample.tx.Serialize(writer);

    Reader reader(writer.Bytes());
    Transaction decoded;
    EXPECT_FALSE(Transaction::Deserialize(reader, decoded, kLimits));
    EXPECT_FALSE(reader.Ok());
}

TEST(TransactionPrimitive, DecodingDoesNotJudgeStructuralValidity) {
    // No inputs, no outputs, no witnesses: consensus will reject this, but the
    // primitive decodes it canonically so that the rejection can name the rule.
    ByteVec bytes;
    AppendU32LE(bytes, 1);  // version
    AppendCompact(bytes, 0);
    AppendCompact(bytes, 0);
    AppendU32LE(bytes, 0);  // locktime
    AppendCompact(bytes, 0);

    Reader reader(bytes);
    Transaction decoded;
    ASSERT_TRUE(Transaction::Deserialize(reader, decoded, kLimits));
    EXPECT_TRUE(reader.Finish());
    EXPECT_TRUE(decoded.inputs.empty());
    EXPECT_TRUE(decoded.outputs.empty());
    EXPECT_TRUE(decoded.witnesses.empty());
    // The txid preimage is everything up to the witness section; the wtxid
    // preimage includes the trailing empty-count byte.
    EXPECT_EQ(decoded.Txid(),
              DoubleSha256(ByteVec(bytes.begin(),
                                   bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() - 1))));
    EXPECT_EQ(decoded.Wtxid(), DoubleSha256(bytes));
}

TEST(TransactionPrimitive, WitnessCountIsNotTiedToInputCountAtThisLayer) {
    // Two witnesses for one input, and a coinbase-style sentinel outpoint, both
    // decode: matching witnesses to inputs and recognising the coinbase are
    // consensus rules, not part of the encoding.
    Transaction tx;
    tx.version = 1;
    tx.inputs.push_back({.outpoint = {.index = 0xFFFFFFFFU}, .sequence = 0xFFFFFFFFU});
    tx.outputs.push_back({.amount = 0, .lock = {.version = 1, .program = {}}});
    tx.locktime = 0;
    const Witness witness{
        .condition = {.version = 1, .threshold = 1, .keys = {{.scheme = 1, .bytes = {0xAA}}}},
        .signatures = {{.scheme = 1, .bytes = {0xBB}}}};
    tx.witnesses.push_back(witness);
    tx.witnesses.push_back(witness);

    Writer writer;
    tx.Serialize(writer);
    Reader reader(writer.Bytes());
    Transaction decoded;
    ASSERT_TRUE(Transaction::Deserialize(reader, decoded, kLimits));
    EXPECT_TRUE(reader.Finish());
    EXPECT_EQ(decoded, tx);
}

}  // namespace
}  // namespace amarian
