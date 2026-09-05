#pragma once

/// \file
/// The typed half of Amarian's RPC: methods, arguments, and results, with no transport.
///
/// A JSON-RPC server is two separable things — a set of operations on a node, and an HTTP
/// listener that carries them. They are separated here on purpose. The operations are
/// where correctness lives: `submitblock` must run a submitted block through exactly the
/// validation an arriving block gets, and `getblocktemplate` must describe exactly the
/// block this node would mine. Both of those are testable as function calls, and are
/// tested that way, without a socket, a port, a thread, or an authentication token
/// anywhere near them.
///
/// The transport is where *security* lives — binding, authentication, request framing,
/// header checks — and it is a separate file for the same reason: a bug in either half
/// should not need the other half to be understood.
///
/// ## What a method may assume
///
/// Nothing about time. Every call is handed `now`, the caller's clock, so a test can
/// drive a template's timestamp and no method reads a clock of its own — the same
/// discipline the node and the assembler already follow.
///
/// Nothing about the chain beyond what `Node` names. A method reaches the chain state,
/// the mempool and the parameters through that struct and holds no state between calls,
/// so two calls in either order behave the same way as the same two calls with anything
/// else in between.
///
/// ## Errors, and the one case that is not an error
///
/// A method fails with `Error` when the *request* was wrong: an unknown method, a
/// malformed argument, a payout that was never configured. A block that the consensus
/// rules refuse is not that. It is a successful call whose result says which rule
/// refused it, because the request was well formed and the answer — "no, and here is
/// why" — is exactly what the miner asked for. Collapsing the two would make a miner
/// unable to tell a rejected block from an RPC it called wrongly.
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

namespace amarian::rpc {

/// Why a request could not be served.
///
/// An enumeration rather than a string, for the same reason `consensus::ValidationError`
/// is one: these are values a caller compares, and a reason that allocated would let a
/// caller make the node allocate by calling badly. `Describe` turns one into text at the
/// transport edge, and `Code` into the number a JSON-RPC client expects.
enum class RpcError : uint8_t {
    /// No method by that name. The only error a transport can produce without a node.
    UnknownMethod,
    /// The arguments were not of the shape the method takes — wrong type, missing, or a
    /// count no overload accepts.
    InvalidParams,
    /// A hex argument was not hex, or was not the length its field requires.
    MalformedHex,
    /// A hex block argument decoded, but not into a block: a truncated encoding, a
    /// length prefix past the end, or trailing bytes.
    MalformedBlock,
    /// A hex transaction argument decoded, but not into a transaction. Separate from
    /// `MalformedBlock` only so that the answer names what the caller sent.
    MalformedTransaction,
    /// A template or a generate call needed a payout lock and neither the request nor the
    /// node supplied one. Refused rather than defaulted: a node that invented a payout
    /// would mine coins to an address nobody holds the key for.
    NoPayout,
    /// A method that offers a transaction to the pool, on a node started without one.
    /// A node with no pool is a legitimate configuration — it validates the chain and
    /// relays nothing — so this is an answer about how the node was started rather than
    /// a fault, and it is distinct from an empty pool, which the read methods report as
    /// zero rather than as this.
    NoMempool,
    /// `mining::BuildBlockTemplate` refused to build a template. Its own description
    /// travels in `Error::detail`, because every value it can return is a statement about
    /// this node or this request rather than about anything received.
    TemplateFailed,
    /// A generate call on a network where proof of work is real. Refused because it would
    /// block the node for an unbounded time, and because a miner on such a network wants
    /// `getblocktemplate` and its own hardware.
    MiningNotPermitted,
    /// The nonce budget ran out before a solution was found. Not a failure of the chain:
    /// the caller may call again.
    NotSolved,
    /// This node's own rules refused a block this node built and solved itself. Always a
    /// bug in the assembler or the rules rather than anything about the request: every
    /// field of that block was computed from the same functions the rules judge it by, so
    /// a refusal means the two have diverged. Separate from `ChainFault` because the fault
    /// is in this build's logic rather than in this machine's disk, and separate from
    /// `TemplateFailed` because the template was built successfully and refused later.
    SelfRejected,
    /// This node's own storage contradicts its own chain. The one error here that is
    /// about the node's health rather than the request, so it is worth alerting on.
    ChainFault,
};

/// A short stable description, for logs and the transport's error body.
[[nodiscard]] std::string_view Describe(RpcError error) noexcept;

/// The JSON-RPC error code a client should see for `error`.
///
/// Mapped here rather than in the transport so that the numbers are chosen once, next to
/// the values they describe. They follow the conventions a Bitcoin-compatible client
/// already handles — -32601 for an unknown method, -8 for a bad argument, -22 for
/// something that would not deserialise — because a miner's tooling is what will call
/// this first, and there is nothing to gain from a private numbering.
[[nodiscard]] int32_t Code(RpcError error) noexcept;

/// A failed request: what went wrong, and the underlying description when another layer
/// produced one.
///
/// `detail` is a `string_view` and never an owned string, so it can only ever point at
/// storage that outlives the call — the static text `mining::Describe` or
/// `chain::Describe` returns. That is what keeps a request an attacker repeats from
/// allocating.
struct Error {
    RpcError code;
    std::string_view detail;
};

/// The node an RPC method acts on.
///
/// Pointers rather than references so the struct is assignable and a test can build one
/// in a line, and non-owning because the node owns all three for its whole run. `state`
/// and `pool` are mutable because `submitblock` connects a block and clears the pool of
/// what that block confirmed; `params` is not, because nothing may edit consensus
/// parameters at runtime.
struct Node {
    chain::ChainState* state = nullptr;
    mempool::Mempool* pool = nullptr;
    const ChainParams* params = nullptr;

    /// Where a template's coinbase pays when a request does not say. Empty on a node
    /// started without `--payout`, which is why `NoPayout` exists.
    std::optional<Lock> payout;
};

/// A served request: the method's result, or why it could not be served.
using Result = std::expected<nlohmann::json, Error>;

/// Serves one request.
///
/// `params` is the JSON-RPC `params` member: an array for positional arguments, an object
/// for named ones, or null for none. Both forms are accepted for every method, because a
/// hand-written `curl` call reaches for names and a client library reaches for positions.
///
/// `now` is the caller's clock in seconds since the Unix epoch, passed in rather than
/// read here.
[[nodiscard]] Result Dispatch(Node& node, std::string_view method, const nlohmann::json& params,
                              int64_t now);

/// Every method name `Dispatch` serves, sorted, for a `help` listing and for a transport
/// that wants to reject an unknown method before touching the node.
[[nodiscard]] std::span<const std::string_view> MethodNames() noexcept;

}  // namespace amarian::rpc
