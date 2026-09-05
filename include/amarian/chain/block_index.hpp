#pragma once

/// \file
/// The tree of known headers, and the rule that picks a tip out of it.
///
/// Every rule about whether a block *may* be valid now exists. This is the piece that
/// decides which one the node actually follows, and it is a different kind of thing: a
/// rule is a pure function of a block, while chain selection is a fact about a set of
/// blocks that arrived over time. The index is where that set lives.
///
/// ## What an index holds
///
/// Headers, not blocks. A header is 92 bytes and is the whole of what selection needs —
/// its predecessor, its height, and the work it claims. Bodies are far larger, arrive
/// later, and are validated separately; keeping them out means a node can learn the
/// shape of the chain, and how much work each branch has, before deciding which bodies
/// are worth asking for.
///
/// Each entry also records how far it has been validated, because that is not a property
/// of the header. A header can be sound while its block is a forgery, and a block can be
/// internally consistent while spending a coin that does not exist. `BlockValidity` is
/// the ladder those answers arrive on.
///
/// ## The selection rule, stated plainly
///
/// The best tip is the entry with the most accumulated work, and among equal work the
/// one this node learned of first. Both halves matter.
///
/// Most work rather than most blocks: a branch can be long and easy, and counting blocks
/// would let an attacker mine many trivial ones. Work is the cost actually paid.
///
/// First seen rather than, say, the lowest hash. A deterministic tie-break sounds
/// better — every node would agree instantly instead of disagreeing until the next
/// block — but it makes withholding free. A miner who finds a block with a low hash
/// could sit on it and publish later to displace whoever won the height, at no cost,
/// which turns a one-confirmation payment into something an attacker can reverse for
/// free. First seen makes withholding lose, which is the property worth having; the
/// disagreement it permits is resolved by the next block either way.
///
/// First seen is recorded explicitly, as a sequence number assigned when the index first
/// learns of a header, rather than read from a clock. The index is therefore a pure
/// function of the order headers were offered to it, which is what makes selection
/// reproducible in a test.

#include <amarian/consensus/params.hpp>
#include <amarian/consensus/validation.hpp>
#include <amarian/consensus/work.hpp>
#include <amarian/primitives/block.hpp>
#include <amarian/util/types.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace amarian::chain {

/// How far a block has been validated. A ladder, not a set of flags: each level implies
/// the one below it, and a block only ever moves up.
enum class BlockValidity : uint8_t {
    /// The header's own rules pass and its predecessor is indexed. Enough to weigh the
    /// branch's work; not enough to believe anything about its contents.
    Header,
    /// `CheckBlock` passes: the body is present and internally consistent. Still says
    /// nothing about whether the coins it spends exist.
    Body,
    /// Connected to the unspent output set at its height, with every contextual rule
    /// satisfied. This is what "valid" means without qualification.
    Full,
};

/// Whether a block can ever be connected.
///
/// Separate from `BlockValidity` because failure is not a rung on that ladder — it is
/// the absence of the whole ladder, and it is inherited downwards in a way validity is
/// not. Keeping them apart means "how far did this get" and "is this ruled out" are two
/// questions with two answers, and no enumerator has to mean both.
enum class BlockFailure : uint8_t {
    None,
    /// A rule rejected this block itself.
    Itself,
    /// An ancestor was rejected. Nothing is wrong with this header, and it may even be
    /// a header the node would otherwise like; it simply builds on a branch that can
    /// never be connected.
    Ancestor,
};

/// One known block: where it sits, what it cost, and what is known about it.
///
/// Non-copyable by way of its owner: entries are allocated once and referred to by
/// pointer for their whole lifetime, because a parent link that could dangle would be a
/// bug in the structure that decides which chain is real. Nothing outside `BlockIndex`
/// can obtain a mutable one.
struct BlockIndexEntry {
    BlockHeader header;

    /// The header's hash, computed once when the entry is created. Also the key this
    /// entry is stored under.
    Hash256 hash;

