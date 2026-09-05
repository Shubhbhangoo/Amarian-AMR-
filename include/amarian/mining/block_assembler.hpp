#pragma once

/// \file
/// Building a block that this node would itself accept, and finding the nonce that
/// makes it valid.
///
/// Every other layer answers *is this block allowed*. This is the one layer that answers
/// *what block should exist next*, and the two are deliberately not the same code: an
/// assembler that shared a line with the validator could produce a block that passes
/// because both agree on a mistake. So this file computes the next block from the chain
/// and the parameters, and then hands it to `consensus::CheckBlock` and
/// `chain::ChainState::AcceptBlock` to be judged like any block that arrived from a
/// stranger. A template that this node's own rules refuse is a bug reported here rather
/// than a block the network rejects later.
///
/// ## What a miner does not get to choose
///
/// The height, the previous hash, the target, and the reward. All four are computed from
/// the tip: `chain::NextTargetBits` decides the difficulty, `BlockReward` decides the
/// issuance, and the header's own claims about them are checked against the node's
/// computation by `consensus::ContextualCheckBlockHeader` and
/// `consensus::CheckCoinbaseAmount`. Assembling them here rather than accepting them
/// from a caller is what makes those checks tautologies for an honest miner and refusals
/// for a dishonest one.
///
/// The timestamp is chosen, within bounds: strictly after the median of the last eleven
/// blocks, and no further ahead of the clock than `MAX_FUTURE_BLOCK_SECONDS`. The payout
/// lock is chosen freely, because who receives the reward is not a consensus question.
///
/// ## Fee selection
///
/// A template draws its non-coinbase transactions from a `mempool::Mempool`, passed in by
/// the caller and never held here. The mining layer keeps no mutable state of its own: the
/// pool arrives as an argument, so two callers may build templates from two different pools
/// — a node and a test in the same process — and neither can be surprised by the other. A
/// null pool means a coinbase-only template, which is what a node that has not accepted a
/// transaction yet produces.
///
/// The fee total is computed from the entries the pool hands back and paid out in the
/// coinbase, and the same total is what `consensus::CheckCoinbaseAmount` is handed below.
/// It is never assumed on either side.
///
/// ## What is not here yet
///
/// A mining RPC, which is Phase 3's remaining work and needs a long-poll and an extranonce
/// protocol rather than a function call.
///
/// The nonce search below is a single-threaded loop over `header.nonce`, which is the
/// honest shape of CPU mining for development and regtest, where a block costs a couple
/// of hash attempts by design. It is not a competitive miner and does not pretend to be:
/// there is no thread pool and no work splitting.

#include <amarian/chain/block_index.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/consensus/target.hpp>
#include <amarian/primitives/block.hpp>
#include <amarian/primitives/lock.hpp>
#include <amarian/util/types.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace amarian::mempool {
/// Forward-declared rather than included: the assembler holds a pointer to a pool and
/// calls it only in the implementation, so a caller that builds coinbase-only templates
/// does not acquire the mempool's headers to do it.
class Mempool;
}  // namespace amarian::mempool

namespace amarian::mining {

/// A block ready to be mined, and the two facts a solver would otherwise recompute.
struct BlockTemplate {
    /// The assembled block, with every field but the nonce final. Solving it changes
    /// `block.header.nonce` and nothing else.
    Block block;

    /// The decoded form of `block.header.target_bits`, so that a solver does not decode
    /// the same compact integer once per attempt.
    Target target{};

    /// What the coinbase claims: the scheduled reward for this height, plus the fees of
    /// the transactions included. The second term is zero for a coinbase-only template,
    /// and is a term rather than an omission so that the reward a miner is paid and the
    /// reward the rules allow are computed from one expression rather than two.
    int64_t reward = 0;

