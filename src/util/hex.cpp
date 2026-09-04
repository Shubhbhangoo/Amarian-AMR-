#include <amarian/util/hex.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace amarian {

namespace {

constexpr std::string_view HEX_DIGITS = "0123456789abcdef";

/// Returns 0..15 for a hex digit, or -1 for anything else. No locale, no whitespace
/// tolerance, no "0x" prefix: the caller must hand over pure hex.
[[nodiscard]] constexpr int HexVal(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

}  // namespace

std::string ToHex(ByteSpan bytes) {
    std::string out;
    out.resize(bytes.size() * 2);
    size_t pos = 0;
    for (const uint8_t byte : bytes) {
        out[pos++] = HEX_DIGITS[static_cast<size_t>(byte >> 4U)];
        out[pos++] = HEX_DIGITS[static_cast<size_t>(byte & 0x0FU)];
    }
    return out;
}

std::optional<ByteVec> FromHex(std::string_view hex) {
    if ((hex.size() % 2) != 0) {
        return std::nullopt;
    }
    ByteVec out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = HexVal(hex[i]);
        const int lo = HexVal(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::optional<ByteVec> FromHexExact(std::string_view hex, size_t expected_len) {
    if (hex.size() != expected_len * 2) {
        return std::nullopt;
    }
    return FromHex(hex);
}

std::optional<Hash256> Hash256FromHexInternal(std::string_view hex) {
    const auto bytes = FromHexExact(hex, Hash256::SIZE);
    if (!bytes.has_value()) {
        return std::nullopt;
    }
    return Hash256::FromBytes(*bytes);
}

std::optional<Hash256> Hash256FromHex(std::string_view hex) {
    auto bytes = FromHexExact(hex, Hash256::SIZE);
    if (!bytes.has_value()) {
        return std::nullopt;
    }
    ByteVec reversed(bytes->rbegin(), bytes->rend());
    return Hash256::FromBytes(reversed);
}

}  // namespace amarian
