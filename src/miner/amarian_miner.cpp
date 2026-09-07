/// CUDA proof-of-work miner. This binary intentionally refuses mainnet and regtest.
#include "cuda_sha256.hpp"

#include <amarian/rpc/client.hpp>
#include <amarian/rpc/http.hpp>
#include <amarian/primitives/lock.hpp>
#include <amarian/util/args.hpp>
#include <amarian/util/hex.hpp>
#include <amarian/util/serialize.hpp>

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace {
using json = nlohmann::json;
using namespace amarian;

void Options(ArgsParser& p) {
    p.Add({.name="help", .kind=ArgKind::Flag, .help="Show this help."});
    p.Add({.name="chain", .kind=ArgKind::String, .value_hint="<network>", .help="Must be testnet."});
    p.Add({.name="datadir", .kind=ArgKind::String, .value_hint="<dir>", .help="Node datadir containing .cookie."});
    p.Add({.name="rpcport", .kind=ArgKind::Integer, .value_hint="<port>", .help="RPC port. Default: 12511."});
    p.Add({.name="wallet", .kind=ArgKind::String, .value_hint="<path>", .help="Wallet path connected to the node; required for mining."});
    p.Add({.name="blocks", .kind=ArgKind::Integer, .value_hint="<n>", .help="Blocks to mine; 0 means keep mining."});
    p.Add({.name="grid", .kind=ArgKind::Integer, .value_hint="<n>", .help="CUDA blocks per launch. Default: 4096."});
    p.Add({.name="threads", .kind=ArgKind::Integer, .value_hint="<n>", .help="CUDA threads per block. Default: 256."});
}

std::expected<std::string, std::string> ReadCookie(const std::filesystem::path& path) {
    const auto cookie = amarian::rpc::Cookie::Read(path);
    if (!cookie) return std::unexpected(cookie.error());
    return std::string(cookie->Authorization());
}

std::expected<json, std::string> Call(const std::string& authorization, uint16_t port,
                                      std::string_view method, const json& params) {
    asio::io_context io;
    asio::ip::tcp::socket socket(io);
    std::error_code ec;
    socket.connect({asio::ip::address_v4::loopback(), port}, ec);
    if (ec) return std::unexpected("cannot connect to RPC: " + ec.message());
    const std::string body = amarian::rpc::RenderCallBody(method, params, 1);
    const std::string request = amarian::rpc::RenderCall(authorization, "127.0.0.1", port, body);
    asio::write(socket, asio::buffer(request), ec);
    if (ec) return std::unexpected("cannot send RPC request: " + ec.message());
    std::string buffer;
    std::array<char, 8192> chunk{};
    for (;;) {
        const auto parsed = amarian::rpc::ParseResponse(buffer);
        if (parsed.state == amarian::rpc::ResponseState::Complete) {
            auto envelope = json::parse(parsed.body, nullptr, false);
            if (!envelope.is_object()) return std::unexpected("RPC returned malformed JSON");
            if (!envelope["error"].is_null()) return std::unexpected(envelope["error"].dump());
            return envelope["result"];
        }
        if (parsed.state == amarian::rpc::ResponseState::Malformed)
            return std::unexpected(parsed.failure);
        const size_t n = socket.read_some(asio::buffer(chunk), ec);
        if (ec == asio::error::eof) {
            const auto finished = amarian::rpc::FinishResponse(buffer);
            if (finished.state != amarian::rpc::ResponseState::Complete)
                return std::unexpected(finished.failure);
            auto envelope = json::parse(finished.body, nullptr, false);
            if (!envelope.is_object()) return std::unexpected("RPC returned malformed JSON");
            if (!envelope["error"].is_null()) return std::unexpected(envelope["error"].dump());
            return envelope["result"];
        }
        if (ec) return std::unexpected("cannot read RPC reply: " + ec.message());
        buffer.append(chunk.data(), n);
        if (buffer.size() > amarian::rpc::MAX_RESPONSE_BYTES)
            return std::unexpected("RPC reply is too large");
    }
}

int Run(int argc, char** argv) {
    ArgsParser parser("amarian-miner", "amarian-miner [options]\nCUDA testnet-only proof-of-work miner.");
    Options(parser);
    if (const auto status = parser.Parse(argc, argv); !status) {
        std::fprintf(stderr, "amarian-miner: %s\n", status.error().Message().c_str()); return 2;
    }
    if (parser.Has("help")) { std::puts(parser.HelpText().c_str()); return 0; }
    if (!parser.Has("chain") || parser.GetString("chain") != "testnet") {
        std::fputs("amarian-miner: refused; this executable is testnet-only (use --chain testnet).\n", stderr);
        return 2;
    }
    if (!parser.Has("datadir") || !parser.Has("wallet")) {
        std::fputs("amarian-miner: --datadir and --wallet are required; connect a wallet before mining.\n", stderr);
        return 2;
    }
    const int64_t raw_port = parser.GetInt("rpcport", 12511);
    const int64_t raw_grid = parser.GetInt("grid", 4096);
    const int64_t raw_threads = parser.GetInt("threads", 256);
    const int64_t requested_blocks = parser.GetInt("blocks", 0);
    if (raw_port < 1 || raw_port > 65535 || raw_grid < 1 || raw_grid > 65535 || raw_threads < 1 || raw_threads > 1024 || requested_blocks < 0) {
        std::fputs("amarian-miner: invalid numeric option.\n", stderr); return 2;
    }
    const auto authorization = ReadCookie(std::filesystem::path(parser.GetString("datadir")) / ".cookie");
    if (!authorization) { std::fprintf(stderr, "amarian-miner: %s\n", authorization.error().c_str()); return 3; }
    if (!std::filesystem::exists(parser.GetString("wallet"))) {
        std::fputs("amarian-miner: wallet file does not exist; create/open the wallet first.\n", stderr);
        return 2;
    }
    const auto address = Call(*authorization, static_cast<uint16_t>(raw_port), "getnewaddress", json::array());
    if (!address || !address->is_object() || !address->contains("lock")) {
        std::fprintf(stderr, "amarian-miner: node wallet is not connected: %s\n",
                     address ? "getnewaddress returned no lock" : address.error().c_str());
        return 3;
    }
    const auto lock_bytes = FromHex(address->at("lock").get<std::string>());
    Lock payout_lock;
    Reader lock_reader{ByteSpan{lock_bytes.value_or(ByteVec{})}};
    if (!lock_bytes || !Lock::Deserialize(lock_reader, payout_lock, 128) ||
        !lock_reader.Finish() || payout_lock.IsUnspendable()) {
        std::fputs("amarian-miner: wallet returned an invalid payout lock.\n", stderr);
        return 3;
    }
    const json payout = json{{"version", payout_lock.version},
                             {"program", ToHex(ByteSpan{payout_lock.program})}};
    uint64_t mined = 0;
    // Each CUDA thread walks a short nonce strip, so the fixed first SHA-256
    // block is reused instead of recomputed for every nonce.
    const uint64_t batch = uint64_t(raw_grid) * uint64_t(raw_threads) * 16U;
    std::printf("Amarian CUDA miner: TESTNET ONLY, wallet %s, payout %s, grid %lld x %lld threads\n",
                parser.GetString("wallet").c_str(), address->at("address").get<std::string>().c_str(),
                raw_grid, raw_threads);
    for (;;) {
        auto templ = Call(*authorization, static_cast<uint16_t>(raw_port), "getblocktemplate", json::array({payout}));
        if (!templ) { std::fprintf(stderr, "template: %s\n", templ.error().c_str()); return 3; }
        auto block = FromHex((*templ)["block"].get<std::string>());
        const auto target = FromHexExact((*templ)["target"].get<std::string>(), 32);
        if (!block || block->size() < 92 || !target) { std::fputs("miner: node returned an invalid template.\n", stderr); return 1; }
        std::array<uint8_t,92> header{}; std::copy_n(block->begin(), 92, header.begin());
        std::array<uint8_t,32> target_array{}; std::copy_n(target->begin(), 32, target_array.begin());
        uint64_t found = 0;
        uint64_t next_nonce = 0;
        uint64_t dispatched = 0;
        const auto started = std::chrono::steady_clock::now();
        auto last_tip_poll = started;
        bool solved = false;
        bool stale = false;
        bool exhausted = false;
        while (!solved && !stale && !exhausted) {
            const auto poll_now = std::chrono::steady_clock::now();
            if (poll_now - last_tip_poll >= std::chrono::seconds(2)) {
                auto info = Call(*authorization, static_cast<uint16_t>(raw_port), "getblockchaininfo", json::array());
                if (!info) { std::fprintf(stderr, "tip poll: %s\n", info.error().c_str()); return 3; }
                if ((*info)["best_block_hash"].get<std::string>() !=
                    (*templ)["previous_block_hash"].get<std::string>()) {
                    std::fprintf(stderr, "template stale at height %u; refreshing\n",
                                 (*templ)["height"].get<unsigned>());
                    stale = true;
                    break;
                }
                last_tip_poll = poll_now;
            }
            if (poll_now - started >= std::chrono::seconds(30)) {
                std::fprintf(stderr, "refreshing template after 30 seconds\n");
                stale = true;
                break;
            }
            const uint64_t count = batch <= UINT64_MAX - next_nonce
                                      ? batch
                                      : UINT64_MAX - next_nonce + 1U;
            if (gpu::Mine(header, target_array, next_nonce, count,
                          static_cast<unsigned>(raw_grid), static_cast<unsigned>(raw_threads), found)) {
                solved = true;
            }
            dispatched += count;
            if (!solved) {
                if (count == UINT64_MAX - next_nonce + 1U) exhausted = true;
                else next_nonce += count;
            }
        }
        if (stale) continue;
        if (!solved) { std::fputs("miner: nonce space exhausted without a solution.\n", stderr); return 1; }
        for (int i=0;i<8;++i) (*block)[84+i] = uint8_t(found >> (8*i));
        auto result = Call(*authorization, static_cast<uint16_t>(raw_port), "submitblock", json::array({ToHex(ByteSpan{*block})}));
        if (!result) { std::fprintf(stderr, "submit: %s\n", result.error().c_str()); return 3; }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
        const double hashrate = seconds > 0.0 ? static_cast<double>(dispatched) / seconds : 0.0;
        if (!result->is_null()) {
            std::fprintf(stderr, "height %u rejected after %llu dispatched hashes: %s\n",
                         (*templ)["height"].get<unsigned>(),
                         static_cast<unsigned long long>(dispatched), result->dump().c_str());
            continue;
        }
        std::printf("height %u accepted nonce %llu after %llu dispatched hashes in %.2fs (%.0f H/s)\n",
                    (*templ)["height"].get<unsigned>(), static_cast<unsigned long long>(found),
                    static_cast<unsigned long long>(dispatched), seconds, hashrate);
        ++mined;
        if (requested_blocks != 0 && mined >= static_cast<uint64_t>(requested_blocks)) break;
    }
    return 0;
}
}

int main(int argc, char** argv) { return Run(argc, argv); }
