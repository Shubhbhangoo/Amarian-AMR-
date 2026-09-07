#pragma once

/// \file
/// bech32m address encoding and decoding (BIP-350).
///
/// An Amarian address is:
///
///   <hrp>1<version><32-byte commitment><6-char checksum>
///
/// It encodes a Lock commitment - the 32-byte TaggedHash of a SpendCondition.
/// For version 1 (condition commitment) the program is exactly 32 bytes.
///
/// The human-readable prefix (HRP) is not yet chosen and will be fixed during
/// Phase 5. It is a parameter here rather than a constant so the choice can
/// be made in one place.

#include <amarian/primitives/lock.hpp>
#include <amarian/util/types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace amarian::wallet {

/// The version byte for a condition-commitment address.
inline constexpr uint8_t ADDRESS_VERSION_CONDITION = 1;

/// Length of a bech32m address witness program (the 32-byte hash).
inline constexpr size_t ADDRESS_PROGRAM_BYTES = 32;

/// Encodes a 32-byte commitment as a bech32m address string.
///
/// `hrp` is the human-readable prefix (e.g. "amr" or "tamr").
/// `version` is the address version byte (1 for condition commitment).
/// `program` must be exactly 32 bytes.
///
/// Returns the bech32m string, including the HRP and checksum.
[[nodiscard]] std::string EncodeAddress(std::string_view hrp, uint8_t version,
                                         ByteSpan program);

/// Decodes a bech32m address string back into its components.
///
/// Returns nullopt if the string is not a valid bech32m address for `hrp`.
struct DecodedAddress {
    uint8_t version;
    ByteVec program;
};
[[nodiscard]] std::optional<DecodedAddress> DecodeAddress(std::string_view hrp,
                                                           std::string_view address);

/// Converts a Lock into an address string.
///
/// For a version-1 Lock whose program is the 32-byte SpendCondition hash,
/// this encodes the program as a bech32m address with the given HRP.
///
/// Returns the address string, or nullopt if the Lock cannot be encoded as
/// an address (wrong version or wrong program length).
[[nodiscard]] std::optional<std::string> LockToAddress(const Lock& lock, std::string_view hrp);

/// Converts an address string back into a Lock.
///
/// Returns nullopt if the address is not valid for the given HRP or does
/// not encode a recognised lock version.
[[nodiscard]] std::optional<Lock> AddressToLock(std::string_view address, std::string_view hrp);

}  // namespace amarian::wallet
