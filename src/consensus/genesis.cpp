#include <amarian/consensus/genesis.hpp>
#include <amarian/consensus/target.hpp>
#include <amarian/primitives/merkle.hpp>

#include <array>
#include <cstring>

namespace amarian {
namespace {

/// The genesis coinbase: no reward, an output nobody can ever spend, and the dated
/// reference in the bytes that take the place of a witness.
[[nodiscard]] Transaction BuildGenesisCoinbase(const ChainParams& params) {
    Transaction coinbase;
    coinbase.version = 1;
    coinbase.inputs.push_back(MakeCoinbaseInput(0));

    // Zero facets, and a lock that has no satisfying witness by rule rather than by
    // improbability. The program is the network's chain id, which is what makes this
    // block belong to this network and no other.
    const std::array<uint8_t, Hash256::SIZE>& chain_id = params.chain_id.Array();
    coinbase.outputs.push_back(TxOutput{
        .amount = 0,
        .lock = Lock{.version = LOCK_VERSION_UNSPENDABLE,
                     .program = ByteVec(chain_id.begin(), chain_id.end())},
    });

    coinbase.locktime = 0;
    coinbase.coinbase_data = ByteVec(GENESIS_MESSAGE.begin(), GENESIS_MESSAGE.end());
    return coinbase;
}

}  // namespace

Block BuildGenesisBlock(const ChainParams& params) {
    Block genesis;
    genesis.transactions.push_back(BuildGenesisCoinbase(params));

    genesis.header.version = 1;
    genesis.header.height = 0;
    genesis.header.prev_block = Hash256{};
    // The root is computed rather than recorded: recording it would be a second place
    // for the coinbase's contents to be described, and the two could disagree.
    const std::optional<Hash256> root = genesis.ComputeMerkleRoot();
    genesis.header.merkle_root = root.value_or(Hash256{});
    genesis.header.timestamp = params.genesis_timestamp;
    genesis.header.target_bits = params.genesis_bits;
    genesis.header.nonce = params.genesis_nonce;
    return genesis;
}

GenesisFault CheckGenesis(const ChainParams& params) {
    const std::optional<Target> target = CompactToTarget(params.genesis_bits);
    if (!target.has_value()) {
        return GenesisFault::MalformedTarget;
    }

    const Block genesis = BuildGenesisBlock(params);

    // The structural rules genesis is defined by. Later validation will check these
    // for every block, but genesis is the block that validation starts from, so it
    // cannot be the one block nobody checked.
    if (genesis.transactions.size() != 1) {
        return GenesisFault::MalformedBlock;
    }
    const Transaction& coinbase = genesis.transactions.front();
    if (!coinbase.IsCoinbase() || coinbase.outputs.size() != 1) {
        return GenesisFault::MalformedBlock;
    }
    if (coinbase.outputs.front().amount != 0 || !coinbase.outputs.front().lock.IsUnspendable()) {
        return GenesisFault::MalformedBlock;
    }
    if (genesis.header.height != 0 || !genesis.header.prev_block.IsZero()) {
        return GenesisFault::MalformedBlock;
    }
    if (genesis.header.merkle_root != genesis.ComputeMerkleRoot()) {
        return GenesisFault::MalformedBlock;
    }

    const Hash256 hash = genesis.Hash();
    if (hash != params.genesis_hash) {
        return GenesisFault::HashMismatch;
    }
    if (!HashMeetsTarget(hash, *target)) {
        return GenesisFault::InsufficientWork;
    }
    return GenesisFault::None;
}

std::string_view Describe(GenesisFault fault) noexcept {
    switch (fault) {
        case GenesisFault::None:
            return "ok";
        case GenesisFault::HashMismatch:
            return "genesis does not hash to the recorded value";
        case GenesisFault::MalformedTarget:
            return "genesis target_bits is not a valid compact target";
        case GenesisFault::InsufficientWork:
            return "genesis hash does not meet its own target";
        case GenesisFault::MalformedBlock:
            return "genesis does not have the shape genesis is defined to have";
    }
    return "unknown genesis fault";
}

std::optional<uint64_t>
FindGenesisNonce(const ChainParams& params, uint64_t start_nonce, uint64_t attempts) {
    const std::optional<Target> target = CompactToTarget(params.genesis_bits);
    if (!target.has_value()) {
        return std::nullopt;
    }

    // The header is built once and only its nonce changes, which is the whole reason
    // the nonce is the last field in the layout: the first 84 bytes are constant
    // across the search.
    BlockHeader header = BuildGenesisBlock(params).header;
    for (uint64_t offset = 0; offset < attempts; ++offset) {
        header.nonce = start_nonce + offset;
        if (HashMeetsTarget(header.Hash(), *target)) {
            return header.nonce;
        }
    }
    return std::nullopt;
}

}  // namespace amarian
