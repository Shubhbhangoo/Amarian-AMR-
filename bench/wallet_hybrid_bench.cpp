/// Phase 7 benchmark: classical, post-quantum, and hybrid ownership.

#include <amarian/consensus/params.hpp>
#include <amarian/consensus/validation.hpp>
#include <amarian/crypto/signature.hpp>
#include <amarian/primitives/coin.hpp>
#include <amarian/primitives/sighash.hpp>
#include <amarian/primitives/spend_condition.hpp>
#include <amarian/primitives/transaction.hpp>
#include <amarian/wallet/hybrid.hpp>
#include <amarian/wallet/signing.hpp>

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace amarian::wallet {
namespace {

using crypto::SCHEME_ML_DSA_44;
using crypto::SCHEME_SCHNORR_SECP256K1;

struct Samples {
    Coin schnorr_coin;
    Coin mldsa44_coin;
    Coin hybrid_coin;
    Transaction schnorr;
    Transaction mldsa44;
    Transaction hybrid;
};

Transaction BaseTransaction() {
    Transaction tx;
    tx.version = 1;
    tx.locktime = 0;
    tx.inputs.push_back(TxInput{.outpoint = OutPoint{Hash256::FromBytes(
                                      std::array<uint8_t, Hash256::SIZE>{0x42}),
                                  0},
                              .sequence = 0});
    tx.outputs.push_back(TxOutput{.amount = 99'000, .lock = Lock{1, ByteVec(32, 0xBB)}});
    tx.witnesses.emplace_back();
    return tx;
}

Transaction SignSingle(Transaction tx, const Coin& coin, const SpendCondition& condition,
                       ByteSpan private_key, uint16_t scheme) {
    const SigHashMidstates midstates = ComputeSigHashMidstates(tx);
    const Hash256 sighash = SignatureHash(REGTEST_PARAMS.chain_id, tx, midstates, 0,
                                          coin.output.amount, condition);
    ByteVec signature;
    if (scheme == SCHEME_SCHNORR_SECP256K1) {
        signature = *SignSchnorr(private_key, sighash);
    } else {
        signature = *SignMldsa44(private_key, sighash);
    }
    tx.witnesses[0].condition = condition;
    tx.witnesses[0].signatures.push_back(
        Signature{.scheme = scheme, .bytes = std::move(signature)});
    return tx;
}

Samples MakeSamples() {
    Samples samples;
    std::array<uint8_t, 32> seed{};
    seed.fill(0x11);
    const auto schnorr = GenerateSchnorrKey(seed);
    const auto mldsa44 = GenerateMldsa44Key();
    if (!schnorr.has_value() || !mldsa44.has_value()) std::abort();

    SpendCondition schnorr_condition;
    schnorr_condition.version = CONDITION_VERSION_THRESHOLD;
    schnorr_condition.threshold = 1;
    schnorr_condition.keys.push_back(PublicKey{.scheme = SCHEME_SCHNORR_SECP256K1,
                                               .bytes = schnorr->public_key});

    SpendCondition mldsa_condition;
    mldsa_condition.version = CONDITION_VERSION_THRESHOLD;
    mldsa_condition.threshold = 1;
    mldsa_condition.keys.push_back(
        PublicKey{.scheme = SCHEME_ML_DSA_44, .bytes = mldsa44->public_key});

    const SpendCondition hybrid_condition =
        MakeHybridCondition(schnorr->public_key, mldsa44->public_key);

    for (Coin* coin : {&samples.schnorr_coin, &samples.mldsa44_coin, &samples.hybrid_coin}) {
        coin->output.amount = 100'000;
        coin->height = 1;
        coin->is_coinbase = false;
    }

    samples.schnorr = BaseTransaction();
    samples.mldsa44 = BaseTransaction();
    samples.hybrid = BaseTransaction();

    const Hash256 schnorr_commitment = SpendConditionCommitment(schnorr_condition);
    samples.schnorr_coin.output.lock = Lock{LOCK_VERSION_CONDITION_COMMITMENT,
                                            ByteVec(schnorr_commitment.Data(),
                                                    schnorr_commitment.Data() + Hash256::SIZE)};
    samples.schnorr = SignSingle(std::move(samples.schnorr), samples.schnorr_coin,
                                 schnorr_condition,
                                 schnorr->private_key, SCHEME_SCHNORR_SECP256K1);

    const Hash256 mldsa_commitment = SpendConditionCommitment(mldsa_condition);
    samples.mldsa44_coin.output.lock = Lock{LOCK_VERSION_CONDITION_COMMITMENT,
                                            ByteVec(mldsa_commitment.Data(),
                                                    mldsa_commitment.Data() + Hash256::SIZE)};
    samples.mldsa44 = SignSingle(std::move(samples.mldsa44), samples.mldsa44_coin,
                                 mldsa_condition,
                                 mldsa44->private_key, SCHEME_ML_DSA_44);

    const Hash256 hybrid_commitment = SpendConditionCommitment(hybrid_condition);
    samples.hybrid_coin.output.lock = Lock{LOCK_VERSION_CONDITION_COMMITMENT,
                                           ByteVec(hybrid_commitment.Data(),
                                                   hybrid_commitment.Data() + Hash256::SIZE)};
    auto hybrid = SignHybridInput(samples.hybrid, samples.hybrid_coin, 0, hybrid_condition,
                                  schnorr->private_key, mldsa44->private_key, REGTEST_PARAMS);
    if (!hybrid.has_value()) std::abort();
    samples.hybrid = std::move(*hybrid);
    return samples;
}

const Samples& SamplesForBenchmark() {
    static const Samples samples = MakeSamples();
    return samples;
}

void BM_VerifySchnorr(benchmark::State& state) {
    const auto& samples = SamplesForBenchmark();
    for (auto _ : state) {
        benchmark::DoNotOptimize(consensus::CheckSpendAuthorisation(
            samples.schnorr, std::span<const Coin>(&samples.schnorr_coin, 1), REGTEST_PARAMS));
    }
}

void BM_VerifyMldsa44(benchmark::State& state) {
    const auto& samples = SamplesForBenchmark();
    for (auto _ : state) {
        benchmark::DoNotOptimize(consensus::CheckSpendAuthorisation(
            samples.mldsa44, std::span<const Coin>(&samples.mldsa44_coin, 1), REGTEST_PARAMS));
    }
}

void BM_VerifyHybrid(benchmark::State& state) {
    const auto& samples = SamplesForBenchmark();
    for (auto _ : state) {
        benchmark::DoNotOptimize(consensus::CheckSpendAuthorisation(
            samples.hybrid, std::span<const Coin>(&samples.hybrid_coin, 1), REGTEST_PARAMS));
    }
}

void BM_TransactionMetrics(benchmark::State& state) {
    const auto& samples = SamplesForBenchmark();
    state.counters["schnorr_weight"] = benchmark::Counter(
        static_cast<double>(samples.schnorr.Weight()));
    state.counters["mldsa44_weight"] = benchmark::Counter(
        static_cast<double>(samples.mldsa44.Weight()));
    state.counters["hybrid_weight"] = benchmark::Counter(
        static_cast<double>(samples.hybrid.Weight()));
    state.counters["schnorr_tx_per_block"] = benchmark::Counter(
        static_cast<double>(REGTEST_PARAMS.max_block_weight / samples.schnorr.Weight()));
    state.counters["mldsa44_tx_per_block"] = benchmark::Counter(
        static_cast<double>(REGTEST_PARAMS.max_block_weight / samples.mldsa44.Weight()));
    state.counters["hybrid_tx_per_block"] = benchmark::Counter(
        static_cast<double>(REGTEST_PARAMS.max_block_weight / samples.hybrid.Weight()));
    for (auto _ : state) benchmark::DoNotOptimize(samples.hybrid.Weight());
}

BENCHMARK(BM_VerifySchnorr)->MinTime(0.5);
BENCHMARK(BM_VerifyMldsa44)->MinTime(0.5);
BENCHMARK(BM_VerifyHybrid)->MinTime(0.5);
BENCHMARK(BM_TransactionMetrics)->Iterations(1);

}  // namespace
}  // namespace amarian::wallet
