#include <amarian/mempool.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <queue>
#include <utility>
#include <vector>

namespace amarian::mempool {
namespace {

/// A 128-bit unsigned value, as a high and a low half.
struct Product {
    uint64_t hi;
    uint64_t lo;
};

/// The exact 128-bit product of two 64-bit values, assembled from 32-bit halves.
///
/// Hand-built rather than `__int128` for the same reason the ASERT arithmetic is
/// hand-built: the extension is not available on every toolchain this has to build
/// with, and eviction order deciding itself differently under a different compiler
/// is a difference between two nodes that nothing would report.
[[nodiscard]] Product Mul64(uint64_t a, uint64_t b) noexcept {
    constexpr uint64_t LOW32 = 0xFFFF'FFFFULL;
    const uint64_t a_lo = a & LOW32;
    const uint64_t a_hi = a >> 32;
    const uint64_t b_lo = b & LOW32;
    const uint64_t b_hi = b >> 32;

    const uint64_t ll = a_lo * b_lo;
    const uint64_t lh = a_lo * b_hi;
    const uint64_t hl = a_hi * b_lo;
    const uint64_t hh = a_hi * b_hi;

    // At most three values below 2^32 summed, so this cannot itself overflow.
    const uint64_t mid = (ll >> 32) + (lh & LOW32) + (hl & LOW32);
    return Product{.hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32),
                   .lo = (ll & LOW32) | (mid << 32)};
}

/// Lexicographic comparison of two 128-bit values: -1, 0 or +1.
[[nodiscard]] int CompareProducts(const Product& a, const Product& b) noexcept {
    if (a.hi != b.hi) {
        return a.hi < b.hi ? -1 : 1;
    }
    if (a.lo != b.lo) {
        return a.lo < b.lo ? -1 : 1;
    }
    return 0;
}

/// Compares `fee_a/weight_a` against `fee_b/weight_b` exactly: -1, 0 or +1.
///
/// Cross-multiplied rather than divided, because division throws away the fraction
/// that decides most of these comparisons: two packages less than one facet per
/// weight unit apart would compare equal and eviction would then pick between them
/// by hash-table order.
///
/// The cross products need 128 bits. A fee reaches `MAX_MONEY` (8.4e16 facets) and a
/// package's weight reaches `MAX_MEMPOOL_WEIGHT` (5e7), so `fee * weight` reaches
/// 4.2e24 — five orders of magnitude past what an `int64_t` holds. This is where the
/// earlier shape of this code was wrong: it scaled the fee by 2^16 before dividing,
/// which wraps for any fee above about 1.4e14 facets. That is 14 000 AMR, a fee a
/// real transaction can pay, and past it a wrapped negative rate made the
/// highest-paying transaction in the pool look like the cheapest thing in it — first
/// to be evicted, last to be mined.
///
/// Total for adversarial inputs. A non-positive fee is treated as zero and a
/// weightless package as cheaper than any package with weight, so no input produces
/// an inconsistent ordering.
[[nodiscard]] int CompareFeerate(int64_t fee_a, size_t weight_a, int64_t fee_b,
                                 size_t weight_b) noexcept {
    if (weight_a == 0 || weight_b == 0) {
        if (weight_a == weight_b) {
            return 0;
        }
        return weight_a == 0 ? -1 : 1;
    }
    const uint64_t scaled_a = fee_a > 0 ? static_cast<uint64_t>(fee_a) : 0;
    const uint64_t scaled_b = fee_b > 0 ? static_cast<uint64_t>(fee_b) : 0;
    return CompareProducts(Mul64(scaled_a, static_cast<uint64_t>(weight_b)),
                           Mul64(scaled_b, static_cast<uint64_t>(weight_a)));
}

/// Fee per weight unit, rounded down. For reporting only — `CompareFeerate` decides
/// everything the pool acts on, because this loses the fraction.
[[nodiscard]] int64_t FeerateFloor(int64_t fee, size_t weight) noexcept {
    if (weight == 0 || fee <= 0) {
        return 0;
    }
    return fee / static_cast<int64_t>(weight);
}

}  // namespace
std::string_view Describe(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::Coinbase:
            return "a coinbase exists only inside a block";
        case RejectReason::AlreadyPresent:
            return "already in the mempool";
        case RejectReason::ConsensusRuleFailed:
            return "a consensus rule refused it";
        case RejectReason::MissingOrSpentInput:
            return "an input names an outpoint that is not unspent and not in the mempool";
        case RejectReason::ZeroWeight:
            return "no weight, so no fee rate";
        case RejectReason::FeeBelowRelayMinimum:
            return "pays less than the minimum relay fee rate";
        case RejectReason::TooManyAncestors:
            return "too many unconfirmed ancestors";
        case RejectReason::TooManyDescendants:
            return "would give an ancestor too many unconfirmed descendants";
        case RejectReason::ConflictFeerateTooLow:
            return "conflicts with a mempool package that pays a higher fee rate";
        case RejectReason::ConflictFeeTooLow:
            return "conflicts with mempool transactions paying more in total fees";
        case RejectReason::ConflictsTooMany:
            return "would replace more mempool transactions than one transaction may";
        case RejectReason::PoolFull:
            return "the mempool is full of transactions paying more";
    }
    // Unreachable: the switch is total, and -Wswitch-enum makes a new enumerator a build
    // failure here rather than a silent fall-through.
    return "unknown rejection";
}

