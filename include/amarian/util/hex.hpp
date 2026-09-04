#pragma once

/// \file
/// Hex encoding/decoding with strict parsing.
///
/// Strictness matters: hex arrives from RPC, config files and test vectors. A decoder
/// that silently accepts odd-length input or non-hex characters turns user error into
/// silent wrong-address / wrong-amount behaviour.

#include <amarian/util/result.hpp>
#include <amarian/util/types.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace amarian {

/// Lowercase hex, no prefix, no separators.
[[nodiscard]] std::string ToHex(ByteSpan bytes);

/// Decodes lowercase or uppercase hex. Rejects odd length, non-hex characters,
/// whitespace and any prefix. Returns std::nullopt on any deviation.
[[nodiscard]] std::optional<ByteVec> FromHex(std::string_view hex);

/// Decodes exactly `expected_len` bytes, i.e. `2 * expected_len` hex characters.
[[nodiscard]] std::optional<ByteVec> FromHexExact(std::string_view hex, size_t expected_len);

/// Parses a 32-byte value written in reversed (explorer / RPC) byte order.
[[nodiscard]] std::optional<Hash256> Hash256FromHex(std::string_view hex);

/// Parses a 32-byte value written in internal byte order.
[[nodiscard]] std::optional<Hash256> Hash256FromHexInternal(std::string_view hex);

}  // namespace amarian
