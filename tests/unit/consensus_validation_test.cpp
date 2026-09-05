/// \file
/// The spend-authorisation rules: whether an input is actually allowed to spend the
/// output it names.
///
/// These are the rules that decide whether coins can be stolen, so the tests use **real
/// signatures** rather than stubs. ML-DSA-44 is the scheme used throughout, for two
/// reasons: it needs only OpenSSL, so this file adds no new dependency beyond the one
/// `test_crypto` already documents, and it signs in about two milliseconds where
/// SLH-DSA-SHA2-128s takes several hundred.
///
/// Scope is deliberately the new rules only. The context-free header, transaction and
/// block rules in the same translation unit are exercised elsewhere; what is asserted
/// here is the part that was not previously enforceable at all: the lock commitment, the
/// threshold walk, the two soft-fork extension points, and the fee.

#include <amarian/consensus/validation.hpp>

#include <amarian/consensus/params.hpp>
#include <amarian/crypto/signature.hpp>
#include <amarian/primitives/sighash.hpp>

#include <openssl/core_names.h>
#include <openssl/evp.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace amarian::consensus {
namespace {

const ChainParams& Params() {
    return REGTEST_PARAMS;
}

/// An ML-DSA-44 key pair that can sign more than once.
///
/// The node never signs — `amarian::crypto` exposes no signing interface on purpose — so
/// a test that wants a real signature has to produce one through OpenSSL directly, the
/// same reasoned exception `tests/unit/CMakeLists.txt` records for `test_crypto`.
class SigningKey {
  public:
    SigningKey() {
        EVP_PKEY_CTX* gen = EVP_PKEY_CTX_new_from_name(nullptr, "ML-DSA-44", nullptr);
        if (gen != nullptr && EVP_PKEY_keygen_init(gen) > 0) {
            EVP_PKEY_generate(gen, &key_);
        }
        EVP_PKEY_CTX_free(gen);

        size_t length = 0;
        EVP_PKEY_get_octet_string_param(key_, OSSL_PKEY_PARAM_PUB_KEY, nullptr, 0, &length);
        public_bytes_.resize(length);
        EVP_PKEY_get_octet_string_param(
            key_, OSSL_PKEY_PARAM_PUB_KEY, public_bytes_.data(), length, &length);
        public_bytes_.resize(length);
    }

    SigningKey(const SigningKey&) = delete;
    SigningKey& operator=(const SigningKey&) = delete;
    SigningKey(SigningKey&&) = delete;
    SigningKey& operator=(SigningKey&&) = delete;

    ~SigningKey() { EVP_PKEY_free(key_); }

    [[nodiscard]] PublicKey Public() const {
        return PublicKey{.scheme = crypto::SCHEME_ML_DSA_44, .bytes = public_bytes_};
    }

    [[nodiscard]] Signature Sign(const Hash256& message) const {
        Signature signature{.scheme = crypto::SCHEME_ML_DSA_44, .bytes = {}};
        EVP_SIGNATURE* algorithm = EVP_SIGNATURE_fetch(nullptr, "ML-DSA-44", nullptr);
        EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_from_pkey(nullptr, key_, nullptr);
        size_t length = 0;
        if (algorithm != nullptr && context != nullptr &&
            EVP_PKEY_sign_message_init(context, algorithm, nullptr) > 0 &&
            EVP_PKEY_sign(context, nullptr, &length, message.Data(), Hash256::SIZE) > 0) {
            signature.bytes.resize(length);
            if (EVP_PKEY_sign(
                    context, signature.bytes.data(), &length, message.Data(), Hash256::SIZE) > 0) {
                signature.bytes.resize(length);
            } else {
                signature.bytes.clear();
            }
        }
        EVP_PKEY_CTX_free(context);
        EVP_SIGNATURE_free(algorithm);
        return signature;
    }