// --- The shape of the pool: who depends on whom -----------------------------------

std::unordered_set<Hash256> Mempool::DescendantsOf(const Hash256& wtxid) const {
    std::unordered_set<Hash256> reached;
    if (!entries_.contains(wtxid)) {
        return reached;
    }
    std::queue<Hash256> frontier;
    frontier.push(wtxid);
    reached.insert(wtxid);
    while (!frontier.empty()) {
        const Hash256 current = frontier.front();
        frontier.pop();
        const auto it = entries_.find(current);
        if (it == entries_.end()) {
            continue;
        }
        for (const Hash256& child : it->second.children) {
            if (reached.insert(child).second) {
                frontier.push(child);
            }
        }
    }
    return reached;
}

std::unordered_set<Hash256> Mempool::AncestorsOf(const Hash256& wtxid) const {
    std::unordered_set<Hash256> reached;
    if (!entries_.contains(wtxid)) {
        return reached;
    }
    std::queue<Hash256> frontier;
    frontier.push(wtxid);
    reached.insert(wtxid);
    while (!frontier.empty()) {
        const Hash256 current = frontier.front();
        frontier.pop();
        const auto it = entries_.find(current);
        if (it == entries_.end()) {
            continue;
        }
        for (const Hash256& parent : it->second.parents) {
            if (reached.insert(parent).second) {
                frontier.push(parent);
            }
        }
    }
    return reached;
}

void Mempool::DescendantTotals(const Hash256& wtxid, int64_t& fee, size_t& weight) const {
    fee = 0;
    weight = 0;
    for (const Hash256& id : DescendantsOf(wtxid)) {
        const auto it = entries_.find(id);
        if (it != entries_.end()) {
            fee += it->second.entry.fee;
            weight += it->second.entry.weight;
        }
    }
}

std::unordered_set<Hash256> Mempool::DirectConflicts(const Transaction& tx) const {
    std::unordered_set<Hash256> conflicts;
    for (const TxInput& input : tx.inputs) {
        const auto it = spent_by_.find(input.outpoint);
        if (it != spent_by_.end()) {
            conflicts.insert(it->second);
        }
    }
    return conflicts;
}

// --- Removal ----------------------------------------------------------------------

