/// \file
/// Prints, checks, and regenerates the genesis parameters of each network.
///
/// The nonces recorded in `consensus/params.hpp` were produced by `--mine`, and this
/// tool exists so that a reader can reproduce them rather than trust them: `--check`
/// rebuilds each network's genesis from its parameters and compares the result with
/// the recorded hash, which is the same check a node performs at startup.
///
///   amarian-genesis            print each network's genesis and check it
///   amarian-genesis --mine     search for the smallest satisfying nonce per network
///
/// `--mine` reports the *smallest* nonce that meets the target, not the first one a
/// thread happens to find, so its answer does not depend on how many cores ran it.

#include <amarian/consensus/genesis.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/consensus/target.hpp>
#include <amarian/util/hex.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace amarian;

constexpr std::array<const ChainParams*, 3> ALL_NETWORKS{
    &MAINNET_PARAMS,
    &TESTNET_PARAMS,
    &REGTEST_PARAMS,
};

void PrintGenesis(const ChainParams& params) {
    const Block genesis = BuildGenesisBlock(params);
    const GenesisFault fault = CheckGenesis(params);

    Writer writer;
    genesis.Serialize(writer);

    std::printf("%s\n", std::string(params.name).c_str());
    std::printf("  chain_id     %s\n", params.chain_id.ToHex().c_str());
    std::printf("  timestamp    %lld\n", static_cast<long long>(genesis.header.timestamp));
    std::printf("  target_bits  0x%08x\n", genesis.header.target_bits);
    std::printf("  nonce        %llu\n", static_cast<unsigned long long>(genesis.header.nonce));
    std::printf("  merkle_root  %s\n", genesis.header.merkle_root.ToHex().c_str());
    std::printf("  hash         %s\n", genesis.Hash().ToHex().c_str());
    std::printf("  coinbase     %s\n", genesis.transactions.front().Txid().ToHex().c_str());
    std::printf("  size         %zu bytes, weight %zu\n", writer.Size(), genesis.Weight());
    std::printf("  check        %s\n", std::string(Describe(fault)).c_str());
}

/// Searches for the smallest nonce meeting the target, in rounds across all threads.
///
/// Every round is searched to completion before the next begins, so the smallest hit
/// in the first round that produces one is the smallest overall — a first-past-the-post
/// search across threads would instead return whichever stripe got lucky, and would
/// record a different constant on a machine with a different core count.
std::optional<uint64_t> MineSmallestNonce(const ChainParams& params) {
    const unsigned hardware = std::max(1U, std::thread::hardware_concurrency());
    constexpr uint64_t STRIPE = uint64_t{1} << 24;

    for (uint64_t round = 0; round < (uint64_t{1} << 12); ++round) {
        const uint64_t round_base = round * STRIPE * hardware;
        std::vector<std::optional<uint64_t>> found(hardware);
        std::vector<std::thread> workers;
        workers.reserve(hardware);

        for (unsigned worker = 0; worker < hardware; ++worker) {
            workers.emplace_back([&params, &found, round_base, worker]() {
                found[worker] = FindGenesisNonce(params, round_base + (worker * STRIPE), STRIPE);
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }

        std::optional<uint64_t> best;
        for (const std::optional<uint64_t>& hit : found) {
            if (hit.has_value() && (!best.has_value() || *hit < *best)) {
                best = hit;
            }
        }
        if (best.has_value()) {
            return best;
        }
        std::printf("  searched %llu nonces\r",
                    static_cast<unsigned long long>(round_base + (STRIPE * hardware)));
        std::fflush(stdout);
    }
    return std::nullopt;
}

void MineGenesis(const ChainParams& params) {
    std::printf(
        "%s: mining genesis at 0x%08x\n", std::string(params.name).c_str(), params.genesis_bits);
    const std::optional<uint64_t> nonce = MineSmallestNonce(params);
    if (!nonce.has_value()) {
        std::printf("  no nonce found in the searched range\n");
        return;
    }

    ChainParams mined = params;
    mined.genesis_nonce = *nonce;
    const Hash256 hash = BuildGenesisBlock(mined).Hash();
    std::printf("  nonce  %llu\n", static_cast<unsigned long long>(*nonce));
    std::printf("  hash   %s\n", hash.ToHex().c_str());

    // Printed in the form the parameter table needs, so transcribing it is a copy
    // rather than a retyping.
    std::printf("  literal {");
    const std::array<uint8_t, Hash256::SIZE>& bytes = hash.Array();
    for (size_t index = 0; index < bytes.size(); ++index) {
        std::printf("%s0x%02X,", (index % 16 == 0) ? "\n      " : " ", bytes[index]);
    }
    std::printf("}\n");
}

}  // namespace

int main(int argc, char** argv) {
    const bool mine = argc > 1 && std::string_view(argv[1]) == "--mine";
    for (const ChainParams* params : ALL_NETWORKS) {
        if (mine) {
            MineGenesis(*params);
        } else {
            PrintGenesis(*params);
        }
    }
    return 0;
}
