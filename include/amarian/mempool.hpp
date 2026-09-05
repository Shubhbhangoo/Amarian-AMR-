#pragma once

/// \file
/// The mempool: unconfirmed transactions this node knows of and might mine.
///
/// A transaction set under a weight cap, ranked by fee per weight unit, from which
/// the block assembler draws. A transaction is *accepted* if it passes every
/// consensus rule the next block would apply to it, pays at least
/// `MIN_RELAY_FEERATE`, keeps its ancestor and descendant counts inside their
/// bounds, and either conflicts with nothing in the pool or outbids what it
/// conflicts with.
///
/// Three properties are deliberate, because each one is a class of bug that a
/// mempool gets wrong quietly:
///
/// **Removal is link-exact.** Every entry records both its in-pool parents and its
/// in-pool children, and every removal repairs both directions. A removal that
/// forgot one side would leave an entry naming a parent that no longer exists, and
/// `GetTemplates` — which will not emit a transaction before its parents — would
/// then refuse to mine that entry for as long as the pool lived, with nothing
/// anywhere reporting a fault.
///
/// **Eviction removes packages, not entries.** The pool evicts the entry with the
/// lowest *descendant* fee rate together with all of its descendants. Evicting an
/// entry alone would either orphan its children or, worse, leave the pool holding a
/// child whose parent it no longer has — and a child whose parent is neither
/// confirmed nor in the pool is unminable. Ranking by descendant fee rate is what
/// keeps a low-fee parent that a high-fee child is paying for from being thrown
/// away: the pair is judged, and evicted, as the one economic unit it is.
///
/// **Selection is package-oriented.** `GetTemplates` walks entries by descendant
/// fee rate and, for each, emits that entry's not-yet-included ancestors ahead of
/// it, skipping the package whole if it does not fit. A single pass that merely
/// skipped an entry whose parents were absent would silently drop exactly the
/// child-pays-for-parent packages the ranking exists to prefer.
///
/// Replacement is a two-part economic test, and both parts are needed. The arriving
/// transaction must have a strictly higher fee rate than every cluster it conflicts
/// with — otherwise mining it would earn less per unit of block space than mining
/// what is already there — *and* it must pay strictly more absolute fee than the
/// total of everything its arrival would remove, so that a small high-rate
/// transaction cannot displace a large well-paying package. Amarian has no
/// replaceability flag: any transaction may be replaced if the test is met, because
/// a flag that a sender sets is not a property a miner has any reason to respect.
///
/// The mempool is not part of consensus. Two nodes can hold different mempools
/// without either being wrong, so this type is visible to the mining layer and the
/// RPC layer and to nothing that decides validity. That is also why it may hold a
/// `std::unordered_map` keyed on a hash and compute a fee rate in whatever way is
/// convenient: none of it has to match another node byte for byte.

#include <amarian/consensus/params.hpp>
#include <amarian/consensus/validation.hpp>
#include <amarian/primitives/block.hpp>
#include <amarian/primitives/outpoint.hpp>
#include <amarian/primitives/transaction.hpp>
#include <amarian/utxo/coins.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace amarian::mempool {

/// Total weight the pool may hold, in weight units: twenty-five full blocks.
inline constexpr size_t MAX_MEMPOOL_WEIGHT = 50'000'000;

/// The least a transaction may pay to enter the pool, in facets per weight unit.
///
/// One facet per weight unit, so a full block's worth of minimum-fee transactions
/// pays `MAX_BLOCK_WEIGHT` facets. Low, because Amarian's base unit is small and a
/// relay floor is a spam bound rather than a price; a miner that wants more chooses
/// what to include.
inline constexpr int64_t MIN_RELAY_FEERATE = 1;

/// How many in-pool descendants one transaction may have.
inline constexpr size_t MAX_MEMPOOL_DESCENDANTS = 25;

/// How many in-pool ancestors one transaction may have.
inline constexpr size_t MAX_MEMPOOL_ANCESTORS = 25;