    /// Copied out of the header, and checked against the predecessor's before the entry
    /// was created, so it is `parent->height + 1` for every entry but genesis.
    uint32_t height = 0;

    /// The work of this block and every ancestor, genesis included.
    Work total_work;

    /// Null for genesis and for nothing else.
    const BlockIndexEntry* parent = nullptr;

    BlockValidity validity = BlockValidity::Header;
    BlockFailure failure = BlockFailure::None;

    /// The order in which this index first learned of this header, counting from zero at
    /// genesis. The first-seen half of the selection rule, and the reason it is
    /// reproducible: it comes from the order of `AddHeader` calls and not from a clock.
    uint64_t sequence = 0;

    /// Whether this block could still become the tip. False once it or an ancestor has
    /// been rejected.
    [[nodiscard]] bool IsEligible() const noexcept { return failure == BlockFailure::None; }

    /// The ancestor at `wanted_height`, this entry itself when that is its own height,
    /// or null when `wanted_height` is above it.
    ///
    /// Walks parent links, so the cost is the number of blocks between the two heights.
    /// That is what every current caller wants — median time past and, later,
    /// retargeting, both look a fixed short distance back from a tip. A caller that
    /// needs an arbitrary ancestor of a long chain in a hot path will want a skip list,
    /// and this is the function it should replace.
    [[nodiscard]] const BlockIndexEntry* Ancestor(uint32_t wanted_height) const noexcept;
};

// --- Why a header was not taken ---------------------------------------------

/// A reason the index could not place a header that is not a rule violation.
enum class IndexError : uint8_t {
    /// Its `prev_block` is not indexed. Headers arrive out of order routinely, so this
    /// is an ordinary event and not evidence of anything: the header is not remembered,
    /// and offering it again once its predecessor is known will succeed.
    UnknownPredecessor,
    /// An ancestor was rejected, so no descendant of it can ever be connected.
    PredecessorInvalid,
};

/// Why a header was not added: either the consensus rule that rejected it, or an
/// index-level reason that is not a rule violation at all.
///
/// A sum type rather than one flat enumeration because the two halves call for opposite
/// reactions. A `ValidationError` means the header can never be valid on this network
/// and whoever sent it is broken or hostile. An `IndexError` means this node is not yet
/// in a position to judge, or has already judged an ancestor — neither of which is
/// something to hold against the sender. Flattening them would make it easy to write a
/// peer-scoring rule that bans a peer for being early.
using HeaderError = std::variant<consensus::ValidationError, IndexError>;

/// A short stable description, for logs and RPC. Never parsed by anything.
[[nodiscard]] std::string_view Describe(IndexError error) noexcept;

/// The description of whichever half `error` holds.
[[nodiscard]] std::string_view Describe(const HeaderError& error) noexcept;

// --- The selection rule -----------------------------------------------------

/// Whether `candidate` is a better chain tip than `incumbent`: more accumulated work,
/// or equal work and learned of earlier.
///
/// An ineligible candidate is never better, and an eligible one is always better than an
/// ineligible incumbent, so a rejected branch drops out of consideration without the
/// caller having to filter for it. A strict order: `IsBetterTip(a, a)` is false, and no
/// two distinct entries are each better than the other, because sequence numbers are
/// unique.
[[nodiscard]] bool IsBetterTip(const BlockIndexEntry& candidate,
                               const BlockIndexEntry& incumbent) noexcept;

// --- What a header must be checked against ----------------------------------

/// The median timestamp of the up-to-`MEDIAN_TIME_SPAN` blocks ending at `entry`, which
/// a child's timestamp must be strictly greater than.
///
/// Fewer than `MEDIAN_TIME_SPAN` blocks near genesis is not a special case: the median of
/// what exists is the answer, and for genesis itself that is its own timestamp.
[[nodiscard]] int64_t MedianTimePastAt(const BlockIndexEntry& entry);

