#pragma once

/// \file
/// The node's on-disk state: coins, bodies, undo records, the header tree, and the tip.
///
/// This is the layer that makes a chain survive a restart, and it is the first layer that
/// links a database. Everything below it — consensus, the unspent output set, chain
/// selection — is written against interfaces (`utxo::CoinsView`, `chain::BlockStore`,
/// `chain::ChainSink`) that this file implements. That direction is deliberate and it is
/// enforced by CMake: a bug in RocksDB's option handling cannot reach the code that decides
/// whether a block is valid, because that code cannot see RocksDB at all.
///
/// ## One database, five families
///
/// All of it lives in a single RocksDB instance with five column families, because the
/// property the node depends on is *atomicity across* them: a write batch spanning column
/// families is applied whole or not at all, and five separate databases could not promise
/// that. The families are
///
///   - `coins`  — the unspent output set, keyed by outpoint.
///   - `blocks` — block bodies, keyed by hash.
///   - `undo`   — what each connected block spent, keyed by that block's hash.
///   - `index`  — one record per known header: the header, its arrival order, how far it
///                was validated, and whether it was ruled out.
///   - `meta`   — the active tip, the schema version, and which network this is.
///
/// Keys are the canonical encodings the rest of the project already uses, not a
/// storage-specific format. A coin's key is `OutPoint::Serialize`, a block's key is its
/// hash's internal byte order. There is no second encoding of anything here, which means
/// there is no second place for two encodings of one value to appear.
///
/// ## What is *not* here
///
/// Pruning, a UTXO set hash, and any bound on how much of the set is held in memory. The
/// first two are later phases. The third is a deliberate present-tense choice rather than
/// an omission: this node commits every block, so its in-memory staging layer holds one
/// block's changes and no more, and the read path goes to the database every time. That is
/// slower than a large cache and simpler to be sure of, and the interface admits the cache
/// later without a caller changing.
///
/// RocksDB's own headers stay in the implementation file. The handle is held behind a
/// pointer to an incomplete type, so nothing that includes this acquires a dependency on a
/// database's API — the same boundary `crypto` draws around OpenSSL, for the same reason.

#include <amarian/chain/block_index.hpp>
#include <amarian/chain/block_store.hpp>
#include <amarian/chain/chain_state.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/primitives/block.hpp>
#include <amarian/primitives/coin.hpp>
#include <amarian/primitives/outpoint.hpp>
#include <amarian/util/types.hpp>
#include <amarian/utxo/coins.hpp>
#include <amarian/utxo/connect.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace amarian::storage {

/// Why a database operation did not produce an answer.
///
/// Separate from every validation vocabulary in the project, and the separation is the
/// point: none of these is a statement about a block. A node that confused "this disk is
/// broken" with "this block is invalid" would rule out honest chains on bad hardware, and
/// one that confused them the other way would treat a forgery as a hardware fault.
enum class DbError : uint8_t {
    /// The directory could not be created, or RocksDB refused to open it. Includes the
    /// ordinary case of a second process already holding the lock.
    CannotOpen,

    /// A read failed for a reason other than the key being absent. An absent key is not an
    /// error anywhere in this interface — it is `std::nullopt`, or an empty result.
    ReadFailed,

    /// A write or a commit was refused.
    WriteFailed,

    /// A stored value did not decode, decoded to something inconsistent, or a stored header
    /// no longer passes the rules it passed when it was written. Always this node's own
    /// state contradicting itself, never a peer's fault.
    CorruptRecord,

    /// The directory holds another network's chain. Refused rather than reconciled: a
    /// mainnet datadir opened as regtest would otherwise begin overwriting real state with
    /// blocks that cost nothing to produce.
    WrongNetwork,

    /// The recorded tip is not among the recorded headers, so the coins set on disk
    /// describes a chain this database cannot name.
    TipNotIndexed,
};

/// A short stable description for a log a node operator has to act on.
[[nodiscard]] std::string_view Describe(DbError error) noexcept;

/// One `index` record: a header and everything the index had decided about it.
///
/// `sequence` is stored because it is half of the chain selection rule — the first-seen
/// tie-break between branches of equal work. Recomputing it on load would mean a node broke
/// ties differently after a restart than before one.
struct StoredHeader {
    BlockHeader header;
    uint64_t sequence;
    chain::BlockValidity validity;
    chain::BlockFailure failure;

    /// Header, then arrival order, then the two recorded judgements.
    static constexpr size_t SERIALIZED_SIZE = BlockHeader::SERIALIZED_SIZE + sizeof(uint64_t) + 2;
};

