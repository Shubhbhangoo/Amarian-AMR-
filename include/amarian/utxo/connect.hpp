#pragma once

/// \file
/// Applying a block to the UTXO set, and taking it back off again.
///
/// A block is an edit to the set of unspent coins: every input removes one, every output
/// adds one. This file is the only place that edit is performed, and it performs it in
/// both directions, because a chain that can only move forward cannot reorganise — and a
/// chain that cannot reorganise is not a consensus system, it is a ledger with an opinion.
///
/// ## Why an undo record exists
///
/// Connecting is not invertible from the block alone. The outputs a block created are in
/// it, so removing them again needs nothing else; the coins it *spent* are not — the block
/// names their outpoints, not their contents. Restoring them requires the amounts, locks,
/// creation heights and coinbase flags that were removed, which is exactly what `BlockUndo`
/// records. It is written when the block is connected because that is the only moment the
/// data is in hand.
///
/// This is why `BlockUndo` has an encoding. A reorganisation may begin after a restart, so
/// the undo data for every block on the active chain has to survive one.
///
/// ## Atomicity
///
/// Both operations build their own `CoinsCache` over the caller's set and flush it only on
/// success. A rule that fails half way through a block therefore leaves the caller's set
/// exactly as it was, rather than partially edited in a way no later code could repair.
/// One extra hash map per block is nothing measured against the signature verification the
/// same block requires, and it removes the entire class of bug where a rejected block has
/// already changed the state.
///
/// ## What is *not* here
///
/// Which chain is the real one. `ConnectBlock` applies a block to a set; deciding that this
/// block is the one to apply — that its predecessor is the current tip, that its branch has
/// the most work — belongs to the block index above. Keeping that decision out means these
/// two functions are pure functions of a block, a set and a height, and can be tested
/// without a chain.

#include <amarian/consensus/params.hpp>
#include <amarian/consensus/validation.hpp>
#include <amarian/primitives/block.hpp>
#include <amarian/primitives/coin.hpp>
#include <amarian/util/serialize.hpp>
#include <amarian/utxo/coins.hpp>

#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

namespace amarian::utxo {

/// The coins one transaction spent, in input order.
struct TxUndo {
    /// One per input, `spent[i]` being the coin `tx.inputs[i]` consumed.
    std::vector<Coin> spent;

    void Serialize(Writer& writer) const;

    /// Leaves `out` untouched on failure. `max_inputs` bounds the count against the same
    /// limit consensus places on a transaction's inputs, so a corrupted record cannot ask
    /// for an unbounded allocation.
    [[nodiscard]] static bool
    Deserialize(Reader& reader, TxUndo& out, size_t max_inputs, size_t max_lock_program_size);

    friend bool operator==(const TxUndo&, const TxUndo&) noexcept = default;
};

/// Everything needed to undo one block: the coins each of its transactions spent.
///
/// One entry per **non-coinbase** transaction, in block order. The coinbase spends nothing,
/// so giving it an empty entry would be a slot that must always be empty — a thing to be
/// checked rather than a thing that cannot happen. `transactions.size()` is therefore the
/// block's transaction count minus one, and `DisconnectBlock` checks exactly that.
struct BlockUndo {
    std::vector<TxUndo> transactions;

    void Serialize(Writer& writer) const;

    /// Leaves `out` untouched on failure.
    [[nodiscard]] static bool
    Deserialize(Reader& reader, BlockUndo& out, const ChainParams& params);

