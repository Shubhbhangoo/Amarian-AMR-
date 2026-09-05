/// \file
/// The signature hash's properties: fixed size, and sensitivity to everything it is
/// supposed to commit to.
///
/// There are no published vectors to check against — this is Amarian's own
/// construction — so the tests state the properties instead. Each one changes exactly
/// one thing a signature is supposed to be bound to and requires the message to move.
/// A missing commitment is precisely a test here that would pass with the field
/// removed.

#include <amarian/primitives/sighash.hpp>

#include <amarian/crypto/hash.hpp>
#include <amarian/primitives/lock.hpp>
#include <amarian/util/serialize.hpp>

#include <gtest/gtest.h>

#include <cstdint>

namespace amarian {
namespace {

[[nodiscard]] PublicKey MakeKey(uint16_t scheme, uint8_t fill) {
    return PublicKey{.scheme = scheme, .bytes = ByteVec(32, fill)};
}

[[nodiscard]] SpendCondition MakeCondition(uint8_t threshold) {
    return SpendCondition{.version = CONDITION_VERSION_THRESHOLD,
                          .threshold = threshold,
                          .keys = {MakeKey(1, 0xAA), MakeKey(1, 0xBB)}};
}

/// A two-input, two-output transaction: enough shape for the per-input fields to
/// actually distinguish anything.
[[nodiscard]] Transaction MakeTx() {
    Hash256 first{};
    first.Array()[0] = 0x11;
    Hash256 second{};
    second.Array()[0] = 0x22;

    Transaction tx;
    tx.version = 1;
    tx.locktime = 0;
    tx.inputs = {
        TxInput{.outpoint = OutPoint{.txid = first, .index = 0}, .sequence = 0},
        TxInput{.outpoint = OutPoint{.txid = second, .index = 1}, .sequence = 0},
    };
    tx.outputs = {
        TxOutput{.amount = 1'000,
                 .lock = Lock{.version = LOCK_VERSION_CONDITION_COMMITMENT,
                              .program = ByteVec(32, 0x01)}},
        TxOutput{.amount = 2'000,
                 .lock = Lock{.version = LOCK_VERSION_CONDITION_COMMITMENT,
                              .program = ByteVec(32, 0x02)}},
    };
    return tx;
}

[[nodiscard]] Hash256 MakeChainId(uint8_t fill) {
    Hash256 id{};
    id.Array()[0] = fill;
    return id;
}

/// The signature hash of input 0 of `tx`, under a 2-of-2 condition.
[[nodiscard]] Hash256 SigHashOf(const Transaction& tx,
                                const Hash256& chain_id = MakeChainId(0xAB),
                                uint32_t index = 0,
                                int64_t spent_amount = 5'000,
                                const SpendCondition& condition = MakeCondition(2)) {
    return SignatureHash(
        chain_id, tx, ComputeSigHashMidstates(tx), index, spent_amount, condition);
}

TEST(PrimitivesSigHash, ThePreimageIsAlwaysTheDocumentedFixedSize) {
    const Transaction small = MakeTx();
    Transaction large = MakeTx();
    for (int i = 0; i < 200; ++i) {
        large.inputs.push_back(large.inputs[0]);
        large.outputs.push_back(large.outputs[0]);
    }

    EXPECT_EQ(SignatureHashPreimage(MakeChainId(0xAB),
                                    small,
                                    ComputeSigHashMidstates(small),
                                    0,
                                    5'000,
                                    MakeCondition(2))
                  .size(),
              SIGHASH_PREIMAGE_SIZE);
    // The denial-of-service property: a 201-input transaction's per-input preimage is
    // the same length as a two-input one's, so validation cost is linear in inputs and
    // not quadratic.
    EXPECT_EQ(SignatureHashPreimage(MakeChainId(0xAB),
                                    large,
                                    ComputeSigHashMidstates(large),
                                    0,
                                    5'000,
                                    MakeCondition(2))
                  .size(),
              SIGHASH_PREIMAGE_SIZE);
}

TEST(PrimitivesSigHash, IsDeterministic) {
    const Transaction tx = MakeTx();
    EXPECT_EQ(SigHashOf(tx), SigHashOf(tx));
}

TEST(PrimitivesSigHash, CommitsToTheChainId) {
    const Transaction tx = MakeTx();
    EXPECT_NE(SigHashOf(tx, MakeChainId(0xAB)), SigHashOf(tx, MakeChainId(0xCD)));
}

TEST(PrimitivesSigHash, CommitsToTheInputIndex) {
    const Transaction tx = MakeTx();
    EXPECT_NE(SigHashOf(tx, MakeChainId(0xAB), 0), SigHashOf(tx, MakeChainId(0xAB), 1));
}

TEST(PrimitivesSigHash, CommitsToTheSpentAmount) {
    const Transaction tx = MakeTx();
    EXPECT_NE(SigHashOf(tx, MakeChainId(0xAB), 0, 5'000),
              SigHashOf(tx, MakeChainId(0xAB), 0, 5'001));
}

TEST(PrimitivesSigHash, CommitsToTheRevealedCondition) {
    const Transaction tx = MakeTx();
    // Same keys, different threshold: a 2-of-2 signature must not authorise a 1-of-2.
    EXPECT_NE(SigHashOf(tx, MakeChainId(0xAB), 0, 5'000, MakeCondition(2)),
              SigHashOf(tx, MakeChainId(0xAB), 0, 5'000, MakeCondition(1)));
}

TEST(PrimitivesSigHash, CommitsToTheTransactionVersionAndLocktime) {
    const Transaction base = MakeTx();

    Transaction other_version = base;
    other_version.version = 2;
    EXPECT_NE(SigHashOf(base), SigHashOf(other_version));

    Transaction other_locktime = base;
    other_locktime.locktime = 500'000;
    EXPECT_NE(SigHashOf(base), SigHashOf(other_locktime));
}

TEST(PrimitivesSigHash, CommitsToEveryOutpoint) {
    const Transaction base = MakeTx();

    Transaction other_txid = base;
    other_txid.inputs[1].outpoint.txid.Array()[0] = 0x33;
    EXPECT_NE(SigHashOf(base), SigHashOf(other_txid));

    Transaction other_index = base;
    other_index.inputs[1].outpoint.index = 7;
    EXPECT_NE(SigHashOf(base), SigHashOf(other_index));
}

TEST(PrimitivesSigHash, CommitsToEverySequence) {
    Transaction changed = MakeTx();
    changed.inputs[1].sequence = 0xFFFF'FFFEU;
    EXPECT_NE(SigHashOf(MakeTx()), SigHashOf(changed));
}

TEST(PrimitivesSigHash, CommitsToEveryOutputAmountAndLock) {
    const Transaction base = MakeTx();

    Transaction other_amount = base;
    other_amount.outputs[1].amount = 2'001;
    EXPECT_NE(SigHashOf(base), SigHashOf(other_amount));

    Transaction other_program = base;
    other_program.outputs[1].lock.program[0] = 0x99;
    EXPECT_NE(SigHashOf(base), SigHashOf(other_program));

    Transaction other_lock_version = base;
    other_lock_version.outputs[1].lock.version = 2;
    EXPECT_NE(SigHashOf(base), SigHashOf(other_lock_version));
}

TEST(PrimitivesSigHash, CommitsToTheInputAndOutputCounts) {
    const Transaction base = MakeTx();

    Transaction extra_input = base;
    extra_input.inputs.push_back(base.inputs[1]);
    EXPECT_NE(SigHashOf(base), SigHashOf(extra_input));

    Transaction extra_output = base;
    extra_output.outputs.push_back(base.outputs[1]);
    EXPECT_NE(SigHashOf(base), SigHashOf(extra_output));

    // Removing an output is the attack the outputs hash exists to stop.
    Transaction fewer_outputs = base;
    fewer_outputs.outputs.pop_back();
    EXPECT_NE(SigHashOf(base), SigHashOf(fewer_outputs));
}

TEST(PrimitivesSigHash, DoesNotCommitToTheWitnessSection) {
    // A signature cannot commit to the bytes that contain it. Witness malleability is
    // prevented by the wtxid instead, and this test pins the boundary: if the sighash
    // ever started covering witnesses, signing would become impossible.
    Transaction with_witness = MakeTx();
    with_witness.witnesses = {
        Witness{.condition = MakeCondition(2), .signatures = {}},
        Witness{.condition = MakeCondition(2), .signatures = {}},
    };
    EXPECT_EQ(SigHashOf(MakeTx()), SigHashOf(with_witness));
}

TEST(PrimitivesSigHash, TheMidstatesAreDomainSeparatedFromEachOther) {
    // A transaction whose three lists are all empty: without the domain byte the three
    // midstates would be hashes of the same bytes and therefore equal.
    Transaction empty;
    empty.version = 1;
    const SigHashMidstates midstates = ComputeSigHashMidstates(empty);
    EXPECT_NE(midstates.outpoints, midstates.sequences);
    EXPECT_NE(midstates.outpoints, midstates.outputs);
    EXPECT_NE(midstates.sequences, midstates.outputs);
}

TEST(PrimitivesSigHash, TheConditionCommitmentIsWhatAVersionOneLockHolds) {
    // The same 32 bytes appear in the output that creates a coin and in the message
    // that authorises spending it. This is the check that they are one value.
    const SpendCondition condition = MakeCondition(2);
    Writer writer;
    condition.Serialize(writer);
    EXPECT_EQ(SpendConditionCommitment(condition),
              TaggedHash("Amarian/SpendCondition", writer.Bytes()));
    EXPECT_EQ(SPEND_CONDITION_TAG, "Amarian/SpendCondition");
    EXPECT_NE(SpendConditionCommitment(MakeCondition(1)), SpendConditionCommitment(condition));
}

TEST(PrimitivesSigHash, TheSigHashIsNotAPlainDoubleSha256OfThePreimage) {
    // Domain separation against the txid/wtxid/PoW construction. A message that could
    // also be read as a transaction id is a message that could be replayed as one.
    const Transaction tx = MakeTx();
    const ByteVec preimage = SignatureHashPreimage(
        MakeChainId(0xAB), tx, ComputeSigHashMidstates(tx), 0, 5'000, MakeCondition(2));
    EXPECT_NE(SigHashOf(tx), DoubleSha256(preimage));
    EXPECT_NE(SigHashOf(tx), Sha256(preimage));
}

}  // namespace
}  // namespace amarian
