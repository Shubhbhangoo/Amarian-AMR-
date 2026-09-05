#include <amarian/chain/block_store.hpp>

#include <amarian/primitives/block.hpp>
#include <amarian/util/types.hpp>
#include <amarian/utxo/connect.hpp>

#include <optional>
#include <utility>

namespace amarian::chain {

bool MemoryBlockStore::HaveBlock(const Hash256& hash) const {
    return blocks_.contains(hash);
}

std::optional<Block> MemoryBlockStore::GetBlock(const Hash256& hash) const {
    const auto found = blocks_.find(hash);
    if (found == blocks_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<utxo::BlockUndo> MemoryBlockStore::GetUndo(const Hash256& hash) const {
    const auto found = undo_.find(hash);
    if (found == undo_.end()) {
        return std::nullopt;
    }
    return found->second;
}

void MemoryBlockStore::PutBlock(const Hash256& hash, const Block& block) {
    // `emplace` rather than `insert_or_assign`: a body already under this hash hashes to
    // this hash, so it is the same block, and overwriting it would be work with no effect.
    blocks_.emplace(hash, block);
}

void MemoryBlockStore::PutUndo(const Hash256& hash, const utxo::BlockUndo& undo) {
    undo_.insert_or_assign(hash, undo);
}

}  // namespace amarian::chain
