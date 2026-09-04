#pragma once

/// \file
/// Core byte and 256-bit blob types shared by every Amarian layer.
///
/// Amarian keeps a single spelling for "bytes" (`uint8_t`) so that binding to the
/// C cryptographic libraries (libsecp256k1, OpenSSL) never needs a reinterpret cast
/// at a consensus boundary.

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace amarian {

using ByteVec = std::vector<uint8_t>;
using ByteSpan = std::span<const uint8_t>;
using MutableByteSpan = std::span<uint8_t>;

/// A 256-bit opaque value: block hashes, transaction ids, Merkle nodes, commitments.
///
/// Byte order is the internal (hash-output) order. Textual presentation reverses the
/// bytes to match the big-endian convention users expect from block explorers; that
/// reversal lives only in ToHex()/FromHex() and never in serialisation.
class Hash256 {
public:
    static constexpr size_t SIZE = 32;

    constexpr Hash256() noexcept : data_{} {}

    explicit constexpr Hash256(const std::array<uint8_t, SIZE>& bytes) noexcept : data_(bytes) {}

    /// Copies exactly SIZE bytes; shorter or longer spans are a programming error.
    static Hash256 FromBytes(ByteSpan bytes);

    [[nodiscard]] constexpr const std::array<uint8_t, SIZE>& Array() const noexcept {
        return data_;
    }

    [[nodiscard]] constexpr std::array<uint8_t, SIZE>& Array() noexcept { return data_; }

    [[nodiscard]] constexpr const uint8_t* Data() const noexcept { return data_.data(); }

    [[nodiscard]] constexpr uint8_t* Data() noexcept { return data_.data(); }

    [[nodiscard]] constexpr ByteSpan Span() const noexcept { return ByteSpan{data_}; }

    [[nodiscard]] constexpr MutableByteSpan MutableSpan() noexcept {
        return MutableByteSpan{data_};
    }

    [[nodiscard]] constexpr bool IsZero() const noexcept {
        return std::ranges::all_of(data_, [](uint8_t b) { return b == 0; });
    }

    /// Reversed-byte hex, i.e. the form printed by explorers and RPC.
    [[nodiscard]] std::string ToHex() const;

    /// Internal-order hex; used for test vectors and tagged-hash debugging.
    [[nodiscard]] std::string ToHexInternal() const;

    friend constexpr bool operator==(const Hash256&, const Hash256&) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(const Hash256& a,
                                                      const Hash256& b) noexcept {
        return a.data_ <=> b.data_;
    }

private:
    std::array<uint8_t, SIZE> data_;
};

static_assert(sizeof(Hash256) == Hash256::SIZE, "Hash256 must be a bare 32-byte blob");

/// Constant-time equality for secrets and MAC-like comparisons.
///
/// Consensus hash comparisons do not need this (both operands are public), but key and
/// signature handling does, and having one spelling avoids accidental leaks.
[[nodiscard]] bool ConstantTimeEqual(ByteSpan a, ByteSpan b) noexcept;

}  // namespace amarian

template <>
struct std::hash<amarian::Hash256> {
    /// Hash256 values are already uniformly distributed; take the low 8 bytes.
    [[nodiscard]] size_t operator()(const amarian::Hash256& h) const noexcept {
        size_t out = 0;
        std::memcpy(&out, h.Data(), sizeof(out));
        return out;
    }
};
