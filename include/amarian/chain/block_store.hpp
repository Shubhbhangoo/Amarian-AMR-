#pragma once

/// \file
/// Where block bodies and undo records live.
///
/// The index holds headers, because a header is all chain *selection* needs. Applying a
/// chain needs more: connecting a block needs its transactions, and disconnecting one
/// needs the coins it spent — which are not in the block at all, only their outpoints
/// are. Both are far larger than a header, both are written once and read rarely, and
/// both must survive a restart. So they belong behind an interface a database can
/// implement, and not inside the index.
///
/// ## Why an interface
///
/// Persistence is the next component and it changes nothing above this line: a
/// RocksDB-backed store is another implementation of these five functions. Keeping the
/// interface here means `chain` — the code that decides which chain is real — does not
/// link a database. That is the same layering rule that keeps consensus free of storage,
/// and it is enforced the same way, by the linker.
///
/// ## What is not here
///
/// Any notion of which blocks are worth keeping. A store answers questions about the
/// blocks it was given. Pruning, and the policy of what a node retains, is Phase 11's.

#include <amarian/primitives/block.hpp>
#include <amarian/util/types.hpp>
#include <amarian/utxo/connect.hpp>

#include <cstddef>
#include <optional>
#include <unordered_map>

namespace amarian::chain {

/// Block bodies and undo records, keyed by block hash.
class BlockStore {
public:
    BlockStore() = default;
    BlockStore(const BlockStore&) = default;
    BlockStore(BlockStore&&) = default;
    BlockStore& operator=(const BlockStore&) = default;
    BlockStore& operator=(BlockStore&&) = default;
    virtual ~BlockStore() = default;

    /// Whether this node holds the body of `hash`.
    ///
    /// Separate from `GetBlock` for the reason `CoinsView::HaveCoin` is separate from
    /// `GetCoin`: deciding how far up a branch this node could connect asks only about
    /// presence, and a database can answer that without deserialising a megabyte.
    [[nodiscard]] virtual bool HaveBlock(const Hash256& hash) const = 0;

    /// The body of `hash`, or `std::nullopt` when this node does not have it.
    ///
    /// By value, because for any backend that is not a memory map the read is a
    /// deserialisation into a fresh object and pretending otherwise would put a lifetime
    /// question into the interface that only one implementation could answer.
    [[nodiscard]] virtual std::optional<Block> GetBlock(const Hash256& hash) const = 0;

    /// The record needed to reverse `hash`. Present only for a block that has been
    /// connected: a block that has merely been received has nothing to reverse.
    [[nodiscard]] virtual std::optional<utxo::BlockUndo> GetUndo(const Hash256& hash) const = 0;

    /// Stores a body. Storing one already present changes nothing and is not an error —
    /// the hash names the bytes, so the two are the same block.
    virtual void PutBlock(const Hash256& hash, const Block& block) = 0;

    /// Stores the record that reverses `hash`, replacing any earlier one.
    ///
    /// Replacing rather than keeping the first is deliberate. A block reconnected after a
    /// reorganisation produces its undo record again, and the two are equal — but they are
    /// equal because connecting is deterministic, which is a property to rely on rather
    /// than a reason to keep a stale copy and hope.
    virtual void PutUndo(const Hash256& hash, const utxo::BlockUndo& undo) = 0;
};

/// A store that keeps everything in memory.
///
/// Not a placeholder: it implements every one of the five functions with the real
/// semantics, and it is what regtest and the unit tests run against, where a chain is a
/// few dozen blocks and a restart is not a scenario. What it cannot do is survive a
/// process exit, which is exactly the one thing the RocksDB implementation adds.
///
/// It is deliberately unbounded. A store that silently dropped a body would turn a node's
/// own storage into a way to stall its chain, and the decision about what a node may
/// forget is a pruning policy rather than a container detail.
class MemoryBlockStore final : public BlockStore {
public:
    [[nodiscard]] bool HaveBlock(const Hash256& hash) const override;
    [[nodiscard]] std::optional<Block> GetBlock(const Hash256& hash) const override;
    [[nodiscard]] std::optional<utxo::BlockUndo> GetUndo(const Hash256& hash) const override;
    void PutBlock(const Hash256& hash, const Block& block) override;
    void PutUndo(const Hash256& hash, const utxo::BlockUndo& undo) override;

    /// How many bodies are held. For a test that wants to assert a body was stored once,
    /// and for the startup log line that says how much of a chain this node has.
    [[nodiscard]] size_t BlockCount() const noexcept { return blocks_.size(); }

    /// How many undo records are held. Fewer than `BlockCount` whenever the node holds a
    /// block it has never connected — a branch it knows about but did not follow.
    [[nodiscard]] size_t UndoCount() const noexcept { return undo_.size(); }

private:
    std::unordered_map<Hash256, Block> blocks_;
    std::unordered_map<Hash256, utxo::BlockUndo> undo_;
};

}  // namespace amarian::chain
