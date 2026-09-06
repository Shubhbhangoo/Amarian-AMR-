/// \file
/// The explorer REST API. Pure functions: decide what to answer from the chain state
/// and the path alone, with no socket involvement. See the header for the endpoint list.

#include <amarian/explorer/http_api.hpp>

#include <amarian/chain/block_index.hpp>
#include <amarian/chain/chain_state.hpp>
#include <amarian/consensus/issuance.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/consensus/work.hpp>
#include <amarian/primitives/amount.hpp>
#include <amarian/util/hex.hpp>
#include <amarian/util/types.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <cctype>

namespace amarian::explorer {

using json = nlohmann::json;

namespace {

/// Split a path into segments. Leading and trailing slashes are stripped; empty segments
/// are skipped.
[[nodiscard]] std::vector<std::string_view> PathSegments(std::string_view path) {
    std::vector<std::string_view> segments;
    while (!path.empty() && path.front() == '/') {
        path.remove_prefix(1);
    }
    while (!path.empty()) {
        const auto slash = path.find('/');
        const auto seg =
            slash == std::string_view::npos ? path : path.substr(0, slash);
        if (!seg.empty()) {
            segments.push_back(seg);
        }
        if (slash == std::string_view::npos) {
            break;
        }
        path = path.substr(slash + 1);
    }
    return segments;
}

/// Parse a hex string into a Hash256. Returns nullopt on failure.
[[nodiscard]] std::optional<Hash256> ParseHash(std::string_view hex) {
    const auto bytes = FromHex(hex);
    if (!bytes.has_value() || bytes->size() != Hash256::SIZE) {
        return std::nullopt;
    }
    return Hash256::FromBytes(*bytes);
}

/// Build a 404 reply.
[[nodiscard]] ExplorerReply NotFound(std::string_view detail) {
    json body = json::object();
    body["error"] = "not_found";
    body["detail"] = std::string(detail);
    return ExplorerReply{.http_status = 404, .body = body.dump()};
}

/// Build a 400 reply.
[[nodiscard]] ExplorerReply BadRequest(std::string_view detail) {
    json body = json::object();
    body["error"] = "bad_request";
    body["detail"] = std::string(detail);
    return ExplorerReply{.http_status = 400, .body = body.dump()};
}

/// Parse an unsigned integer from a string_view.
[[nodiscard]] std::optional<uint32_t> ParseUint32(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    uint32_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        // Overflow guard
        if (value > (UINT32_MAX - static_cast<uint32_t>(c - '0')) / 10) {
            return std::nullopt;
        }
        value = value * 10 + static_cast<uint32_t>(c - '0');
    }
    return value;
}

}  // namespace

json TipJson(const chain::ChainState& state) {
    const chain::BlockIndexEntry& tip = state.Tip();
    return json{{"hash", tip.hash.ToHex()},
                {"height", tip.height},
                {"work", tip.total_work.ToHex()},
                {"timestamp", tip.header.timestamp},
                {"target_bits", tip.header.target_bits},
                {"version", tip.header.version},
                {"nonce", tip.header.nonce},
                {"prev_block", tip.header.prev_block.ToHex()},
                {"merkle_root", tip.header.merkle_root.ToHex()}};
}

json BlockJson(const chain::BlockIndexEntry& entry) {
    return json{{"hash", entry.hash.ToHex()},
                {"height", entry.height},
                {"work", entry.total_work.ToHex()},
                {"timestamp", entry.header.timestamp},
                {"target_bits", entry.header.target_bits},
                {"version", entry.header.version},
                {"nonce", entry.header.nonce},
                {"prev_block", entry.header.prev_block.ToHex()},
                {"merkle_root", entry.header.merkle_root.ToHex()}};
}