void Mempool::Unlink(const Hash256& wtxid) {
    const auto it = entries_.find(wtxid);
    if (it == entries_.end()) {
        return;
    }
    const EntryInternal& doomed = it->second;

    // Both directions, which is the whole point of this function existing. A removal
    // that repaired only the parents' `children` sets would leave every child naming
    // a parent the pool no longer has, and `GetTemplates` — which never emits a
    // transaction before its parents — would refuse to mine those children for as
    // long as the pool lived, reporting nothing.
    for (const Hash256& parent : doomed.parents) {
        const auto pit = entries_.find(parent);
        if (pit != entries_.end()) {
            pit->second.children.erase(wtxid);
        }
    }
    for (const Hash256& child : doomed.children) {
        const auto cit = entries_.find(child);
        if (cit != entries_.end()) {
            cit->second.parents.erase(wtxid);
        }
    }

    // The indices, guarded on the value: an outpoint is only this entry's to give up
    // if this entry is the one recorded as spending it.
    for (const TxInput& input : doomed.entry.tx.inputs) {
        const auto sit = spent_by_.find(input.outpoint);
        if (sit != spent_by_.end() && sit->second == wtxid) {
            spent_by_.erase(sit);
        }
    }
    const auto tit = by_txid_.find(doomed.entry.txid);
    if (tit != by_txid_.end() && tit->second == wtxid) {
        by_txid_.erase(tit);
    }

    total_weight_ -= doomed.entry.weight;
    total_fees_ -= doomed.entry.fee;
    entries_.erase(it);
}

void Mempool::Remove(const Hash256& wtxid) {
    Unlink(wtxid);
}

size_t Mempool::RemoveRecursive(const Hash256& wtxid) {
    const std::unordered_set<Hash256> doomed = DescendantsOf(wtxid);
    for (const Hash256& id : doomed) {
        Unlink(id);
    }
    return doomed.size();
}

size_t Mempool::RemoveForBlock(const Block& block) {
    size_t removed = 0;

    // Confirmed, so removed on its own: a child of a mined transaction is no longer
    // waiting on the pool for anything. Matched by txid rather than wtxid, because
    // the copy that got mined may carry a different witness than the copy this node
    // holds — same effects, different encoding — and it is the effects that confirmed.
    for (const Transaction& tx : block.transactions) {
        const auto it = by_txid_.find(tx.Txid());
        if (it == by_txid_.end()) {
            continue;
        }
        const Hash256 wtxid = it->second;
        Remove(wtxid);
        ++removed;
    }

    // Then anything left that spends what the block spent. A conflicting transaction
    // can never confirm now, and one left in the pool would be handed to the next
    // template and make it a block this node refuses. Recursive, because a child of a
    // conflicting transaction is spending a coin that will never exist.
    for (const Transaction& tx : block.transactions) {
        if (tx.IsCoinbase()) {
            continue;
        }
        for (const TxInput& input : tx.inputs) {
            const auto it = spent_by_.find(input.outpoint);
            if (it == spent_by_.end()) {
                continue;
            }
            const Hash256 spender = it->second;
            removed += RemoveRecursive(spender);
        }
    }
    return removed;
}

size_t Mempool::RemoveForReorg(const utxo::CoinsView& base, uint32_t spend_height,
                               const ChainParams& params) {
    // A snapshot of the keys, because removal here is recursive and takes entries this loop
    // has not reached yet; iterating `entries_` itself would invalidate the iterator on the
    // first removal.
    const std::vector<Hash256> candidates = Wtxids();

    size_t removed = 0;
    for (const Hash256& wtxid : candidates) {
        const auto it = entries_.find(wtxid);
        if (it == entries_.end()) {
            // Already gone with an ancestor, which is also why the order of this walk does
            // not matter: an entry judged before its now-invalid parent resolves through
            // that parent and passes, and then leaves with it when the parent is reached.
            continue;
        }
        const Transaction& tx = it->second.entry.tx;

        const std::optional<std::vector<Coin>> spent = ResolveInputs(tx, base, spend_height);
        if (!spent.has_value()) {
            // An input names an outpoint that is neither unspent on the new chain nor
            // created by anything still in the pool. The commonest cause is the one this
            // function exists for: the output was created by a block that has just been
            // reversed, so it does not exist and cannot be made to.
            removed += RemoveRecursive(wtxid);
            continue;
        }

        // The same call acceptance makes, so maturity, the locktime, the fee and the
        // signatures are all re-judged at the new height by the one implementation of each
        // rule. `tx` is not read after this: `RemoveRecursive` erases the entry it refers
        // to, and the condition of an `if` is fully evaluated before its body runs.
        if (const consensus::Computed<int64_t> fee =
                consensus::CheckTransactionInputs(tx, *spent, spend_height, params);
            !fee.has_value()) {
            removed += RemoveRecursive(wtxid);
        }
    }
    return removed;
}

