#pragma once

/// \file
/// The chain the node is on, and the step that moves it.
///
/// The index answers *which chain should I be on* and `PlanChainSwitch` answers *what
/// would it take to get there*. Neither touches the unspent output set, so neither makes a
/// chain exist. This file is the part that acts: it takes the plan, reverses the blocks on
/// the branch being left, applies the blocks on the branch being joined, and keeps the
/// coins set in step with the tip at every point in between.
///
/// ## The invariant this class exists to hold
///
/// The coins set is the set of outputs unspent *as of the active tip*. That is a
/// relationship between two objects, which is why they are held together here rather than
/// passed to a free function: a caller holding the set and the index separately could
/// apply a block to one without extending the other, and nothing would notice until a
/// later block was validated against a set describing a different chain.
///
/// ## Atomicity is per block, not per reorganisation
///
/// `ConnectBlock` and `DisconnectBlock` each stage their work and commit it in one go, so
/// every state this class passes through is the set for *some* valid chain. A crash in the
/// middle of a hundred-block reorganisation leaves a shorter chain, never a corrupt one,
/// and the next activation continues from there.
///
/// The alternative — one staging layer over the whole reorganisation — was rejected. It
/// would hold the entire change set of an initial sync in memory before committing
/// anything, which is unbounded in the length of the chain, and it would buy atomicity
/// over a span where a partial result is already correct.
///
/// ## Why activation refuses to shrink the chain
///
/// A header is cheap and a body is not, so a peer can announce a heavier branch it has no
/// intention of sending. If activation simply reverted to the fork point and applied
/// whatever bodies had arrived, that announcement alone would cost this node its tip. So
/// the target is first truncated to the highest block whose whole path from the fork point
/// is available, and the switch does not begin unless that truncated target still beats
/// the current tip. Every block carries at least one unit of work, so an ancestor always
/// has strictly less total work than its descendant — which makes the guard a proof rather
/// than a heuristic: no sequence of withheld bodies can move this node's tip backwards.

#include <amarian/chain/block_index.hpp>
#include <amarian/chain/block_store.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/consensus/validation.hpp>
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

class ChainState;

/// The blocks this node has applied: genesis first, one per height, no gaps.
///
/// A vector indexed by height rather than a linked walk from the tip. Both are correct, and
/// the difference is that `AtHeight` is the question everything above this layer asks —
/// confirmation counts, the maturity check, a wallet rescan, a P2P block locator — and a
/// parent walk answers it in time proportional to the chain's length.
///
/// Mutation is `ChainState`'s alone. Extending this vector without applying the block to
/// the coins set, or the reverse, would break the one invariant that class exists to hold,
/// so the two operations are not separately reachable.
class ActiveChain {
public:
    /// The block at `height`, or `nullptr` when the chain is not that long.
    ///
    /// Total rather than checked: a caller asking about a height beyond the tip is asking a
    /// meaningful question — "is this block buried yet" — and the answer is no, not an
    /// error.
    [[nodiscard]] const BlockIndexEntry* AtHeight(uint32_t height) const noexcept;

    /// The most recently applied block, or `nullptr` for a chain with nothing in it.
    ///
    /// A `ChainState`'s chain always holds at least genesis, so its tip is never null. The
    /// null case exists because a default-constructed `ActiveChain` has to mean something.
    [[nodiscard]] const BlockIndexEntry* Tip() const noexcept;

    /// Whether `entry` is on this chain. Answered by height, not by search: an entry is on
    /// the chain exactly when the chain's block at that entry's height is that entry.
    [[nodiscard]] bool Contains(const BlockIndexEntry& entry) const noexcept;

    /// How many blocks are applied. One more than the tip's height, because genesis is
    /// height zero — which is why there is no `Height()` here to be confused with it.
    [[nodiscard]] size_t Length() const noexcept { return blocks_.size(); }

    [[nodiscard]] bool IsEmpty() const noexcept { return blocks_.empty(); }

private:
    friend class ChainState;

    std::vector<const BlockIndexEntry*> blocks_;
};

