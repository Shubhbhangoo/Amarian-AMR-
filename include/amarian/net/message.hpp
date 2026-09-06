#pragma once

/// \file
/// P2P message framing: magic, command, length, and checksum.
///
/// Every P2P message on the wire starts with a 14-byte header:
///
///   magic     | command  | length   | checksum
///   4 bytes   | 2 bytes  | 4 bytes  | 4 bytes
///
/// All integers are fixed-width little-endian, matching the project's serialisation
/// convention. The checksum is the first four bytes of DoubleSha256(payload); it is
/// a corruption guard, not an authentication mechanism.
///
/// The header itself is fixed-width so that a reader can read exactly 14 bytes and
/// then decide what to do:
///
///   * magic is checked against the network's expected value. A mismatch
///     disconnects the peer immediately.
///   * A recognised command continues; an unknown one is ignored rather than
///     parsed.
///   * length is bounds-checked against the protocol maximum and against the bytes
///     the peer announced before any allocation, following the same principle as
///     util/serialize.hpp.
///   * checksum is verified once the payload has arrived.

#include <amarian/util/serialize.hpp>
#include <amarian/util/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace amarian::net {

/// Size in bytes of every wire header.
inline constexpr size_t HEADER_SIZE = 14;

/// Largest payload this protocol version will accept from a peer.
///
/// Must exceed MAX_BLOCK_WEIGHT with room for serialisation overhead so that a
/// block can arrive. 4 MB is provisional.
inline constexpr size_t MAX_MESSAGE_SIZE = 4U << 20;

/// Whether the payload four bytes is the first four of DoubleSha256(payload).
[[nodiscard]] bool ValidateChecksum(ByteSpan payload, uint32_t checksum);

// --- Commands ---------------------------------------------------------------

/// Numeric message identifiers.
///
/// These appear on the wire as little-endian uint16, and must be dense from 0 so
/// that a switch can be exhaustive. An unknown command id is ignored, not rejected:
/// ignoring an unknown id is how future message types can be added without a
/// protocol break.
enum class Command : uint16_t {
    Hello = 0,
    HelloAck = 1,
    GetHeaders = 2,
    Headers = 3,
    Inv = 4,
    GetData = 5,
    Tx = 6,
    Block = 7,
    GetAddr = 8,
    Addr = 9,
    Ping = 10,
    Pong = 11,
};

/// The count of valid commands. Used for bounds checks and exhaustive switches.
inline constexpr size_t COMMAND_COUNT = 12;

[[nodiscard]] constexpr bool IsKnownCommand(uint16_t id) noexcept {
    return id < COMMAND_COUNT;
}

[[nodiscard]] std::string_view CommandName(Command cmd);

// --- Framing ----------------------------------------------------------------

/// A complete message from the wire, after framing.
struct Message {
    Command command;
    ByteVec payload;
};

/// What the framer returned.
enum class FramingResult {
    /// A whole message was consumed. `message` is populated; `consumed` is the
    /// number of bytes that made it up.
    Complete,
    /// Only a prefix has arrived. The caller should read more bytes and call again.
    NeedMore,
    /// The bytes so far describe a message that violates protocol: bad magic,
    /// oversized payload, checksum mismatch, or a command the framer knows is
    /// never sent on the wire (reserved/unknown).
    Malformed,
};

struct Framed {
    FramingResult result = FramingResult::NeedMore;
    Message message;
    /// How many bytes of `buffer` this message consumed. Only valid when result is
    /// Complete.
    size_t consumed = 0;
    /// Why, when Malformed.
    std::string failure;
};

/// Frame one message from a buffer, possibly incomplete.
///
/// Restartable: the caller reads into a buffer and calls this repeatedly until
/// the result is Complete or Malformed. There is no parser state between calls:
/// the entire buffer is rescanned each time, which is acceptable because the
/// header is small and the body size is known once the header is read.
[[nodiscard]] Framed FrameMessage(ByteSpan buffer, const std::array<uint8_t, 4>& expected_magic);

/// Serialise a complete message for sending on the wire.
///
/// Prepends the 14-byte header to the payload, computing the checksum from
/// DoubleSha256(payload).
[[nodiscard]] ByteVec SerialiseMessage(Command command, ByteSpan payload,
                                        const std::array<uint8_t, 4>& magic);

}  // namespace amarian::net