/// Why the pool refused a transaction.
///
/// An enumeration and not a string, for the same reason `consensus::ValidationError`
/// is one: acceptance is a path an attacker can drive at will, and a reason that
/// allocated would let them make a node allocate. `Describe` turns one into text at
/// the edges — logs and RPC — and nowhere else.
enum class RejectReason : uint8_t {
    Coinbase,             ///< A coinbase exists only inside a block.
    AlreadyPresent,       ///< This exact wtxid is already in the pool.
    ConsensusRuleFailed,  ///< A rule the next block would apply refused it; see
                          ///< `Rejection::rule` for which one.
    MissingOrSpentInput,  ///< An input names an outpoint that is neither unspent
                          ///< nor created by a transaction already in the pool.
    ZeroWeight,           ///< No weight, so no fee rate exists for it.
    FeeBelowRelayMinimum,
    TooManyAncestors,
    TooManyDescendants,
    ConflictFeerateTooLow,  ///< Does not outbid a conflicting cluster's fee rate.
    ConflictFeeTooLow,      ///< Outbids on rate but pays less in total than it would remove.
    ConflictsTooMany,       ///< Would evict more than `MAX_REPLACEMENT_EVICTIONS`.
    PoolFull,               ///< Does not fit, and nothing cheaper could be evicted for it.
};

/// A short stable description, for logs and RPC. Never parsed by anything.
[[nodiscard]] std::string_view Describe(RejectReason reason) noexcept;

/// A refusal: the pool's reason, and the consensus rule behind it when there was one.
///
/// Two fields rather than one flattened enumeration because the pool's policy and
/// the chain's rules are different vocabularies with different lifetimes — a policy
/// reason may change in a release, a consensus rule may not — and collapsing them
/// would hide which kind of thing rejected a transaction.
struct Rejection {
    RejectReason reason;
    std::optional<consensus::ValidationError> rule;
};

/// A transaction in the pool, with the facts the pool ranks it by.
///
/// `txid` and `wtxid` are stored rather than recomputed. Both are double-SHA-256
/// over a fresh serialisation, and linking a new arrival to its in-pool parents
/// compares one txid per input per candidate; recomputing them there made accepting
/// a transaction cost a hash per pool entry.
struct Entry {
    Transaction tx;
    Hash256 txid;
    Hash256 wtxid;
    /// What the transaction pays: inputs minus outputs, as `CheckTransactionInputs`
    /// computed it. Always positive — a zero-fee transaction cannot reach the pool.
    int64_t fee = 0;
    size_t weight = 0;
    /// When this node accepted it, in seconds since the Unix epoch. Supplied by the
    /// caller, never read from a clock here.
    int64_t time = 0;
};

/// The mempool: entries keyed by wtxid, with an outpoint index over them.
class Mempool {
public:
    /// The most entries one arriving transaction may displace.
    ///
    /// A bound on work, not on economics. Without it a single transaction could be
    /// made to conflict with an arbitrarily large part of the pool, and the pool
    /// would do the whole eviction before deciding whether the replacement was even
    /// worth it. A sender who genuinely needs to displace more than this can do it
    /// in two steps.
    static constexpr size_t MAX_REPLACEMENT_EVICTIONS = 100;

    /// Offers `tx` to the pool, looking its inputs up in `base` and in the pool.
    ///
    /// The form callers should use. `base` is the node's UTXO set; an input may also
    /// name an output of a transaction already in the pool, which is what lets a
    /// chain of unconfirmed transactions exist at all — and therefore what makes the
    /// child-pays-for-parent ranking reachable rather than theoretical.
    ///
    /// `spend_height` is the height of the next block, because that is the earliest
    /// block this transaction could appear in. `now` is the accepting node's clock,
    /// passed in so that nothing here reads one.
    ///
    /// Returns the stored entry on success. The pointer stays valid until that entry
    /// is removed.
    [[nodiscard]] std::expected<const Entry*, Rejection>
    Accept(const Transaction& tx, const utxo::CoinsView& base, uint32_t spend_height, int64_t now,
           const ChainParams& params);