size_t Mempool::Trim(size_t target_weight) {
    size_t evicted = 0;
    while (total_weight_ > target_weight && !entries_.empty()) {
        // The cheapest *package*, not the cheapest entry. Ranking by descendant fee
        // rate is what stops a low-fee parent that a high-fee child is paying for
        // from being thrown away: the two are one economic unit and are judged as one.
        std::optional<Hash256> worst;
        int64_t worst_fee = 0;
        size_t worst_weight = 0;
        for (const auto& [id, unused_links] : entries_) {
            int64_t fee = 0;
            size_t weight = 0;
            DescendantTotals(id, fee, weight);
            if (!worst.has_value()) {
                worst = id;
                worst_fee = fee;
                worst_weight = weight;
                continue;
            }
            const int cmp = CompareFeerate(fee, weight, worst_fee, worst_weight);
            // Ties broken by hash, so that eviction is reproducible from one run to
            // the next rather than following the order a hash table happens to have.
            if (cmp < 0 || (cmp == 0 && id < *worst)) {
                worst = id;
                worst_fee = fee;
                worst_weight = weight;
            }
        }
        if (!worst.has_value()) {
            break;
        }
        evicted += RemoveRecursive(*worst);
    }
    return evicted;
}

// --- Acceptance -------------------------------------------------------------------

std::optional<std::vector<Coin>> Mempool::ResolveInputs(const Transaction& tx,
                                                        const utxo::CoinsView& base,
                                                        uint32_t spend_height) const {
    std::vector<Coin> spent;
    spent.reserve(tx.inputs.size());
    for (const TxInput& input : tx.inputs) {
        if (std::optional<Coin> confirmed = base.GetCoin(input.outpoint); confirmed.has_value()) {
            spent.push_back(std::move(*confirmed));
            continue;
        }

        // Not on chain — but it may be an output of a transaction already in the pool.
        // Resolving those is what lets a chain of unconfirmed transactions exist at
        // all, and therefore what makes the child-pays-for-parent ranking reachable
        // instead of theoretical: without it a child could never be accepted, so no
        // parent could ever have one paying its way.
        const Entry* const parent = FindByTxid(input.outpoint.txid);
        if (parent == nullptr || input.outpoint.index >= parent->tx.outputs.size()) {
            return std::nullopt;
        }
        Coin coin;
        coin.output = parent->tx.outputs[input.outpoint.index];
        // The height the coin would be created at, which is the height being spent
        // into. Only the maturity rule reads it, and only for a coinbase — which can
        // never be in the pool — so nothing depends on this beyond its consistency.
        coin.height = spend_height;
        coin.is_coinbase = false;
        spent.push_back(std::move(coin));
    }
    return spent;
}

std::expected<const Entry*, Rejection> Mempool::Accept(const Transaction& tx,
                                                      const utxo::CoinsView& base,
                                                      uint32_t spend_height, int64_t now,
                                                      const ChainParams& params) {
    if (tx.IsCoinbase()) {
        return std::unexpected(Rejection{.reason = RejectReason::Coinbase, .rule = std::nullopt});
    }

    // The context-free rules before the lookups. Not only because they are cheaper:
    // they are what guarantees no input names the coinbase sentinel and no outpoint
    // appears twice, so the loop below cannot be made to look up a null outpoint or
    // to double-count one coin. The overload this delegates to checks them again,
    // which costs one structural pass and buys a public entry point that is safe on
    // its own rather than one that trusts its caller to have gone through here.
    if (const consensus::Verdict shape = consensus::CheckTransaction(tx, params); !shape) {
        return std::unexpected(
            Rejection{.reason = RejectReason::ConsensusRuleFailed, .rule = shape.error()});
    }

    const std::optional<std::vector<Coin>> spent = ResolveInputs(tx, base, spend_height);
    if (!spent.has_value()) {
        return std::unexpected(Rejection{.reason = RejectReason::MissingOrSpentInput,
                                         .rule = consensus::ValidationError::TxInputMissingOrSpent});
    }
    return Accept(tx, *spent, spend_height, now, params);
}

