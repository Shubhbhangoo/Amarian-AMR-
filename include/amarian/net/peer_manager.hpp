#pragma once

/// \file
/// Peer connection management: outbound connections, address book, and reconnect.
///
/// The peer manager owns the Asio `io_context` and the set of connected peers.
/// It is the P2P side of the same event loop the RPC server lives in, and shares
/// its thread.
///
/// Phase 4: outbound-only connections, configurable via `--connect`. Full address
/// gossip, bucketed address tables (tried/new), and DNS seeds arrive later.

#include <amarian/chain/chain_state.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/net/handshake.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace amarian::net {

/// Configuration for the peer manager.
struct NetConfig {
    /// Peers to connect to on startup. Required until DNS seeds land.
    std::vector<std::string> connect;

    /// The network this node belongs to.
    const ChainParams* params = nullptr;

    /// The chain state, for sync decisions.
    chain::ChainState* state = nullptr;

    /// Maximum outbound connections.
    size_t max_outbound = 8;

    /// Seconds between reconnection attempts to a failed peer.
    std::chrono::seconds reconnect_delay{30};
};

/// Manages all P2P connections.
///
/// One per node, created by the daemon and given the same `io_context` the RPC
/// server uses. Thread-compatible: all methods are called from the single event
/// loop thread.
class PeerManager {
public:
    explicit PeerManager(const NetConfig& config);
    ~PeerManager();

    PeerManager(const PeerManager&) = delete;
    PeerManager& operator=(const PeerManager&) = delete;
    PeerManager(PeerManager&&) = delete;
    PeerManager& operator=(PeerManager&&) = delete;

    /// Start connecting to configured peers. Called after the event loop starts.
    void Start();

    /// Stop all connections. Safe to call from any thread.
    void Stop();

    /// Whether any outbound connection has completed the handshake.
    [[nodiscard]] bool HasActivePeer() const noexcept;

    /// Number of connected peers.
    [[nodiscard]] size_t PeerCount() const noexcept;

    /// The best height among connected peers (for display / sync decisions).
    [[nodiscard]] uint32_t BestPeerHeight() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace amarian::net