    /// Offers `tx` with the coins it spends supplied directly, `spent_coins[i]`
    /// being the coin `tx.inputs[i]` spends.
    ///
    /// The lower half of the call above, for a caller that has already done the
    /// lookup. A mismatched count is refused by `CheckTransactionInputs` rather than
    /// trusted, so a wrong span cannot read past its end.
    [[nodiscard]] std::expected<const Entry*, Rejection>
    Accept(const Transaction& tx, std::span<const Coin> spent_coins, uint32_t spend_height,
           int64_t now, const ChainParams& params);

    /// Removes one entry, repairing both link directions.
    ///
    /// Its children stay, with one fewer in-pool parent each. That is right for a
    /// transaction that was *mined*: its outputs are confirmed now, so a child of it
    /// is no longer waiting on the pool for anything. It is wrong for a transaction
    /// being thrown away, and `RemoveRecursive` is the call for that.
    void Remove(const Hash256& wtxid);

    /// Removes an entry and every descendant of it. Returns how many entries went.
    ///
    /// The call for discarding a transaction, because a child whose parent is
    /// neither confirmed nor in the pool can never be mined.
    [[nodiscard]] size_t RemoveRecursive(const Hash256& wtxid);

    /// Updates the pool for a block that has just been connected.
    ///
    /// Two things happen, and both are required for the next template to be valid.
    /// Every transaction the block contains is removed, since it is confirmed. Then
    /// every remaining entry that spends an outpoint the block also spent is removed
    /// *with its descendants*: a conflicting transaction can never confirm, and one
    /// left in the pool would be offered in the next template and make it invalid.
    ///
    /// Returns how many entries were removed in total.
    [[nodiscard]] size_t RemoveForBlock(const Block& block);

    /// Re-judges every entry against the chain as it now stands and removes the ones that
    /// no longer hold, each with its descendants. Returns how many entries went.
    ///
    /// For after a reorganisation, which is the only event that can make a transaction the
    /// pool already accepted invalid without anything arriving. Three things change when a
    /// block is reversed, and all three are handled here by simply asking the rules again:
    ///
    /// - An output *created* by a reversed block no longer exists, so an entry spending one
    ///   can never confirm. It is not enough to drop that entry: its own children spend
    ///   coins that will never exist either, which is why removal is recursive.
    /// - `spend_height` is now lower, so a coinbase spend that was mature is immature
    ///   again, and a transaction whose locktime the old height satisfied may fail at the
    ///   new one.
    /// - A coin the reversed block *spent* is unspent again, which invalidates nothing —
    ///   but it is what makes re-offering the reversed block's own transactions possible,
    ///   and that is the node's call to make rather than this one's.
    ///
    /// Re-asked rather than tracked. The pool could record which coins each entry depends
    /// on and invalidate by intersection, which is faster and is a second model of what
    /// makes a transaction valid — and the two models disagreeing would leave an invalid
    /// transaction in the pool, which reaches a block template and makes it a block this
    /// node's own rules refuse.
    ///
    /// `spend_height` is the height of the block that would now confirm these, so one more
    /// than the new tip's.
    [[nodiscard]] size_t RemoveForReorg(const utxo::CoinsView& base, uint32_t spend_height,
                                        const ChainParams& params);

    /// Evicts lowest-descendant-fee-rate packages until the pool holds at most
    /// `target_weight`. Returns how many entries were removed.
    ///
    /// A target rather than a bare "trim to the limit", because the caller that
    /// needs room needs room for something specific: making space for a transaction
    /// of weight *w* means trimming to `MAX_MEMPOOL_WEIGHT - w`, and a trim that
    /// only ever restored the limit itself could never make space for anything.
    [[nodiscard]] size_t Trim(size_t target_weight);

    /// Entries for a block template, ancestors before descendants, by descendant
    /// fee rate descending, within `available_weight`.
    ///
    /// The order is a valid block order: a transaction never appears before one it
    /// spends from. A package that does not fit is skipped whole and does not stop
    /// the walk, so a large low-rate package cannot block the smaller ones behind it.
    ///
    /// Entries and not transactions, so that a caller summing the fees it is about to
    /// commit to in a coinbase reads each one from the entry it was handed instead of
    /// looking it up again under a wtxid it has to recompute — two double-SHA-256
    /// hashes per transaction, on the path that produces every block.
    ///
    /// The pointers stay valid until the pool next changes.
    [[nodiscard]] std::vector<const Entry*> GetTemplates(size_t available_weight) const;