    /// The fee total of the non-coinbase transactions in `block`, which is the second
    /// term of `reward`.
    ///
    /// Reported rather than left to be rederived: a caller that wanted it would have to
    /// look every input up in the UTXO set again, and `consensus::CheckCoinbaseAmount`
    /// needs exactly this number to judge the coinbase.
    int64_t fees = 0;
};

/// Why a template could not be built.
///
/// None of these is a statement about a block that arrived — there is no such block yet.
/// They are statements about the caller's request, this node's own chain, or this build's
/// own parameters, which is why they are separate from every validation vocabulary in the
/// project.
enum class TemplateError : uint8_t {
    /// `coinbase_data` is longer than `params.block_limits.tx.max_coinbase_data_size`.
    /// The one error here a caller causes, and refused rather than truncated: a miner
    /// whose bytes were silently cut would be mining a block it did not ask for, and
    /// `consensus::CheckTransaction` would reject the untruncated version anyway.
    CoinbaseDataTooLarge,

    /// The tip is at the highest height a header can express. Unreachable on any real
    /// chain — 2^32 blocks at five minutes each is forty thousand years — and checked
    /// because the alternative is a silent wrap to height zero.
    HeightOverflow,

    /// `chain::NextTargetBits` produced a compact target that does not decode. A fault
    /// in this build's chain parameters, not in anything received.
    MalformedTarget,

    /// The assembled coinbase produced no Merkle root, which would mean a block with no
    /// transactions in it. Unreachable: the coinbase is added before the root is asked
    /// for.
    NoMerkleRoot,

    /// The assembled header claims a height, a predecessor, a target or a timestamp that
    /// `consensus::ContextualCheckBlockHeader` refuses.
    HeaderRefused,

    /// The assembled block claims a reward that `consensus::CheckCoinbaseAmount` refuses.
    ///
    /// This and `HeaderRefused` mean the same thing and it is always a bug in this file:
    /// every field they judge was computed here from the same functions the rules use, so
    /// a disagreement means the assembler and the rules have diverged. The one thing not
    /// to do about that is mine the block anyway.
    RewardRefused,
};

[[nodiscard]] std::string_view Describe(TemplateError error) noexcept;

/// Builds the block that would extend `tip`, paying its reward to `payout`.
///
/// `now` is the node's wall clock in seconds. The header's timestamp is `now` raised, if
/// necessary, to one second past the median of the eleven blocks ending at `tip` — a tip
/// whose timestamps run ahead of this machine's clock would otherwise produce a block
/// this node itself would reject as too old. It is not lowered: a clock far ahead of the
/// network produces a block peers will refuse until it is not, which is a fault to fix
/// on this machine rather than one to paper over here.
///
/// `coinbase_data` is the miner's arbitrary bytes, bounded by
/// `params.block_limits.tx.max_coinbase_data_size`. It sits outside the txid and inside
/// the wtxid, so changing it changes the Merkle root without changing what the coinbase
/// pays — which is what makes it the escape hatch when a nonce range is exhausted, and
/// what makes two miners with the same payout produce different work.
///
/// `pool` is where the template's non-coinbase transactions come from, or null for a
/// coinbase-only template. It is read and not modified: a template is a proposal, and a
/// transaction stays in the pool until a block containing it is actually connected. The
/// pool must outlive the call, and must not be modified during it.
///
/// The returned block is unsolved: its nonce is zero and its hash almost certainly does
/// not meet its target. `SolveHeader` is the other half.
[[nodiscard]] std::expected<BlockTemplate, TemplateError>
BuildBlockTemplate(const chain::BlockIndexEntry& tip, const Lock& payout, int64_t now,
                   ByteVec coinbase_data, const ChainParams& params,
                   const mempool::Mempool* pool = nullptr);

/// Searches at most `attempts` nonces, starting from `header.nonce`, for one whose hash
/// meets `target`. Leaves the solving nonce in `header` and returns true, or leaves the
/// first nonce it did not try and returns false.
///
/// Bounded rather than unbounded so that a caller stays in control: a node that must
/// answer a shutdown signal, or notice that a peer has already published this height,
/// cannot do either from inside a loop that does not return. `attempts` is a budget and
/// not a range — because the nonce left behind is the first untried one, n calls of one
/// attempt search exactly what one call of n attempts searches.
///
/// Only the nonce moves. It is the last field in the header's layout precisely so that
/// the first eighty-four bytes are constant across a search — the same reason
/// `FindGenesisNonce` is shaped this way.
[[nodiscard]] bool SolveHeader(BlockHeader& header, const Target& target,
                              uint64_t attempts) noexcept;

}  // namespace amarian::mining
