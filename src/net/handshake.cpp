/// \file
/// Implementation of the P2P handshake state machine.

#include <amarian/net/handshake.hpp>

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace amarian::net {

Handshake::Handshake(const ChainParams& params, uint64_t local_nonce)
    : params_(params), local_nonce_(local_nonce) {
    // Outbound starts with AwaitingSendHello; inbound can call OnReceiveHello
    // to skip past that.
}

HelloPayload Handshake::BuildHello() const {
    HelloPayload hello;
    hello.protocol_version = 1;  // our protocol version
    hello.services = NODE_HISTORICAL | NODE_RELAY;
    hello.timestamp = params_.genesis_timestamp;  // placeholder; real node passes current time
    hello.chain_id = params_.chain_id;
    hello.best_height = 0;    // placeholder; caller fills this
    hello.nonce = local_nonce_;
    hello.user_agent = "amarian/0.1";
    return hello;
}

HelloAckPayload Handshake::BuildHelloAck() {
    return HelloAckPayload{};
}

void Handshake::OnSendHello() noexcept {
    if (state_ == HandshakeState::AwaitingSendHello) {
        state_ = HandshakeState::AwaitingReceiveHello;
    }
}

void Handshake::OnReceiveHello() noexcept {
    if (state_ == HandshakeState::AwaitingSendHello) {
        // Inbound path: peer's Hello arrived before we sent ours.
        state_ = HandshakeState::AwaitingReceiveHelloAck;
    } else if (state_ == HandshakeState::AwaitingReceiveHello) {
        // Outbound path: Hello we sent was answered with a Hello.
        state_ = HandshakeState::AwaitingReceiveHelloAck;
    }
}

std::optional<HelloAckPayload> Handshake::ReceiveHello(const HelloPayload& hello) {
    // Chain_id check (second-line separation, after magic).
    if (hello.chain_id != params_.chain_id) {
        failure_reason_ = "peer is on a different network";
        state_ = HandshakeState::Failed;
        return std::nullopt;
    }

    // Self-connection detection.
    if (hello.nonce == local_nonce_) {
        failure_reason_ = "connected to self";
        state_ = HandshakeState::Failed;
        return std::nullopt;
    }

    // Protocol version must be at least what we support.
    if (hello.protocol_version < 1) {
        failure_reason_ = "peer protocol version too old";
        state_ = HandshakeState::Failed;
        return std::nullopt;
    }

    peer_.protocol_version = hello.protocol_version;
    peer_.services = hello.services;
    peer_.timestamp = hello.timestamp;
    peer_.chain_id = hello.chain_id;
    peer_.best_height = hello.best_height;
    peer_.nonce = hello.nonce;
    peer_.user_agent = hello.user_agent;

    state_ = HandshakeState::AwaitingReceiveHelloAck;
    return HelloAckPayload{};
}

void Handshake::OnSendHelloAck() noexcept {
    // No state change; we wait for the peer's HelloAck.
}

bool Handshake::OnReceiveHelloAck() noexcept {
    if (state_ != HandshakeState::AwaitingReceiveHelloAck) {
        failure_reason_ = "received HelloAck at unexpected time";
        state_ = HandshakeState::Failed;
        return false;
    }
    state_ = HandshakeState::Complete;
    return true;
}

}  // namespace amarian::net