/// A block a consensus rule refused during activation, and the rule that refused it.
///
/// Reported rather than merely logged because the block's origin matters to a layer this
/// one cannot see. A peer that sent a block failing its own rules has proved something
/// about itself, and Phase 4's peer accounting needs the hash to know which peer.
///
/// No member has a default. `ValidationError` has no "nothing was wrong" enumerator — it is
/// the error type of an `expected`, so the absence of an error is the absence of the whole
/// value — and inventing one here so that this struct could be default-constructed would
/// put a meaningless state into a type whose only reason to exist is a meaningful one.
struct RejectedBlock {
    Hash256 hash;
    uint32_t height;
    consensus::ValidationError error;
};

/// What one activation did.
struct ActivationSummary {
    /// The tip when activation stopped. Never null: activation either improves the tip or
    /// leaves it alone.
    const BlockIndexEntry* tip = nullptr;

    size_t connected = 0;
    size_t disconnected = 0;

    /// Blocks refused along the way, in the order they were refused. Empty in the ordinary
    /// case; a non-empty vector here is not an activation failure — refusing an invalid
    /// block and carrying on to the next-best branch is activation working.
    std::vector<RejectedBlock> rejected;

    /// Whether the tip is now the index's best header. False when the heaviest branch is
    /// one this node cannot reach yet, which during a sync is the normal state.
    bool reached_best_header = false;

    /// Whether any block was reversed. The mempool and any wallet need to know, because a
    /// transaction that was confirmed is now unconfirmed.
    [[nodiscard]] bool Reorganised() const noexcept { return disconnected > 0; }
};

/// Why activation could not continue.
///
/// Every value here means this node's own storage contradicts its own chain, which is a
/// different kind of event from a block being invalid. An invalid block is a fact about the
/// network and activation handles it by moving on; these are facts about this machine, and
/// there is no branch to move on to because the same storage backs all of them.
enum class ActivationError : uint8_t {
    /// A body the store said it had could not be read, or a block on the active chain has
    /// no body to reverse it with.
    BlockBodyUnavailable,

    /// A block on the active chain has no undo record. It was connected, so one was
    /// written; its absence means the record was lost, and the coins it spent cannot be
    /// restored from the block alone — their amounts and locks are not in it.
    UndoRecordUnavailable,

    /// Reversing a block left the coins set in a state the undo record does not describe.
    /// The set and the chain have diverged; see the accompanying `revert` for which way.
    RevertRejected,

    /// The plan named a block to reverse that is not the tip, or a fork point outside the
    /// active chain. The index and the active chain disagree about what is applied.
    ChainOutOfStep,

    /// A `ChainSink` refused to make a tip durable. The in-memory chain and the stored one
    /// have parted company, so continuing would advance a tip that a restart would not
    /// find — which is worse than stopping, because the coins set on disk would then
    /// describe a chain the node no longer believes in.
    CommitFailed,

    /// Storage reported a fault while a block was being judged, so the verdict cannot be
    /// attributed to the block. Stopping is the only safe reaction: see `ChainSink::HasFault`
    /// for why the alternative — recording the rejection — is unrecoverable.
    StorageFaulted,
};

[[nodiscard]] std::string_view Describe(ActivationError error) noexcept;

/// An activation that stopped, and the block it stopped on.
struct ActivationFailure {
    ActivationError error;
    Hash256 block;

    /// Set only for `RevertRejected`, where the distinction between a coin that is missing
    /// and one that is present but different is the difference between two kinds of damage.
    std::optional<utxo::DisconnectError> revert;
};

