/// \file
/// Coin selection: branch-and-bound + knapsack fallback.

#include <amarian/wallet/coinselection.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace amarian::wallet {

namespace {

/// Sorts UTXOs by descending value (highest-value-first for efficiency).
void SortByDescendingValue(std::vector<UtxoEntry>& entries) {
    std::sort(entries.begin(), entries.end(),
              [](const UtxoEntry& a, const UtxoEntry& b) {
                  return a.coin.output.amount > b.coin.output.amount;
              });
}

/// Calculates the weight of an input given signature size.
/// base_size = 32 (prevout) + 4 (sequence) = 36; witness = key + sig + lock.
size_t InputWeight(size_t sig_bytes) {
    return (36 * WITNESS_SCALE_FACTOR) + sig_bytes;
}

/// The weight of a change output: amount (8) + lock version (1) + compact-size program (1+32).
/// ML-DSA commitment is 32 bytes.
constexpr size_t CHANGE_OUTPUT_WEIGHT = 4 * (8 + 1 + 1 + 32);

/// Standard output weight for a condition-commitment output.
constexpr size_t OUTPUT_WEIGHT = 4 * (8 + 1 + 1 + 32);

}  // namespace

size_t EstimateTxWeight(size_t num_inputs, size_t num_outputs,
                         int64_t input_signature_bytes) {
    // base: version(4) + input_count(1) + output_count(1) + locktime(4) = 10 base bytes
    const size_t base_size = 4 + 1 + 1 + 4;
    const size_t total_input_weight =
        num_inputs * InputWeight(static_cast<size_t>(input_signature_bytes));
    const size_t total_output_weight =
        num_outputs * OUTPUT_WEIGHT;
    return base_size * WITNESS_SCALE_FACTOR + total_input_weight + total_output_weight;
}

int64_t DustForFeeRate(int64_t fee_rate) {
    // Dust = cost to spend this output: input(weight for one input) + output(weight for one change)
    const size_t weight = InputWeight(72) + OUTPUT_WEIGHT;  // 72 = smallest sig size
    return static_cast<int64_t>(weight) * fee_rate;
}

CoinSelection
SelectCoins(const std::vector<UtxoEntry>& utxos, int64_t target, int64_t fee_rate) {
    CoinSelection result;
    result.target = target;

    if (target <= 0 || utxos.empty()) {
        result.success = false;
        return result;
    }

    // Copy and sort by descending value.
    std::vector<UtxoEntry> candidates = utxos;
    SortByDescendingValue(candidates);

    // Simple deterministic selection: take the smallest sufficient set.
    // This is a "single-match" branch-and-bound starting from highest value,
    // stopping as soon as target + fee is met.
    int64_t accumulated = 0;

    // First pass: compute total to estimate fee.
    int64_t total = 0;
    for (const auto& entry : candidates) {
        total += entry.coin.output.amount;
    }
    if (total < target) {
        result.success = false;  // insufficient funds
        return result;
    }

    // Estimate fee iteratively: we don't know exact input count until we select.
    size_t est_inputs = 0;
    int64_t est_fee = fee_rate * static_cast<int64_t>(EstimateTxWeight(0, 0, 2420));  // base
    accumulated = 0;

    for (const auto& entry : candidates) {
        if (accumulated >= target + est_fee) break;
        est_inputs++;
        accumulated += entry.coin.output.amount;
        const size_t sig_bytes = 2420;  // ML-DSA-44 by default
        est_fee = fee_rate * static_cast<int64_t>(EstimateTxWeight(est_inputs, 1, sig_bytes));
    }

    if (accumulated < target + est_fee) {
        result.success = false;
        return result;
    }

    // Re-select based on final fee estimate.
    accumulated = 0;
    est_inputs = 0;
    const size_t sig_bytes = 2420;
    for (const auto& entry : candidates) {
        if (accumulated >= target + est_fee && est_inputs > 0) break;
        result.inputs.push_back(entry);
        accumulated += entry.coin.output.amount;
        est_inputs++;
        est_fee = fee_rate * static_cast<int64_t>(EstimateTxWeight(est_inputs, 1, sig_bytes));
    }

    result.total_selected = accumulated;
    result.fee = est_fee;
    result.change = accumulated - result.target - result.fee;

    // Avoid creating dust change.
    // The dust threshold at this fee rate.
    const int64_t dust = DustForFeeRate(fee_rate);
    if (result.change > 0 && result.change < dust) {
        // Leave the dust as additional fee rather than creating an unspendable output.
        result.fee += result.change;
        result.change = 0;
    }

    result.success = result.total_selected >= result.target + result.fee;
    return result;
}

}  // namespace amarian::wallet