    friend bool operator==(const BlockUndo&, const BlockUndo&) noexcept = default;
};

/// What connecting a block produced: the record needed to undo it, and the fees it paid.
///
/// The fees are returned rather than checked here so that `CheckCoinbaseAmount` — which is
/// consensus's, not the UTXO layer's — remains the single place the supply ceiling is
/// enforced. `ConnectBlock` does call it; the value is returned as well because block
/// assembly and the RPC layer both want it.
struct ConnectResult {
    BlockUndo undo;
    int64_t total_fees = 0;
};

/// Why a disconnect failed. Never a statement about the block's validity.
///
/// A separate enumeration from `consensus::ValidationError` on purpose, and the distinction
/// is not cosmetic. A block that fails to *connect* is invalid and must be rejected — the
/// node carries on, and is right to. A block that fails to *disconnect* was already
/// accepted and already applied, so a failure here means the undo record and the set no
/// longer describe each other: a corrupted database, or a caller disconnecting a block that
/// is not the tip. There is no valid continuation from that. Sharing one enumeration would
/// invite exactly one wrong reaction — treating a corrupted state as a rejected block — so
/// the type system is used to forbid it.
enum class DisconnectError : uint8_t {
    UndoDoesNotDescribeBlock,  ///< Undo entry count, or an input count, disagrees with the block.
    CreatedCoinMissing,        ///< An output the block created is not in the set to remove.
    CreatedCoinDoesNotMatch,   ///< It is there, but not what the block created.
    SpentOutpointOccupied,     ///< An outpoint the block spent is somehow unspent again.
};

/// A one-line explanation, for a log a node operator has to act on.
[[nodiscard]] std::string_view Describe(DisconnectError error) noexcept;

/// Applies `block` at `block.header.height` to `coins`, or rejects it.
///
/// Preconditions, which are the caller's to establish and are *not* re-run here:
///
///  - `consensus::CheckBlock` has passed, so the block is structurally sound, its Merkle
///    root commits to its transactions, no two of them spend one outpoint, and every
///    transaction is individually well formed.
///  - `consensus::CheckBlockHeader` and `ContextualCheckBlockHeader` have passed, so its
///    work, height, linkage and timestamps are right.
///
/// They are preconditions rather than checks because re-running them would re-serialise and
/// re-hash every transaction of every block during a sync — a cost paid on the hot path to
/// re-derive an answer the caller already has. The cheap bounds that *memory safety*
/// depends on are still re-checked, which is the same line `CheckSpendAuthorisation` draws.
///
/// On success `coins` has been advanced and the result holds the undo record and the total
/// fees. On failure `coins` is untouched and the error is the rule that rejected the block.
///
/// Transactions are applied in block order, and each one's outputs enter the set before the
/// next transaction is looked at. A transaction may therefore spend an output created
/// earlier in the same block, and may not spend one created later: **within a block,
/// transactions must be topologically ordered**. That rule is not a separate check — it is
/// what applying the block in order means, and a block that violates it is rejected with
/// `TxInputMissingOrSpent`, because at the moment the spend is examined the coin genuinely
/// does not exist.
[[nodiscard]] consensus::Computed<ConnectResult>
ConnectBlock(const Block& block, CoinsCache& coins, const ChainParams& params);

/// Reverses `ConnectBlock` exactly: removes the coins `block` created and restores the ones
/// it spent, using `undo`.
///
/// Transactions are walked in reverse order, and within each transaction the outputs it
/// created are removed before the inputs it spent are restored — the mirror image of
/// connecting. Order matters for a transaction that spends an output created earlier in the
/// same block: undoing forwards would restore a coin the later transaction had not yet
/// given back.
///
/// Every removal is checked for **full equality** against what the block says it created,
/// not merely for presence. "Revert restores exactly what was removed" is the property a
/// reorganisation depends on, and an approximate revert is a silent chain split — two nodes
/// that both think they are on the same chain, holding different sets. Checking is a
/// comparison per output; the alternative is trusting a database.
///
/// On success `coins` has been moved back and the set is bit-for-bit what it was before the
/// matching `ConnectBlock`. On failure `coins` is untouched.
///
/// Takes no `ChainParams`: undoing a block enforces no rule that could differ between
/// networks. It restores what was recorded, and checks only that what it restores is what
/// was recorded.
[[nodiscard]] std::expected<void, DisconnectError>
DisconnectBlock(const Block& block, const BlockUndo& undo, CoinsCache& coins);

}  // namespace amarian::utxo
