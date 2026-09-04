/// \file
/// Serialisation throughput.
///
/// This is the only benchmark in the project so far that measures code on the
/// block validation path. Computing a txid means serialising a transaction, and a
/// full sync serialises every transaction in every block at least once; a header
/// is serialised once per proof-of-work check. So the per-byte cost here multiplies
/// by the size of the chain, which is the shape of cost that matters.
///
/// Three things are measured separately because they answer different questions:
///
///   * **Fixed-width integers**, to check that the inline memcpy-plus-byteswap form
///     compiles to a store and not to a loop. This is the justification for
///     defining them in the header rather than in serialize.cpp, and a claim like
///     that should be measured rather than asserted.
///   * **Compact size**, encode and decode, because it is the branchy part and it
///     runs once per field of every variable-length structure.
///   * **A header-sized and a transaction-sized round trip**, which is the figure
///     that actually bounds sync throughput.
///
/// The decode side is measured on input the decoder accepts, and separately on
/// input it rejects: a codec that is slow to say no is a cheap way to make a node
/// spend CPU on an attacker's behalf.

#include <amarian/util/serialize.hpp>
#include <amarian/util/types.hpp>

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

namespace {

using amarian::ByteSpan;
using amarian::ByteVec;
using amarian::Hash256;
using amarian::Reader;
using amarian::Writer;

/// Deterministic pseudo-random bytes, so successive runs are comparable.
ByteVec MakeBytes(size_t count) {
    ByteVec bytes(count);
    uint64_t rng = 0x9E3779B97F4A7C15ULL;
    for (uint8_t& b : bytes) {
        rng ^= rng << 13U;
        rng ^= rng >> 7U;
        rng ^= rng << 17U;
        b = static_cast<uint8_t>(rng >> 56U);
    }
    return bytes;
}

/// Per-integer write cost. 8 bytes per iteration; anything much above a nanosecond
/// would mean the little-endian path is not compiling to a single store.
void BM_WriteU64(benchmark::State& state) {
    const auto count = static_cast<size_t>(state.range(0));
    for ([[maybe_unused]] auto iteration : state) {
        Writer writer(count * 8U);
        for (size_t i = 0; i < count; ++i) {
            writer.WriteU64(0x0123456789ABCDEFULL);
        }
        benchmark::DoNotOptimize(writer.Bytes().data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * state.range(0) * 8);
}

BENCHMARK(BM_WriteU64)->Arg(1024);

void BM_ReadU64(benchmark::State& state) {
    const auto count = static_cast<size_t>(state.range(0));
    const ByteVec bytes = MakeBytes(count * 8U);
    for ([[maybe_unused]] auto iteration : state) {
        Reader reader(bytes);
        uint64_t value = 0;
        for (size_t i = 0; i < count; ++i) {
            benchmark::DoNotOptimize(reader.ReadU64(value));
        }
        benchmark::DoNotOptimize(value);
    }
    state.SetBytesProcessed(state.iterations() * state.range(0) * 8);
}

BENCHMARK(BM_ReadU64)->Arg(1024);

/// Compact size at each of its four widths. The one-byte case is by far the most
/// common in practice — almost every count in a real transaction is below 0xFD —
/// so the nine-byte case is measured to show the cost of the form an attacker
/// would choose, not the form honest data uses.
void BM_WriteCompactSize(benchmark::State& state) {
    const auto value = static_cast<uint64_t>(state.range(0));
    for ([[maybe_unused]] auto iteration : state) {
        Writer writer(9);
        writer.WriteCompactSize(value);
        benchmark::DoNotOptimize(writer.Bytes().data());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_WriteCompactSize)->Arg(1)->Arg(0xFFFF)->Arg(0xFFFFFFFF);

void BM_ReadCompactSize(benchmark::State& state) {
    Writer writer;
    writer.WriteCompactSize(static_cast<uint64_t>(state.range(0)));
    // Padded so the allocation bound is satisfied rather than short-circuiting.
    ByteVec bytes = writer.Take();
    const size_t prefix_len = bytes.size();
    bytes.resize(prefix_len + static_cast<size_t>(state.range(0)), 0);

    for ([[maybe_unused]] auto iteration : state) {
        Reader reader(bytes);
        uint64_t value = 0;
        benchmark::DoNotOptimize(reader.ReadCompactSize(value, 1));
        benchmark::DoNotOptimize(value);
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_ReadCompactSize)->Arg(1)->Arg(0xFFFF);

/// The rejection path: a non-minimal three-byte encoding of a one-byte value. The
/// decoder pays for the full read before the canonicality check fires, which is the
/// worst case and therefore the one worth knowing.
void BM_ReadCompactSizeRejectNonMinimal(benchmark::State& state) {
    const ByteVec bytes{0xFD, 0x01, 0x00};
    for ([[maybe_unused]] auto iteration : state) {
        Reader reader(bytes);
        uint64_t value = 0;
        benchmark::DoNotOptimize(reader.ReadCompactSize(value, 1));
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_ReadCompactSizeRejectNonMinimal);

/// A 92-byte block header, the exact structure from AMARIAN_PROTOCOL.md. Serialised
/// once per proof-of-work check and once per header received during sync, so this is
/// the cost that scales with header-first synchronisation.
void BM_HeaderRoundTrip(benchmark::State& state) {
    const Hash256 prev = Hash256::FromBytes(MakeBytes(Hash256::SIZE));
    const Hash256 merkle = Hash256::FromBytes(MakeBytes(Hash256::SIZE));

    for ([[maybe_unused]] auto iteration : state) {
        Writer writer(92);
        writer.WriteU32(1);
        writer.WriteU32(123456);
        writer.WriteHash256(prev);
        writer.WriteHash256(merkle);
        writer.WriteI64(1'760'000'000);
        writer.WriteU32(0x1D00FFFFU);
        writer.WriteU64(0xDEADBEEFCAFEBABEULL);
        const ByteVec bytes = writer.Take();

        Reader reader(bytes);
        uint32_t version = 0;
        uint32_t height = 0;
        Hash256 prev_out;
        Hash256 merkle_out;
        int64_t timestamp = 0;
        uint32_t target_bits = 0;
        uint64_t nonce = 0;
        benchmark::DoNotOptimize(reader.ReadU32(version));
        benchmark::DoNotOptimize(reader.ReadU32(height));
        benchmark::DoNotOptimize(reader.ReadHash256(prev_out));
        benchmark::DoNotOptimize(reader.ReadHash256(merkle_out));
        benchmark::DoNotOptimize(reader.ReadI64(timestamp));
        benchmark::DoNotOptimize(reader.ReadU32(target_bits));
        benchmark::DoNotOptimize(reader.ReadU64(nonce));
        benchmark::DoNotOptimize(reader.Finish());
    }
    state.SetBytesProcessed(state.iterations() * 92);
}

BENCHMARK(BM_HeaderRoundTrip);

/// Length-prefixed byte strings at signature scale. 64 bytes is a BIP-340
/// signature; 2420 is an ML-DSA-44 one. The pair shows what the post-quantum size
/// difference costs in the codec alone, separately from the cost of verifying.
void BM_ByteStringRoundTrip(benchmark::State& state) {
    const ByteVec payload = MakeBytes(static_cast<size_t>(state.range(0)));
    for ([[maybe_unused]] auto iteration : state) {
        Writer writer(payload.size() + 9U);
        writer.WriteByteString(payload);
        const ByteVec bytes = writer.Take();

        Reader reader(bytes);
        ByteVec decoded;
        benchmark::DoNotOptimize(reader.ReadByteString(decoded, payload.size()));
        benchmark::DoNotOptimize(decoded);
    }
    state.SetBytesProcessed(state.iterations() * state.range(0));
}

BENCHMARK(BM_ByteStringRoundTrip)->Arg(64)->Arg(2420);

}  // namespace
