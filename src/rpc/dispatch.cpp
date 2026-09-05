#include <amarian/rpc/dispatch.hpp>

#include <amarian/chain/block_index.hpp>
#include <amarian/consensus/issuance.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/consensus/target.hpp>
#include <amarian/consensus/validation.hpp>
#include <amarian/consensus/work.hpp>
#include <amarian/mempool.hpp>
#include <amarian/mining/block_assembler.hpp>
#include <amarian/primitives/amount.hpp>
#include <amarian/primitives/block.hpp>
#include <amarian/primitives/lock.hpp>
#include <amarian/primitives/transaction.hpp>
#include <amarian/util/hex.hpp>
#include <amarian/util/serialize.hpp>
#include <amarian/util/types.hpp>
#include <amarian/utxo/coins.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace amarian::rpc {

std::string_view Describe(RpcError error) noexcept {
    switch (error) {
        case RpcError::UnknownMethod:
            return "no such method";
        case RpcError::InvalidParams:
            return "the arguments are not of the shape this method takes";
        case RpcError::MalformedHex:
            return "an argument was not hex of the required length";
        case RpcError::MalformedBlock:
            return "the submitted bytes are not an encoded block";
        case RpcError::MalformedTransaction:
            return "the submitted bytes are not an encoded transaction";
        case RpcError::NoPayout:
            return "no payout lock was given and this node was started without one";
        case RpcError::NoMempool:
            return "this node was started without a transaction pool";
        case RpcError::TemplateFailed:
            return "this node could not assemble a block it would accept";
        case RpcError::MiningNotPermitted:
            return "this network's proof of work is real, so mine with getblocktemplate";
        case RpcError::NotSolved:
            return "the nonce budget ran out before a solution was found";
        case RpcError::SelfRejected:
            return "this node's own rules refused a block this node built";
        case RpcError::ChainFault:
            return "this node's storage contradicts its own chain";
    }
    // Unreachable: the switch is total, and -Wswitch-enum makes a new enumerator a build
    // failure here rather than a silent fall-through.
    return "unknown RPC error";
}

int32_t Code(RpcError error) noexcept {
    // The numbers a Bitcoin-compatible client already handles, so that miner tooling does
    // not need an Amarian-specific error table: -32601 method not found, -8 invalid
    // parameter, -22 deserialisation, -5 bad key, -25 verify, -20 database, -1 misc.
    switch (error) {
        case RpcError::UnknownMethod:
            return -32601;
        case RpcError::InvalidParams:
        case RpcError::MalformedHex:
            return -8;
        case RpcError::MalformedBlock:
        case RpcError::MalformedTransaction:
            return -22;
        case RpcError::NoPayout:
            return -5;
        case RpcError::NoMempool:
        case RpcError::MiningNotPermitted:
        case RpcError::NotSolved:
            return -1;
        case RpcError::TemplateFailed:
        case RpcError::SelfRejected:
            return -25;
        case RpcError::ChainFault:
            return -20;
    }
    return -1;
}

