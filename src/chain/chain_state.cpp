#include <amarian/chain/chain_state.hpp>

#include <amarian/chain/block_index.hpp>
#include <amarian/chain/block_store.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/consensus/validation.hpp>
#include <amarian/primitives/block.hpp>
#include <amarian/util/types.hpp>
#include <amarian/utxo/coins.hpp>
#include <amarian/utxo/connect.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <vector>

namespace amarian::chain {
namespace {

[[nodiscard]] std::unexpected<ActivationFailure> Stopped(ActivationError error,
                                                        const Hash256& block) {
    return std::unexpected(
        ActivationFailure{.error = error, .block = block, .revert = std::nullopt});
}

}  // namespace

const BlockIndexEntry* ActiveChain::AtHeight(uint32_t height) const noexcept {
    if (height >= blocks_.size()) {
        return nullptr;
    }
    return blocks_[height];
}

const BlockIndexEntry* ActiveChain::Tip() const noexcept {
    return blocks_.empty() ? nullptr : blocks_.back();
}

bool ActiveChain::Contains(const BlockIndexEntry& entry) const noexcept {
    // By height, not by search. The chain holds one block per height, so an entry is on it
    // exactly when it is *the* block at its own height — which also means a fork's entry at
    // the same height as an applied one correctly answers false rather than matching.
    return AtHeight(entry.height) == &entry;
}

std::string_view Describe(ActivationError error) noexcept {
    switch (error) {
        case ActivationError::BlockBodyUnavailable:
            return "a block on the path has no stored body";
        case ActivationError::UndoRecordUnavailable:
            return "a connected block has no stored undo record";
        case ActivationError::RevertRejected:
            return "reversing a block disagreed with the unspent output set";
        case ActivationError::ChainOutOfStep:
            return "the block index and the active chain disagree about what is applied";
        case ActivationError::CommitFailed:
            return "the new tip could not be made durable";
        case ActivationError::StorageFaulted:
            return "storage reported a fault, so a block's verdict cannot be trusted";
    }
    // Unreachable: the switch is total, and -Wswitch-enum makes a new enumerator a build
    // failure here rather than a silent fall-through.
    return "unknown activation error";
}

ChainState::ChainState(BlockIndex& index, utxo::CoinsCache& coins, BlockStore& store,
                       const ChainParams& params)
    : index_(&index), coins_(&coins), store_(&store), params_(&params) {
    chain_.blocks_.push_back(&index.Genesis());
}

const BlockIndexEntry& ChainState::Tip() const noexcept {
    // Never empty. The constructor seeds genesis, and nothing removes it: every switch
    // reverses down to a fork point, a fork point is a common ancestor, and genesis is an
    // ancestor of every entry the index holds.
    return *chain_.blocks_.back();
}

void ChainState::ResumeAt(const BlockIndexEntry& tip) {
    // Walk up by parent links and reverse, rather than by `Ancestor(h)` per height, which
    // would make restoring a chain of n blocks cost n^2/2 parent steps.
    std::vector<const BlockIndexEntry*> upwards;
    upwards.reserve(size_t{tip.height} + 1);
    for (const BlockIndexEntry* entry = &tip; entry != nullptr; entry = entry->parent) {
        upwards.push_back(entry);
    }
    chain_.blocks_.assign(upwards.rbegin(), upwards.rend());
}

bool ChainState::CommitEntry(const BlockIndexEntry& entry) {
    return sink_ == nullptr || sink_->CommitHeader(entry);
}

bool ChainState::StorageFaulted() const {
    return sink_ != nullptr && sink_->HasFault();
}

bool ChainState::CommitCurrentTip() {
    if (sink_ == nullptr) {
        // No sink is an in-memory chain, which is a complete thing to be. The changes stay
        // in `coins_` and the tip is whatever `chain_` says, exactly as before persistence
        // existed.
        return true;
    }
    return sink_->CommitTip(*coins_, Tip().hash);
}

std::expected<const BlockIndexEntry*, HeaderError> ChainState::AcceptBlock(const Block& block,
                                                                          int64_t now) {
    const std::expected<const BlockIndexEntry*, HeaderError> indexed =
        index_->AddHeader(block.header, now, *params_);
    if (!indexed.has_value()) {
        return indexed;
    }
    const BlockIndexEntry* const entry = *indexed;

    // A header indexed earlier can have been ruled out since — by its own body failing on a
    // previous offer, or by a `RecordFailure` on an ancestor reaching it. `AddHeader` does
    // not catch that: it examines the *parent* of a new header, and for one it already knows
    // it returns the existing entry without judging it again.
    if (!entry->IsEligible()) {
        return std::unexpected(HeaderError{IndexError::AlreadyRuledOut});
    }

    // The hash names the bytes, so a body already stored under it is this body. Checking it
    // again would spend a megabyte of hashing to reach the same verdict.
    if (store_->HaveBlock(entry->hash)) {
        return entry;
    }

    // Every rule that needs no chain, now, so that nothing structurally invalid is ever
    // stored and activation never has to consider it. `CheckBlock` re-runs the header rules
    // `AddHeader` already applied; they are cheap beside the Merkle root, and a body that
    // does not match its own header is exactly what this call is here to catch.
    if (const consensus::Verdict sound = consensus::CheckBlock(block, *params_); !sound) {
        index_->RecordFailure(*entry);
        (void)CommitEntry(*entry);
        return std::unexpected(HeaderError{sound.error()});
    }

    store_->PutBlock(entry->hash, block);
    index_->RecordValidity(*entry, BlockValidity::Body);
    (void)CommitEntry(*entry);
    return entry;
}

std::expected<void, ActivationFailure> ChainState::DisconnectTip(ActivationSummary& summary) {
    const BlockIndexEntry& entry = Tip();

    // Genesis is seeded rather than connected, so it has nothing to reverse and no undo
    // record to reverse it with. A plan that named it disagrees with the chain.
    if (entry.parent == nullptr) {
        return Stopped(ActivationError::ChainOutOfStep, entry.hash);
    }

    const std::optional<Block> block = store_->GetBlock(entry.hash);
    if (!block.has_value()) {
        return Stopped(ActivationError::BlockBodyUnavailable, entry.hash);
    }

    // The block names the outpoints it spent but not the coins behind them, so the amounts
    // and locks being restored exist only in the undo record. Without it this block cannot be
    // reversed by any amount of recomputation.
    const std::optional<utxo::BlockUndo> undo = store_->GetUndo(entry.hash);
    if (!undo.has_value()) {
        return Stopped(ActivationError::UndoRecordUnavailable, entry.hash);
    }

    const std::expected<void, utxo::DisconnectError> reversed =
        utxo::DisconnectBlock(*block, *undo, *coins_);
    if (!reversed.has_value()) {
        return std::unexpected(ActivationFailure{.error = ActivationError::RevertRejected,
                                                 .block = entry.hash,
                                                 .revert = reversed.error()});
    }

    chain_.blocks_.pop_back();
    if (!CommitCurrentTip()) {
        return Stopped(ActivationError::CommitFailed, entry.hash);
    }
    ++summary.disconnected;
    return {};
}

std::expected<bool, ActivationFailure> ChainState::ConnectTip(const BlockIndexEntry& entry,
                                                              ActivationSummary& summary) {
    // The precondition that makes ruling a block out permanent, below, sound. An entry's
    // ancestry is fixed by its header's `prev_block`, so there is exactly one coins set a
    // block can ever be applied against — the one as of its parent. Connecting it only while
    // its parent is the tip is what guarantees the set in hand is that one, and therefore
    // that a block which fails here fails on the only chain it could ever be part of.
    if (entry.parent != &Tip()) {
        return Stopped(ActivationError::ChainOutOfStep, entry.hash);
    }

    const std::optional<Block> block = store_->GetBlock(entry.hash);
    if (!block.has_value()) {
        return Stopped(ActivationError::BlockBodyUnavailable, entry.hash);
    }

    // The context-free rules, at most once in a block's life. `CheckBlock` reads nothing but
    // the block, so a block reconnected after a reorganisation cannot newly fail them, and
    // recording that it passed is what stops a run of reorganisations from rebuilding the same
    // Merkle root each time. A block that arrived through `AcceptBlock` is already past this.
    if (entry.validity < BlockValidity::Body) {
        if (const consensus::Verdict sound = consensus::CheckBlock(*block, *params_); !sound) {
            // A body that came back from a faulted store may not be the body that was
            // written, and ruling a block out is permanent and durable. Stop instead.
            if (StorageFaulted()) {
                return Stopped(ActivationError::StorageFaulted, entry.hash);
            }
            index_->RecordFailure(entry);
            (void)CommitEntry(entry);
            summary.rejected.push_back({entry.hash, entry.height, sound.error()});
            return false;
        }
        index_->RecordValidity(entry, BlockValidity::Body);
    }

    // Always run, even for an entry already marked `Full`. `Full` records that this block was
    // connected once, not that it is connected now: a reorganisation away and back must apply
    // it again, and it can legitimately fail the second time — the coins its inputs name are
    // the ones the *current* chain leaves unspent, not the ones the old chain did.
    const consensus::Computed<utxo::ConnectResult> applied =
        utxo::ConnectBlock(*block, *coins_, *params_);
    if (!applied.has_value()) {
        // Every input this block spends was looked up in the coins set, and a set backed by
        // faulted storage reports a coin it cannot read as one that does not exist — which
        // is indistinguishable, here, from the block spending a coin that never did. The
        // block may be perfectly valid, so its verdict is discarded rather than recorded.
        if (StorageFaulted()) {
            return Stopped(ActivationError::StorageFaulted, entry.hash);
        }
        index_->RecordFailure(entry);
        (void)CommitEntry(entry);
        summary.rejected.push_back({entry.hash, entry.height, applied.error()});
        return false;
    }

    store_->PutUndo(entry.hash, applied->undo);
    chain_.blocks_.push_back(&entry);
    index_->RecordValidity(entry, BlockValidity::Full);
    (void)CommitEntry(entry);

    // The coins changes and the new tip, together or not at all. Per block rather than per
    // activation: a sync that connected half a million blocks before its first commit would
    // hold half a million blocks' worth of changes in memory, and would lose all of them to
    // one interruption. Per block bounds both, and every committed state is a chain that
    // was valid — which is what makes an interrupted reorganisation recoverable rather than
    // merely detectable.
    if (!CommitCurrentTip()) {
        return Stopped(ActivationError::CommitFailed, entry.hash);
    }
    ++summary.connected;
    return true;
}

std::expected<ActivationSummary, ActivationFailure> ChainState::ActivateBestChain() {
    ActivationSummary summary;
    summary.tip = &Tip();

    for (;;) {
        const BlockIndexEntry* const target = index_->BestHeader();

        // A null best header means genesis itself is ruled out, which is a statement about
        // this build's chain parameters rather than about any block that arrived. `CheckGenesis`
        // exists so that a node stops long before reaching here.
        if (target == nullptr || target == &Tip()) {
            summary.tip = &Tip();
            summary.reached_best_header = target == &Tip();
            return summary;
        }

        const ChainSwitch plan = PlanChainSwitch(&Tip(), target);

        // Both entries came from one index, so they share genesis and a fork point exists.
        // Its absence, or a fork point that is not on the chain being left, would mean the
        // entries do not form the tree the index documents — and then no block can be trusted
        // to be applied against the set its own ancestry implies.
        if (plan.fork_point == nullptr) {
            return Stopped(ActivationError::ChainOutOfStep, target->hash);
        }
        if (!chain_.Contains(*plan.fork_point)) {
            return Stopped(ActivationError::ChainOutOfStep, plan.fork_point->hash);
        }

        // How far up the new branch this node could actually get. The walk stops at the first
        // absent body rather than skipping it: a chain has no gaps, so a body this node lacks
        // puts everything above it out of reach however much of it has already arrived.
        size_t reachable = 0;
        while (reachable < plan.connect.size() &&
               store_->HaveBlock(plan.connect[reachable]->hash)) {
            ++reachable;
        }
        const BlockIndexEntry* const attainable =
            reachable == 0 ? plan.fork_point : plan.connect[reachable - 1];

        // The guard against paying for an announcement. Reverting in order to arrive at a tip
        // no better than the one already held is a strict loss, and it is a loss any peer can
        // inflict for the price of a header it never follows with a body — so it is refused
        // outright rather than attempted and regretted.
        if (!IsBetterTip(*attainable, Tip())) {
            summary.tip = &Tip();
            return summary;
        }

        for (const BlockIndexEntry* const entry : plan.disconnect) {
            if (entry != &Tip()) {
                return Stopped(ActivationError::ChainOutOfStep, entry->hash);
            }
            const std::expected<void, ActivationFailure> reversed = DisconnectTip(summary);
            if (!reversed.has_value()) {
                return std::unexpected(reversed.error());
            }
        }

        for (size_t position = 0; position < reachable; ++position) {
            const std::expected<bool, ActivationFailure> applied =
                ConnectTip(*plan.connect[position], summary);
            if (!applied.has_value()) {
                return std::unexpected(applied.error());
            }
            if (!*applied) {
                // This branch is ruled out from here up. Stop rather than skipping the block:
                // its descendants are unreachable, and the next-best branch may share none of
                // what was just applied, so the plan has to be made again from where the chain
                // now stands.
                break;
            }
        }

        summary.tip = &Tip();

        // Loop. Every pass either returns, advances the tip under `IsBetterTip`, or rules out
        // at least one entry. Advances are bounded because `IsBetterTip` is a strict order over
        // a finite index and the tip only ever rises under it; rulings-out are bounded because
        // `RecordFailure` never reverses. So the loop cannot run forever, and it stops only
        // once the tip is the best header this node can reach.
    }
}

}  // namespace amarian::chain
