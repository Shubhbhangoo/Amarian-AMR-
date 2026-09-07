#pragma once

/// \file
/// A RESTful HTTP API for block, transaction and address exploration.
///
/// This is the read-only half of the RPC surface, served as a plain HTTP REST API (GET
/// requests returning JSON) that a block explorer, a wallet UI or a monitoring tool can call
/// without authenticating — because every answer it gives is a property of the chain the node
/// is on, and the node's address is loopback-only, so only a process on the same machine can
/// reach it. Authentication is deliberately absent: a port a block explorer talks to must be
/// reachable by the web server it sits behind, and making an operator configure credentials
/// on both sides gains nothing over the kernel's loopback guard.
///
/// The endpoints:
///
///   GET /explorer/tip              — the active tip's hash, height, work and timestamp
///   GET /explorer/block/<hash>     — a block's header by hash
///   GET /explorer/block-at/<height>— the block at a height
///   GET /explorer/mempool          — transactions in the mempool with fee rates
///   GET /explorer/status           — node information summary
///
/// Every endpoint returns JSON and never a redirect, a form or a page. An explorer that wants
/// a human page renders from these endpoints on its own side.
///
/// ## Separation from the RPC interface
///
/// The explorer API lives in its own listener because it serves on a different port, which
/// is what lets an operator expose it to a local web server without exposing `generate` and
/// `submitblock` to the same machine.

#include <amarian/chain/block_index.hpp>
#include <amarian/chain/chain_state.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/mempool.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace amarian::explorer {

/// The largest reply this server will produce.
inline constexpr size_t MAX_EXPLORER_RESPONSE = 4U << 20;

/// The largest request the explorer server reads. Headers only — GET has no body.
inline constexpr size_t MAX_EXPLORER_HEADERS = 4096;

/// The longest path we will consider.
inline constexpr size_t MAX_PATH_LENGTH = 256;

/// A request the explorer server answered.
struct ExplorerReply {
    int http_status = 200;
    std::string body;
    std::string content_type = "application/json";
};

/// Routes one GET request to the right endpoint and returns the reply.
///
/// Pure: takes the chain state, block index, mempool and path and returns what the explorer
/// should see, without touching a socket.
[[nodiscard]] ExplorerReply Route(const chain::ChainState& state,
                                   const chain::BlockIndex& index,
                                   const mempool::Mempool* pool,
                                   std::string_view path);

/// Renders the active chain's tip as JSON.
[[nodiscard]] nlohmann::json TipJson(const chain::ChainState& state);

/// Renders a block index entry's header as JSON.
[[nodiscard]] nlohmann::json BlockJson(const chain::BlockIndexEntry& entry);

/// Renders mempool contents as JSON.
[[nodiscard]] nlohmann::json MempoolJson(const mempool::Mempool* pool);

/// Renders status summary as JSON.
[[nodiscard]] nlohmann::json StatusJson(const chain::ChainState& state,
                                         const mempool::Mempool* pool);

}  // namespace amarian::explorer