/// The node's on-disk state, and the three interfaces the layers below use to reach it.
///
/// Implements `utxo::CoinsView` so that the unspent output set the validator reads is the
/// stored one; `chain::BlockStore` so that bodies and undo records outlive the process; and
/// `chain::ChainSink` so that advancing the tip is durable at the moment it happens rather
/// than at some later flush.
///
/// Not a `utxo::CoinsSink`. Writing one coin at a time with no batch would let a crash land
/// between two coins of the same block, and the whole point of `CommitTip` is that this
/// cannot happen. The narrower interface is the one that is safe to expose.
class ChainDb final
    : public utxo::CoinsView
    , public chain::BlockStore
    , public chain::ChainSink {
public:
    /// Opens or creates the database under `directory`, checking it belongs to `params`'
    /// network.
    ///
    /// A fresh directory is stamped with the network's `chain_id` and the schema version. An
    /// existing one is checked against both, and a mismatch is refused rather than migrated:
    /// there is no migration to run yet, and silently accepting a stamp this build does not
    /// understand is how a node ends up interpreting one schema's bytes under another's
    /// rules.
    [[nodiscard]] static std::expected<std::unique_ptr<ChainDb>, DbError>
    Open(const std::string& directory, const ChainParams& params);

    ChainDb(const ChainDb&) = delete;
    ChainDb& operator=(const ChainDb&) = delete;
    ChainDb(ChainDb&&) = delete;
    ChainDb& operator=(ChainDb&&) = delete;
    ~ChainDb() override;

    // --- utxo::CoinsView ----------------------------------------------------

    [[nodiscard]] std::optional<Coin> GetCoin(const OutPoint& outpoint) const override;
    [[nodiscard]] bool HaveCoin(const OutPoint& outpoint) const override;

    // --- chain::BlockStore --------------------------------------------------

    [[nodiscard]] bool HaveBlock(const Hash256& hash) const override;
    [[nodiscard]] std::optional<Block> GetBlock(const Hash256& hash) const override;
    [[nodiscard]] std::optional<utxo::BlockUndo> GetUndo(const Hash256& hash) const override;
    void PutBlock(const Hash256& hash, const Block& block) override;
    void PutUndo(const Hash256& hash, const utxo::BlockUndo& undo) override;

    // --- chain::ChainSink ---------------------------------------------------

    [[nodiscard]] bool CommitHeader(const chain::BlockIndexEntry& entry) override;
    [[nodiscard]] bool CommitTip(utxo::CoinsCache& changes, const Hash256& tip) override;

    /// Whether this database has reported a fault since it was opened. Latching: once true,
    /// always true.
    ///
    /// Covers both directions, because in both directions the interface being implemented
    /// has nowhere to put a fault. `GetCoin` and `GetBlock` return an `optional`, so a record
    /// that could not be read is indistinguishable from one that is not there; `PutBlock` and
    /// `PutUndo` return nothing at all. A node that treated either silence as an answer would
    /// rule out a valid block on a bad disk, or commit a tip whose undo record was never
    /// stored — so the fault is latched here and consulted where it matters: by `ChainState`
    /// before a rejection is made permanent, and by `CommitTip` before a tip is made durable.
    ///
    /// Not clearable. A read that failed has already been answered wrongly, and a write that
    /// was refused has already been reported as done; clearing the latch would be claiming
    /// the loss had been repaired rather than merely stopped.
    [[nodiscard]] bool HasFault() const noexcept override;

    // --- startup ------------------------------------------------------------

    /// Every stored header record, ordered by arrival.
    ///
    /// Arrival order is a topological order of the tree: `BlockIndex::AddHeader` refuses a
    /// header whose predecessor it does not hold, so a header can only have been stored after
    /// its parent was. Replaying in this order therefore never presents a child before its
    /// parent, and no separate sort by height is needed.
    [[nodiscard]] std::expected<std::vector<StoredHeader>, DbError> LoadHeaders() const;

    /// The tip the last run committed, or `std::nullopt` for a database that has never had
    /// one — a fresh directory, whose chain is genesis alone.
    [[nodiscard]] std::expected<std::optional<Hash256>, DbError> LoadTip() const;

    /// Flushes RocksDB's memtables and write-ahead log to the operating system.
    ///
    /// Not called during normal operation. Every commit is already atomic and already
    /// durable against the process dying; this is for the stronger property of durability
    /// against the *machine* dying, which is worth paying for once at shutdown and not once
    /// per block.
    [[nodiscard]] bool Sync();

private:
    struct Impl;

    explicit ChainDb(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

/// The chain as a previous run left it: the header tree, and the tip the coins set matches.
struct LoadedChain {
    /// Every header that could be restored, genesis included.
    chain::BlockIndex index;

    /// The entry the stored coins set corresponds to. Never null — genesis for a database
    /// that has never committed a tip.
    const chain::BlockIndexEntry* tip;

    /// How many stored headers were replayed into the index.
    size_t restored = 0;
};

/// Rebuilds the header tree from `db` and finds the entry its coins set belongs to.
///
/// Replays stored headers through `chain::BlockIndex::AddHeader` rather than reconstructing
/// entries directly, so that every rule a header passed when it arrived is applied again to
/// the bytes actually on disk. A tampered header does not become valid by having been
/// written to this node's own database, and this is where that is established — before any
/// of it is weighed for the tip.
///
/// Recorded failures are replayed in a second pass, after every header is in place. A block
/// ruled out by its own body's rules is marked, and the index propagates that to its
/// descendants exactly as it did the first time. Doing it in one pass instead would refuse
/// the descendants outright at `AddHeader`, losing headers this node genuinely knows.
///
/// One rule is deliberately *not* re-applied: the ceiling on how far ahead of the clock a
/// timestamp may be. Every other header rule is a fact about the header, and a fact does not
/// change while a node is switched off — but that one compares against the clock, so a
/// machine whose time has been corrected backwards would refuse headers it accepted an hour
/// earlier, up to and including its own committed tip, whose coins are already on disk. The
/// check has already been made, at the only moment it means anything: when the header
/// arrived. So the replay clock is `now` or the highest stored timestamp, whichever is
/// later, which satisfies the ceiling for every stored header and leaves every other rule to
/// judge them exactly as it did before.
///
/// `now` is passed in rather than read from a clock, so that a restart is reproducible in a
/// test.
[[nodiscard]] std::expected<LoadedChain, DbError>
LoadChain(const ChainDb& db, int64_t now, const ChainParams& params);

}  // namespace amarian::storage
