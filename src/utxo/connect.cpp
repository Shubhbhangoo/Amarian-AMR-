/// \file
/// Connecting and disconnecting a block, and the encodings of the undo record.

#include <amarian/primitives/amount.hpp>
#include <amarian/util/overflow.hpp>
#include <amarian/utxo/connect.hpp>

#include <cstddef>
#include <optional>
#include <utility>

namespace amarian::utxo {
namespace {

using consensus::Computed;
using consensus::ValidationError;
using consensus::Verdict;

/// The outpoint of output `index` of the transaction with id `txid`.
[[nodiscard]] OutPoint OutPointFor(const Hash256& txid, size_t index) noexcept {
    // The cast is safe because a transaction's output count is bounded by `TxLimits`,
    // which is far below 2^32, and `CheckTransaction` has already enforced it.
    return OutPoint{.txid = txid, .index = static_cast<uint32_t>(index)};
}

/// Whether an output is worth keeping in the UTXO set at all.
///
/// A lock nothing can ever open holds coins that are gone. Storing it would mean every node
/// carrying it for ever, so it is dropped — the same conclusion `lock.hpp` anticipates. The
/// coins are still destroyed, which is the point: this decides only whether the set has to
/// remember that they were.
///
/// Connect and disconnect must agree on this predicate exactly. They do because it is one
/// function used by both, which is the only way to make "the same outputs were skipped" a
/// fact rather than a hope.
[[nodiscard]] bool IsStored(const TxOutput& output) noexcept {
    return !output.lock.IsUnspendable();
}

}  // namespace

// --- The undo record's encoding ---------------------------------------------------

void TxUndo::Serialize(Writer& writer) const {
    writer.WriteCompactSize(spent.size());
    for (const Coin& coin : spent) {
        coin.Serialize(writer);
    }
}

bool TxUndo::Deserialize(Reader& reader,
                         TxUndo& out,
                         size_t max_inputs,
                         size_t max_lock_program_size) {
    uint64_t count = 0;
    if (!reader.ReadCompactSize(count, Coin::MIN_SERIALIZED_SIZE) ||
        std::cmp_greater(count, max_inputs)) {
        reader.Fail();
        return false;
    }
    const std::optional<size_t> narrowed = TryNarrow<size_t>(count);
    if (!narrowed.has_value()) {
        reader.Fail();
        return false;
    }

    TxUndo decoded;
    decoded.spent.reserve(*narrowed);
    for (size_t index = 0; index < *narrowed; ++index) {
        Coin coin;
        if (!Coin::Deserialize(reader, coin, max_lock_program_size)) {
            return false;
        }
        decoded.spent.push_back(std::move(coin));
    }
    out = std::move(decoded);
    return true;
}

void BlockUndo::Serialize(Writer& writer) const {
    writer.WriteCompactSize(transactions.size());
    for (const TxUndo& tx_undo : transactions) {
        tx_undo.Serialize(writer);
    }
}

bool BlockUndo::Deserialize(Reader& reader, BlockUndo& out, const ChainParams& params) {
    // One entry per non-coinbase transaction, so a block's transaction limit bounds the
    // count. An empty `TxUndo` is one byte, which is what the count is checked against
    // before anything is reserved. That the count matches the *particular* block being
    // disconnected is `DisconnectBlock`'s check, not the codec's: the codec does not know
    // which block this record belongs to and should not pretend to.
    uint64_t count = 0;
    if (!reader.ReadCompactSize(count, 1) ||
        std::cmp_greater(count, params.block_limits.max_transactions)) {
        reader.Fail();
        return false;
    }
    const std::optional<size_t> narrowed = TryNarrow<size_t>(count);
    if (!narrowed.has_value()) {
        reader.Fail();
        return false;
    }

    BlockUndo decoded;
    decoded.transactions.reserve(*narrowed);
    for (size_t index = 0; index < *narrowed; ++index) {
        TxUndo tx_undo;
        if (!TxUndo::Deserialize(reader,
                                 tx_undo,
                                 params.block_limits.tx.max_inputs,
                                 params.block_limits.tx.max_lock_program_size)) {
            return false;
        }
        decoded.transactions.push_back(std::move(tx_undo));
    }
    out = std::move(decoded);
    return true;
}

std::string_view Describe(DisconnectError error) noexcept {
    switch (error) {
        case DisconnectError::UndoDoesNotDescribeBlock:
            return "undo record does not describe the block being disconnected";
        case DisconnectError::CreatedCoinMissing:
            return "an output the block created is not in the UTXO set";
        case DisconnectError::CreatedCoinDoesNotMatch:
            return "an output the block created is in the UTXO set with different contents";
        case DisconnectError::SpentOutpointOccupied:
            return "an outpoint the block spent is unspent again";
    }
    // Unreachable: the switch is total, and -Wswitch-enum makes a new enumerator a build
    // failure here rather than a silent fall-through.
    return "unknown disconnect error";
}

// --- Connect ---------------------------------------------------------------------

Computed<ConnectResult>
ConnectBlock(const Block& block, CoinsCache& coins, const ChainParams& params) {
    // `CheckBlock` guarantees both of these. They are re-checked because the loop below
    // indexes from them, and a bound that memory safety depends on is not one to take on
    // trust from a caller.
    if (block.transactions.empty()) {
        return std::unexpected(ValidationError::BlockNoTransactions);
    }
    if (!block.transactions.front().IsCoinbase()) {
        return std::unexpected(ValidationError::BlockFirstTxNotCoinbase);
    }

    // Every change lands here, and reaches `coins` only if the whole block succeeds. A rule
    // that fails half way through therefore leaves the caller's set exactly as it was.
    CoinsCache batch = CoinsCache::Over(coins);

    const uint32_t height = block.header.height;
    ConnectResult result;
    result.undo.transactions.reserve(block.transactions.size() - 1);
    int64_t total_fees = 0;

    for (size_t tx_index = 0; tx_index < block.transactions.size(); ++tx_index) {
        const Transaction& tx = block.transactions[tx_index];
        const bool is_coinbase = tx_index == 0;
        if (tx.IsCoinbase() != is_coinbase) {
            // Also `CheckBlock`'s, and re-checked for the same reason: the coinbase flag
            // decides the maturity rule for every coin created below.
            return std::unexpected(ValidationError::BlockMultipleCoinbases);
        }

        if (!is_coinbase) {
            // Removing each coin as it is found is what makes a double spend *within* the
            // block impossible here rather than only in `CheckBlock`: the second attempt
            // finds nothing. The removed value is exactly what the undo record needs, so
            // one lookup serves validation and the record both.
            TxUndo tx_undo;
            tx_undo.spent.reserve(tx.inputs.size());
            for (const TxInput& input : tx.inputs) {
                std::optional<Coin> coin = batch.SpendCoin(input.outpoint);
                if (!coin.has_value()) {
                    return std::unexpected(ValidationError::TxInputMissingOrSpent);
                }
                tx_undo.spent.push_back(std::move(*coin));
            }

            const Computed<int64_t> fee =
                consensus::CheckTransactionInputs(tx, tx_undo.spent, height, params);
            if (!fee.has_value()) {
                return std::unexpected(fee.error());
            }
            if (!TryAccumulate(total_fees, *fee) || !IsValidAmount(total_fees)) {
                return std::unexpected(ValidationError::BlockFeesOutOfRange);
            }

            result.undo.transactions.push_back(std::move(tx_undo));
        }

        const Hash256 txid = tx.Txid();
        for (size_t out_index = 0; out_index < tx.outputs.size(); ++out_index) {
            const TxOutput& output = tx.outputs[out_index];
            if (!IsStored(output)) {
                continue;
            }
            const Coin coin{.output = output, .height = height, .is_coinbase = is_coinbase};
            if (!batch.AddCoin(OutPointFor(txid, out_index), coin)) {
                // An outpoint that is already unspent. Overwriting it would destroy coins
                // silently, so it is rejected instead. Amarian makes this unreachable by
                // construction — height in the coinbase's `sequence` makes every coinbase
                // txid distinct, and a duplicate non-coinbase txid would have had to
                // re-spend outpoints the first one already consumed — but the set enforces
                // the invariant rather than assuming it, because the cost of being wrong
                // about it is a coin that vanishes with no rule having rejected anything.
                return std::unexpected(ValidationError::TxCreatesExistingOutpoint);
            }
        }
    }

    // The supply ceiling, last, and still consensus's rule rather than this layer's: the
    // fees are the one input to it that only the UTXO set could have produced.
    if (const Verdict amount = consensus::CheckCoinbaseAmount(block, total_fees, params);
        !amount.has_value()) {
        return std::unexpected(amount.error());
    }

    batch.Flush(coins);
    result.total_fees = total_fees;
    return result;
}

// --- Disconnect ------------------------------------------------------------------

std::expected<void, DisconnectError>
DisconnectBlock(const Block& block, const BlockUndo& undo, CoinsCache& coins) {
    if (block.transactions.empty() || undo.transactions.size() + 1 != block.transactions.size()) {
        return std::unexpected(DisconnectError::UndoDoesNotDescribeBlock);
    }

    CoinsCache batch = CoinsCache::Over(coins);
    const uint32_t height = block.header.height;

    // Reverse block order, and this is not a stylistic mirror: a transaction may spend an
    // output created earlier in the same block, so undoing forwards would try to remove a
    // coin the later transaction has not yet given back.
    for (size_t remaining = block.transactions.size(); remaining > 0; --remaining) {
        const size_t tx_index = remaining - 1;
        const Transaction& tx = block.transactions[tx_index];
        const bool is_coinbase = tx_index == 0;

        // Remove what it created, checking each coin for full equality against what the
        // block says was created there. Presence alone would let a set that had drifted
        // pass, and "revert restores exactly what was removed" is the property a
        // reorganisation stakes everything on — an approximate revert is two nodes that
        // both believe they are on the same chain holding different money.
        const Hash256 txid = tx.Txid();
        for (size_t out_index = 0; out_index < tx.outputs.size(); ++out_index) {
            const TxOutput& output = tx.outputs[out_index];
            if (!IsStored(output)) {
                continue;
            }
            const std::optional<Coin> removed = batch.SpendCoin(OutPointFor(txid, out_index));
            if (!removed.has_value()) {
                return std::unexpected(DisconnectError::CreatedCoinMissing);
            }
            const Coin created{.output = output, .height = height, .is_coinbase = is_coinbase};
            if (*removed != created) {
                return std::unexpected(DisconnectError::CreatedCoinDoesNotMatch);
            }
        }

        if (is_coinbase) {
            continue;
        }

        // Restore what it spent. The undo vector is indexed by non-coinbase position, which
        // is `tx_index - 1` because the coinbase has no entry.
        const TxUndo& tx_undo = undo.transactions[tx_index - 1];
        if (tx_undo.spent.size() != tx.inputs.size()) {
            return std::unexpected(DisconnectError::UndoDoesNotDescribeBlock);
        }
        for (size_t remaining_input = tx.inputs.size(); remaining_input > 0; --remaining_input) {
            const size_t input_index = remaining_input - 1;
            if (!batch.AddCoin(tx.inputs[input_index].outpoint, tx_undo.spent[input_index])) {
                // The outpoint this block spent is unspent already, so something else has
                // put a coin there. Restoring over it would create money.
                return std::unexpected(DisconnectError::SpentOutpointOccupied);
            }
        }
    }

    batch.Flush(coins);
    return {};
}

}  // namespace amarian::utxo
