#pragma once

/// \file
/// The set of unspent coins, and the layered cache that changes it.
///
/// This is the state a chain *is*. A block is a proposed edit to this set; the chain's
/// history is the sequence of edits that were applied. Every node that has validated the
/// same blocks holds the same set, and two nodes that disagree about it disagree about who
/// owns what — so the operations here are the narrowest they can be, and each one that can
/// fail says exactly how.
///
/// The design is three types with one job each:
///
///  - `CoinsView` is the read interface. Anything that can answer "is this outpoint
///    unspent, and if so what is in it" is a view: an empty set, an in-memory map, a
///    RocksDB column family, or a cache over any of those. Consensus never sees one — it
///    is handed `Coin` values — so a view is free to do I/O.
///  - `CoinsSink` is the write interface: one call, "the entry for this outpoint is now
///    *this*". Separate from the view because the things that consume writes (a database
///    batch, a parent cache) are not always things that answer reads.
///  - `CoinsCache` is both, over a base view. It is the only mutable UTXO set in the
///    system: the node's in-memory set is a cache over an empty view or over storage, and
///    a single block's changes are a cache over that. Nesting is how atomicity is had for
///    free — a batch that fails is simply never flushed.
///
/// There is deliberately **no read-through caching**. A `CoinsCache` holds only entries
/// that *differ* from its base, so its map is exactly the change set: no dirty flags, no
/// `mutable`, and a flush that writes precisely what changed. A read for an untouched
/// outpoint goes to the base every time. That costs a lookup that a read-through cache
/// would have saved, and buys the property that "what is in the map" and "what must be
/// written" are the same question — which is the property a reorg has to be able to trust.

#include <amarian/primitives/coin.hpp>
#include <amarian/primitives/outpoint.hpp>

#include <cstddef>
#include <optional>
#include <unordered_map>

namespace amarian::utxo {

/// Read access to a set of unspent coins.
///
/// `std::nullopt` means *not unspent*: either the outpoint never existed or it has already
/// been spent. The two are indistinguishable to consensus and must stay that way — a set
/// that could tell them apart would have to remember every coin ever spent.
class CoinsView {
public:
    CoinsView() = default;
    CoinsView(const CoinsView&) = default;
    CoinsView(CoinsView&&) = default;
    CoinsView& operator=(const CoinsView&) = default;
    CoinsView& operator=(CoinsView&&) = default;
    virtual ~CoinsView() = default;

    [[nodiscard]] virtual std::optional<Coin> GetCoin(const OutPoint& outpoint) const = 0;

    /// Whether the outpoint is unspent. Separate from `GetCoin` because a database backend
    /// can answer existence without deserialising a coin, and the duplicate-outpoint rule
    /// asks nothing else.
    [[nodiscard]] virtual bool HaveCoin(const OutPoint& outpoint) const = 0;
};

/// A view in which nothing exists.
///
/// The state before the genesis block, and so the base every chain starts from: a fresh
/// regtest node, a node syncing from zero, and every test that does not want a database.
/// Stateless, so one instance can serve any number of caches.
class EmptyCoinsView final : public CoinsView {
public:
    [[nodiscard]] std::optional<Coin> GetCoin(const OutPoint&) const override {
        return std::nullopt;
    }

    [[nodiscard]] bool HaveCoin(const OutPoint&) const override { return false; }
};

/// Write access to a set of unspent coins: one entry at a time, replace-or-erase.
///
/// `std::nullopt` erases. A single call rather than separate add and spend operations
/// because a flush is a sequence of "the truth about this outpoint is now X", and the
/// receiver — a parent cache, a database write batch — should not have to care which of
/// the two a given entry was.
class CoinsSink {
public:
    CoinsSink() = default;
    CoinsSink(const CoinsSink&) = default;
    CoinsSink(CoinsSink&&) = default;
    CoinsSink& operator=(const CoinsSink&) = default;
    CoinsSink& operator=(CoinsSink&&) = default;
    virtual ~CoinsSink() = default;

