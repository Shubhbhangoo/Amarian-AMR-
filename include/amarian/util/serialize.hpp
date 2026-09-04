#pragma once

/// \file
/// Canonical bounded serialisation.
///
/// Deserialisation is the security perimeter. Every byte a node acts on arrives
/// through here — from a peer, from a block file, from an RPC argument — so three
/// properties are built into these types rather than left to each caller to
/// remember:
///
///   * **One encoding per value.** Fixed-width little-endian integers, and lengths
///     in the shortest compact form that fits. A longer encoding of the same value
///     is rejected, never normalised: normalising is how a malleable encoding
///     becomes invisible, and two encodings of one transaction are two txids.
///   * **Bounds before allocation.** Every length is checked against the bytes
///     actually remaining before anything is sized or reserved. A length prefix is
///     an attacker-chosen number, and treating it as a size to allocate is the
///     oldest memory-exhaustion bug there is.
///   * **Failure is sticky.** Once a Reader has failed, no later read can appear to
///     succeed and no later read consumes anything. That is what makes checking
///     once at the end of a parse safe, and it removes the class of bug where a
///     structure ends up half-populated from garbage that was never in the input.
///
/// Trailing bytes are a parse failure rather than a curiosity: `Finish()` requires
/// the input to have been exactly consumed, because a structure that ignores bytes
/// at the end has two encodings again.
///
/// The byte-moving primitives are defined inline here because computing a txid runs
/// them millions of times during a sync. The branchy canonicality logic — compact
/// size and length-prefixed strings — lives in serialize.cpp, where it is read more
/// often than it is executed.
///
/// This layer has no domain knowledge on purpose. It does not know what an amount
/// is, what MAX_MONEY is, or how long a public key should be; every bound is a
/// parameter. Those rules belong to `primitives` and `consensus`, and a codec that
/// knew them would be a second place for them to be enforced differently.

#include <amarian/util/types.hpp>

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace amarian {

static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big,
              "serialisation assumes a pure little- or big-endian host");

/// Byte length of `value` in the canonical compact-size encoding.
///
/// constexpr so that size and weight arithmetic can be checked at compile time
/// instead of being asserted against a magic number.
[[nodiscard]] constexpr size_t CompactSizeLen(uint64_t value) noexcept {
    if (value < 0xFDU) {
        return 1;
    }
    if (value <= 0xFFFFU) {
        return 3;
    }
    if (value <= 0xFFFFFFFFU) {
        return 5;
    }
    return 9;
}

/// Appends to an owned buffer. Deliberately unbounded: a Writer serialises a
/// structure that already exists in memory, so its size is bounded by whatever
/// admitted that structure in the first place — which is the Reader.
class Writer {
public:
    Writer() = default;

    /// `reserve_hint` avoids reallocation while serialising; it is not a limit.
    explicit Writer(size_t reserve_hint) { bytes_.reserve(reserve_hint); }

    void WriteU8(uint8_t value) { bytes_.push_back(value); }

    void WriteU16(uint16_t value) { AppendLittleEndian(value); }

    void WriteU32(uint32_t value) { AppendLittleEndian(value); }

    void WriteU64(uint64_t value) { AppendLittleEndian(value); }

    /// Two's complement, little-endian. Amounts and timestamps are signed.
    void WriteI64(int64_t value) { AppendLittleEndian(static_cast<uint64_t>(value)); }

    /// Raw bytes, no length prefix. For fixed-width fields only.
    void WriteBytes(ByteSpan bytes) { bytes_.insert(bytes_.end(), bytes.begin(), bytes.end()); }

    /// Internal byte order — the order that is hashed, never display order.
    void WriteHash256(const Hash256& hash) { WriteBytes(hash.Span()); }

    void WriteCompactSize(uint64_t value);

    /// Compact-size length followed by the bytes themselves.
    void WriteByteString(ByteSpan bytes);

    [[nodiscard]] const ByteVec& Bytes() const noexcept { return bytes_; }

    [[nodiscard]] size_t Size() const noexcept { return bytes_.size(); }

    /// Moves the buffer out. The Writer is empty afterwards.
    [[nodiscard]] ByteVec Take() noexcept { return std::move(bytes_); }

private:
    template<std::unsigned_integral T>
    void AppendLittleEndian(T value) {
        if constexpr (std::endian::native == std::endian::big) {
            value = std::byteswap(value);
        }
        std::array<uint8_t, sizeof(T)> raw{};
        std::memcpy(raw.data(), &value, sizeof(T));
        bytes_.insert(bytes_.end(), raw.begin(), raw.end());
    }

    ByteVec bytes_;
};

