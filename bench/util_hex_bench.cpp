/// \file
/// Hex codec throughput.
///
/// Hex is not on the block validation path, but it is squarely on the RPC path:
/// `getblock` with verbosity 0 returns an entire block as hex, so encode
/// throughput bounds how fast a node can serve raw blocks to an explorer or to a
/// wallet syncing over RPC. Decode throughput bounds `sendrawtransaction` and
/// `submitblock`, both of which accept operator- or peer-supplied hex.
///
/// Sizes cover a typical transaction, a large transaction, and a block-sized
/// payload, because per-call overhead and per-byte cost matter at different scales.

#include <amarian/util/hex.hpp>
#include <amarian/util/types.hpp>

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace {

/// Deterministic pseudo-random bytes: a fixed sequence keeps successive runs
/// comparable, and xorshift is cheap enough not to distort setup cost.
amarian::ByteVec MakeBytes(size_t count) {
    amarian::ByteVec bytes(count);
    uint64_t rng = 0x9E3779B97F4A7C15ULL;
    for (uint8_t& b : bytes) {
        rng ^= rng << 13U;
        rng ^= rng >> 7U;
        rng ^= rng << 17U;
        b = static_cast<uint8_t>(rng >> 56U);
    }
    return bytes;
}

void BM_ToHex(benchmark::State& state) {
    const amarian::ByteVec bytes = MakeBytes(static_cast<size_t>(state.range(0)));
    for ([[maybe_unused]] auto iteration : state) {
        std::string hex = amarian::ToHex(bytes);
        benchmark::DoNotOptimize(hex);
    }
    state.SetBytesProcessed(state.iterations() * state.range(0));
}

BENCHMARK(BM_ToHex)->Arg(250)->Arg(4096)->Arg(1 << 20);

void BM_FromHex(benchmark::State& state) {
    const std::string hex = amarian::ToHex(MakeBytes(static_cast<size_t>(state.range(0))));
    for ([[maybe_unused]] auto iteration : state) {
        std::optional<amarian::ByteVec> decoded = amarian::FromHex(hex);
        benchmark::DoNotOptimize(decoded);
    }
    state.SetBytesProcessed(state.iterations() * state.range(0));
}

BENCHMARK(BM_FromHex)->Arg(250)->Arg(4096)->Arg(1 << 20);

/// Per-hash cost. This is the figure that matters for responses listing thousands
/// of ids — `getblock` verbosity 1, `getrawmempool` — where the byte count is
/// small but the call count is not.
void BM_Hash256ToHex(benchmark::State& state) {
    const amarian::Hash256 hash = amarian::Hash256::FromBytes(MakeBytes(amarian::Hash256::SIZE));
    for ([[maybe_unused]] auto iteration : state) {
        std::string hex = hash.ToHex();
        benchmark::DoNotOptimize(hex);
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_Hash256ToHex);

void BM_Hash256FromHex(benchmark::State& state) {
    const std::string hex = amarian::Hash256::FromBytes(MakeBytes(amarian::Hash256::SIZE)).ToHex();
    for ([[maybe_unused]] auto iteration : state) {
        std::optional<amarian::Hash256> parsed = amarian::Hash256FromHex(hex);
        benchmark::DoNotOptimize(parsed);
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_Hash256FromHex);

/// The rejection path, measured separately: an RPC endpoint under garbage load
/// spends its time here, and a decoder that is slow to say no is a cheap way to
/// waste a node's CPU. The input is valid hex except for its final character, so
/// the decoder pays full price before rejecting.
void BM_FromHexRejectLate(benchmark::State& state) {
    std::string hex = amarian::ToHex(MakeBytes(static_cast<size_t>(state.range(0))));
    hex.back() = 'z';
    for ([[maybe_unused]] auto iteration : state) {
        std::optional<amarian::ByteVec> decoded = amarian::FromHex(hex);
        benchmark::DoNotOptimize(decoded);
    }
    state.SetBytesProcessed(state.iterations() * state.range(0));
}

BENCHMARK(BM_FromHexRejectLate)->Arg(4096)->Arg(1 << 20);

}  // namespace
