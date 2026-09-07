#pragma once

/// \file
/// Peer connection management: outbound connections, address book, and reconnect.
///
/// The peer manager owns the Asio `io_context`, listener, and set of connected peers.
/// It runs its event loop on one dedicated thread so the RPC listener remains
/// responsive while peer sockets are active.
///
/// Phase 4: inbound and outbound connections, configurable via `--p2p-port` and
/// `--connect`. Full address gossip, bucketed address tables (tried/new), and DNS
/// seeds arrive later.

#include <amarian/chain/chain_state.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/net/handshake.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
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

    /// Node-layer mempool hooks. The network layer never decides transaction
    /// validity; it asks the owner to accept or retrieve a transaction.
    std::function<bool(const Transaction&)> accept_transaction;
    std::function<std::optional<Transaction>(const Hash256&)> get_transaction;
    /// Called after a received block changes the active chain.
    std::function<void()> chain_changed;

    /// Maximum outbound connections.
    size_t max_outbound = 8;

    /// Maximum inbound connections. Inbound peers never displace configured
    /// outbound peers; excess sockets are closed immediately.
    size_t max_inbound = 32;

    /// Local TCP port for inbound peers. Zero disables the listener.
    uint16_t p2p_port = 0;

    /// Seconds between reconnection attempts to a failed peer.
    std::chrono::seconds reconnect_delay{30};

    /// Optional persistent address-book file. Empty disables persistence.
    std::filesystem::path address_book;

    /// Misbehavior points required for a temporary endpoint ban.
    int ban_threshold = 100;

    /// How long a scored endpoint remains banned.
    std::chrono::seconds ban_duration{std::chrono::hours{24}};
};

/// Bytes transferred by the peer manager's live sockets.
struct TrafficStats {
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;
};

/// Manages all P2P connections.
///
/// One per node, created by the daemon. Socket callbacks run on the manager's
/// dedicated event-loop thread; lifecycle calls are made by the owner thread.
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

    /// Return transport bytes counted since this manager started.
    [[nodiscard]] TrafficStats Traffic() const noexcept;

    /// Announce a locally accepted block to connected peers. Peers request the
    /// body with getdata, so announcements never bypass validation.
    void AnnounceBlock(const Hash256& hash);

    /// Announce a locally accepted transaction to connected peers.
    void AnnounceTransaction(const Hash256& hash);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace amarian::net