/// Where the chain's state is made durable.
///
/// Two calls, and the split between them is the whole design. A header record is a fact
/// about a block the node has *learned*; a tip record is a fact about which chain the node
/// *follows*. Losing the first costs a re-download. Losing the second, or writing half of
/// it, is the corruption every other guard in this file exists to prevent.
///
/// So `CommitTip` is required to be atomic and `CommitHeader` is not. A node interrupted
/// between the coins changes and the tip pointer would come back holding a set that
/// describes neither the chain it left nor the one it was moving to, and no later code
/// could tell which — there is no record of what was half-applied. The two facts must
/// therefore land together or not at all. Whether an implementation achieves that with a
/// write batch, a journal, or a rename is its own business; that it achieves it is the
/// contract, and `ChainState` relies on it.
///
/// Failure of `CommitTip` is a hard stop rather than something to retry or ignore: a node
/// that carried on after a refused tip commit would be advancing a tip a restart will not
/// find, which is the one state from which no correct continuation exists. A refused
/// `CommitHeader` is not reported at all, because `AcceptBlock`'s errors are statements
/// about the block and this one would be a statement about the disk — and the disk will say
/// it again, fatally, at the next tip commit.
class ChainSink {
public:
    ChainSink() = default;
    ChainSink(const ChainSink&) = default;
    ChainSink(ChainSink&&) = default;
    ChainSink& operator=(const ChainSink&) = default;
    ChainSink& operator=(ChainSink&&) = default;
    virtual ~ChainSink() = default;

    /// Records `entry`'s header and everything the index has decided about it — its arrival
    /// order, how far it has been validated, and whether it has been ruled out.
    ///
    /// Arrival order is stored rather than recomputed because it is half of the chain
    /// selection rule. A node that renumbered its headers on restart would break ties
    /// between equal-work branches differently after a restart than before one, which is a
    /// node changing its mind about which chain is real for no reason an observer could see.
    [[nodiscard]] virtual bool CommitHeader(const BlockIndexEntry& entry) = 0;

    /// Persists everything in `changes` together with the fact that the active tip is now
    /// `tip`, atomically, and empties `changes`.
    ///
    /// `changes` is passed by mutable reference because flushing it is how it is consumed:
    /// on success the layer is empty and the sink holds what it held. On failure the sink
    /// must be unchanged, and `changes` may be left as it was — the caller is stopping
    /// either way.
    [[nodiscard]] virtual bool CommitTip(utxo::CoinsCache& changes, const Hash256& tip) = 0;

    /// Whether the storage behind this sink has reported a fault since it was opened.
    ///
    /// This exists because the read interfaces below the chain layer — `utxo::CoinsView`,
    /// `BlockStore` — answer with an `optional` and have nowhere to put "I could not tell
    /// you". Absent and unreadable therefore look identical to a validator, and a coin that
    /// exists but could not be read makes a valid block look like a double spend.
    ///
    /// That confusion has to be resolved *before* a rejection is recorded, because recording
    /// one is irreversible and is written to disk: a transient read error would otherwise
    /// permanently rule out a valid block and every descendant of it, on this node only,
    /// across every future restart. So a rejection reached while storage is faulted is not
    /// attributed to the block — activation stops, and the block is judged again on a run
    /// where the disk answers.
    ///
    /// Defaulted to false rather than left pure. A sink with no storage under it has no
    /// faults to report, and that is an answer rather than a stub.
    [[nodiscard]] virtual bool HasFault() const { return false; }
};

/// The active chain, the coins set that corresponds to it, and the operations that move
/// both together.
///
/// Holds its collaborators by reference rather than owning them. The index outlives any one
/// activation and is shared with the header-synchronisation logic; the coins set is
/// `utxo::CoinsCache`, which is deliberately neither copyable nor movable so that no
/// staging layer can be duplicated out from under the set it stages over; and the store is
/// an interface whose implementation the node chooses at startup. What this class owns is
/// the one thing none of them can hold — the correspondence between them.
class ChainState {
public:
    /// Binds a state to an index, a coins set and a store, with genesis applied.
    ///
    /// Genesis starts on the chain rather than being connected. Its only output is
    /// unspendable, and `utxo::ConnectBlock` stores no unspendable output, so the set before
    /// genesis and the set after it are the same set: "genesis is applied" and "genesis is
    /// not applied" describe identical state, and there is nothing for a first activation to
    /// do about the difference. Seeding it instead of connecting it also means the node
    /// never needs genesis's *body* in the store to make progress, which matters because
    /// genesis is a chain parameter — derivable from `BuildGenesisBlock` — rather than a
    /// block that arrived from anywhere.
    ///
    /// `coins` must be the set as of genesis, which for a fresh node is an empty one.
    ChainState(BlockIndex& index, utxo::CoinsCache& coins, BlockStore& store,
               const ChainParams& params);

