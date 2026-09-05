#include <amarian/chain/block_index.hpp>

#include <amarian/consensus/genesis.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace amarian::chain {

// --- Entries ----------------------------------------------------------------------

const BlockIndexEntry* BlockIndexEntry::Ancestor(uint32_t wanted_height) const noexcept {
    if (wanted_height > height) {
        return nullptr;
    }
    const BlockIndexEntry* walk = this;
    while (walk != nullptr && walk->height > wanted_height) {
        walk = walk->parent;
    }
    return walk;
}

// --- Descriptions -----------------------------------------------------------------

std::string_view Describe(IndexError error) noexcept {
    switch (error) {
        case IndexError::UnknownPredecessor:
            return "the header's predecessor is not known";
        case IndexError::PredecessorInvalid:
            return "the header builds on a branch that was rejected";
        case IndexError::AlreadyRuledOut:
            return "the block has already been ruled out";
    }
    // Unreachable: the switch is total, and -Wswitch-enum makes a new enumerator a build
    // failure here rather than a silent fall-through.
    return "unknown index error";
}

std::string_view Describe(const HeaderError& error) noexcept {
    return std::visit([](auto reason) { return Describe(reason); }, error);
}

// --- Selection --------------------------------------------------------------------

bool IsBetterTip(const BlockIndexEntry& candidate, const BlockIndexEntry& incumbent) noexcept {
    if (!candidate.IsEligible()) {
        return false;
    }
    if (!incumbent.IsEligible()) {
        return true;
    }
    if (candidate.total_work != incumbent.total_work) {
        return candidate.total_work > incumbent.total_work;
    }
    // Equal work: the one this node saw first keeps the tip. Sequence numbers are unique,
    // so this is a strict order and never reports two entries as better than each other.
    return candidate.sequence < incumbent.sequence;
}

// --- Context for a child ----------------------------------------------------------

int64_t MedianTimePastAt(const BlockIndexEntry& entry) {
    std::vector<int64_t> timestamps;
    timestamps.reserve(MEDIAN_TIME_SPAN);
    const BlockIndexEntry* walk = &entry;
    while (walk != nullptr && timestamps.size() < MEDIAN_TIME_SPAN) {
        timestamps.push_back(walk->header.timestamp);
        walk = walk->parent;
    }
    // Walking back collects newest first; `MedianTimePast` is specified newest last.
    std::ranges::reverse(timestamps);
    return consensus::MedianTimePast(timestamps);
}

uint32_t NextTargetBits(const BlockIndexEntry& parent, const ChainParams& params) noexcept {
    const uint32_t inherited = parent.header.target_bits;

    // Less work than the floor demands is the same statement as "easier than the floor",
    // and comparing work rather than the compact encodings avoids having to reason about
    // mantissa-and-exponent ordering. Unreachable through the index, since
    // `CheckBlockHeader` rejects such a header before it is stored — but this answer is
    // also what a miner will be handed, and a miner asking for a target should never be
    // able to receive an invalid one.
    if (Work::OfCompactTarget(inherited) < Work::OfCompactTarget(params.pow_limit_bits)) {
        return params.pow_limit_bits;
    }
    return inherited;
}

consensus::HeaderContext
HeaderContextFor(const BlockIndexEntry& parent, int64_t now, const ChainParams& params) {
    return consensus::HeaderContext{
        .prev_hash = parent.hash,
        .prev_height = parent.height,
        .median_time_past = MedianTimePastAt(parent),
        .expected_bits = NextTargetBits(parent, params),
        .now = now,
    };
}

// --- Planning a switch ------------------------------------------------------------

ChainSwitch PlanChainSwitch(const BlockIndexEntry* from, const BlockIndexEntry* to) {
    ChainSwitch plan;
    if (to == nullptr) {
        return plan;
    }

    const BlockIndexEntry* older = from;
    const BlockIndexEntry* newer = to;

    // Bring the deeper branch up to the other's height, recording what is walked past.
    while (older != nullptr && older->height > newer->height) {
        plan.disconnect.push_back(older);
        older = older->parent;
    }
    while (newer != nullptr && (older == nullptr || newer->height > older->height)) {
        plan.connect.push_back(newer);
        newer = newer->parent;
    }

    // Now at equal heights, so step both back together until they meet.
    while (older != nullptr && newer != nullptr && older != newer) {
        plan.disconnect.push_back(older);
        plan.connect.push_back(newer);
        older = older->parent;
        newer = newer->parent;
    }

    // Equal here means the fork point, and both being null means there was no chain to
    // fork from — the whole of `to`'s ancestry is to be connected.
    plan.fork_point = older == newer ? older : nullptr;

    // `disconnect` came out tip first, which is the order it must be applied in.
    // `connect` came out tip first too, which is the opposite of what it needs.
    std::ranges::reverse(plan.connect);
    return plan;
}

// --- The index --------------------------------------------------------------------

BlockIndex BlockIndex::ForNetwork(const ChainParams& params) {
    return BlockIndex{BuildGenesisBlock(params).header};
}

BlockIndex::BlockIndex(const BlockHeader& genesis_header) {
    auto entry = std::make_unique<BlockIndexEntry>();
    entry->header = genesis_header;
    entry->hash = genesis_header.Hash();
    entry->height = genesis_header.height;
    entry->total_work = Work::OfCompactTarget(genesis_header.target_bits);
    entry->parent = nullptr;

    // Fully valid by definition rather than by validation. Genesis is a chain parameter,
    // checked against its recorded hash by `consensus::CheckGenesis` before a node starts,
    // and its single output is unspendable and therefore never stored — so applying it to
    // an unspent output set is a no-op and there is no weaker state for it to sit in.
    entry->validity = BlockValidity::Full;
    entry->sequence = next_sequence_++;

    BlockIndexEntry* raw = entry.get();
    entries_.emplace(raw->hash, std::move(entry));
    genesis_ = raw;
    best_header_ = raw;
}