std::expected<const Entry*, Rejection> Mempool::Accept(const Transaction& tx,
                                                       std::span<const Coin> spent_coins,
                                                       uint32_t spend_height, int64_t now,
                                                       const ChainParams& params) {
    if (tx.IsCoinbase()) {
        return std::unexpected(Rejection{.reason = RejectReason::Coinbase, .rule = std::nullopt});
    }
    const Hash256 wtxid = tx.Wtxid();
    if (entries_.contains(wtxid)) {
        return std::unexpected(
            Rejection{.reason = RejectReason::AlreadyPresent, .rule = std::nullopt});
    }

    if (const consensus::Verdict shape = consensus::CheckTransaction(tx, params); !shape) {
        return std::unexpected(
            Rejection{.reason = RejectReason::ConsensusRuleFailed, .rule = shape.error()});
    }

    // Maturity, the fee, and the signatures, in one call. The fee this returns is the
    // one the pool ranks by — recomputing it here would be a second implementation of
    // the rule that decides whether a transaction mints coins, and two implementations
    // of that rule are two things that can disagree about the supply.
    const consensus::Computed<int64_t> fee =
        consensus::CheckTransactionInputs(tx, spent_coins, spend_height, params);
    if (!fee.has_value()) {
        return std::unexpected(
            Rejection{.reason = RejectReason::ConsensusRuleFailed, .rule = fee.error()});
    }

    const size_t weight = tx.Weight();
    if (weight == 0) {
        return std::unexpected(Rejection{.reason = RejectReason::ZeroWeight, .rule = std::nullopt});
    }
    if (weight > MAX_MEMPOOL_WEIGHT) {
        return std::unexpected(Rejection{.reason = RejectReason::PoolFull, .rule = std::nullopt});
    }
    if (*fee < static_cast<int64_t>(weight) * MIN_RELAY_FEERATE) {
        return std::unexpected(
            Rejection{.reason = RejectReason::FeeBelowRelayMinimum, .rule = std::nullopt});
    }

    // --- What this arrival would displace ------------------------------------------
    //
    // Every pool entry spending an outpoint this transaction also spends, and every
    // descendant of one: a replaced transaction's children spend coins that will never
    // exist, so they go with it and they count against the economics of the swap.
    const std::unordered_set<Hash256> direct = DirectConflicts(tx);
    std::unordered_set<Hash256> doomed;
    for (const Hash256& id : direct) {
        for (const Hash256& reached : DescendantsOf(id)) {
            doomed.insert(reached);
        }
    }
    if (doomed.size() > MAX_REPLACEMENT_EVICTIONS) {
        return std::unexpected(
            Rejection{.reason = RejectReason::ConflictsTooMany, .rule = std::nullopt});
    }

    if (!doomed.empty()) {
        // Rule one: a strictly higher fee rate than every cluster being displaced.
        // Per cluster and not against their total, because a miner replacing them is
        // giving up each cluster's rate separately for this one's.
        for (const Hash256& id : direct) {
            int64_t cluster_fee = 0;
            size_t cluster_weight = 0;
            DescendantTotals(id, cluster_fee, cluster_weight);
            if (CompareFeerate(*fee, weight, cluster_fee, cluster_weight) <= 0) {
                return std::unexpected(
                    Rejection{.reason = RejectReason::ConflictFeerateTooLow, .rule = std::nullopt});
            }
        }
        // Rule two: strictly more absolute fee than everything the swap removes.
        // Without it a small transaction at a high rate could displace a large
        // well-paying package and leave the miner with less money for the same block.
        int64_t displaced_fee = 0;
        for (const Hash256& id : doomed) {
            const auto it = entries_.find(id);
            if (it != entries_.end()) {
                displaced_fee += it->second.entry.fee;
            }
        }
        if (*fee <= displaced_fee) {
            return std::unexpected(
                Rejection{.reason = RejectReason::ConflictFeeTooLow, .rule = std::nullopt});
        }
    }

    // --- Where this arrival sits in the pool's shape --------------------------------
    std::unordered_set<Hash256> parents;
    for (const TxInput& input : tx.inputs) {
        const auto it = by_txid_.find(input.outpoint.txid);
        if (it != by_txid_.end()) {
            parents.insert(it->second);
        }
    }
    for (const Hash256& parent : parents) {
        // A transaction that spends an output of something it also conflicts with. The
        // replacement would take the parent away and leave this input naming a coin
        // that no longer exists anywhere, so it is refused rather than accepted into a
        // pool it could never be mined out of.
        if (doomed.contains(parent)) {
            return std::unexpected(
                Rejection{.reason = RejectReason::MissingOrSpentInput,
                          .rule = consensus::ValidationError::TxInputMissingOrSpent});
        }
    }

    // Counted against the pool as it will be, so the entries this arrival displaces do
    // not count against its limits.
    std::unordered_set<Hash256> ancestors;
    for (const Hash256& parent : parents) {
        for (const Hash256& id : AncestorsOf(parent)) {
            if (!doomed.contains(id)) {
                ancestors.insert(id);
            }
        }
    }
    if (ancestors.size() > MAX_MEMPOOL_ANCESTORS) {
        return std::unexpected(
            Rejection{.reason = RejectReason::TooManyAncestors, .rule = std::nullopt});
    }
    for (const Hash256& id : ancestors) {
        size_t surviving = 0;
        for (const Hash256& descendant : DescendantsOf(id)) {
            if (descendant != id && !doomed.contains(descendant)) {
                ++surviving;
            }
        }
        if (surviving + 1 > MAX_MEMPOOL_DESCENDANTS) {
            return std::unexpected(
                Rejection{.reason = RejectReason::TooManyDescendants, .rule = std::nullopt});
        }
    }

    // --- Committed ------------------------------------------------------------------
    //
    // Every reason to refuse has been checked, so the displaced entries go now. If the
    // trim below then evicts this very transaction the pool has lost them for an
    // arrival it did not keep — which cannot happen for a non-empty `doomed`, because
    // an arrival that outbid a cluster on both rate and total fee is not the cheapest
    // package in a pool that still contains that cluster's peers.
    for (const Hash256& id : doomed) {
        Unlink(id);
    }

    EntryInternal internal;
    internal.entry.tx = tx;
    internal.entry.txid = tx.Txid();
    internal.entry.wtxid = wtxid;
    internal.entry.fee = *fee;
    internal.entry.weight = weight;
    internal.entry.time = now;
    internal.parents = parents;

    const auto it = entries_.emplace(wtxid, std::move(internal)).first;
    for (const Hash256& parent : it->second.parents) {
        const auto pit = entries_.find(parent);
        if (pit != entries_.end()) {
            pit->second.children.insert(wtxid);
        }
    }
    for (const TxInput& input : it->second.entry.tx.inputs) {
        spent_by_[input.outpoint] = wtxid;
    }
    by_txid_[it->second.entry.txid] = wtxid;
    total_weight_ += weight;
    total_fees_ += *fee;

    // Over the cap, so the pool sheds its cheapest packages — and this arrival is
    // judged by that same rule rather than exempted from it, which is what makes the
    // cap a cap. The earlier shape of this code trimmed to `MAX_MEMPOOL_WEIGHT` while
    // asking for room *beyond* it, so the trim was satisfied the moment the limit was
    // met and never freed the space the arrival needed: a full pool refused everything
    // from then on, permanently, however much the arrival paid.
    if (total_weight_ > MAX_MEMPOOL_WEIGHT) {
        (void)Trim(MAX_MEMPOOL_WEIGHT);
        if (!entries_.contains(wtxid)) {
            return std::unexpected(
                Rejection{.reason = RejectReason::PoolFull, .rule = std::nullopt});
        }
    }
    return Find(wtxid);
}

