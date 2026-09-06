#pragma once

/// \file
/// Wire-level P2P message payload types.
///
/// Each struct below corresponds to one `Command` and carries the payload fields
/// specified in NETWORK.md. Every struct has a `Serialize(Writer&)` and a
/// `static Deserialize(Reader&, T&, bounds)` pair, following the same canonical
/// encoding convention as the primitives layer.
///
/// The serialisation of every message is:
///
///   - integers fixed-width little-endian (u8, u16, u32, u64)
///   - variable-length byte strings compact-size + bytes
///   - hash256 as 32 raw bytes, internal order
///   - vectors of items prefixed with a compact-size count

#include <amarian/primitives/block.hpp>
#include <amarian/primitives/transaction.hpp>
#include <amarian/util/serialize.hpp>
#include <amarian/util/types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace amarian::net {

// --- Service flags ----------------------------------------------------------

/// What services a peer advertises.
using ServiceFlags = uint64_t;

inline constexpr ServiceFlags NODE_NONE = 0;
/// Serves historical blocks on request.
inline constexpr ServiceFlags NODE_HISTORICAL = 1;
/// Relays transactions.
inline constexpr ServiceFlags NODE_RELAY = 2;

// --- Hello ------------------------------------------------------------

struct HelloPayload {
    static constexpr size_t MIN_SERIALIZED_SIZE = 4 + 8 + 8 + 32 + 4 + 8; // protocol + services + timestamp + chain_id + best_height + nonce

    uint32_t protocol_version = 0;
    ServiceFlags services = NODE_NONE;
    int64_t timestamp = 0;
    Hash256 chain_id;
    uint32_t best_height = 0;
    uint64_t nonce = 0;
    std::string user_agent;  // compact-size length-prefixed

    void Serialize(Writer& writer) const;
    [[nodiscard]] static bool Deserialize(Reader& reader, HelloPayload& out, size_t max_user_agent_len);
};

// --- HelloAck ---------------------------------------------------------

struct HelloAckPayload {
    // No fields – just a command with an empty payload.
    void Serialize(Writer& writer) const;
    [[nodiscard]] static bool Deserialize(Reader& reader, HelloAckPayload&);
};

// --- GetHeaders -------------------------------------------------------

/// A request for headers starting from a known locator chain.
///
/// The peer that receives this sends headers starting at the first locator entry
/// the receiving peer knows about, plus one block. `stop_hash` limits the
/// response; if zero, as many headers as fit in one message are sent.
struct GetHeadersPayload {
    static constexpr size_t MIN_SERIALIZED_SIZE = 4 + 32;  // protocol_version + stop_hash

    uint32_t protocol_version = 0;
    /// Locator hashes, newest first. The peer skips past these to find the
    /// first unknown hash.
    std::vector<Hash256> locator;
    Hash256 stop_hash;

    void Serialize(Writer& writer) const;
    [[nodiscard]] static bool Deserialize(Reader& reader, GetHeadersPayload& out, size_t max_items);
};

// --- Headers ----------------------------------------------------------

/// One or more headers, in chain order.
struct HeadersPayload {
    /// Minimum size for 1 header.
    static constexpr size_t MIN_SERIALIZED_SIZE = 1 + BlockHeader::SERIALIZED_SIZE;
    /// Maximum headers per message, by protocol limit.
    static constexpr size_t MAX_COUNT = 2'000;

    std::vector<BlockHeader> headers;

    void Serialize(Writer& writer) const;
    [[nodiscard]] static bool Deserialize(Reader& reader, HeadersPayload& out);
};

// --- Inv / GetData ----------------------------------------------------

enum class InventoryType : uint32_t {
    Error = 0,
    Transaction = 1,
    Block = 2,
};

struct Inventory {
    InventoryType type = InventoryType::Error;
    Hash256 hash;
};

struct GetDataPayload {
    std::vector<Inventory> items;

    void Serialize(Writer& writer) const;
    [[nodiscard]] static bool Deserialize(Reader& reader, GetDataPayload& out, size_t max_items);
};

using InvPayload = GetDataPayload; // same structure

void SerializeInventory(Writer& writer, const Inventory& inv);
[[nodiscard]] bool DeserializeInventory(Reader& reader, Inventory& out);

// --- Tx / Block -------------------------------------------------------

/// A transaction announcement or relay.
struct TxPayload {
    Transaction tx;

    void Serialize(Writer& writer) const;
    [[nodiscard]] static bool Deserialize(Reader& reader, TxPayload& out, const TxLimits& limits);
};

/// A block relay.
struct BlockPayload {
    Block block;

    void Serialize(Writer& writer) const;
    [[nodiscard]] static bool Deserialize(Reader& reader, BlockPayload& out, const BlockLimits& limits);
};

// --- GetAddr / Addr ---------------------------------------------------

/// A request for peer addresses. Empty payload.
using GetAddrPayload = HelloAckPayload;  // structurally identical

struct NetworkAddress {
    /// 4 = IPv4, 6 = IPv6, borrowed from the C address family constants.
    uint8_t addr_ver = 4;
    std::array<uint8_t, 16> address{};  // zero-padded
    uint16_t port = 0;

    void Serialize(Writer& writer) const;
    [[nodiscard]] static bool Deserialize(Reader& reader, NetworkAddress& out);
};

struct AddrPayload {
    std::vector<NetworkAddress> addresses;

    void Serialize(Writer& writer) const;
    [[nodiscard]] static bool Deserialize(Reader& reader, AddrPayload& out, size_t max_items);
};

// --- Ping / Pong ------------------------------------------------------

struct PingPayload {
    uint64_t nonce = 0;

    void Serialize(Writer& writer) const;
    [[nodiscard]] static bool Deserialize(Reader& reader, PingPayload& out);
};

using PongPayload = PingPayload;

}  // namespace amarian::net