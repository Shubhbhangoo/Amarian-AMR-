#include <amarian/primitives/block.hpp>
#include <amarian/primitives/merkle.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace amarian {
namespace {

/// Generous bounds: these tests are about encoding and weight, not about the
/// resource limits, which have their own coverage in the transaction tests.
constexpr BlockLimits TEST_LIMITS{
    .max_transactions = 1024,
    .tx = {.max_inputs = 64,
           .max_outputs = 64,
           .max_witnesses = 64,
           .max_lock_program_size = 128,
           .max_keys = 8,
           .max_key_size = 4096,
           .max_signatures = 8,
           .max_signature_size = 4096,
           .max_coinbase_data_size = 100},
};

Hash256 DigestOf(uint8_t fill) {
    return Hash256::FromBytes(ByteVec(Hash256::SIZE, fill));
}

BlockHeader SampleHeader() {
    BlockHeader header;
    header.version = 1;
    header.height = 4242;
    header.prev_block = DigestOf(0x11);
    header.merkle_root = DigestOf(0x22);
    header.timestamp = 1'757'000'000;
    header.target_bits = 0x1D00FFFFU;
    header.nonce = 0x0102030405060708ULL;
    return header;
}

/// One spendable-looking transaction with a witness, so the base and witness
/// sections are both non-trivial.
Transaction SampleTransaction(uint8_t fill) {
    Transaction transaction;
    transaction.version = 1;
    transaction.inputs.push_back(
        TxInput{.outpoint = {.txid = DigestOf(fill), .index = 0}, .sequence = 0xFFFFFFFFU});
    transaction.outputs.push_back(
        TxOutput{.amount = 5'000'000'000, .lock = {.version = 0, .program = ByteVec(32, fill)}});
    transaction.locktime = 0;

    Witness witness;
    witness.condition.version = 0;
    witness.condition.threshold = 1;
    witness.condition.keys.push_back(PublicKey{.scheme = 0, .bytes = ByteVec(32, fill)});
    witness.signatures.push_back(Signature{.scheme = 0, .bytes = ByteVec(64, fill)});
    transaction.witnesses.push_back(std::move(witness));
    return transaction;
}

TEST(BlockHeader, IsExactlyNinetyTwoBytes) {
    Writer writer;
    SampleHeader().Serialize(writer);
    EXPECT_EQ(writer.Size(), BlockHeader::SERIALIZED_SIZE);
    EXPECT_EQ(writer.Size(), 92U);
}

TEST(BlockHeader, RoundTrips) {
    const BlockHeader original = SampleHeader();
    Writer writer;
    original.Serialize(writer);

    Reader reader(writer.Bytes());
    BlockHeader decoded;
    ASSERT_TRUE(BlockHeader::Deserialize(reader, decoded));
    EXPECT_TRUE(reader.Finish());
    EXPECT_EQ(decoded, original);
}

TEST(BlockHeader, ShortInputLeavesTheOutputUntouched) {
    Writer writer;
    SampleHeader().Serialize(writer);
    ByteVec truncated = writer.Bytes();
    truncated.pop_back();

    Reader reader(truncated);
    BlockHeader decoded;
    EXPECT_FALSE(BlockHeader::Deserialize(reader, decoded));
    EXPECT_EQ(decoded, BlockHeader{});
}

TEST(BlockHeader, EveryFieldIsCommittedToByTheHash) {
    const BlockHeader base = SampleHeader();
    const Hash256 base_hash = base.Hash();

    std::vector<BlockHeader> mutations(7, base);
    ++mutations[0].version;
    ++mutations[1].height;
    mutations[2].prev_block = DigestOf(0x12);
    mutations[3].merkle_root = DigestOf(0x23);
    ++mutations[4].timestamp;
    mutations[5].target_bits = 0x1D00FFFEU;
    ++mutations[6].nonce;

    for (size_t index = 0; index < mutations.size(); ++index) {
        EXPECT_NE(mutations[index].Hash(), base_hash) << "field " << index;
    }
}

TEST(Block, RoundTripsWithItsTransactions) {
    Block block;
    block.header = SampleHeader();
    block.transactions.push_back(SampleTransaction(0xAA));
    block.transactions.push_back(SampleTransaction(0xBB));

    Writer writer;
    block.Serialize(writer);

    Reader reader(writer.Bytes());
    Block decoded;
    ASSERT_TRUE(Block::Deserialize(reader, decoded, TEST_LIMITS));
    EXPECT_TRUE(reader.Finish());
    EXPECT_EQ(decoded, block);
}

TEST(Block, TransactionCountAboveTheLimitIsRejected) {
    Block block;
    block.header = SampleHeader();
    block.transactions.push_back(SampleTransaction(0xAA));
    block.transactions.push_back(SampleTransaction(0xBB));

    Writer writer;
    block.Serialize(writer);

    BlockLimits limits = TEST_LIMITS;
    limits.max_transactions = 1;
    Reader reader(writer.Bytes());
    Block decoded;
    EXPECT_FALSE(Block::Deserialize(reader, decoded, limits));
    EXPECT_FALSE(reader.Ok());
}

TEST(Block, MerkleRootIsComputedOverWitnessTransactionIds) {
    Block block;
    block.header = SampleHeader();
    block.transactions.push_back(SampleTransaction(0xAA));
    block.transactions.push_back(SampleTransaction(0xBB));

    const auto root = block.ComputeMerkleRoot();
    ASSERT_TRUE(root.has_value());

    std::vector<Hash256> wtxids;
    for (const Transaction& transaction : block.transactions) {
        wtxids.push_back(transaction.Wtxid());
    }
    EXPECT_EQ(root, ComputeMerkleRoot(wtxids));

    // A witness-only change moves the root, because the tree commits to wtxids.
    block.transactions[0].witnesses[0].signatures[0].bytes.assign(64, 0xCC);
    EXPECT_NE(block.ComputeMerkleRoot(), root);
}

TEST(Block, EmptyBlockHasNoMerkleRoot) {
    Block block;
    block.header = SampleHeader();
    EXPECT_FALSE(block.ComputeMerkleRoot().has_value());
}

TEST(Block, WitnessBytesCostOneAndBaseBytesCostFour) {
    Block block;
    block.header = SampleHeader();
    block.transactions.push_back(SampleTransaction(0xAA));

    const auto serialized_size = [&block] {
        Writer writer;
        block.Serialize(writer);
        return writer.Size();
    };

    const size_t weight_before = block.Weight();
    const size_t size_before = serialized_size();

    // A signature is witness data only: weight rises by exactly its byte count.
    block.transactions[0].witnesses[0].signatures.push_back(
        Signature{.scheme = 0, .bytes = ByteVec(64, 0xDD)});
    const size_t witness_bytes = serialized_size() - size_before;
    ASSERT_GT(witness_bytes, 0U);
    EXPECT_EQ(block.Weight(), weight_before + witness_bytes);

    // An output is base data: weight rises by four times its byte count.
    const size_t weight_with_witness = block.Weight();
    const size_t size_with_witness = serialized_size();
    block.transactions[0].outputs.push_back(
        TxOutput{.amount = 1, .lock = {.version = 0, .program = ByteVec(32, 0xEE)}});
    const size_t base_bytes = serialized_size() - size_with_witness;
    ASSERT_GT(base_bytes, 0U);
    EXPECT_EQ(block.Weight(), weight_with_witness + base_bytes * WITNESS_SCALE_FACTOR);
}

TEST(Block, WeightCountsTheHeaderAndTheTransactionCount) {
    // An empty block still has a header and a count, and both are base bytes.
    Block block;
    block.header = SampleHeader();
    EXPECT_EQ(block.Weight(), (BlockHeader::SERIALIZED_SIZE + 1) * WITNESS_SCALE_FACTOR);
}

}  // namespace
}  // namespace amarian
