/// ile
/// P2P framing and payload throughput.
///
/// These numbers bound the CPU cost of relaying bytes before socket latency,
/// congestion, and peer scheduling are included. They are deliberately kept
/// separate from a live-node benchmark because a live P2P run is not yet a
/// supported acceptance path.

#include <amarian/net/message.hpp>
#include <amarian/net/protocol.hpp>

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

using amarian::ByteVec;
using amarian::ByteSpan;
using amarian::Writer;
using amarian::net::Command;
using amarian::net::FrameMessage;
using amarian::net::SerialiseMessage;

constexpr std::array<uint8_t, 4> MAGIC{0xF5, 0xB9, 0xD4, 0xC0};

ByteVec MakePayload(size_t size) {
    ByteVec payload(size);
    uint64_t state = 0x9E3779B97F4A7C15ULL;
    for (uint8_t& byte : payload) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        byte = static_cast<uint8_t>(state >> 56U);
    }
    return payload;
}

void BM_SerialiseMessage(benchmark::State& state) {
    const ByteVec payload = MakePayload(static_cast<size_t>(state.range(0)));
    for ([[maybe_unused]] auto iteration : state) {
        ByteVec wire = SerialiseMessage(Command::Block, payload, MAGIC);
        benchmark::DoNotOptimize(wire);
    }
    state.SetBytesProcessed(state.iterations() * state.range(0));
}

BENCHMARK(BM_SerialiseMessage)->Arg(8)->Arg(256)->Arg(4096)->Arg(1 << 20);

void BM_FrameMessage(benchmark::State& state) {
    const ByteVec payload = MakePayload(static_cast<size_t>(state.range(0)));
    const ByteVec wire = SerialiseMessage(Command::Block, payload, MAGIC);
    for ([[maybe_unused]] auto iteration : state) {
        const amarian::net::Framed framed = FrameMessage(wire, MAGIC);
        benchmark::DoNotOptimize(framed.message.payload.data());
    }
    state.SetBytesProcessed(state.iterations() * state.range(0));
}

BENCHMARK(BM_FrameMessage)->Arg(8)->Arg(256)->Arg(4096)->Arg(1 << 20);

void BM_HelloPayloadRoundTrip(benchmark::State& state) {
    amarian::net::HelloPayload hello;
    hello.protocol_version = 1;
    hello.services = amarian::net::NODE_HISTORICAL | amarian::net::NODE_RELAY;
    hello.timestamp = 1'788'598'800;
    hello.best_height = 100'000;
    hello.nonce = 0x123456789ABCDEF0ULL;
    hello.user_agent = "/Amarian:0.1.0-dev/";
    for ([[maybe_unused]] auto iteration : state) {
        Writer writer;
        hello.Serialize(writer);
        const ByteVec wire = writer.Take();
        amarian::net::HelloPayload decoded;
        amarian::Reader reader(wire);
        benchmark::DoNotOptimize(
            amarian::net::HelloPayload::Deserialize(reader, decoded, 256));
        benchmark::DoNotOptimize(decoded);
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_HelloPayloadRoundTrip);

}  // namespace