    virtual void Write(const OutPoint& outpoint, const std::optional<Coin>& coin) = 0;
};

/// A mutable layer of changes over a base view. Construct one with `CoinsCache::Over`.
///
/// Reads fall through to the base for anything this layer has not touched; writes stay
/// here until `Flush` moves them into a sink. The base is a reference and is never
/// modified, so a cache can be abandoned by destroying it.
///
/// Lifetime: the base must outlive the cache. That is the caller's obligation and it is
/// the ordinary one for a reference member — the alternative, a shared pointer, would buy
/// nothing here because every cache in this system is a local variable or a member whose
/// base is declared before it.
class CoinsCache final : public CoinsView, public CoinsSink {
public:
    /// A new, empty layer over `base`.
    ///
    /// A named function rather than a constructor, and that is not a matter of taste. With a
    /// public `CoinsCache(const CoinsView&)`, the expression `CoinsCache layer(other_cache)`
    /// resolves to the *copy* constructor — an exact match beats a derived-to-base
    /// conversion — and silently produces a duplicate of `other_cache`'s change set sharing
    /// `other_cache`'s base, instead of a layer above it. The two spellings are identical,
    /// and the second one quietly breaks atomicity: a batch built that way and then flushed
    /// writes its parent's changes back over the parent and loses its own. Since a cache over
    /// a cache is the *normal* case here — a block's batch over the node's set — the
    /// constructor is private and the copy operations are deleted, so the mistake cannot be
    /// written at all.
    [[nodiscard]] static CoinsCache Over(const CoinsView& base) noexcept {
        return CoinsCache(base);
    }

    /// Deleted: a cache's contents mean nothing apart from the base they are a change set
    /// against, so duplicating one is never what a caller meant. See `Over`.
    CoinsCache(const CoinsCache&) = delete;
    CoinsCache(CoinsCache&&) = delete;
    CoinsCache& operator=(const CoinsCache&) = delete;
    CoinsCache& operator=(CoinsCache&&) = delete;
    ~CoinsCache() override = default;

    [[nodiscard]] std::optional<Coin> GetCoin(const OutPoint& outpoint) const override;
    [[nodiscard]] bool HaveCoin(const OutPoint& outpoint) const override;

    void Write(const OutPoint& outpoint, const std::optional<Coin>& coin) override;

    /// Adds a coin at an outpoint that is currently unspent by nothing.
    ///
    /// Returns false and changes nothing if a coin is already there. The caller decides
    /// what that means: for a block being connected it is `TxCreatesExistingOutpoint` —
    /// overwriting a live coin destroys it — and for a block being disconnected it is a
    /// corrupted undo record.
    [[nodiscard]] bool AddCoin(const OutPoint& outpoint, const Coin& coin);

    /// Removes a coin and returns it, or `std::nullopt` if it was not there.
    ///
    /// Returning the coin is what lets connecting a block do one lookup instead of two:
    /// the value is needed to validate the spend *and* to record in the undo data, and it
    /// is exactly what the removal already had in hand.
    [[nodiscard]] std::optional<Coin> SpendCoin(const OutPoint& outpoint);

    /// Moves every change into `sink` and empties this layer.
    ///
    /// After it returns, the cache holds nothing and the sink holds everything it held.
    /// This is the only place a change becomes visible to anything else, which is what
    /// makes "build a batch, flush it only on success" a complete atomicity story.
    void Flush(CoinsSink& sink);

    /// Throws the changes away without applying them. A cache is also abandoned by simply
    /// destroying it; this exists for the case where the same cache is reused for the next
    /// attempt.
    void Discard() noexcept { entries_.clear(); }

    /// How many outpoints this layer has an opinion about. Not the size of the UTXO set:
    /// this counts changes against the base, which for a root cache — one whose base is
    /// `EmptyCoinsView` — happens to be the same number.
    [[nodiscard]] size_t ChangeCount() const noexcept { return entries_.size(); }

private:
    explicit CoinsCache(const CoinsView& base) noexcept : base_(&base) {}

    /// Records that an outpoint is spent, without leaving a record if the base does not
    /// think it exists either.
    ///
    /// The distinction matters and it is the reason this is not just `Write(outpoint, {})`.
    /// If the base has the coin, this layer must carry a tombstone, or a read would fall
    /// through and find it again. If the base does not — because this layer created the
    /// coin, or because the base is the empty view — the entry is erased outright.
    /// Otherwise a root cache would accumulate one tombstone per coin ever spent and the
    /// in-memory UTXO set would grow with the chain's whole history rather than with its
    /// unspent output count.
    void MarkSpent(const OutPoint& outpoint);

    /// Pointer rather than a reference member so that the class has a trivial destructor and
    /// the base is spelled the same way everywhere it is read. Never null: `Over` binds it
    /// from a reference, and it is never reseated.
    const CoinsView* base_;

    /// Every entry is a change against `base_`. `std::nullopt` is a spend.
    std::unordered_map<OutPoint, std::optional<Coin>> entries_;
};

}  // namespace amarian::utxo