// --- Selection --------------------------------------------------------------------

std::vector<const Entry*> Mempool::GetTemplates(size_t available_weight) const {
    // Candidates ranked by descendant fee rate, which is the rate a miner actually
    // earns by taking a transaction *and* what depends on it.
    struct Candidate {
        Hash256 id;
        int64_t fee;
        size_t weight;
    };
    std::vector<Candidate> ranked;
    ranked.reserve(entries_.size());
    for (const auto& [id, unused_links] : entries_) {
        int64_t fee = 0;
        size_t weight = 0;
        DescendantTotals(id, fee, weight);
        ranked.push_back(Candidate{.id = id, .fee = fee, .weight = weight});
    }
    std::sort(ranked.begin(), ranked.end(), [](const Candidate& a, const Candidate& b) {
        const int cmp = CompareFeerate(a.fee, a.weight, b.fee, b.weight);
        // Ties by hash, so a template built twice from one pool is the same template.
        return cmp != 0 ? cmp > 0 : a.id < b.id;
    });

    std::vector<const Entry*> selected;
    std::unordered_set<Hash256> included;
    size_t used = 0;
    for (const Candidate& candidate : ranked) {
        if (included.contains(candidate.id)) {
            continue;
        }

        // The candidate together with every ancestor not already in the block. A
        // package and not an entry: a single pass that merely skipped a transaction
        // whose parents were absent would drop exactly the child-pays-for-parent
        // packages the ranking above exists to prefer, and would drop them silently.
        std::vector<Hash256> package;
        size_t package_weight = 0;
        for (const Hash256& id : AncestorsOf(candidate.id)) {
            if (included.contains(id)) {
                continue;
            }
            const auto it = entries_.find(id);
            if (it == entries_.end()) {
                continue;
            }
            package.push_back(id);
            package_weight += it->second.entry.weight;
        }
        if (used + package_weight > available_weight) {
            // Skipped whole, and the walk continues: a package too large for the room
            // left must not block the smaller ones ranked behind it.
            continue;
        }

        // Emitted parents-first, which makes the result a valid block order. The
        // package is at most `MAX_MEMPOOL_ANCESTORS + 1` entries, so the quadratic
        // walk is bounded by a constant this pool sets.
        while (!package.empty()) {
            bool progressed = false;
            for (size_t i = 0; i < package.size(); ++i) {
                const auto it = entries_.find(package[i]);
                if (it == entries_.end()) {
                    package.erase(package.begin() + static_cast<std::ptrdiff_t>(i));
                    progressed = true;
                    break;
                }
                const bool ready =
                    std::ranges::all_of(it->second.parents, [&included](const Hash256& parent) {
                        return included.contains(parent);
                    });
                if (!ready) {
                    continue;
                }
                selected.push_back(&it->second.entry);
                included.insert(package[i]);
                used += it->second.entry.weight;
                package.erase(package.begin() + static_cast<std::ptrdiff_t>(i));
                progressed = true;
                break;
            }
            if (!progressed) {
                // Unreachable while the links are a directed acyclic graph, which they
                // are: an input names a txid, and a transaction's txid commits to its
                // inputs, so a cycle would need a hash preimage. Broken out of rather
                // than looped on, because a template that never returned would stop
                // this node producing blocks at all.
                break;
            }
        }
    }
    return selected;
}

