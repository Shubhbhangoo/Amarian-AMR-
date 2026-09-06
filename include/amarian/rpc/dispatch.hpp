#pragma once

/// \file
/// The typed half of Amarian's RPC: methods, arguments, and results, with no transport.
///
/// (Documentation omitted for brevity — see the original file)
///
/// ## Byte order, stated once
///
/// Every 32-byte hash in an argument or a result is in reversed display order, the form
/// `Hash256::ToHex` prints and every explorer shows. Every other hex string — a
/// serialised block, a lock program, coinbase bytes — is in wire order, because those
/// are byte strings and not numbers. This is the same split Bitcoin's RPC uses.

#include <amarian/chain/chain_state.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/mempool.hpp>
#include <amarian/primitives/lock.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

// Forward declaration: wallet::Wallet is in the wallet namespace.
namespace amarian::wallet {
class Wallet;
}

namespace amarian::rpc {

/// Why a request could not be served.
///
/// An enumeration rather than a string, for the same reason `consensus::ValidationError`
/// is one: these are values a caller compares, and a reason that allocated would let a
/// caller make the node allocate by calling badly. `Describe` turns one into text at the
/// transport edge, and `Code` into the number a JSON-RPC client expects.
enum class RpcError : uint8_t {
    UnknownMethod,
    InvalidParams,
    MalformedHex,
    MalformedBlock,
    MalformedTransaction,
    NoPayout,
    NoMempool,
    TemplateFailed,
    MiningNotPermitted,
    NotSolved,
    SelfRejected,
    ChainFault,
};

[[nodiscard]] std::string_view Describe(RpcError error) noexcept;
[[nodiscard]] int32_t Code(RpcError error) noexcept;

struct Error {
    RpcError code;
    std::string_view detail;
};

/// The node an RPC method acts on.
struct Node {
    chain::ChainState* state = nullptr;
    mempool::Mempool* pool = nullptr;
    const ChainParams* params = nullptr;

    /// Optional wallet for wallet RPC methods.
    wallet::Wallet* wallet = nullptr;

    /// Where a template's coinbase pays when a request does not say.
    std::optional<Lock> payout;
};

using Result = std::expected<nlohmann::json, Error>;

[[nodiscard]] Result Dispatch(Node& node, std::string_view method, const nlohmann::json& params,
                              int64_t now);

[[nodiscard]] std::span<const std::string_view> MethodNames() noexcept;

}  // namespace amarian::rpc