std::expected<const BlockIndexEntry*, HeaderError>
BlockIndex::AddHeader(const BlockHeader& header, int64_t now, const ChainParams& params) {
    const Hash256 hash = header.Hash();

    // Idempotent: a peer sending a header twice is not misbehaving.
    if (const BlockIndexEntry* existing = Find(hash); existing != nullptr) {
        return existing;
    }

    // Cheapest first, and cheapest of all is the work the header claims: one hash and one
    // comparison, no lookup, nothing allocated. A flood of fabricated headers stops here.
    if (const consensus::Verdict verdict = consensus::CheckBlockHeader(header, params);
        !verdict.has_value()) {
        return std::unexpected(HeaderError{verdict.error()});
    }

    const BlockIndexEntry* parent = Find(header.prev_block);
    if (parent == nullptr) {
        return std::unexpected(HeaderError{IndexError::UnknownPredecessor});
    }
    if (!parent->IsEligible()) {
        return std::unexpected(HeaderError{IndexError::PredecessorInvalid});
    }

    const consensus::HeaderContext context = HeaderContextFor(*parent, now, params);
    if (const consensus::Verdict verdict =
            consensus::ContextualCheckBlockHeader(header, context, params);
        !verdict.has_value()) {
        // Deliberately not remembered. `HeaderTimestampTooFarAhead` is a verdict that
        // expires: the same header becomes acceptable as this node's clock advances, and a
        // node that recorded the rejection would refuse forever a block the network
        // accepted.
        return std::unexpected(HeaderError{verdict.error()});
    }

    auto entry = std::make_unique<BlockIndexEntry>();
    entry->header = header;
    entry->hash = hash;
    entry->height = header.height;
    // The encoding was accepted by `CheckBlockHeader`, so this is never the zero that an
    // unusable target would yield.
    entry->total_work = parent->total_work + Work::OfCompactTarget(header.target_bits);
    entry->parent = parent;
    entry->validity = BlockValidity::Header;
    entry->sequence = next_sequence_++;

    BlockIndexEntry* raw = entry.get();
    entries_.emplace(hash, std::move(entry));
    children_.emplace(header.prev_block, raw);

    // Adding an entry can only raise the best tip, never lower it, so one comparison is
    // the whole update. A new entry also has the highest sequence number of any entry, so
    // it never wins a tie: first seen is preserved without a special case.
    if (best_header_ == nullptr || IsBetterTip(*raw, *best_header_)) {
        best_header_ = raw;
    }
    return raw;
}

const BlockIndexEntry* BlockIndex::Find(const Hash256& hash) const noexcept {
    const auto found = entries_.find(hash);
    return found == entries_.end() ? nullptr : found->second.get();
}

BlockIndexEntry* BlockIndex::FindMutable(const Hash256& hash) noexcept {
    const auto found = entries_.find(hash);
    return found == entries_.end() ? nullptr : found->second.get();
}

const BlockIndexEntry& BlockIndex::Genesis() const noexcept {
    return *genesis_;
}

const BlockIndexEntry* BlockIndex::BestHeader() const noexcept {
    return best_header_;
}

std::vector<const BlockIndexEntry*> BlockIndex::ChildrenOf(const BlockIndexEntry& entry) const {
    std::vector<const BlockIndexEntry*> children;
    const auto range = children_.equal_range(entry.hash);
    for (auto child = range.first; child != range.second; ++child) {
        children.push_back(child->second);
    }
    return children;
}

void BlockIndex::RecordValidity(const BlockIndexEntry& entry, BlockValidity reached) noexcept {
    BlockIndexEntry* stored = FindMutable(entry.hash);
    if (stored == nullptr) {
        return;
    }
    // `BlockValidity` is declared in ascending order precisely so that this comparison is
    // the whole of "never lower a recorded level".
    if (reached > stored->validity) {
        stored->validity = reached;
    }
}

size_t BlockIndex::RecordFailure(const BlockIndexEntry& entry) {
    BlockIndexEntry* root = FindMutable(entry.hash);
    if (root == nullptr || root->failure != BlockFailure::None) {
        return 0;
    }
    root->failure = BlockFailure::Itself;
    size_t marked = 1;

    // Walk the subtree once, marking everything below as ruled out by its ancestry. Doing
    // it now is what lets `BestHeader` decide eligibility by reading one field.
    std::vector<const BlockIndexEntry*> frontier{root};
    while (!frontier.empty()) {
        const BlockIndexEntry* current = frontier.back();
        frontier.pop_back();
        const auto range = children_.equal_range(current->hash);
        for (auto child = range.first; child != range.second; ++child) {
            BlockIndexEntry* descendant = child->second;
            if (descendant->failure != BlockFailure::None) {
                continue;
            }
            descendant->failure = BlockFailure::Ancestor;
            ++marked;
            frontier.push_back(descendant);
        }
    }

    // Marking can only remove candidates, so the incumbent is still best unless it was one
    // of the ones removed.
    if (best_header_ == nullptr || !best_header_->IsEligible()) {
        RescanForBestHeader();
    }
    return marked;
}

void BlockIndex::RescanForBestHeader() noexcept {
    best_header_ = nullptr;
    for (const auto& element : entries_) {
        const BlockIndexEntry* candidate = element.second.get();
        if (!candidate->IsEligible()) {
            continue;
        }
        if (best_header_ == nullptr || IsBetterTip(*candidate, *best_header_)) {
            best_header_ = candidate;
        }
    }
}

}  // namespace amarian::chain
