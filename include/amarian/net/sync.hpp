#pragma once

/// \file
/// Headers-first chain synchronisation.
///
/// Implements the sync algorithm from NETWORK.md:
///
///   1. Send getheaders with a locator pointing to the current tip.
///   2. Validate received headers: connect to a known header, check target_bits
///      matches the node's own retarget, check the hash meets the target, check
///      the timestamp rules.
///   3. Once a header chain demonstrates more accumulated work than the current
///      tip, request its blocks from peers.
///   4. Blocks are validated and connected; on success, the tip advances.
///
/// This file is the sync state machine; it does not manage sockets.

#include <amarian/chain/block_index.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/net/message.hpp>
#include <amarian/net/protocol.hpp>

#include <functional>
#include <vector>

namespace amarian::net {

/// Where the sync state machine is.
enum class SyncState {
    /// Waiting for getheaders request to be sent.
    AwaitingGetHeaders,
    /// Waiting for headers response.
    AwaitingHeaders,
    /// Validating received headers against the local chain.
    ValidatingHeaders,
    /// Requesting blocks that are worth downloading.
    DownloadingBlocks,
    /// Synchronised with this peer; no more to do.
    Synced,
};

/// Callbacks the sync state machine uses to talk to the network.
struct SyncCallbacks {
    /// Send a message to the peer.
    std::function<void(Command, ByteVec)> send_message;
    /// Called when a block should be connected to the chain.
    std::function<void(const std::vector<uint8_t>&)> connect_block;
};

/// Headers-first sync logic.
///
/// This is a pure state machine driven by received messages.
/// It does not own sockets; it just decides what to send next.
class Sync {
public:
    explicit Sync(const chain::BlockIndex& index, const ChainParams& params,
                  const SyncCallbacks& callbacks);

    /// Begin synchronisation: send getheaders.
    void Start();

    /// Handle a received headers message.
    /// Returns the blocks that should be requested (hashes).
    std::vector<Hash256> OnHeaders(const HeadersPayload& headers);

    /// Handle a received block (validation result reported externally).
    void OnBlockReceived();

    /// Current sync state.
    [[nodiscard]] SyncState State() const noexcept { return state_; }

    /// The best tip the sync has seen from this peer.
    [[nodiscard]] Hash256 RemoteTip() const noexcept { return remote_tip_; }

private:
    const chain::BlockIndex& index_;
    SyncCallbacks callbacks_;
    SyncState state_ = SyncState::AwaitingGetHeaders;
    Hash256 remote_tip_;
    /// Hashes of blocks we've requested but not yet received.
    std::vector<Hash256> pending_blocks_;
    /// Blocks whose download is in progress.
    size_t inflight_count_ = 0;
};

}  // namespace amarian::net