    /// The entry for `wtxid`, or null. Valid until that entry is removed.
    [[nodiscard]] const Entry* Find(const Hash256& wtxid) const noexcept;

    /// The entry whose *txid* is `txid`, or null.
    ///
    /// Distinct from `Find`: a txid commits to a transaction's effects and a wtxid to
    /// its whole encoding, so an input naming a parent by txid must be resolved
    /// through this one.
    [[nodiscard]] const Entry* FindByTxid(const Hash256& txid) const noexcept;

    /// The pool entry spending `outpoint`, or null.
    [[nodiscard]] const Entry* SpenderOf(const OutPoint& outpoint) const noexcept;

    [[nodiscard]] size_t Size() const noexcept { return entries_.size(); }
    [[nodiscard]] size_t TotalWeight() const noexcept { return total_weight_; }
    [[nodiscard]] int64_t TotalFees() const noexcept { return total_fees_; }

    /// Every wtxid in the pool, in unspecified order. For RPC listing.
    [[nodiscard]] std::vector<Hash256> Wtxids() const;

    /// The fee rate, in facets per weight unit rounded down, of the cheapest package
    /// the pool would evict first. Zero when the pool is empty.
    ///
    /// What a caller needs to answer "would this transaction get in", and what an
    /// RPC reports as the pool's effective floor once it is full.
    [[nodiscard]] int64_t LowestPackageFeerate() const;

private:
    struct EntryInternal {
        Entry entry;
        std::unordered_set<Hash256> children;
        std::unordered_set<Hash256> parents;
    };

    std::unordered_map<Hash256, EntryInternal> entries_;

    /// Which entry spends a given outpoint. One entry per outpoint: the pool never
    /// holds two transactions spending the same coin, because the second is either
    /// refused or replaces the first.
    ///
    /// This index is what keeps accepting a transaction linear in its own input
    /// count. Scanning every entry's inputs instead made acceptance quadratic in the
    /// pool's size, on a path an attacker chooses how often to drive.
    std::unordered_map<OutPoint, Hash256> spent_by_;

    /// txid to wtxid, for resolving an input that names an in-pool parent.
    std::unordered_map<Hash256, Hash256> by_txid_;

    size_t total_weight_ = 0;
    int64_t total_fees_ = 0;

    /// Every entry reachable downward from `wtxid`, including it. Bounded by the
    /// pool's size, and in practice by `MAX_MEMPOOL_DESCENDANTS`.
    [[nodiscard]] std::unordered_set<Hash256> DescendantsOf(const Hash256& wtxid) const;

    /// Every entry reachable upward from `wtxid`, including it.
    [[nodiscard]] std::unordered_set<Hash256> AncestorsOf(const Hash256& wtxid) const;

    /// The total fee and weight of `wtxid` together with all its descendants.
    void DescendantTotals(const Hash256& wtxid, int64_t& fee, size_t& weight) const;

    /// The in-pool entries spending any input of `tx`.
    [[nodiscard]] std::unordered_set<Hash256> DirectConflicts(const Transaction& tx) const;

    /// The coin each of `tx`'s inputs spends, taken from `base` or from a transaction
    /// already in the pool. Absent when an input names an outpoint neither can supply.
    ///
    /// Shared by acceptance and by the reorganisation sweep so that a transaction is
    /// judged against the same coins in both. Two resolutions of "what does this input
    /// spend" would be two answers, and the sweep exists precisely to catch entries whose
    /// answer has changed.
    [[nodiscard]] std::optional<std::vector<Coin>>
    ResolveInputs(const Transaction& tx, const utxo::CoinsView& base, uint32_t spend_height) const;

    /// Unlinks one entry and drops it from every index. The single place an entry
    /// leaves the pool, so the indices cannot fall out of step with the map.
    void Unlink(const Hash256& wtxid);
};

}  // namespace amarian::mempool
