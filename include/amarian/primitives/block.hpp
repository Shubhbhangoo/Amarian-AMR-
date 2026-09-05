#pragma once

/// \file
/// Block headers, blocks, and their canonical encodings.
///
/// The header is fixed at 92 bytes with no variable-length field, which is what
/// makes it safe to hash directly and cheap to relay: a header is either exactly
/// 92 bytes or it is not a header. Three fields depart from Bitcoin's 80-byte
/// layout — `height` is present, `timestamp` is signed 64-bit, and `nonce` is
/// 64-bit — each for a reason recorded in AMARIAN_PROTOCOL.md.
///
/// As with transactions, decoding here is canonical and bounded but does not
/// judge. Whether `height` follows its predecessor, whether `target_bits` is what
/// the node itself would compute, and whether the hash meets the target are
/// consensus rules; a header that fails all three still decodes, so that a node
/// can reject it *as* invalid rather than as unparseable.

#include <amarian/primitives/transaction.hpp>
#include <amarian/util/serialize.hpp>
#include <amarian/util/types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace amarian {

/// A block header. Exactly 92 bytes, always.
struct BlockHeader {
    /// Fixed serialised length. Every field is fixed-width, so this is a property
    /// of the format rather than of a particular header, and a length check is a
    /// complete structural check.
    static constexpr size_t SERIALIZED_SIZE = 92;

    uint32_t version = 0;
    uint32_t height = 0;
    Hash256 prev_block;
    Hash256 merkle_root;
    /// Seconds since the Unix epoch. Signed 64-bit: a 32-bit unsigned timestamp
    /// stops working in 2106, which is inside the issuance schedule.
    int64_t timestamp = 0;
    /// Compact-encoded target. A claim by whoever produced the block; the node's
    /// own recomputation is the fact. See consensus/target.hpp.
    uint32_t target_bits = 0;
    /// 64-bit, so mining never has to roll the coinbase extranonce and rebuild the
    /// Merkle root merely to keep searching.
    uint64_t nonce = 0;

    void Serialize(Writer& writer) const;

    /// Leaves `out` untouched on failure. Does not require the reader to be at its
    /// end: a header is usually followed by the block's transactions.
    [[nodiscard]] static bool Deserialize(Reader& reader, BlockHeader& out) noexcept;

    /// Block ID: SHA-256d over the 92-byte serialisation. This is the value proof
    /// of work is checked against and the value `prev_block` refers to.
    [[nodiscard]] Hash256 Hash() const;

    friend bool operator==(const BlockHeader&, const BlockHeader&) noexcept = default;
};

/// Resource bounds for decoding one block.
///
/// Parameters rather than constants for the same reason as `TxLimits`: a node
/// syncing history, a node accepting a relayed block, and a test constructing a
/// pathological case are entitled to different ceilings, and hard-coding one here
/// would put a consensus rule in the codec.
struct BlockLimits {
    size_t max_transactions;
    TxLimits tx;
};

/// A block: one header and the transactions it commits to.
struct Block {
    BlockHeader header;
    std::vector<Transaction> transactions;

    void Serialize(Writer& writer) const;

    /// Leaves `out` untouched on failure.
    [[nodiscard]] static bool Deserialize(Reader& reader, Block& out, const BlockLimits& limits);

    [[nodiscard]] Hash256 Hash() const { return header.Hash(); }

    /// The Merkle root the block's transactions actually produce, which is what
    /// `header.merkle_root` must equal. Empty blocks have no root: a block with no
    /// coinbase is invalid, but that is a consensus rule and this reports the
    /// absence rather than inventing a value for it.
    [[nodiscard]] std::optional<Hash256> ComputeMerkleRoot() const;

    /// `base_size × 4 + witness_size`, where the witness section is everything the
    /// txid preimage omits — including its own count prefix, per DECISIONS #33.
    [[nodiscard]] size_t Weight() const;

    friend bool operator==(const Block&, const Block&) noexcept = default;
};

}  // namespace amarian
