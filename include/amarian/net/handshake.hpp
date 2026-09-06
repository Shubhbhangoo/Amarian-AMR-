#pragma once

/// \file
/// The P2P handshake state machine.
#include <cassert>
///
/// The handshake follows the sequence from NETWORK.md:
///
///   1. Outbound peer: send Hello, expect Hello.
///      Inbound peer: receive Hello, respond Hello.
///   2. Both sides: after receiving Hello, send HelloAck.
///   3. Both sides: after receiving HelloAck, the handshake is complete.
///
/// Before the handshake is complete, **no other message type is processed**.
/// A peer that sends anything else is disconnected immediately, which removes
/// a class of state-machine confusion before it can exist.

#include <amarian/net/protocol.hpp>
#include <amarian/consensus/params.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace amarian::net {

/// What the handshake machine knows about a peer. Produced by the exchange.
struct PeerHello {
    uint32_t protocol_version = 0;
    ServiceFlags services = NODE_NONE;
    int64_t timestamp = 0;
    Hash256 chain_id;
    uint32_t best_height = 0;
    uint64_t nonce = 0;
    std::string user_agent;
};

/// State of a single handshake.
enum class HandshakeState {
    /// Waiting to send Hello. Only the outbound side enters this.
    AwaitingSendHello,
    /// Sent Hello, waiting to receive Hello from the peer.
    AwaitingReceiveHello,
    /// Received Hello, sent HelloAck, waiting for HelloAck.
    AwaitingReceiveHelloAck,
    /// Both Hello and HelloAck have crossed. The connection is live.
    Complete,
    /// The handshake failed irrecoverably (wrong chain_id, our own nonce).
    Failed,
};

/// A single-peer handshake machine.
///
/// Call `OnSendHello()` / `OnReceiveHello()` / `OnSendHelloAck()` /
/// `OnReceiveHelloAck()` in the order the events actually happen.
///
/// Thread-compatible: one handshake per connection, called from one thread.
class Handshake {
public:
    explicit Handshake(const ChainParams& params, uint64_t local_nonce);

    HandshakeState State() const noexcept { return state_; }

    /// Returns the Hello payload to send for an outbound connection.
    [[nodiscard]] HelloPayload BuildHello() const;

    /// Returns the HelloAck payload to send (empty payload).
    [[nodiscard]] static HelloAckPayload BuildHelloAck();

    /// Processes a received Hello. Returns the HelloAck that should be sent back.
    /// On failure, State() becomes Failed and the caller should disconnect.
    [[nodiscard]] std::optional<HelloAckPayload> ReceiveHello(const HelloPayload& hello);

    /// Called when Hello has been sent (outbound) or received+answered (inbound).
    void OnSendHello() noexcept;
    void OnSendHelloAck() noexcept;

    /// Records that Hello arrived. Inbound connections start here.
    void OnReceiveHello() noexcept;

    /// Records that HelloAck arrived. Completes the handshake.
    bool OnReceiveHelloAck() noexcept;

    /// The peer's identity after a successful handshake.
    [[nodiscard]] const PeerHello& Peer() const {
        assert(state_ == HandshakeState::Complete);
        return peer_;
    }

    [[nodiscard]] const std::string& FailureReason() const noexcept { return failure_reason_; }

private:
    const ChainParams& params_;
    uint64_t local_nonce_;
    HandshakeState state_ = HandshakeState::AwaitingSendHello;
    PeerHello peer_;
    std::string failure_reason_;
};

}  // namespace amarian::net