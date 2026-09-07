/// \file
/// Headers-first synchronisation state machine.

#include <amarian/net/sync.hpp>

#include <amarian/chain/block_index.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/consensus/validation.hpp>
#include <amarian/net/protocol.hpp>

#include <vector>

namespace amarian::net {

namespace {

/// Build a locator from the best header chain known.
/// Starts at the current tip and goes back exponentially (every 2nd, 4th, 8th...)
/// to the genesis level. This is what gives a peer enough information to find
/// the fork even if the two chains diverged far back.
[[nodiscard]] std::vector<Hash256> BuildLocator(const chain::BlockIndex& index) {
    std::vector<Hash256> locator;
    const chain::BlockIndexEntry* best = index.BestHeader();
    if (!best) {
        return locator;
    }

    uint32_t step = 1;
    locator.push_back(best->hash);
    const chain::BlockIndexEntry* entry = best->parent;

    while (entry && entry->height > 0) {
        locator.push_back(entry->hash);
        if (locator.size() >= 10) {
            step *= 2;
        }
        uint32_t target_height = (entry->height > step) ? entry->height - step : 0;
        entry = entry->Ancestor(target_height);
    }

    locator.push_back(index.Genesis().hash);
    return locator;
}

}  // namespace

Sync::Sync(const chain::BlockIndex& index, const ChainParams& params,
           const SyncCallbacks& callbacks)
    : index_(index), callbacks_(callbacks) {
    (void)params;
}

void Sync::Start() {
    GetHeadersPayload getheaders;
    getheaders.protocol_version = 1;
    getheaders.locator = BuildLocator(index_);

    Writer writer;
    getheaders.Serialize(writer);
    ByteVec payload = writer.Take();
    if (callbacks_.send_message) {
        callbacks_.send_message(Command::GetHeaders, payload);
    }
    state_ = SyncState::AwaitingHeaders;
}

std::vector<Hash256> Sync::OnHeaders(const HeadersPayload& headers) {
    std::vector<Hash256> blocks_to_request;

    if (headers.headers.empty()) {
        state_ = SyncState::Synced;
        return blocks_to_request;
    }

    // Validate each header in sequence. The index is const here — headers are
    // added externally via the peer. We just report what to request.
    

    for (const auto& hdr : headers.headers) {
        // Check if the chain has a candidate with more work.
        // We rely on the external caller to actually add headers to the index.
        remote_tip_ = Hash256{};  // placeholder

        // Mark blocks needed.
        const chain::BlockIndexEntry* existing = index_.Find(hdr.Hash());
        if (!existing) {
            // Need to request this block.
            blocks_to_request.push_back(hdr.Hash());
        } else if (existing->total_work > index_.BestHeader()->total_work) {
            remote_tip_ = existing->hash;
        }
    }

    // Send another getheaders if we found new work (the caller handles
    // actual header validation externally).
    GetHeadersPayload getheaders;
    getheaders.protocol_version = 1;
    getheaders.locator = BuildLocator(index_);
    Writer writer;
    getheaders.Serialize(writer);
    ByteVec payload = writer.Take();
    if (callbacks_.send_message) {
        callbacks_.send_message(Command::GetHeaders, payload);
    }
    state_ = SyncState::AwaitingHeaders;

    return blocks_to_request;
}

void Sync::OnBlockReceived() {
    if (inflight_count_ > 0) {
        --inflight_count_;
    }
    if (pending_blocks_.empty() && inflight_count_ == 0) {
        state_ = SyncState::Synced;
    }
}

}  // namespace amarian::net