    ChainState(const ChainState&) = delete;
    ChainState& operator=(const ChainState&) = delete;
    ChainState(ChainState&&) = delete;
    ChainState& operator=(ChainState&&) = delete;
    ~ChainState() = default;

    /// The chain as it stands. Read-only, for the reason `ActiveChain`'s mutators are
    /// private: the only way to change it is to change the coins set with it.
    [[nodiscard]] const ActiveChain& Chain() const noexcept { return chain_; }

    /// The applied tip. Never null.
    [[nodiscard]] const BlockIndexEntry& Tip() const noexcept;

    /// Directs every change of recorded state through `sink`, which must outlive this state.
    ///
    /// Optional, and its absence is not a stub: a state with no sink is a complete in-memory
    /// chain, which is what a test wants and what a node that has not been told where to
    /// keep its data has. Attaching one does not retroactively persist anything, so it
    /// belongs immediately after construction and before the first `AcceptBlock`.
    void PersistTo(ChainSink& sink) noexcept { sink_ = &sink; }

    /// Restores the active chain as the path from genesis up to `tip`, applying nothing.
    ///
    /// For a node starting on a set that a previous run already advanced. The coins set this
    /// state was constructed with must be the set *as of* `tip`, which is exactly what a
    /// `CommitTip` of that tip left behind — the atomicity of that call is what makes this
    /// assumption safe to make rather than merely hopeful.
    ///
    /// Cannot fail: every entry's ancestry reaches genesis by the index's own invariant, so
    /// there is always a path to walk.
    void ResumeAt(const BlockIndexEntry& tip);

    /// Takes a block this node has received: indexes its header, checks the body against
    /// every rule that does not need a chain, and stores it.
    ///
    /// Does *not* activate. The two are separate because their failures are separate: a
    /// block can be perfectly valid and still not worth switching to, and a switch can fail
    /// for reasons that say nothing about the block that triggered it. Splitting them also
    /// means a synchronising node can accept a run of blocks and activate once.
    ///
    /// Idempotent. Offering a block whose header is already indexed and whose body is
    /// already stored returns the existing entry and touches nothing, which is the ordinary
    /// case when a block arrives from two peers at once.
    [[nodiscard]] std::expected<const BlockIndexEntry*, HeaderError>
    AcceptBlock(const Block& block, int64_t now);

    /// Moves the active chain as far towards the index's best header as this node's stored
    /// bodies allow, applying and reversing blocks so that the coins set follows.
    ///
    /// Returns what it did. A block refused by a rule is recorded in the summary and ruled
    /// out in the index, and activation continues towards the next-best branch — that is
    /// success, not failure. The error case is reserved for this node's storage
    /// contradicting itself, which no other branch can fix.
    ///
    /// Idempotent and cheap when there is nothing to do: an already-best tip is one
    /// comparison.
    [[nodiscard]] std::expected<ActivationSummary, ActivationFailure> ActivateBestChain();

private:
    /// Applies one block, which must extend the current tip. `false` means a rule refused
    /// it, and it has been ruled out in the index; the coins set is untouched either way
    /// unless the return is `true`.
    [[nodiscard]] std::expected<bool, ActivationFailure> ConnectTip(const BlockIndexEntry& entry,
                                                                   ActivationSummary& summary);

    /// Reverses the current tip.
    [[nodiscard]] std::expected<void, ActivationFailure> DisconnectTip(ActivationSummary& summary);

    /// Makes the current tip durable, if there is a sink. False means the commit was refused
    /// and the caller must stop.
    [[nodiscard]] bool CommitCurrentTip();

    /// Records `entry` in the sink, if there is one. False means the write was refused.
    [[nodiscard]] bool CommitEntry(const BlockIndexEntry& entry);

    /// Whether storage has reported a fault, so that a rejection cannot be blamed on the
    /// block that appeared to cause it. See `ChainSink::HasFault`.
    [[nodiscard]] bool StorageFaulted() const;

    BlockIndex* index_;
    utxo::CoinsCache* coins_;
    BlockStore* store_;
    const ChainParams* params_;
    ChainSink* sink_ = nullptr;
    ActiveChain chain_;
};

}  // namespace amarian::chain
