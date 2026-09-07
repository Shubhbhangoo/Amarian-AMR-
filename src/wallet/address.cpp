/// \file
/// bech32m address encoding (BIP-350).

#include <amarian/wallet/address.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace amarian::wallet {
namespace {

constexpr std::string_view CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
constexpr uint32_t GEN[] = {0x3B6A57B2U, 0x26508E6DU, 0x1EA119FAU,
                             0x3D4233DDU, 0x2A1462B3U};
constexpr uint32_t BECH32M_CONST = 0x2BC830A3U;

[[nodiscard]] uint8_t CharToValue(char c) noexcept {
    const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    const auto found = CHARSET.find(lower);
    if (found != std::string_view::npos) return static_cast<uint8_t>(found);
    return 0xFF;
}

[[nodiscard]] uint32_t PolyModStep(uint32_t chk, uint32_t value) noexcept {
    const uint32_t top = chk >> 25;
    chk = ((chk & 0x1FFFFFFU) << 5) ^ value;
    for (int i = 0; i < 5; ++i) {
        if ((top >> i) & 1) chk ^= GEN[i];
    }
    return chk;
}

/// PolyMod over the full value list (hrp_expand || data || [6 zeros]).
[[nodiscard]] uint32_t PolyMod(ByteSpan values) noexcept {
    uint32_t chk = 1;
    for (size_t i = 0; i < values.size(); ++i) {
        chk = PolyModStep(chk, values[i]);
    }
    return chk;
}

ByteVec Convert8To5(ByteSpan data) noexcept {
    ByteVec result;
    result.reserve((data.size() * 8 + 4) / 5);
    uint32_t buffer = 0;
    int bits = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        buffer = (buffer << 8) | data[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            result.push_back(static_cast<uint8_t>((buffer >> bits) & 0x1F));
        }
    }
    if (bits > 0) {
        result.push_back(static_cast<uint8_t>((buffer << (5 - bits)) & 0x1F));
    }
    return result;
}

std::optional<ByteVec> Convert5To8(ByteSpan data, size_t expected_bytes) noexcept {
    ByteVec result;
    result.reserve(expected_bytes);
    uint32_t buffer = 0;
    int bits = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] > 31) return std::nullopt;
        buffer = (buffer << 5) | data[i];
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    if (bits >= 5 || (bits > 0 && ((buffer << (8 - bits)) & 0xFF) != 0)) {
        return std::nullopt;
    }
    if (result.size() != expected_bytes) return std::nullopt;
    return result;
}

/// Build HRP expand for bech32 checksum.
void HrpExpand(std::string_view hrp, ByteVec& out) {
    out.reserve(hrp.size() * 2 + 1);
    for (char c : hrp) {
        out.push_back(static_cast<uint8_t>((c >> 5) & 0x1F));
        out.push_back(static_cast<uint8_t>(c & 0x1F));
    }
    out.push_back(0);
}

/// Compute the 6 5-bit checksum values for bech32m.
void ComputeChecksum5(ByteSpan hrp_expand, ByteSpan data_5bit, uint8_t checksum_out[6]) {
    // Build PolyMod input: hrp_expand || data_5bit || 0 0 0 0 0 0
    ByteVec input;
    input.reserve(hrp_expand.size() + data_5bit.size() + 6);
    input.insert(input.end(), hrp_expand.begin(), hrp_expand.end());
    input.insert(input.end(), data_5bit.begin(), data_5bit.end());
    for (int i = 0; i < 6; ++i) input.push_back(0);

    uint32_t poly = PolyMod(input) ^ BECH32M_CONST;
    for (int i = 0; i < 6; ++i) {
        checksum_out[i] = static_cast<uint8_t>((poly >> (5 * (5 - i))) & 0x1F);
    }
}

}  // namespace

