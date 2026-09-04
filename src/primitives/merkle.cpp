#include <amarian/crypto/hash.hpp>
#include <amarian/primitives/merkle.hpp>
#include <amarian/util/serialize.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace amarian {
namespace {

constexpr std::string_view MERKLE_LEAF_TAG = "Amarian/MerkleLeaf";
constexpr std::string_view MERKLE_BRANCH_TAG = "Amarian/MerkleBranch";
constexpr std::string_view MERKLE_ROOT_TAG = "Amarian/MerkleRoot";

[[nodiscard]] Hash256 MerkleBranch(const Hash256& left, const Hash256& right) {
    std::array<uint8_t, Hash256::SIZE * 2> bytes{};
    std::copy(left.Array().begin(), left.Array().end(), bytes.begin());
    std::copy(right.Array().begin(), right.Array().end(), bytes.begin() + Hash256::SIZE);
    return TaggedHash(MERKLE_BRANCH_TAG, bytes);
}

}  // namespace

std::optional<Hash256> ComputeMerkleRoot(std::span<const Hash256> wtxids) {
    if (wtxids.empty()) {
        return std::nullopt;
    }

    std::vector<Hash256> level;
    level.reserve(wtxids.size());
    for (const Hash256& wtxid : wtxids) {
        level.push_back(TaggedHash(MERKLE_LEAF_TAG, wtxid.Span()));
    }

    while (level.size() > 1) {
        std::vector<Hash256> next;
        next.reserve((level.size() + 1U) / 2U);
        for (size_t index = 0; index + 1U < level.size(); index += 2U) {
            next.push_back(MerkleBranch(level[index], level[index + 1U]));
        }
        if (level.size() % 2U != 0U) {
            next.push_back(level.back());
        }
        level = std::move(next);
    }

    Writer root_preimage(CompactSizeLen(wtxids.size()) + Hash256::SIZE);
    root_preimage.WriteCompactSize(wtxids.size());
    root_preimage.WriteHash256(level.front());
    return TaggedHash(MERKLE_ROOT_TAG, root_preimage.Bytes());
}

}  // namespace amarian