namespace {

using json = nlohmann::json;

/// The argument at `index` positionally, or under `name` by keyword, or null when it was
/// not supplied.
///
/// Both forms for every method, and a JSON `null` counted as absent so that a client
/// filling a positional array up to the argument it cares about does not have to invent
/// values for the ones before it.
///
/// The object lookup compares keys rather than building a `std::string` to search with:
/// a caller that repeats a request with an unknown field should not make the node
/// allocate, which is the same reason `Error::detail` is a `string_view`.
[[nodiscard]] const json* Arg(const json& params, size_t index, std::string_view name) {
    const json* found = nullptr;
    if (params.is_array()) {
        if (index < params.size()) {
            found = &params[index];
        }
    } else if (params.is_object()) {
        for (auto it = params.begin(); it != params.end(); ++it) {
            if (it.key() == name) {
                found = &it.value();
                break;
            }
        }
    }
    return (found != nullptr && !found->is_null()) ? found : nullptr;
}

[[nodiscard]] std::expected<std::string_view, Error> AsString(const json& value) {
    if (!value.is_string()) {
        return std::unexpected(Error{.code = RpcError::InvalidParams,
                                     .detail = "expected a string"});
    }
    return std::string_view{value.get_ref<const std::string&>()};
}

[[nodiscard]] std::expected<uint64_t, Error> AsUint(const json& value) {
    // `is_number_unsigned` and not `is_number_integer`: a negative height or a negative
    // block count is a malformed request, not a value to clamp.
    if (!value.is_number_unsigned()) {
        return std::unexpected(Error{.code = RpcError::InvalidParams,
                                     .detail = "expected a non-negative integer"});
    }
    return value.get<uint64_t>();
}

[[nodiscard]] std::expected<ByteVec, Error> AsBytes(const json& value) {
    const std::expected<std::string_view, Error> text = AsString(value);
    if (!text.has_value()) {
        return std::unexpected(text.error());
    }
    std::optional<ByteVec> bytes = FromHex(*text);
    if (!bytes.has_value()) {
        return std::unexpected(Error{.code = RpcError::MalformedHex,
                                     .detail = "expected an even number of hex digits"});
    }
    return std::move(*bytes);
}

/// A lock written as `{"version": n, "program": "<hex>"}`.
///
/// An object and not a string, because Amarian has no address encoding until Phase 5 and
/// inventing one here would mean inventing a checksum, a human-readable prefix and a
/// version byte that the wallet then has to live with. A caller naming the two fields it
/// is actually choosing cannot be silently wrong about either.
[[nodiscard]] std::expected<Lock, Error> ParseLock(const json& value, const ChainParams& params) {
    if (!value.is_object()) {
        return std::unexpected(Error{.code = RpcError::InvalidParams,
                                     .detail = "a lock is {version, program}"});
    }
    const json* version = Arg(value, 0, "version");
    const json* program = Arg(value, 1, "program");
    if (version == nullptr || program == nullptr) {
        return std::unexpected(Error{.code = RpcError::InvalidParams,
                                     .detail = "a lock needs both version and program"});
    }
    const std::expected<uint64_t, Error> raw = AsUint(*version);
    if (!raw.has_value()) {
        return std::unexpected(raw.error());
    }
    if (*raw > 0xFFU) {
        return std::unexpected(Error{.code = RpcError::InvalidParams,
                                     .detail = "a lock version is one byte"});
    }
    // Refused rather than honoured. Version 0 is the permanent burn, so a template paying
    // it would mine the whole reward into an output no witness can ever satisfy — a mistake
    // a miner would discover only after the coins were gone.
    if (*raw == LOCK_VERSION_UNSPENDABLE) {
        return std::unexpected(Error{.code = RpcError::InvalidParams,
                                     .detail = "lock version 0 can never be spent"});
    }
    std::expected<ByteVec, Error> bytes = AsBytes(*program);
    if (!bytes.has_value()) {
        return std::unexpected(bytes.error());
    }
    if (bytes->size() > params.block_limits.tx.max_lock_program_size) {
        return std::unexpected(Error{.code = RpcError::InvalidParams,
                                     .detail = "the lock program is longer than a block may carry"});
    }
    return Lock{.version = static_cast<uint8_t>(*raw), .program = std::move(*bytes)};
}

/// The payout the request named, or the node's default, or a refusal.
///
/// Never a lock this file invented. A node that defaulted the payout would mine coins to
/// an output nobody holds a witness for, which is indistinguishable from burning them.
[[nodiscard]] std::expected<Lock, Error> ResolvePayout(const Node& node, const json& args,
                                                      size_t index, std::string_view name) {
    if (const json* given = Arg(args, index, name); given != nullptr) {
        return ParseLock(*given, *node.params);
    }
    if (node.payout.has_value()) {
        return *node.payout;
    }
    return std::unexpected(
        Error{.code = RpcError::NoPayout, .detail = Describe(RpcError::NoPayout)});
}

/// Anything serialisable, as wire-order hex.
///
/// One function rather than one per type, because the reason a caller gets hex at all is
/// the same in every case: it is the exact byte string this node would put on the wire, so
/// a miner or a wallet can hand it back without having reimplemented the encoding.
template <typename T>
[[nodiscard]] std::string SerialisedHex(const T& value) {
    Writer writer;
    value.Serialize(writer);
    return ToHex(writer.Bytes());
}

/// The chain's tip and the fields every method reports about it.
///
/// Gathered once per call rather than recomputed per field: `MedianTimePastAt` walks eleven
/// blocks and `NextTargetBits` reads the retarget window, and two methods that reported
/// them from separate calls could disagree if a block arrived in between.
struct TipFacts {
    const chain::BlockIndexEntry* entry;
    uint32_t next_height;
    uint32_t next_bits;
    int64_t median_time;
};

[[nodiscard]] TipFacts FactsFor(const Node& node) {
    const chain::BlockIndexEntry& tip = node.state->Tip();
    return TipFacts{.entry = &tip,
                    // Saturating, so that reporting the next height cannot wrap past the
                    // end of the header field. `BuildBlockTemplate` refuses that chain
                    // outright; a read method has no reason to fail on it.
                    .next_height = tip.height < std::numeric_limits<uint32_t>::max()
                                       ? tip.height + 1U
                                       : tip.height,
                    .next_bits = chain::NextTargetBits(tip, *node.params),
                    .median_time = chain::MedianTimePastAt(tip)};
}

/// What the pool holds, or what an absent pool holds, which is nothing.
///
/// A node started without a pool reports zeros rather than failing: "how many transactions
/// are waiting" has a true answer on such a node, and it is none. The methods that would
/// have to *change* the pool are the ones that refuse with `NoMempool`.
[[nodiscard]] json MempoolFacts(const mempool::Mempool* pool) {
    if (pool == nullptr) {
        return json{{"size", 0},
                    {"weight", 0},
                    {"total_fees", 0},
                    {"lowest_package_feerate", 0},
                    {"present", false}};
    }
    return json{{"size", pool->Size()},
                {"weight", pool->TotalWeight()},
                {"total_fees", pool->TotalFees()},
                {"lowest_package_feerate", pool->LowestPackageFeerate()},
                {"present", true}};
}

[[nodiscard]] Result GetBlockchainInfo(Node& node, const json&, int64_t) {
    const TipFacts facts = FactsFor(node);
    return json{{"chain", std::string(node.params->name)},
                {"chain_id", node.params->chain_id.ToHex()},
                {"blocks", facts.entry->height},
                {"best_block_hash", facts.entry->hash.ToHex()},
                {"total_work", facts.entry->total_work.ToHex()},
                {"median_time", facts.median_time},
                {"tip_time", facts.entry->header.timestamp},
                {"target_bits", facts.entry->header.target_bits},
                {"next_target_bits", facts.next_bits},
                {"trivial_difficulty", node.params->trivial_difficulty},
                {"issuance_end_height", IssuanceEndHeight(node.params->issuance)},
                {"max_money", MAX_MONEY},
                {"base_unit", "facet"},
                {"units_per_coin", FACETS_PER_AMR},
                {"mempool", MempoolFacts(node.pool)}};
}

[[nodiscard]] Result GetMiningInfo(Node& node, const json&, int64_t) {
    const TipFacts facts = FactsFor(node);
    // Null rather than an error when the compact form does not decode: this method's job is
    // to report what the node believes, and "the target this build computed is unusable" is
    // a more useful thing to be told than a refusal to answer at all.
    const std::optional<Target> target = CompactToTarget(facts.next_bits);
    return json{{"chain", std::string(node.params->name)},
                {"blocks", facts.entry->height},
                {"next_height", facts.next_height},
                {"target_bits", facts.entry->header.target_bits},
                {"next_target_bits", facts.next_bits},
                {"next_target", target.has_value() ? json(ToHex(*target)) : json(nullptr)},
                {"next_reward", BlockReward(facts.next_height, node.params->issuance)},
                {"median_time", facts.median_time},
                {"max_block_weight", node.params->max_block_weight},
                {"trivial_difficulty", node.params->trivial_difficulty},
                {"mempool", MempoolFacts(node.pool)}};
}

/// The window a solver may move the timestamp within, as the rules define it.
///
/// Reported rather than left implicit because `time` is in the mutable list: a miner that
/// rolls the timestamp to refresh its search needs to know where the two walls are, and the
/// walls are consensus rules — one second past the median of the last eleven blocks, and no
/// more than `MAX_FUTURE_BLOCK_SECONDS` past the clock.
struct TimeWindow {
    int64_t min_time;
    int64_t max_time;
};

[[nodiscard]] TimeWindow WindowFor(const TipFacts& facts, int64_t now) {
    constexpr int64_t CEILING = std::numeric_limits<int64_t>::max();
    return TimeWindow{
        .min_time = facts.median_time < CEILING ? facts.median_time + 1 : facts.median_time,
        .max_time = now < CEILING - MAX_FUTURE_BLOCK_SECONDS ? now + MAX_FUTURE_BLOCK_SECONDS
                                                            : CEILING};
}

[[nodiscard]] Result GetBlockTemplate(Node& node, const json& args, int64_t now) {
    const std::expected<Lock, Error> payout = ResolvePayout(node, args, 0, "payout");
    if (!payout.has_value()) {
        return std::unexpected(payout.error());
    }
    ByteVec coinbase_data;
    if (const json* given = Arg(args, 1, "coinbase_data"); given != nullptr) {
        std::expected<ByteVec, Error> bytes = AsBytes(*given);
        if (!bytes.has_value()) {
            return std::unexpected(bytes.error());
        }
        coinbase_data = std::move(*bytes);
    }

    const TipFacts facts = FactsFor(node);
    const std::expected<mining::BlockTemplate, mining::TemplateError> built =
        mining::BuildBlockTemplate(*facts.entry, *payout, now, std::move(coinbase_data),
                                   *node.params, node.pool);
    if (!built.has_value()) {
        // The assembler's own description travels out, because every value it can return is
        // a statement about this node or this request rather than about anything received.
        return std::unexpected(
            Error{.code = RpcError::TemplateFailed, .detail = mining::Describe(built.error())});
    }
    const Block& block = built->block;

    // Transaction 0 is the coinbase, which the miner is being handed rather than asked
    // about, so the list starts at 1. Each entry carries what a miner needs to rebuild the
    // block itself if it drops or appends anything: the bytes, both hashes, the weight, and
    // the fee. The fee comes from the pool entry, found by the wtxid this loop had to
    // compute anyway for the field of the same name — a map lookup, not a second hashing of
    // the transaction.
    json transactions = json::array();
    for (size_t i = 1; i < block.transactions.size(); ++i) {
        const Transaction& tx = block.transactions[i];
        const Hash256 wtxid = tx.Wtxid();
        const mempool::Entry* entry = node.pool != nullptr ? node.pool->Find(wtxid) : nullptr;
        transactions.push_back(json{{"data", SerialisedHex(tx)},
                                    {"txid", tx.Txid().ToHex()},
                                    {"wtxid", wtxid.ToHex()},
                                    {"weight", tx.Weight()},
                                    {"fee", entry != nullptr ? json(entry->fee) : json(nullptr)}});
    }

    const TimeWindow window = WindowFor(facts, now);
    return json{{"height", block.header.height},
                {"version", block.header.version},
                {"previous_block_hash", block.header.prev_block.ToHex()},
                {"merkle_root", block.header.merkle_root.ToHex()},
                {"timestamp", block.header.timestamp},
                {"min_time", window.min_time},
                {"max_time", window.max_time},
                {"target", ToHex(built->target)},
                {"target_bits", block.header.target_bits},
                // The whole 64-bit field, stated as a range so a solver splitting work
                // across threads does not have to assume Bitcoin's 32 bits.
                {"nonce_range", "0000000000000000ffffffffffffffff"},
                {"coinbase_value", built->reward},
                {"fees", built->fees},
                {"coinbase_data_limit", node.params->block_limits.tx.max_coinbase_data_size},
                {"weight", block.Weight()},
                {"weight_limit", node.params->max_block_weight},
                {"mutable", json::array({"nonce", "time", "coinbase/append"})},
                {"transactions", std::move(transactions)},
                // The unsolved block exactly as this node would encode it. A miner that
                // only rolls the nonce needs nothing else: change the last eight bytes and
                // hand this string back to `submitblock`.
                {"block", SerialisedHex(block)}};
}

/// Runs `block` through exactly the path a block arriving from a stranger takes, and reports
/// whether this node is now on it.
///
/// Shared by `submitblock` and the generate method deliberately. A block this node mined has
/// to be judged by the same code as a block a peer sent; two paths would eventually disagree,
/// and a disagreement between a node's miner and its validator is a chain split with one
/// participant.
///
/// An empty return means the block is on the active chain. A non-empty one is static text
/// saying why it is not, which is a successful answer to a well-formed request rather than an
/// error — see the header. The `Error` case is reserved for this node's own storage failing.
[[nodiscard]] std::expected<std::string_view, Error> Submit(Node& node, const Block& block,
                                                           int64_t now) {
    const std::expected<const chain::BlockIndexEntry*, chain::HeaderError> accepted =
        node.state->AcceptBlock(block, now);
    if (!accepted.has_value()) {
        return chain::Describe(accepted.error());
    }
    const chain::BlockIndexEntry& entry = **accepted;
    const std::expected<chain::ActivationSummary, chain::ActivationFailure> activated =
        node.state->ActivateBestChain();
    if (!activated.has_value()) {
        return std::unexpected(Error{.code = RpcError::ChainFault,
                                     .detail = chain::Describe(activated.error().error)});
    }
    // A block can pass every rule that does not need a chain and still be refused when it is
    // connected — a spend of a coin that does not exist is only visible then. Without this
    // loop such a block would be reported as accepted, which is the one answer a miner must
    // never be given about a block that was thrown away.
    for (const chain::RejectedBlock& rejected : activated->rejected) {
        if (rejected.hash == entry.hash) {
            return consensus::Describe(rejected.error);
        }
    }
    if (!node.state->Chain().Contains(entry)) {
        return std::string_view{"the block is valid but this node is on a heavier chain"};
    }
    if (node.pool != nullptr) {
        // The pool must lose what this block confirmed, and what this block made
        // unconfirmable, before another template is built. A pool that kept either would
        // offer an already-mined or now-conflicting transaction, and the next block this
        // node produced would be invalid.
        static_cast<void>(node.pool->RemoveForBlock(block));
    }
    return std::string_view{};
}

[[nodiscard]] Result SubmitBlock(Node& node, const json& args, int64_t now) {
    const json* given = Arg(args, 0, "block");
    if (given == nullptr) {
        return std::unexpected(Error{.code = RpcError::InvalidParams,
                                     .detail = "submitblock takes one hex-encoded block"});
    }
    const std::expected<ByteVec, Error> bytes = AsBytes(*given);
    if (!bytes.has_value()) {
        return std::unexpected(bytes.error());
    }
    Block block;
    Reader reader{*bytes};
    // `Finish` as well as `Deserialize`: a block followed by trailing bytes decodes fine and
    // is not the block the submitter sent. Accepting it would mean two byte strings hashing
    // to one block id, which is the shape of every malleability bug there has ever been.
    if (!Block::Deserialize(reader, block, node.params->block_limits) || !reader.Finish()) {
        return std::unexpected(Error{.code = RpcError::MalformedBlock,
                                     .detail = Describe(RpcError::MalformedBlock)});
    }
    const std::expected<std::string_view, Error> outcome = Submit(node, block, now);
    if (!outcome.has_value()) {
        return std::unexpected(outcome.error());
    }
    // Null means "this node is on your block now". Anything else is why it is not. One rule,
    // so a miner never has to guess which kind of answer it is holding.
    return outcome->empty() ? json(nullptr) : json(std::string{*outcome});
}

/// The nonce budget one generate call spends per block before giving up.
///
/// Bounded rather than unbounded so the call returns: a search with no ceiling would hold the
/// node inside one request with no way for the caller to stop it. On a network whose target is
/// trivial a block costs a handful of attempts, so exhausting this budget means the target was
/// not trivial after all — which is why it is reported rather than retried.
inline constexpr uint64_t DEFAULT_GENERATE_ATTEMPTS = uint64_t{1} << 26;

/// A ceiling on one call's block count, so that a single request cannot occupy the node
/// indefinitely. A caller wanting more calls again.
inline constexpr uint64_t MAX_GENERATE_BLOCKS = 1'000;

/// Mines blocks with this node's own CPU, on a network where that is meaningful.
///
/// Every block goes through `Submit`, so a generated block is validated exactly as an arriving
/// one is. Blocks mined before a failure stay on the chain: this call is not atomic and cannot
/// be, because each block it makes is a fact the network can already have seen.
[[nodiscard]] Result Generate(Node& node, const json& args, int64_t now) {
    // The gate, and it is first. On a network with real proof of work this loop would run for
    // an unbounded time and produce nothing, and a miner there wants `getblocktemplate` and
    // hardware of its own.
    if (!node.params->trivial_difficulty) {
        return std::unexpected(Error{.code = RpcError::MiningNotPermitted,
                                     .detail = Describe(RpcError::MiningNotPermitted)});
    }
    uint64_t blocks = 1;
    if (const json* given = Arg(args, 0, "blocks"); given != nullptr) {
        const std::expected<uint64_t, Error> count = AsUint(*given);
        if (!count.has_value()) {
            return std::unexpected(count.error());
        }
        blocks = *count;
    }
    if (blocks == 0 || blocks > MAX_GENERATE_BLOCKS) {
        return std::unexpected(Error{.code = RpcError::InvalidParams,
                                     .detail = "generate makes between 1 and 1000 blocks"});
    }
    const std::expected<Lock, Error> payout = ResolvePayout(node, args, 1, "payout");
    if (!payout.has_value()) {
        return std::unexpected(payout.error());
    }
    uint64_t attempts = DEFAULT_GENERATE_ATTEMPTS;
    if (const json* given = Arg(args, 2, "attempts"); given != nullptr) {
        const std::expected<uint64_t, Error> budget = AsUint(*given);
        if (!budget.has_value()) {
            return std::unexpected(budget.error());
        }
        if (*budget == 0) {
            return std::unexpected(Error{.code = RpcError::InvalidParams,
                                         .detail = "a nonce budget of zero searches nothing"});
        }
        attempts = *budget;
    }

    json hashes = json::array();
    for (uint64_t mined = 0; mined < blocks; ++mined) {
        // Rebuilt every iteration, from the tip the previous iteration produced. Reusing a
        // template would mine the same height twice, and reusing its transaction selection
        // after `Submit` cleared the pool would mine transactions that are already confirmed.
        std::expected<mining::BlockTemplate, mining::TemplateError> built =
            mining::BuildBlockTemplate(node.state->Tip(), *payout, now, ByteVec{}, *node.params,
                                       node.pool);
        if (!built.has_value()) {
            return std::unexpected(Error{.code = RpcError::TemplateFailed,
                                         .detail = mining::Describe(built.error())});
        }
        if (!mining::SolveHeader(built->block.header, built->target, attempts)) {
            return std::unexpected(
                Error{.code = RpcError::NotSolved, .detail = Describe(RpcError::NotSolved)});
        }
        const std::expected<std::string_view, Error> outcome = Submit(node, built->block, now);
        if (!outcome.has_value()) {
            return std::unexpected(outcome.error());
        }
        if (!outcome->empty()) {
            // This node assembled a block, solved it, and then refused it. The assembler and
            // the rules have diverged, and mining on would produce more of the same.
            return std::unexpected(Error{.code = RpcError::SelfRejected, .detail = *outcome});
        }
        hashes.push_back(built->block.header.Hash().ToHex());
    }
    return hashes;
}

[[nodiscard]] Result GetMempoolInfo(Node& node, const json&, int64_t) {
    json facts = MempoolFacts(node.pool);
    // The limits alongside the levels, so that a caller can tell "nearly full" from "busy"
    // without a second call and without hard-coding this build's constants.
    facts["max_weight"] = mempool::MAX_MEMPOOL_WEIGHT;
    facts["min_relay_feerate"] = mempool::MIN_RELAY_FEERATE;
    return facts;
}

[[nodiscard]] Result GetRawMempool(Node& node, const json&, int64_t) {
    json wtxids = json::array();
    if (node.pool != nullptr) {
        std::vector<Hash256> ids = node.pool->Wtxids();
        // Sorted, because the pool hands them back in its hash table's order. Two identical
        // requests should produce two identical answers, and an unsorted listing would make a
        // client's diff of the pool between two calls meaningless.
        std::ranges::sort(ids);
        for (const Hash256& id : ids) {
            wtxids.push_back(id.ToHex());
        }
    }
    return wtxids;
}

[[nodiscard]] Result GetBlockHash(Node& node, const json& args, int64_t) {
    const json* given = Arg(args, 0, "height");
    if (given == nullptr) {
        return std::unexpected(
            Error{.code = RpcError::InvalidParams, .detail = "getblockhash takes a height"});
    }
    const std::expected<uint64_t, Error> height = AsUint(*given);
    if (!height.has_value()) {
        return std::unexpected(height.error());
    }
    // Beyond the chain and beyond the field are the same answer to the caller — there is no
    // block there — so the width check does not need its own message.
    const chain::BlockIndexEntry* entry =
        *height <= std::numeric_limits<uint32_t>::max()
            ? node.state->Chain().AtHeight(static_cast<uint32_t>(*height))
            : nullptr;
    if (entry == nullptr) {
        return std::unexpected(Error{.code = RpcError::InvalidParams,
                                     .detail = "this chain has no block at that height"});
    }
    return json(entry->hash.ToHex());
}

/// Offers a transaction to this node's pool.
///
/// The only way a transaction reaches the pool until Phase 4 brings relay, which is what makes
/// it the other half of `getblocktemplate`: without it the fee-selection path that every block
/// this node produces runs through would never see a transaction in production.
///
/// Returns an object rather than following `submitblock`'s null-or-reason, because the accepted
/// case has something to say — the txid, the wtxid, the fee and the weight the pool computed,
/// none of which a caller can derive without reimplementing the hashing and the fee rule.
[[nodiscard]] Result SendRawTransaction(Node& node, const json& args, int64_t now) {
    if (node.pool == nullptr) {
        return std::unexpected(
            Error{.code = RpcError::NoMempool, .detail = Describe(RpcError::NoMempool)});
    }
    const json* given = Arg(args, 0, "transaction");
    if (given == nullptr) {
        return std::unexpected(Error{.code = RpcError::InvalidParams,
                                     .detail = "sendrawtransaction takes one hex transaction"});
    }
    const std::expected<ByteVec, Error> bytes = AsBytes(*given);
    if (!bytes.has_value()) {
        return std::unexpected(bytes.error());
    }
    Transaction tx;
    Reader reader{*bytes};
    if (!Transaction::Deserialize(reader, tx, node.params->block_limits.tx) || !reader.Finish()) {
        return std::unexpected(Error{.code = RpcError::MalformedTransaction,
                                     .detail = Describe(RpcError::MalformedTransaction)});
    }
    const chain::BlockIndexEntry& tip = node.state->Tip();
    // The height it would be spent at, which is the next block's. Maturity and locktime are
    // judged against where a transaction would confirm, not against where the chain stands.
    const uint32_t spend_height =
        tip.height < std::numeric_limits<uint32_t>::max() ? tip.height + 1U : tip.height;
    const std::expected<const mempool::Entry*, mempool::Rejection> accepted =
        node.pool->Accept(tx, node.state->Coins(), spend_height, now, *node.params);
    if (!accepted.has_value()) {
        const mempool::Rejection& rejection = accepted.error();
        // Both vocabularies, unflattened: the policy reason the pool refused it, and — when a
        // consensus rule was what refused it — that rule. A caller told only "rejected" cannot
        // tell a fee too low from a signature that does not verify.
        return json{{"accepted", false},
                    {"reason", std::string{mempool::Describe(rejection.reason)}},
                    {"rule", rejection.rule.has_value()
                                 ? json(std::string{consensus::Describe(*rejection.rule)})
                                 : json(nullptr)}};
    }
    const mempool::Entry& entry = **accepted;
    return json{{"accepted", true},
                {"txid", entry.txid.ToHex()},
                {"wtxid", entry.wtxid.ToHex()},
                {"fee", entry.fee},
                {"weight", entry.weight}};
}

[[nodiscard]] Result Help(Node&, const json&, int64_t) {
    json names = json::array();
    for (const std::string_view name : MethodNames()) {
        names.push_back(std::string{name});
    }
    return names;
}

/// One served method: its name, and the function that serves it.
///
/// A table of function pointers rather than a chain of string comparisons in one long
/// function, so that adding a method is one line next to the others and cannot accidentally
/// shadow an earlier branch.
struct Method {
    std::string_view name;
    Result (*handler)(Node&, const json&, int64_t);
};

/// Every method, in name order, which is the order `help` lists them in.
constexpr std::array<Method, 10> METHODS{{
    {.name = "generate", .handler = Generate},
    {.name = "getblockchaininfo", .handler = GetBlockchainInfo},
    {.name = "getblockhash", .handler = GetBlockHash},
    {.name = "getblocktemplate", .handler = GetBlockTemplate},
    {.name = "getmempoolinfo", .handler = GetMempoolInfo},
    {.name = "getmininginfo", .handler = GetMiningInfo},
    {.name = "getrawmempool", .handler = GetRawMempool},
    {.name = "help", .handler = Help},
    {.name = "sendrawtransaction", .handler = SendRawTransaction},
    {.name = "submitblock", .handler = SubmitBlock},
}};

/// The names alone, derived from the table rather than written twice, so the two cannot
/// drift. Computed at compile time, so `MethodNames` has nothing to initialise at run time.
constexpr std::array<std::string_view, METHODS.size()> METHOD_NAMES = [] {
    std::array<std::string_view, METHODS.size()> out{};
    for (size_t i = 0; i < METHODS.size(); ++i) {
        out[i] = METHODS[i].name;
    }
    return out;
}();

}  // namespace

Result Dispatch(Node& node, std::string_view method, const nlohmann::json& params, int64_t now) {
    // Checked once here rather than in every handler. A `Node` missing either of these is a
    // mistake in whatever built it, not a request that could have been answered differently.
    if (node.state == nullptr || node.params == nullptr) {
        return std::unexpected(Error{.code = RpcError::ChainFault,
                                     .detail = "this RPC node is not bound to a chain"});
    }
    // A linear scan over ten entries. A binary search would need the table's sort order to be
    // correct for lookups to work at all, which turns a typo in the list into a method that
    // silently cannot be called; this way the order matters only to `help`.
    for (const Method& candidate : METHODS) {
        if (candidate.name == method) {
            return candidate.handler(node, params, now);
        }
    }
    return std::unexpected(
        Error{.code = RpcError::UnknownMethod, .detail = Describe(RpcError::UnknownMethod)});
}

std::span<const std::string_view> MethodNames() noexcept { return METHOD_NAMES; }

}  // namespace amarian::rpc