/// The `target_bits` a child of `parent` must carry.
///
/// **Amarian has constant difficulty today.** A child inherits its predecessor's target,
/// clamped so that it is never easier than the network's floor. This is a complete rule
/// rather than a stub — it is exactly what a network with no retargeting algorithm does,
/// and it is what regtest, whose blocks cost a couple of hash attempts by design, will
/// always do — but it is not the final one. Choosing a retargeting algorithm is Phase 3
/// and needs the analysis the project's own rules demand before a consensus constant is
/// picked.
///
/// What matters now is the *shape*: the expected target is computed by the node from the
/// chain, and the header's claim is checked against it by `ContextualCheckBlockHeader`. A
/// miner does not get to choose the difficulty they mined at. Replacing the body of this
/// function later changes no caller.
[[nodiscard]] uint32_t NextTargetBits(const BlockIndexEntry& parent,
                                      const ChainParams& params) noexcept;

/// Everything `consensus::ContextualCheckBlockHeader` needs in order to judge a child of
/// `parent`, gathered from the index.
///
/// This is the boundary `validation.hpp` describes: the index produces the facts, and
/// deciding validity from them is a pure function elsewhere. `now` is passed in rather
/// than read from a clock here, for the same reason.
[[nodiscard]] consensus::HeaderContext
HeaderContextFor(const BlockIndexEntry& parent, int64_t now, const ChainParams& params);

// --- Moving from one tip to another -----------------------------------------

/// The work of changing the active chain's tip: what to revert, and what to apply.
///
/// A plan rather than an action, so that the walk over the tree and the mutation of the
/// unspent output set are separate steps. The plan can be inspected, logged, and bounded
/// before anything is written.
struct ChainSwitch {
    /// The deepest block both branches share, and so the state they agree on. Null only
    /// when there was no chain to switch from.
    const BlockIndexEntry* fork_point = nullptr;

    /// The blocks to revert, tip first. Empty when the new tip is a descendant of the
    /// old one, which is the ordinary case of the chain simply advancing.
    std::vector<const BlockIndexEntry*> disconnect;

    /// The blocks to apply, lowest height first.
    std::vector<const BlockIndexEntry*> connect;
};

/// Plans the move from `from` to `to`.
///
/// `from` may be null, meaning there is no active chain yet: the plan is then to connect
/// everything from genesis up to `to`, which is what a node does on first start. `to`
/// must not be null — switching to no chain at all is not an operation — and a null `to`
/// yields an empty plan.
///
/// Both entries must belong to the same index. The walk brings the deeper of the two up
/// to the other's height and then steps both back together, so its cost is the depth of
/// the fork and not the length of the chain.
[[nodiscard]] ChainSwitch PlanChainSwitch(const BlockIndexEntry* from, const BlockIndexEntry* to);

// --- The index --------------------------------------------------------------

/// Every header this node knows of, and the best tip among them.
///
/// Owns its entries. They are allocated individually and never moved, so a `parent`
/// pointer taken today is valid for as long as the index lives — including across
/// rehashes and across a move of the index itself. Holding them by value in the map
/// would happen to work, because a node-based container keeps element addresses stable,
/// but a structure whose correctness rests on stable cross-references should not be one
/// refactor away from dangling.
///
/// Nothing outside this class can obtain a mutable entry: every accessor returns a
/// `const` pointer or reference, and the two functions that change an entry's recorded
/// state are members that look it up again by hash. So the invariants below hold by
/// construction rather than by convention.
///
///  - Every entry but genesis has a non-null `parent` that is also in this index.
///  - `height == parent->height + 1`, and `total_work == parent->total_work + work(bits)`.
///  - `sequence` is unique, and increases in the order headers were offered.
///  - No eligible entry has an ineligible ancestor.
class BlockIndex {
public:
    /// An index containing only `params`' genesis block.
    ///
    /// Genesis is rebuilt from the parameters by `BuildGenesisBlock` rather than accepted
    /// from a caller, so an index cannot be started on a block that is not the network's.
    /// Whether the parameters themselves are sound is `consensus::CheckGenesis`'s
    /// question, and a node answers it at startup before reaching this.
    [[nodiscard]] static BlockIndex ForNetwork(const ChainParams& params);