/// Reads from a borrowed buffer. The buffer must outlive the Reader.
///
/// Every read returns false on failure and leaves its output parameter untouched,
/// so a caller may either check each read or check `Finish()` once — both are safe,
/// because failure is sticky.
class Reader {
public:
    explicit Reader(ByteSpan bytes) noexcept : bytes_(bytes) {}

    /// False once anything has failed. Never returns to true.
    [[nodiscard]] bool Ok() const noexcept { return ok_; }

    /// Zero once failed, so a bound computed from it can never be satisfied.
    [[nodiscard]] size_t Remaining() const noexcept { return ok_ ? bytes_.size() - pos_ : 0; }

    [[nodiscard]] bool AtEnd() const noexcept { return Remaining() == 0; }

    /// Marks the parse failed. For a caller that read valid bytes but found a
    /// structural rule violated, so the sticky flag stays the single source of
    /// truth for whether a parse succeeded.
    void Fail() noexcept { ok_ = false; }

    /// True only if nothing failed *and* the input was exactly consumed. Leftover
    /// bytes fail the parse: ignoring them would give the structure a second
    /// encoding, and a second encoding is a second id.
    [[nodiscard]] bool Finish() noexcept {
        if (pos_ != bytes_.size()) {
            ok_ = false;
        }
        return ok_;
    }

    bool ReadU8(uint8_t& out) noexcept { return ReadLittleEndian(out); }

    bool ReadU16(uint16_t& out) noexcept { return ReadLittleEndian(out); }

    bool ReadU32(uint32_t& out) noexcept { return ReadLittleEndian(out); }

    bool ReadU64(uint64_t& out) noexcept { return ReadLittleEndian(out); }

    /// Two's complement, little-endian. No range check — range is a domain rule and
    /// belongs to whoever knows what the value means.
    bool ReadI64(int64_t& out) noexcept {
        uint64_t raw = 0;
        if (!ReadLittleEndian(raw)) {
            return false;
        }
        out = static_cast<int64_t>(raw);
        return true;
    }

    /// Fills `out` exactly. Reads nothing unless all of it is available.
    bool ReadBytes(MutableByteSpan out) noexcept {
        const uint8_t* src = Consume(out.size());
        if (src == nullptr) {
            return false;
        }
        if (!out.empty()) {
            std::memcpy(out.data(), src, out.size());
        }
        return true;
    }

    bool ReadHash256(Hash256& out) noexcept { return ReadBytes(out.MutableSpan()); }

    /// Borrows `count` bytes without copying. `out` points into the Reader's
    /// buffer, so it is valid exactly as long as that buffer is.
    bool ReadRawSpan(size_t count, ByteSpan& out) noexcept {
        const uint8_t* src = Consume(count);
        if (src == nullptr) {
            return false;
        }
        out = ByteSpan(src, count);
        return true;
    }

    /// Canonical compact size, bounded so it can plausibly be satisfied.
    ///
    /// `min_element_bytes` is the smallest number of bytes one element of the
    /// sequence this length describes can occupy — 1 for a byte string. The count
    /// is rejected unless `count * min_element_bytes` fits in the bytes remaining.
    /// Without that bound a nine-byte input can ask for 2^64 elements, and a loop
    /// that reads them one at a time hangs for a very long time before it fails.
    ///
    /// Non-minimal encodings are rejected: 0xFD 0x01 0x00 is not another way to
    /// write 1.
    bool ReadCompactSize(uint64_t& out, size_t min_element_bytes) noexcept;

    /// Compact-size length then that many bytes. `max_len` is the protocol limit
    /// for this particular field; the length is checked against it *and* against
    /// the bytes remaining before anything is allocated.
    bool ReadByteString(ByteVec& out, size_t max_len);

private:
    /// Advances past `count` bytes and returns where they start, or nullptr and a
    /// failed Reader if there are not that many left.
    [[nodiscard]] const uint8_t* Consume(size_t count) noexcept {
        // pos_ <= bytes_.size() is an invariant, so the subtraction cannot wrap.
        if (!ok_ || bytes_.size() - pos_ < count) {
            ok_ = false;
            return nullptr;
        }
        const uint8_t* start = bytes_.data() + pos_;
        pos_ += count;
        return start;
    }

    template<std::unsigned_integral T>
    bool ReadLittleEndian(T& out) noexcept {
        const uint8_t* src = Consume(sizeof(T));
        if (src == nullptr) {
            return false;
        }
        T value{};
        std::memcpy(&value, src, sizeof(T));
        if constexpr (std::endian::native == std::endian::big) {
            value = std::byteswap(value);
        }
        out = value;
        return true;
    }

    ByteSpan bytes_;
    size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace amarian