// --- Queries ----------------------------------------------------------------------

const Entry* Mempool::Find(const Hash256& wtxid) const noexcept {
    const auto it = entries_.find(wtxid);
    return it == entries_.end() ? nullptr : &it->second.entry;
}

const Entry* Mempool::FindByTxid(const Hash256& txid) const noexcept {
    const auto it = by_txid_.find(txid);
    return it == by_txid_.end() ? nullptr : Find(it->second);
}

const Entry* Mempool::SpenderOf(const OutPoint& outpoint) const noexcept {
    const auto it = spent_by_.find(outpoint);
    return it == spent_by_.end() ? nullptr : Find(it->second);
}

std::vector<Hash256> Mempool::Wtxids() const {
    std::vector<Hash256> out;
    out.reserve(entries_.size());
    for (const auto& [id, unused_links] : entries_) {
        out.push_back(id);
    }
    return out;
}

int64_t Mempool::LowestPackageFeerate() const {
    std::optional<int64_t> lowest;
    for (const auto& [id, unused_links] : entries_) {
        int64_t fee = 0;
        size_t weight = 0;
        DescendantTotals(id, fee, weight);
        const int64_t rate = FeerateFloor(fee, weight);
        if (!lowest.has_value() || rate < *lowest) {
            lowest = rate;
        }
    }
    return lowest.value_or(0);
}

}  // namespace amarian::mempool
