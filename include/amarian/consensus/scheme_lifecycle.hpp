#pragma once

/// \file
/// Scheme lifecycle management: activation, deprecation, and retirement heights.
///
/// Each signature scheme in the registry can have a lifecycle timeline:
///
///   - **activation_height**: the first block at which this scheme may be used
///     to create new locks. Before this height, an unknown scheme is treated as
///     spendable (soft-fork rule) - no node creates outputs under it.
///   - **deprecation_height**: the first block at which using this scheme for
///     *new* locks is discouraged (relay policy refuses, but consensus still
///     accepts spends). Existing UTXOs remain spendable.
///   - **retirement_height**: the first block at which this scheme is no longer
///     accepted for new locks at all (consensus refuses to create outputs).
///     Existing UTXOs remain spendable forever - retirement never invalidates
///     an existing coin.
///
/// ## Emergency migration
///
/// If a scheme is broken (e.g. a cryptographic break), an emergency migration
/// can be triggered by a supermajority of miners signalling a new activation
/// height for a replacement scheme. The old scheme's retirement is accelerated
/// by the same mechanism. This is a social process recorded in the consensus
/// parameters, not an automated action.

#include <amarian/crypto/signature.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace amarian::consensus {

/// The lifecycle state of a single scheme at a given block height.
enum class SchemeState : uint8_t {
    /// Not yet active. The scheme identifier is known but no node may create
    /// outputs under it. An existing output locked to this scheme (from a
    /// previous activation) is still spendable.
    Pending,
    /// Active. New outputs may be created and spent.
    Active,
    /// Deprecated. New outputs are discouraged by relay policy but consensus
    /// still accepts spends of existing UTXOs.
    Deprecated,
    /// Retired. New outputs are refused by consensus. Existing UTXOs remain
    /// spendable forever - retirement never confiscates coins.
    Retired,
};

/// One scheme's lifecycle timeline.
struct SchemeLifecycle {
    uint16_t scheme_id;
    /// The height at which this scheme becomes active. 0 = active from genesis.
    uint32_t activation_height;
    /// The height at which this scheme is deprecated. 0xFFFFFFFF = never deprecated.
    uint32_t deprecation_height;
    /// The height at which this scheme is retired. 0xFFFFFFFF = never retired.
    uint32_t retirement_height;
};

/// The lifecycle registry.
///
/// Built from the scheme table and the network's lifecycle parameters.
/// Immutable after construction: a node's lifecycle policy is fixed at startup.
class SchemeLifecycleRegistry {
public:
    /// Builds the default lifecycle registry for a network.
    /// All currently registered schemes are active from genesis.
    explicit SchemeLifecycleRegistry();

    /// Returns the state of a scheme at a given block height.
    [[nodiscard]] SchemeState GetState(uint16_t scheme_id, uint32_t height) const noexcept;

    /// Whether a new output may be created under this scheme at this height.
    /// False for pending, deprecated, and retired schemes.
    [[nodiscard]] bool MayCreateOutput(uint16_t scheme_id, uint32_t height) const noexcept;

    /// Whether an existing output under this scheme is still spendable.
    /// True for all states except reserved (scheme 0).
    [[nodiscard]] bool IsSpendable(uint16_t scheme_id, uint32_t height) const noexcept;

    /// Registers a custom lifecycle for testing.
    void Register(const SchemeLifecycle& lifecycle);

    /// Returns the lifecycle for a scheme, or nullopt if unknown.
    [[nodiscard]] std::optional<SchemeLifecycle> GetLifecycle(uint16_t scheme_id) const noexcept;

    /// The number of managed schemes.
    [[nodiscard]] size_t Size() const noexcept { return entries_.size(); }

private:
    std::vector<SchemeLifecycle> entries_;
};

}  // namespace amarian::consensus