  private:
    EVP_PKEY* key_ = nullptr;
    ByteVec public_bytes_;
};

/// One key pair per process. Generating an ML-DSA key is cheap but not free, and every
/// test below needs at most two.
SigningKey& FirstKey() {
    static SigningKey key;
    return key;
}

SigningKey& SecondKey() {
    static SigningKey key;
    return key;
}

[[nodiscard]] SpendCondition MakeCondition(std::vector<PublicKey> keys, uint8_t threshold) {
    // Consensus requires strictly ascending keys, so the fixture sorts rather than
    // hoping two freshly generated keys happen to come out in order.
    std::ranges::sort(keys);
    return SpendCondition{
        .version = CONDITION_VERSION_THRESHOLD, .threshold = threshold, .keys = std::move(keys)};
}

[[nodiscard]] Lock LockFor(const SpendCondition& condition) {
    const Hash256 commitment = SpendConditionCommitment(condition);
    return Lock{.version = LOCK_VERSION_CONDITION_COMMITMENT,
                .program = ByteVec(commitment.Span().begin(), commitment.Span().end())};
}

constexpr int64_t SPENT_AMOUNT = 500'000;
constexpr int64_t PAID_AMOUNT = 400'000;

/// A one-input, one-output transaction spending an output locked to `condition`, with the
/// witness left empty for the caller to fill.
[[nodiscard]] Transaction MakeSpend(const SpendCondition& condition) {
    Hash256 funding{};
    funding.Array()[0] = 0x42;

    Transaction tx;
    tx.version = 1;
    tx.inputs = {TxInput{.outpoint = OutPoint{.txid = funding, .index = 0}, .sequence = 0}};
    tx.outputs = {TxOutput{.amount = PAID_AMOUNT,
                           .lock = LockFor(MakeCondition({FirstKey().Public()}, 1))}};
    tx.witnesses = {Witness{.condition = condition, .signatures = {}}};
    return tx;
}

/// The message input 0 of `tx` must be signed over.
[[nodiscard]] Hash256 MessageFor(const Transaction& tx,
                                 const SpendCondition& condition,
                                 int64_t spent_amount = SPENT_AMOUNT) {
    return SignatureHash(
        Params().chain_id, tx, ComputeSigHashMidstates(tx), 0, spent_amount, condition);
}

/// The coins the fixture transactions spend. A single mature, non-coinbase coin, which is
/// the case every authorisation test below is about; the maturity rule has its own tests
/// that build a coinbase coin explicitly.
[[nodiscard]] std::array<Coin, 1> SpentOutputs(const SpendCondition& condition,
                                              int64_t amount = SPENT_AMOUNT) {
    return {Coin{.output = TxOutput{.amount = amount, .lock = LockFor(condition)},
                 .height = 1,
                 .is_coinbase = false}};
}

/// Asserts that a transaction is structurally sound before asking whether it is
/// authorised, so a failure below is never a structural mistake in the fixture.
void ExpectStructurallyValid(const Transaction& tx) {
    const Verdict verdict = CheckTransaction(tx, Params());
    ASSERT_TRUE(verdict.has_value()) << Describe(verdict.error());
}

TEST(Validation, ARealSignatureAuthorisesASingleKeySpend) {
    const SpendCondition condition = MakeCondition({FirstKey().Public()}, 1);
    Transaction tx = MakeSpend(condition);
    tx.witnesses[0].signatures = {FirstKey().Sign(MessageFor(tx, condition))};

    ExpectStructurallyValid(tx);
    const auto spent = SpentOutputs(condition);
    const Verdict verdict = CheckSpendAuthorisation(tx, spent, Params());
    EXPECT_TRUE(verdict.has_value()) << (verdict ? "" : Describe(verdict.error()));
}

TEST(Validation, ASignatureOverADifferentSpentAmountDoesNotAuthorise) {
    // The spent amount is in the message, so a signature is a claim about a specific coin
    // and cannot be lifted onto a larger one.
    const SpendCondition condition = MakeCondition({FirstKey().Public()}, 1);
    Transaction tx = MakeSpend(condition);
    tx.witnesses[0].signatures = {FirstKey().Sign(MessageFor(tx, condition, SPENT_AMOUNT + 1))};

    const auto spent = SpentOutputs(condition);
    const Verdict verdict = CheckSpendAuthorisation(tx, spent, Params());
    ASSERT_FALSE(verdict.has_value());
    EXPECT_EQ(verdict.error(), ValidationError::TxSignatureDoesNotVerify);
}

TEST(Validation, TheRevealedConditionMustBeTheOneTheOutputCommittedTo) {
    // Substituting a condition the spender controls for the one the coin named is the
    // whole attack a commitment lock exists to stop.
    const SpendCondition owner = MakeCondition({FirstKey().Public()}, 1);
    const SpendCondition attacker = MakeCondition({SecondKey().Public()}, 1);
    Transaction tx = MakeSpend(attacker);
    tx.witnesses[0].signatures = {SecondKey().Sign(MessageFor(tx, attacker))};

    // The output being spent is the owner's; the witness reveals the attacker's condition
    // and a signature that is perfectly valid for it.
    const auto spent = SpentOutputs(owner);
    const Verdict verdict = CheckSpendAuthorisation(tx, spent, Params());
    ASSERT_FALSE(verdict.has_value());
    EXPECT_EQ(verdict.error(), ValidationError::TxConditionDoesNotMatchLock);
}

TEST(Validation, ALockProgramOfTheWrongLengthMatchesNothing) {
    const SpendCondition condition = MakeCondition({FirstKey().Public()}, 1);
    Transaction tx = MakeSpend(condition);
    tx.witnesses[0].signatures = {FirstKey().Sign(MessageFor(tx, condition))};

    auto spent = SpentOutputs(condition);
    spent[0].output.lock.program.pop_back();
    const Verdict verdict = CheckSpendAuthorisation(tx, spent, Params());
    ASSERT_FALSE(verdict.has_value());
    EXPECT_EQ(verdict.error(), ValidationError::TxConditionDoesNotMatchLock);
}

TEST(Validation, LockVersionZeroCannotBeSpentByAnything) {
    const SpendCondition condition = MakeCondition({FirstKey().Public()}, 1);
    Transaction tx = MakeSpend(condition);
    tx.witnesses[0].signatures = {FirstKey().Sign(MessageFor(tx, condition))};

    auto spent = SpentOutputs(condition);
    spent[0].output.lock.version = LOCK_VERSION_UNSPENDABLE;
    const Verdict verdict = CheckSpendAuthorisation(tx, spent, Params());
    ASSERT_FALSE(verdict.has_value());
    EXPECT_EQ(verdict.error(), ValidationError::TxSpendsUnspendableOutput);
}

TEST(Validation, AnUnknownLockVersionStaysSpendableWithoutASignatureCheck) {
    // The soft-fork property. A version this build does not implement has no rules for it
    // to enforce, and a node that rejected it would fork itself off the chain as soon as
    // those rules were defined.
    const SpendCondition condition = MakeCondition({FirstKey().Public()}, 1);
    Transaction tx = MakeSpend(condition);
    tx.witnesses[0].signatures = {FirstKey().Sign(MessageFor(tx, condition))};
    // Deliberately unrelated to the condition: an unknown version's program means
    // whatever that version says, which this build cannot know.
    tx.witnesses[0].signatures[0].bytes[0] ^= 0xFF;

    auto spent = SpentOutputs(condition);
    spent[0].output.lock.version = 200;
    EXPECT_TRUE(CheckSpendAuthorisation(tx, spent, Params()).has_value());
}

TEST(Validation, AnUnknownKeySchemeCountsAsSatisfied) {
    // The other soft-fork extension point, and the one place consensus accepts something
    // it has not checked: this build cannot verify a scheme it does not implement, so an
    // upgraded majority enforces the rule instead.
    const SpendCondition condition =
        MakeCondition({PublicKey{.scheme = 40'000, .bytes = ByteVec(48, 0x01)}}, 1);
    Transaction tx = MakeSpend(condition);
    tx.witnesses[0].signatures = {Signature{.scheme = 40'000, .bytes = ByteVec(96, 0x02)}};

    ExpectStructurallyValid(tx);
    const auto spent = SpentOutputs(condition);
    EXPECT_TRUE(CheckSpendAuthorisation(tx, spent, Params()).has_value());
}

TEST(Validation, ATwoOfTwoNeedsBothSignaturesInKeyOrder) {
    const SpendCondition condition =
        MakeCondition({FirstKey().Public(), SecondKey().Public()}, 2);
    Transaction tx = MakeSpend(condition);
    const Hash256 message = MessageFor(tx, condition);

    // Whichever key sorted first must sign first. The ordered forward match is what makes
    // exactly one permutation valid, so a relayer cannot reorder a witness to produce a
    // second wtxid for the same transaction.
    const bool first_key_sorts_first = condition.keys.front() == FirstKey().Public();
    const Signature earlier =
        first_key_sorts_first ? FirstKey().Sign(message) : SecondKey().Sign(message);
    const Signature later =
        first_key_sorts_first ? SecondKey().Sign(message) : FirstKey().Sign(message);

    const auto spent = SpentOutputs(condition);

    tx.witnesses[0].signatures = {earlier, later};
    ExpectStructurallyValid(tx);
    EXPECT_TRUE(CheckSpendAuthorisation(tx, spent, Params()).has_value());

    tx.witnesses[0].signatures = {later, earlier};
    const Verdict swapped = CheckSpendAuthorisation(tx, spent, Params());
    ASSERT_FALSE(swapped.has_value());
    EXPECT_EQ(swapped.error(), ValidationError::TxSignatureDoesNotVerify);
}

TEST(Validation, AThresholdIsNotMetByOfferingOneKeysSignatureTwice) {
    // Without a monotonically advancing key index, one signature repeated would satisfy a
    // 2-of-2 — which is a 1-of-2 wearing a 2-of-2's address.
    const SpendCondition condition =
        MakeCondition({FirstKey().Public(), SecondKey().Public()}, 2);
    Transaction tx = MakeSpend(condition);
    const Signature signature = FirstKey().Sign(MessageFor(tx, condition));
    tx.witnesses[0].signatures = {signature, signature};

    ExpectStructurallyValid(tx);
    const auto spent = SpentOutputs(condition);
    const Verdict verdict = CheckSpendAuthorisation(tx, spent, Params());
    ASSERT_FALSE(verdict.has_value());
    EXPECT_EQ(verdict.error(), ValidationError::TxSignatureDoesNotVerify);
}

TEST(Validation, OneOfTwoIsSatisfiedByEitherKeyAlone) {
    const SpendCondition condition =
        MakeCondition({FirstKey().Public(), SecondKey().Public()}, 1);
    Transaction tx = MakeSpend(condition);
    const Hash256 message = MessageFor(tx, condition);
    const auto spent = SpentOutputs(condition);

    tx.witnesses[0].signatures = {FirstKey().Sign(message)};
    EXPECT_TRUE(CheckSpendAuthorisation(tx, spent, Params()).has_value());

    tx.witnesses[0].signatures = {SecondKey().Sign(message)};
    EXPECT_TRUE(CheckSpendAuthorisation(tx, spent, Params()).has_value());
}

TEST(Validation, TheSpentOutputCountMustMatchTheInputCount) {
    const SpendCondition condition = MakeCondition({FirstKey().Public()}, 1);
    Transaction tx = MakeSpend(condition);
    tx.witnesses[0].signatures = {FirstKey().Sign(MessageFor(tx, condition))};

    const Verdict verdict = CheckSpendAuthorisation(tx, std::span<const Coin>{}, Params());
    ASSERT_FALSE(verdict.has_value());
    EXPECT_EQ(verdict.error(), ValidationError::TxSpentOutputCountMismatch);
}

TEST(Validation, ACoinbaseHasNothingToAuthoriseAndNoFee) {
    Transaction coinbase;
    coinbase.version = 1;
    coinbase.inputs = {MakeCoinbaseInput(1)};
    coinbase.outputs = {TxOutput{.amount = 100,
                                 .lock = LockFor(MakeCondition({FirstKey().Public()}, 1))}};
    ASSERT_TRUE(coinbase.IsCoinbase());

    const Verdict authorised =
        CheckSpendAuthorisation(coinbase, std::span<const Coin>{}, Params());
    ASSERT_FALSE(authorised.has_value());
    EXPECT_EQ(authorised.error(), ValidationError::TxCoinbaseAuthorisesNothing);

    const Computed<int64_t> fee = TransactionFee(coinbase, std::span<const Coin>{});
    ASSERT_FALSE(fee.has_value());
    EXPECT_EQ(fee.error(), ValidationError::TxCoinbaseAuthorisesNothing);
}

TEST(Validation, TheFeeIsWhatIsSpentMinusWhatIsPaid) {
    const SpendCondition condition = MakeCondition({FirstKey().Public()}, 1);
    const Transaction tx = MakeSpend(condition);
    const auto spent = SpentOutputs(condition);

    const Computed<int64_t> fee = TransactionFee(tx, spent);
    ASSERT_TRUE(fee.has_value()) << Describe(fee.error());
    EXPECT_EQ(*fee, SPENT_AMOUNT - PAID_AMOUNT);
}

TEST(Validation, ATransactionMayNotPayOutMoreThanItSpends) {
    // Half of the supply guarantee: no transaction creates value.
    const SpendCondition condition = MakeCondition({FirstKey().Public()}, 1);
    const Transaction tx = MakeSpend(condition);
    const auto spent = SpentOutputs(condition, PAID_AMOUNT - 1);

    const Computed<int64_t> fee = TransactionFee(tx, spent);
    ASSERT_FALSE(fee.has_value());
    EXPECT_EQ(fee.error(), ValidationError::TxOutputsExceedInputs);
}

TEST(Validation, AmountsOutsideTheMoneyRangeAreRejectedOnBothSides) {
    const SpendCondition condition = MakeCondition({FirstKey().Public()}, 1);
    Transaction tx = MakeSpend(condition);

    const auto negative = SpentOutputs(condition, -1);
    ASSERT_FALSE(TransactionFee(tx, negative).has_value());
    EXPECT_EQ(TransactionFee(tx, negative).error(), ValidationError::TxInputSumOutOfRange);

    tx.outputs[0].amount = MAX_MONEY + 1;
    const auto spent = SpentOutputs(condition);
    ASSERT_FALSE(TransactionFee(tx, spent).has_value());
    EXPECT_EQ(TransactionFee(tx, spent).error(), ValidationError::TxOutputSumOutOfRange);
}

TEST(Validation, AKeyOrSignatureOfTheWrongLengthIsRejectedStructurally) {
    // Rejected in the cheap context-free pass, so an attacker cannot make a node reach
    // the cryptography with bytes that could never have parsed.
    SpendCondition condition = MakeCondition({FirstKey().Public()}, 1);
    condition.keys[0].bytes.pop_back();
    const Verdict key = CheckSpendCondition(condition, Params());
    ASSERT_FALSE(key.has_value());
    EXPECT_EQ(key.error(), ValidationError::ConditionKeyWrongSize);

    const Witness witness{
        .condition = MakeCondition({FirstKey().Public()}, 1),
        .signatures = {
            Signature{.scheme = crypto::SCHEME_ML_DSA_44, .bytes = ByteVec(64, 0x01)}}};
    const Verdict signature = CheckWitness(witness, Params());
    ASSERT_FALSE(signature.has_value());
    EXPECT_EQ(signature.error(), ValidationError::WitnessSignatureWrongSize);
}

}  // namespace
}  // namespace amarian::consensus
