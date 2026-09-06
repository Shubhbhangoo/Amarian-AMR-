/// \file
/// Tests for P2P message framing, protocol serialisation, and the handshake.

#include <amarian/net/message.hpp>
#include <amarian/net/protocol.hpp>
#include <amarian/net/handshake.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/util/types.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace amarian::net {
namespace {

// --- Framing tests ---------------------------------------------------------

TEST(NetMessage, HeaderSizeIs14) {
    EXPECT_EQ(HEADER_SIZE, 14);
}

TEST(NetMessage, SerialiseAndFrameRoundTrip) {
    ByteVec payload = {0x01, 0x02, 0x03, 0x04};
    ByteVec wire = SerialiseMessage(Command::Hello, payload, REGTEST_PARAMS.magic);
    EXPECT_EQ(wire.size(), HEADER_SIZE + payload.size());

    Framed framed = FrameMessage(wire, REGTEST_PARAMS.magic);
    ASSERT_EQ(framed.result, FramingResult::Complete);
    EXPECT_EQ(framed.message.command, Command::Hello);
    EXPECT_EQ(framed.message.payload, payload);
    EXPECT_EQ(framed.consumed, wire.size());
}

TEST(NetMessage, FrameMessageNeedsMoreForPartialHeader) {
    std::array<uint8_t, 4> partial = {0x80, 0xB9, 0xD4, 0xC0};  // just the magic
    Framed framed = FrameMessage(partial, REGTEST_PARAMS.magic);
    EXPECT_EQ(framed.result, FramingResult::NeedMore);
}

TEST(NetMessage, FrameMessageRefusesBadMagic) {
    ByteVec payload = {0x01};
    ByteVec wire = SerialiseMessage(Command::Hello, payload, REGTEST_PARAMS.magic);
    // Corrupt the first byte of magic.
    wire[0] = 0xFF;
    Framed framed = FrameMessage(wire, REGTEST_PARAMS.magic);
    EXPECT_EQ(framed.result, FramingResult::Malformed);
    EXPECT_FALSE(framed.failure.empty());
}

TEST(NetMessage, FrameMessageRefusesUnknownCommand) {
    ByteVec payload = {0x01};
    // Serialise with a known command, then twiddle the command bytes.
    ByteVec wire = SerialiseMessage(Command::Hello, payload, REGTEST_PARAMS.magic);
    // Command is at offset 4 (after 4 bytes of magic).
    wire[4] = 0xFF;  // invalid command id
    wire[5] = 0xFF;
    Framed framed = FrameMessage(wire, REGTEST_PARAMS.magic);
    EXPECT_EQ(framed.result, FramingResult::Malformed);
}

TEST(NetMessage, FrameMessageRefusesOversized) {
    Writer writer;
    writer.WriteBytes(ByteSpan(REGTEST_PARAMS.magic));
    writer.WriteU16(static_cast<uint16_t>(Command::Hello));
    writer.WriteU32(MAX_MESSAGE_SIZE + 1);  // exceeds limit
    writer.WriteU32(0);                      // checksum (won't be checked)
    ByteVec wire = writer.Take();
    wire.resize(HEADER_SIZE, 0);
    Framed framed = FrameMessage(wire, REGTEST_PARAMS.magic);
    EXPECT_EQ(framed.result, FramingResult::Malformed);
}

TEST(NetMessage, FrameMessageChecksumMismatch) {
    ByteVec payload = {0x01, 0x02, 0x03};
    ByteVec wire = SerialiseMessage(Command::Hello, payload, REGTEST_PARAMS.magic);
    // Corrupt one byte of payload.
    wire[HEADER_SIZE] = 0xFF;
    Framed framed = FrameMessage(wire, REGTEST_PARAMS.magic);
    EXPECT_EQ(framed.result, FramingResult::Malformed);
}

TEST(NetMessage, FrameMessageAcceptsEmptyPayload) {
    ByteVec wire = SerialiseMessage(Command::HelloAck, {}, REGTEST_PARAMS.magic);
    EXPECT_EQ(wire.size(), HEADER_SIZE);
    Framed framed = FrameMessage(wire, REGTEST_PARAMS.magic);
    ASSERT_EQ(framed.result, FramingResult::Complete);
    EXPECT_TRUE(framed.message.payload.empty());
}

TEST(NetMessage, FrameMessageHandlesMultipleMessages) {
    ByteVec payload1 = {0xAA};
    ByteVec payload2 = {0xBB};
    ByteVec msg1 = SerialiseMessage(Command::Ping, payload1, REGTEST_PARAMS.magic);
    ByteVec msg2 = SerialiseMessage(Command::Pong, payload2, REGTEST_PARAMS.magic);
    ByteVec combined = msg1;
    combined.insert(combined.end(), msg2.begin(), msg2.end());

    Framed framed = FrameMessage(combined, REGTEST_PARAMS.magic);
    ASSERT_EQ(framed.result, FramingResult::Complete);
    EXPECT_EQ(framed.message.command, Command::Ping);
    EXPECT_EQ(framed.message.payload, payload1);
    EXPECT_EQ(framed.consumed, msg1.size());
}

// --- Checksum tests --------------------------------------------------------

TEST(NetMessage, ValidateChecksumAcceptsCorrectChecksum) {
    ByteVec payload = {0x01, 0x02, 0x03, 0x04};
    ByteVec wire = SerialiseMessage(Command::Hello, payload, REGTEST_PARAMS.magic);
    // Read the checksum from the wire.
    uint32_t checksum = 0;
    std::memcpy(&checksum, wire.data() + 10, sizeof(checksum));
    EXPECT_TRUE(ValidateChecksum(payload, checksum));
}

TEST(NetMessage, ValidateChecksumRejectsWrongChecksum) {
    ByteVec payload = {0x01, 0x02, 0x03, 0x04};
    EXPECT_FALSE(ValidateChecksum(payload, 0xDEADBEEF));
}

TEST(NetMessage, ValidateChecksumAcceptsEmpty) {
    EXPECT_TRUE(ValidateChecksum({}, 0));
}

// --- CommandName tests -----------------------------------------------------

TEST(NetMessage, CommandNameIsNotEmpty) {
    for (uint16_t i = 0; i < static_cast<uint16_t>(Command::Pong) + 1; ++i) {
        std::string_view name = CommandName(static_cast<Command>(i));
        EXPECT_FALSE(name.empty());
    }
}

TEST(NetMessage, CommandNameForUnknownIsUnknown) {
    EXPECT_EQ(CommandName(static_cast<Command>(99)), "unknown");
}

// --- Handshake tests -------------------------------------------------------

TEST(NetHandshake, OutboundExchangeCompletes) {
    const uint64_t nonce_a = 0x0102030405060708;
    const uint64_t nonce_b = 0x090A0B0C0D0E0F00;

    Handshake handshake_a(REGTEST_PARAMS, nonce_a);
    Handshake handshake_b(REGTEST_PARAMS, nonce_b);

    // A builds Hello, sends it.
    HelloPayload hello_a = handshake_a.BuildHello();
    EXPECT_EQ(hello_a.chain_id, REGTEST_PARAMS.chain_id);
    EXPECT_EQ(hello_a.nonce, nonce_a);
    handshake_a.OnSendHello();

    // B receives A's Hello.
    auto ack_from_b = handshake_b.ReceiveHello(hello_a);
    ASSERT_TRUE(ack_from_b.has_value());
    EXPECT_EQ(handshake_b.State(), HandshakeState::AwaitingReceiveHelloAck);
    EXPECT_EQ(handshake_b.Peer().nonce, nonce_a);

    // B sends Hello back (outbound path — build and send).
    HelloPayload hello_b = handshake_b.BuildHello();
    handshake_b.OnSendHello();
    EXPECT_EQ(hello_b.nonce, nonce_b);

    // A receives B's Hello.
    auto ack_from_a = handshake_a.ReceiveHello(hello_b);
    ASSERT_TRUE(ack_from_a.has_value());
    EXPECT_EQ(handshake_a.State(), HandshakeState::AwaitingReceiveHelloAck);

    // A sends HelloAck, B sends HelloAck.
    handshake_a.OnSendHelloAck();
    handshake_b.OnSendHelloAck();

    // A receives HelloAck from B.
    handshake_a.OnReceiveHelloAck();
    // B receives HelloAck from A.
    handshake_b.OnReceiveHelloAck();

    EXPECT_EQ(handshake_a.State(), HandshakeState::Complete);
    EXPECT_EQ(handshake_b.State(), HandshakeState::Complete);

    EXPECT_EQ(handshake_a.Peer().nonce, nonce_b);
    EXPECT_EQ(handshake_b.Peer().nonce, nonce_a);
}

TEST(NetHandshake, RefusesWrongChainId) {
    Handshake handshake_a(REGTEST_PARAMS, 0x01);
    Handshake handshake_b(MAINNET_PARAMS, 0x02);  // different network

    HelloPayload hello = handshake_a.BuildHello();
    auto result = handshake_b.ReceiveHello(hello);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(handshake_b.State(), HandshakeState::Failed);
    EXPECT_FALSE(handshake_b.FailureReason().empty());
}

TEST(NetHandshake, DetectsSelfConnection) {
    const uint64_t nonce = 0x1234;
    Handshake handshake_a(REGTEST_PARAMS, nonce);
    Handshake handshake_b(REGTEST_PARAMS, nonce);  // same nonce

    HelloPayload hello = handshake_a.BuildHello();
    auto result = handshake_b.ReceiveHello(hello);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(handshake_b.State(), HandshakeState::Failed);
}

TEST(NetHandshake, RefusesOldProtocolVersion) {
    Handshake handshake(REGTEST_PARAMS, 0x01);
    HelloPayload hello;
    hello.protocol_version = 0;  // too old
    hello.chain_id = REGTEST_PARAMS.chain_id;
    hello.nonce = 0xDEAD;

    auto result = handshake.ReceiveHello(hello);
    EXPECT_FALSE(result.has_value());
}

TEST(NetHandshake, InboundPathCompletes) {
    // Inbound: a peer sends Hello before we do.
    const uint64_t local_nonce = 0x01;
    const uint64_t peer_nonce = 0x02;

    Handshake handshake(REGTEST_PARAMS, local_nonce);

    // We receive the peer's Hello (inbound).
    HelloPayload peer_hello;
    peer_hello.protocol_version = 1;
    peer_hello.chain_id = REGTEST_PARAMS.chain_id;
    peer_hello.nonce = peer_nonce;

    auto ack = handshake.ReceiveHello(peer_hello);
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(handshake.State(), HandshakeState::AwaitingReceiveHelloAck);

    // Send HelloAck
    handshake.OnSendHelloAck();
    // Receive HelloAck
    handshake.OnReceiveHelloAck();

    EXPECT_EQ(handshake.State(), HandshakeState::Complete);
    EXPECT_EQ(handshake.Peer().nonce, peer_nonce);
}

// --- Protocol serialisation tests ------------------------------------------

TEST(NetProtocol, HelloSerialisesAndDeserialises) {
    HelloPayload original;
    original.protocol_version = 1;
    original.services = NODE_HISTORICAL | NODE_RELAY;
    original.timestamp = 1000;
    original.chain_id = REGTEST_PARAMS.chain_id;
    original.best_height = 42;
    original.nonce = 0xDEADBEEF;
    original.user_agent = "amarian/0.1";

    Writer writer;
    original.Serialize(writer);
    ByteVec encoded = writer.Take();

    Reader reader(encoded);
    HelloPayload decoded;
    ASSERT_TRUE(HelloPayload::Deserialize(reader, decoded, 256));
    EXPECT_TRUE(reader.Finish());

    EXPECT_EQ(decoded.protocol_version, original.protocol_version);
    EXPECT_EQ(decoded.services, original.services);
    EXPECT_EQ(decoded.timestamp, original.timestamp);
    EXPECT_EQ(decoded.chain_id, original.chain_id);
    EXPECT_EQ(decoded.best_height, original.best_height);
    EXPECT_EQ(decoded.nonce, original.nonce);
    EXPECT_EQ(decoded.user_agent, original.user_agent);
}

TEST(NetProtocol, HelloAckSerialisesAndDeserialises) {
    Writer writer;
    HelloAckPayload empty;
    empty.Serialize(writer);
    ByteVec encoded = writer.Take();
    EXPECT_TRUE(encoded.empty());

    Reader reader(encoded);
    HelloAckPayload decoded;
    EXPECT_TRUE(HelloAckPayload::Deserialize(reader, decoded));
}

TEST(NetProtocol, GetHeadersSerialisesAndDeserialises) {
    GetHeadersPayload original;
    original.protocol_version = 1;
    original.locator = {REGTEST_PARAMS.genesis_hash};
    original.stop_hash = Hash256{};

    Writer writer;
    original.Serialize(writer);
    ByteVec encoded = writer.Take();

    Reader reader(encoded);
    GetHeadersPayload decoded;
    ASSERT_TRUE(GetHeadersPayload::Deserialize(reader, decoded, 1000));
    EXPECT_TRUE(reader.Finish());
    EXPECT_EQ(decoded.protocol_version, original.protocol_version);
    ASSERT_EQ(decoded.locator.size(), 1);
    EXPECT_EQ(decoded.locator[0], original.locator[0]);
    EXPECT_TRUE(decoded.stop_hash.IsZero());
}

TEST(NetProtocol, PingPongSerialisesAndDeserialises) {
    PingPayload original;
    original.nonce = 0xABCD;

    Writer writer;
    original.Serialize(writer);
    ByteVec encoded = writer.Take();

    Reader reader(encoded);
    PingPayload decoded;
    ASSERT_TRUE(PingPayload::Deserialize(reader, decoded));
    EXPECT_TRUE(reader.Finish());
    EXPECT_EQ(decoded.nonce, original.nonce);
}

TEST(NetProtocol, NetworkAddressRoundTrips) {
    NetworkAddress addr;
    addr.addr_ver = 4;
    addr.address = {10, 0, 0, 1};  // zero-padded IPv4
    addr.address[15] = 1;
    addr.port = 12520;

    Writer writer;
    addr.Serialize(writer);
    ByteVec encoded = writer.Take();

    Reader reader(encoded);
    NetworkAddress decoded;
    ASSERT_TRUE(NetworkAddress::Deserialize(reader, decoded));
    EXPECT_TRUE(reader.Finish());
    EXPECT_EQ(decoded.addr_ver, addr.addr_ver);
    EXPECT_EQ(decoded.port, addr.port);
    EXPECT_EQ(decoded.address[15], 1);
}

}  // namespace
}  // namespace amarian::net