json MempoolJson(const mempool::Mempool* pool) {
    if (pool == nullptr || pool->Size() == 0) {
        return json::array();
    }
    json list = json::array();
    const std::vector<Hash256> ids = pool->Wtxids();
    for (const Hash256& id : ids) {
        const mempool::Entry* entry = pool->Find(id);
        if (entry == nullptr) {
            continue;
        }
        json item = json::object();
        item["wtxid"] = id.ToHex();
        item["txid"] = entry->txid.ToHex();
        item["fee"] = entry->fee;
        item["weight"] = entry->weight;
        item["fee_rate"] = entry->fee_rate;
        list.push_back(std::move(item));
    }
    return list;
}

json StatusJson(const chain::ChainState& state, const mempool::Mempool* pool) {
    const chain::BlockIndexEntry& tip = state.Tip();
    json info = json::object();
    info["best_block_hash"] = tip.hash.ToHex();
    info["blocks"] = tip.height;
    info["mempool_size"] = pool != nullptr ? pool->Size() : 0;
    info["mempool_weight"] = pool != nullptr ? pool->TotalWeight() : 0;
    info["mempool_fees"] = pool != nullptr ? pool->TotalFees() : 0;
    return info;
}

ExplorerReply Route(const chain::ChainState& state, const chain::BlockIndex& /*index*/,
                    const mempool::Mempool* pool, std::string_view path) {
    if (path.empty() || path == "/") {
        return ExplorerReply{
            .http_status = 200,
            .body = json{{"service", "amarian explorer API"},
                         {"version", "0.1"},
                         {"tip", TipJson(state)}}
                        .dump()};
    }

    // Clamp path length
    if (path.size() > MAX_PATH_LENGTH) {
        return ExplorerReply{
            .http_status = 414,
            .body = json{{"error", "uri_too_long"},
                         {"detail", "path exceeds maximum length"}}
                        .dump()};
    }

    const auto segments = PathSegments(path);
    if (segments.empty()) {
        return ExplorerReply{.http_status = 200,
                             .body = json{{"service", "amarian explorer API"}, {"version", "0.1"}}
                                         .dump()};
    }

    const std::string_view first = segments[0];

    // GET /explorer/tip
    if (first == "tip" && segments.size() == 1) {
        return ExplorerReply{.http_status = 200, .body = TipJson(state).dump()};
    }

    // GET /explorer/status
    if (first == "status" && segments.size() == 1) {
        return ExplorerReply{.http_status = 200, .body = StatusJson(state, pool).dump()};
    }

    // GET /explorer/mempool
    if (first == "mempool" && segments.size() == 1) {
        return ExplorerReply{.http_status = 200, .body = MempoolJson(pool).dump()};
    }

    // GET /explorer/block/<hash>
    if (first == "block" && segments.size() == 2) {
        const auto hash = ParseHash(segments[1]);
        if (!hash.has_value()) {
            return BadRequest("block hash must be 64 hex characters (32 bytes)");
        }
        // Search the active chain by iterating heights.
        const chain::ActiveChain& chain = state.Chain();
        for (uint32_t h = 0; h <= state.Tip().height; ++h) {
            const chain::BlockIndexEntry* entry = chain.AtHeight(h);
            if (entry != nullptr && entry->hash == *hash) {
                return ExplorerReply{.http_status = 200, .body = BlockJson(*entry).dump()};
            }
        }
        return NotFound("block not found on the active chain");
    }

    // GET /explorer/block-at/<height>
    if (first == "block-at" && segments.size() == 2) {
        const auto height = ParseUint32(segments[1]);
        if (!height.has_value()) {
            return BadRequest("height must be a non-negative integer");
        }
        const chain::BlockIndexEntry* entry = state.Chain().AtHeight(*height);
        if (entry == nullptr) {
            return NotFound("no block at that height");
        }
        return ExplorerReply{.http_status = 200, .body = BlockJson(*entry).dump()};
    }

    // Unknown endpoint
    return NotFound("unknown endpoint: /" + std::string(first));
}

}  // namespace amarian::explorer