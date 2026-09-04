#pragma once

/// \file
/// Domain-separated, count-committing Merkle roots for block transactions.

#include <amarian/util/types.hpp>

#include <optional>
#include <span>

namespace amarian {

/// Computes the block Merkle commitment for transaction wtxids in block order.
///
/// Each ID is first hashed under `Amarian/MerkleLeaf`; pairs are hashed under
/// `Amarian/MerkleBranch`; an odd node is promoted unchanged. Finally the root
/// and its compact-encoded leaf count are committed under `Amarian/MerkleRoot`.
/// Empty trees have no root and return nullopt.
[[nodiscard]] std::optional<Hash256> ComputeMerkleRoot(std::span<const Hash256> wtxids);

}  // namespace amarian
