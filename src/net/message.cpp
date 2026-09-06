/// \file
/// Message framing: reading headers and serialising them.

#include <amarian/net/message.hpp>

#include <amarian/crypto/hash.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace amarian::net {

bool ValidateChecksum(ByteSpan payload, uint32_t checksum) {
    if (payload.size() == 0) {
        return checksum == 0;
    }
    const Hash256 hash = DoubleSha256(payload);
    uint32_t computed = 0;
    std::memcpy(&computed, hash.Data(), sizeof(computed));
    return computed == checksum;
}

std::string_view CommandName(Command cmd) {
    switch (cmd) {
        case Command::Hello:
            return "hello";
        case Command::HelloAck:
            return "hello_ack";
        case Command::GetHeaders:
            return "getheaders";
        case Command::Headers:
            return "headers";
        case Command::Inv:
            return "inv";
        case Command::GetData:
            return "getdata";
        case Command::Tx:
            return "tx";
        case Command::Block:
            return "block";
        case Command::GetAddr:
            return "getaddr";
        case Command::Addr:
            return "addr";
        case Command::Ping:
            return "ping";
        case Command::Pong:
            return "pong";
    }
    return "unknown";
}

Framed FrameMessage(ByteSpan buffer, const std::array<uint8_t, 4>& expected_magic) {
    Framed framed;
    if (buffer.size() < HEADER_SIZE) {
        return framed;  // NeedMore by default
    }

    // Read the 14-byte header.
    Reader header_reader(buffer.subspan(0, HEADER_SIZE));

    // Magic.
    std::array<uint8_t, 4> magic{};
    if (!header_reader.ReadBytes(magic)) {
        framed.result = FramingResult::Malformed;
        framed.failure = "cannot read magic";
        return framed;
    }
    if (magic != expected_magic) {
        framed.result = FramingResult::Malformed;
        framed.failure = "magic mismatch";
        return framed;
    }

    // Command.
    uint16_t cmd_raw = 0;
    if (!header_reader.ReadU16(cmd_raw)) {
        framed.result = FramingResult::Malformed;
        framed.failure = "cannot read command";
        return framed;
    }
    if (!IsKnownCommand(cmd_raw)) {
        framed.result = FramingResult::Malformed;
        framed.failure = "unknown command " + std::to_string(cmd_raw);
        return framed;
    }

    // Length.
    uint32_t length = 0;
    if (!header_reader.ReadU32(length)) {
        framed.result = FramingResult::Malformed;
        framed.failure = "cannot read length";
        return framed;
    }
    if (length > MAX_MESSAGE_SIZE) {
        framed.result = FramingResult::Malformed;
        framed.failure = "message exceeds maximum size: " + std::to_string(length);
        return framed;
    }

    // Checksum.
    uint32_t checksum = 0;
    if (!header_reader.ReadU32(checksum)) {
        framed.result = FramingResult::Malformed;
        framed.failure = "cannot read checksum";
        return framed;
    }

    // Do not bother with header_reader.Finish(); we know it consumed exactly 14.

    const size_t total_size = HEADER_SIZE + length;
    if (buffer.size() < total_size) {
        return framed;  // NeedMore
    }

    // Copy the payload.
    ByteVec payload(length);
    std::memcpy(payload.data(), buffer.data() + HEADER_SIZE, length);

    // Checksum validation.
    if (!ValidateChecksum(payload, checksum)) {
        framed.result = FramingResult::Malformed;
        framed.failure = "checksum mismatch";
        return framed;
    }

    framed.result = FramingResult::Complete;
    framed.message.command = static_cast<Command>(cmd_raw);
    framed.message.payload = std::move(payload);
    framed.consumed = total_size;
    return framed;
}

ByteVec SerialiseMessage(Command command, ByteSpan payload,
                          const std::array<uint8_t, 4>& magic) {
    Writer writer(HEADER_SIZE + payload.size());

    // Magic.
    writer.WriteBytes(magic);

    // Command as u16.
    writer.WriteU16(static_cast<uint16_t>(command));

    // Length of payload.
    writer.WriteU32(static_cast<uint32_t>(payload.size()));

    // Checksum: first 4 bytes of DoubleSha256(payload).
    uint32_t checksum = 0;
    if (payload.size() > 0) {
        const Hash256 hash = DoubleSha256(payload);
        std::memcpy(&checksum, hash.Data(), sizeof(checksum));
    }
    writer.WriteU32(checksum);

    // Payload.
    writer.WriteBytes(payload);

    return writer.Take();
}

}  // namespace amarian::net