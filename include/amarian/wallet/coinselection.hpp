#pragma once

/// \file
/// Coin selection: branch-and-bound search with a knapsack fallback.
///
/// Requirements, in priority order:
/// 1. Never overpay by more than necessary (excess becomes change, costing outputs)
/// 2. Prefer exact matches (avoid creating change at all)
/// 3. Never create an output below the dust threshold
/// 4. Minimise common-input clustering where possible
/// 5. Be deterministic given the same inputs and randomness

#include <amarian/primitives/coin.hpp>
#include <amarian/primitives/transaction.hpp>
#include <amarian/util/types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace amarian::wallet {

/// Minimum output value worth creating. Any output below this is better left as
/// fee than stored permanently in every node's UTXO set.
///
/// Provisional: set at the cost of spending a single input at a typical fee rate.
/// Phase 12 will tune this against measured fee markets.
inline constexpr int64_t DUST_THRESHOLD = 1000;  // 1000 facets

/// One spendable output the wallet knows about.
struct UtxoEntry {
    OutPoint outpoint;
    Coin coin;
    /// Whether this output is confirmed (true) or still in the mempool (false).
    bool confirmed;
    /// Derivation metadata used to sign this input.
    uint32_t account_id = 0;
    uint32_t derivation_index = 0;
};

/// The result of coin selection.
struct CoinSelection {
    /// Inputs to spend.
    std::vector<UtxoEntry> inputs;
    /// Total value of selected inputs.
    int64_t total_selected = 0;
    /// Target amount (what we needed to send).
    int64_t target = 0;
    /// Change amount (excess returned as a new output, may be 0).
    int64_t change = 0;
    /// Estimated fee for the transaction.
    int64_t fee = 0;
    /// Whether the selection succeeded.
    bool success = false;
};

/// Selects coins to meet `target` (amount to send) plus `fee_rate` per weight unit.
///
/// Uses branch-and-bound for exact-match search, falling back to a knapsack
/// approximation when no exact match exists.
///
/// `utxos` is the set of all spendable outputs the wallet controls. It is sorted
/// inside the function by descending value-density.
[[nodiscard]] CoinSelection
SelectCoins(const std::vector<UtxoEntry>& utxos, int64_t target, int64_t fee_rate);

/// Estimates the transaction weight given `num_inputs`, `num_outputs`, and whether
/// the inputs are P2PKH-like (locked with a condition commitment).
[[nodiscard]] size_t EstimateTxWeight(size_t num_inputs, size_t num_outputs,
                                       int64_t input_signature_bytes);

/// The dust threshold for a given fee rate: any output worth less than the cost
/// to spend it (one input + one output) is dust.
[[nodiscard]] int64_t DustForFeeRate(int64_t fee_rate);

}  // namespace amarian::wallet