std::string EncodeAddress(std::string_view hrp, uint8_t version, ByteSpan program) {
    // Bech32 witness addresses carry the version as one native 5-bit value,
    // followed by the 8-to-5 converted program.
    if (program.size() == std::numeric_limits<size_t>::max()) return {};
    if (version > 31) return {};
    ByteVec five_bit;
    five_bit.push_back(version);
    const ByteVec program_5bit = Convert8To5(program);
    five_bit.insert(five_bit.end(), program_5bit.begin(), program_5bit.end());

    // Build HRP expand.
    ByteVec hrp_expand;
    HrpExpand(hrp, hrp_expand);

    // Compute the 6 checksum 5-bit values.
    uint8_t checksum[6] = {};
    ComputeChecksum5(hrp_expand, five_bit, checksum);

    // Build output string.
    std::string result;
    result.reserve(hrp.size() + 1 + five_bit.size() + 6);
    result += hrp;
    result += '1';
    for (uint8_t v : five_bit) {
        result.push_back(CHARSET[static_cast<size_t>(v)]);
    }
    for (int i = 0; i < 6; ++i) {
        result.push_back(CHARSET[static_cast<size_t>(checksum[i])]);
    }
    return result;
}

std::optional<DecodedAddress> DecodeAddress(std::string_view hrp, std::string_view address) {
    if (address.size() < 8 || address.size() > 90) return std::nullopt;

    auto sep = address.find_last_of('1');
    if (sep == std::string_view::npos || sep == 0 || sep + 7 > address.size()) {
        return std::nullopt;
    }

    const std::string_view actual_hrp = address.substr(0, sep);
    if (actual_hrp != hrp) return std::nullopt;

    const std::string_view data_part = address.substr(sep + 1);

    for (char c : actual_hrp) {
        if (c < 33 || c > 126) return std::nullopt;
    }

    // Decode data part into 5-bit values.
    ByteVec data_values;
    data_values.reserve(data_part.size());
    for (char c : data_part) {
        const uint8_t v = CharToValue(c);
        if (v > 31) return std::nullopt;
        data_values.push_back(v);
    }
    if (data_values.size() < 6) return std::nullopt;

    // Verify checksum: PolyMod(hrp_expand || data_values) == BECH32M_CONST
    // where data_values includes the 6 checksum chars.
    ByteVec hrp_expand;
    HrpExpand(actual_hrp, hrp_expand);

    ByteVec check_input;
    check_input.reserve(hrp_expand.size() + data_values.size());
    check_input.insert(check_input.end(), hrp_expand.begin(), hrp_expand.end());
    check_input.insert(check_input.end(), data_values.begin(), data_values.end());

    if (PolyMod(check_input) != BECH32M_CONST) {
        return std::nullopt;
    }

    // Strip the 6 checksum values.
    const ByteVec payload(data_values.begin(), data_values.end() - 6);
    if (payload.empty()) return std::nullopt;

    const uint8_t version = payload[0];
    if (version > 16) return std::nullopt;

    const ByteVec program_5bit(payload.begin() + 1, payload.end());
    auto program = Convert5To8(program_5bit, ADDRESS_PROGRAM_BYTES);
    if (!program.has_value()) return std::nullopt;

    DecodedAddress decoded;
    decoded.version = version;
    decoded.program = std::move(*program);
    return decoded;
}

std::optional<std::string> LockToAddress(const Lock& lock, std::string_view hrp) {
    if (lock.version != LOCK_VERSION_CONDITION_COMMITMENT) return std::nullopt;
    if (lock.program.size() != ADDRESS_PROGRAM_BYTES) return std::nullopt;
    return EncodeAddress(hrp, ADDRESS_VERSION_CONDITION, lock.program);
}

std::optional<Lock> AddressToLock(std::string_view address, std::string_view hrp) {
    auto decoded = DecodeAddress(hrp, address);
    if (!decoded.has_value()) return std::nullopt;
    Lock lock;
    lock.version = LOCK_VERSION_CONDITION_COMMITMENT;
    lock.program = std::move(decoded->program);
    return lock;
}

}  // namespace amarian::wallet