    BlockIndex(const BlockIndex&) = delete;
    BlockIndex& operator=(const BlockIndex&) = delete;
    BlockIndex(BlockIndex&&) = default;
    BlockIndex& operator=(BlockIndex&&) = default;
    ~BlockIndex() = default;

    /// Validates `header` against the index and adds it, returning its entry.
    ///
    /// The order is the cheap-first order `validation.hpp` specifies. `CheckBlockHeader`
    /// runs first, because one hash and one comparison is the cheapest gate there is
    /// against fabricated headers and it needs no lookup at all. Only then is the
    /// predecessor looked up, and only then are the contextual rules applied.
    ///
    /// Offering a header the index already holds returns the existing entry rather than
    /// an error. A peer sending the same header twice is not misbehaving, and making the
    /// call idempotent means a caller does not have to check first.
    ///
    /// A rejected header is not remembered. That is not an optimisation — it is required
    /// by one of the rules: a header more than `MAX_FUTURE_BLOCK_SECONDS` ahead of this
    /// node's clock is invalid now and valid later, so remembering the rejection would
    /// permanently refuse a block the rest of the network accepts.
    [[nodiscard]] std::expected<const BlockIndexEntry*, HeaderError>
    AddHeader(const BlockHeader& header, int64_t now, const ChainParams& params);

    /// The entry for `hash`, or null when it is not indexed.
    [[nodiscard]] const BlockIndexEntry* Find(const Hash256& hash) const noexcept;

    [[nodiscard]] const BlockIndexEntry& Genesis() const noexcept;

    /// The eligible entry with the most accumulated work, ties going to the one learned
    /// of first.
    ///
    /// Null only if genesis itself has been marked failed, which would mean the build's
    /// own parameters describe an invalid block — a condition `consensus::CheckGenesis`
    /// exists to stop the node on, long before this could be observed.
    [[nodiscard]] const BlockIndexEntry* BestHeader() const noexcept;

    [[nodiscard]] size_t Size() const noexcept { return entries_.size(); }

    /// The indexed headers whose predecessor is `entry`, in no particular order.
    ///
    /// For a caller that has just connected a block and wants to know what could be
    /// connected next.
    [[nodiscard]] std::vector<const BlockIndexEntry*>
    ChildrenOf(const BlockIndexEntry& entry) const;

    /// Records that `entry` has been validated as far as `reached`.
    ///
    /// Never lowers a recorded level, so a caller re-reporting an earlier stage — as one
    /// will, when a block is re-checked after a reorganisation — cannot undo progress.
    void RecordValidity(const BlockIndexEntry& entry, BlockValidity reached) noexcept;

    /// Records that a rule rejected `entry`, and that no descendant of it can ever be
    /// connected. Returns how many entries were newly marked, `entry` included.
    ///
    /// Descendants are marked immediately rather than discovered later, so that
    /// `BestHeader` never has to walk an ancestry to find out whether a candidate is
    /// ruled out. That walk is the difference between selection costing a comparison and
    /// costing the depth of the chain.
    size_t RecordFailure(const BlockIndexEntry& entry);

private:
    explicit BlockIndex(const BlockHeader& genesis_header);

    /// The mutable entry for `hash`, or null. The only way anything mutates an entry.
    [[nodiscard]] BlockIndexEntry* FindMutable(const Hash256& hash) noexcept;

    /// Recomputes `best_header_` from scratch. Needed only after a failure is recorded,
    /// since adding an entry can raise the best but never lower it.
    void RescanForBestHeader() noexcept;

    std::unordered_map<Hash256, std::unique_ptr<BlockIndexEntry>> entries_;
    std::unordered_multimap<Hash256, BlockIndexEntry*> children_;
    const BlockIndexEntry* genesis_ = nullptr;
    const BlockIndexEntry* best_header_ = nullptr;
    uint64_t next_sequence_ = 0;
};

}  // namespace amarian::chain
