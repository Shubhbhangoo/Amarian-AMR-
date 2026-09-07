/// \file
/// `amarian-wallet` — a standalone wallet management CLI.
///
/// Creates, opens, and manages wallets: generate addresses, check balance, send
/// transactions, list transaction history, export/import backups.
///
/// Connects to a running amariand node for chain state (broadcasting, fee estimation)
/// via its RPC interface, or operates offline for address generation and backup.

#include <amarian/consensus/params.hpp>
#include <amarian/rpc/client.hpp>
#include <amarian/rpc/http.hpp>
#include <amarian/util/args.hpp>
#include <amarian/util/hex.hpp>
#include <amarian/wallet/backup.hpp>
#include <amarian/wallet/fees.hpp>
#include <amarian/wallet/seed.hpp>
#include <amarian/wallet/wallet_api.hpp>
#include <amarian/version.hpp>

#include <asio/buffer.hpp>
#include <asio/connect.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace amarian {
namespace {

using json = nlohmann::json;

constexpr int EXIT_USAGE = 2;
constexpr int EXIT_RPC_ERROR = 1;
constexpr int EXIT_TRANSPORT = 3;

std::string FormatAmr(int64_t facets) {
    const bool negative = facets < 0;
    const uint64_t magnitude = negative
                                   ? static_cast<uint64_t>(-(facets + 1)) + 1U
                                   : static_cast<uint64_t>(facets);
    const uint64_t whole = magnitude / static_cast<uint64_t>(FACETS_PER_AMR);
    const uint64_t fraction = magnitude % static_cast<uint64_t>(FACETS_PER_AMR);
    std::string fraction_text = std::to_string(fraction);
    fraction_text.insert(fraction_text.begin(),
                         10U - fraction_text.size(), '0');
    return std::string(negative ? "-" : "") + std::to_string(whole) + "." + fraction_text;
}

void RegisterOptions(ArgsParser& parser) {
    parser.Add({.name = "help", .kind = ArgKind::Flag, .help = "Show this help and exit.", .short_name = 'h'});
    parser.Add({.name = "version", .kind = ArgKind::Flag, .help = "Print version and exit.", .short_name = 'v'});
    parser.Add({.name = "chain", .kind = ArgKind::String, .value_hint = "<network>", .help = "One of mainnet, testnet, regtest. Default: mainnet."});
    parser.Add({.name = "datadir", .kind = ArgKind::String, .value_hint = "<dir>", .help = "The node datadir (for RPC connection). Default: $HOME/.amarian/<network>."});
    parser.Add({.name = "rpcport", .kind = ArgKind::Integer, .value_hint = "<port>", .help = "Node RPC port. Default: the network's."});
    parser.Add({.name = "cookie", .kind = ArgKind::String, .value_hint = "<file>", .help = "RPC credential file. Default: <datadir>/.cookie."});
    parser.Add({.name = "timeout", .kind = ArgKind::Integer, .value_hint = "<seconds>", .help = "RPC timeout. Default: 60."});
    parser.Add({.name = "wallet", .kind = ArgKind::String, .value_hint = "<path>", .help = "Wallet database path. Default: <datadir>/wallet.dat."});
    parser.Add({.name = "wallet-password", .kind = ArgKind::String, .value_hint = "<password>", .help = "Wallet password. Default: empty."});
    parser.Add({.name = "raw", .kind = ArgKind::Flag, .help = "Print raw JSON responses."});
}

std::optional<Network> SelectNetwork(const ArgsParser& parser) {
    if (!parser.Has("chain")) return Network::Mainnet;
    const auto name = parser.GetString("chain");
    if (name == "mainnet") return Network::Mainnet;
    if (name == "testnet") return Network::Testnet;
    if (name == "regtest") return Network::Regtest;
    std::fprintf(stderr, "unknown --chain '%s'\n", name.c_str());
    return std::nullopt;
}

std::optional<std::string> ResolveDataDir(const ArgsParser& parser, const ChainParams& params) {
    if (parser.Has("datadir")) return parser.GetString("datadir");
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        std::fputs("HOME not set, use --datadir\n", stderr);
        return std::nullopt;
    }
    return std::string(home) + "/.amarian/" + std::string(params.name);
}

std::expected<int64_t, std::string> ParseAmount(std::string_view text) {
    if (text.empty()) return std::unexpected("amount must be a positive integer number of facets");
    int64_t amount = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), amount);
    if (parsed.ec == std::errc::result_out_of_range || parsed.ptr != text.data() + text.size()) {
        return std::unexpected("amount must be a positive integer number of facets");
    }
    if (parsed.ec != std::errc{} || amount <= 0) {
        return std::unexpected("amount must be a positive integer number of facets");
    }
    return amount;
}

/// Call the node's RPC method.
std::expected<json, std::string> RpcCall(const rpc::Cookie& cookie, uint16_t port,
                                          std::string_view method, const json& params_val,
                                          std::chrono::seconds timeout) {
    constexpr int64_t REQUEST_ID = 1;
    const auto body = rpc::RenderCallBody(method, params_val, REQUEST_ID);
    const auto request = rpc::RenderCall(cookie.Authorization(), "127.0.0.1", port, body);

    asio::io_context io{1};
    asio::ip::tcp::socket socket(io);
    const asio::ip::tcp::endpoint endpoint(asio::ip::address_v4::loopback(), port);

    std::error_code failure;
    bool connected = false;
    socket.async_connect(endpoint, [&](const std::error_code& ec) {
        failure = ec;
        connected = !ec;
    });
    io.run_for(timeout);
    if (!connected) {
        return std::unexpected("cannot connect to node at 127.0.0.1:" + std::to_string(port));
    }

    io.restart();
    bool wrote = false;
    asio::async_write(socket, asio::buffer(request.data(), request.size()),
                      [&](const std::error_code& ec, size_t) {
                          failure = ec;
                          wrote = !ec;
                      });
    io.run_for(timeout);
    if (!wrote) {
        return std::unexpected("cannot send request");
    }

    std::string buffer;
    std::array<char, 8192> chunk{};
    for (;;) {
        auto parsed = rpc::ParseResponse(buffer);
        if (parsed.state == rpc::ResponseState::Complete) {
            auto envelope = json::parse(parsed.body, nullptr, false);
            if (envelope.is_discarded() || !envelope.is_object()) {
                return std::unexpected("non-JSON response from node");
            }
            const auto err = envelope.find("error");
            if (err != envelope.end() && !err->is_null()) {
                const auto msg = err->find("message");
                const auto data = err->find("data");
                std::string text;
                if (data != err->end() && data->is_string()) text = data->get<std::string>();
                else if (msg != err->end() && msg->is_string()) text = msg->get<std::string>();
                else text = err->dump();
                return std::unexpected(text);
            }
            return envelope;
        }
        if (parsed.state == rpc::ResponseState::Malformed) {
            return std::unexpected(parsed.failure);
        }

        io.restart();
        size_t got = 0;
        bool eof = false, read = false;
        socket.async_read_some(asio::buffer(chunk), [&](const std::error_code& ec, size_t bytes) {
            got = bytes;
            eof = ec == asio::error::eof;
            failure = ec;
            read = !ec || eof;
        });
        io.run_for(timeout);
        if (!read) {
            return std::unexpected("timed out reading reply");
        }
        buffer.append(chunk.data(), got);
        if (eof) {
            auto finished = rpc::FinishResponse(buffer);
            if (finished.state != rpc::ResponseState::Complete) {
                return std::unexpected(finished.failure.empty() ? "incomplete reply" : finished.failure);
            }
            auto envelope = json::parse(finished.body, nullptr, false);
            if (envelope.is_discarded()) return std::unexpected("non-JSON response");
            return envelope;
        }
    }
}

/// Print usage.
void PrintUsage(const ArgsParser& parser, const ChainParams& params) {
    (void)params;
    std::printf("Amarian wallet tool %s\n\n", VersionString().c_str());
    std::puts(parser.HelpText().c_str());
    std::puts("\nCommands (positional, after options):\n");
    std::puts("  create              Create a new wallet");
    std::puts("  getnewaddress       Generate a new receive address");
    std::puts("  listaddresses       List all derived addresses");
    std::puts("  getbalance          Get wallet balance");
    std::puts("  send <addr> <amt>   Send amount (facets) to address");
    std::puts("  listtransactions    List stored transactions");
    std::puts("  backup              Export mnemonic backup");
    std::puts("  restore <mnemonic>  Restore wallet from mnemonic");
    std::puts("\nOptions:\n");
}

std::string JoinWords(const std::vector<std::string>& words) {
    std::string joined;
    for (size_t i = 0; i < words.size(); ++i) {
        if (i != 0) joined += ' ';
        joined += words[i];
    }
    return joined;
}

std::vector<std::string> SplitWords(std::string_view text) {
    std::vector<std::string> words;
    std::string current;
    for (const char c : text) {
        if (c == ' ' || c == '\t') {
            if (!current.empty()) {
                words.push_back(std::move(current));
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) words.push_back(std::move(current));
    return words;
}

int Run(int argc, char* argv[]) {
    ArgsParser parser("amarian-wallet", "amarian-wallet [options] <command> [arguments...]");
    RegisterOptions(parser);

    if (const auto parsed = parser.Parse(argc, argv); !parsed.has_value()) {
        std::fprintf(stderr, "amarian-wallet: %s\n", parsed.error().Message().c_str());
        return EXIT_USAGE;
    }

    if (parser.Has("help")) {
        const auto net = SelectNetwork(parser).value_or(Network::Mainnet);
        PrintUsage(parser, ParamsFor(net));
        return EXIT_SUCCESS;
    }
    if (parser.Has("version")) {
        std::printf("%s\n", VersionString().c_str());
        return EXIT_SUCCESS;
    }

    const auto network = SelectNetwork(parser);
    if (!network) return EXIT_USAGE;
    const ChainParams& params = ParamsFor(*network);

    const auto words = parser.Positional();
    if (words.empty()) {
        std::fputs("amarian-wallet: no command. Try --help.\n", stderr);
        return EXIT_USAGE;
    }
    const auto command = words[0];

    // Resolve paths
    const auto datadir = ResolveDataDir(parser, params);
    if (!datadir) return EXIT_USAGE;
    const auto wallet_path = parser.Has("wallet")
        ? parser.GetString("wallet")
        : *datadir + "/wallet.dat";
    const auto wallet_pass = parser.GetString("wallet-password", "");

    // Open or create wallet based on command
    std::optional<wallet::Wallet> wallet;
    if (command == "create") {
        wallet.emplace(wallet_path, wallet_pass, params);
        // Print seed mnemonic
        auto backup = wallet->ExportBackup();
        std::printf("Wallet created at %s\n", wallet_path.c_str());
        std::printf("Mnemonic (backup this):\n%s\n", JoinWords(backup.mnemonic).c_str());
        return EXIT_SUCCESS;
    } else if (command == "restore") {
        if (words.size() < 2) {
            std::fputs("amarian-wallet: restore needs a mnemonic phrase as argument\n", stderr);
            return EXIT_USAGE;
        }
        // Recreate wallet from mnemonic
        wallet::WalletBackup backup;
        backup.mnemonic = SplitWords(words[1]);
        auto seed = wallet::SeedFromMnemonic(backup.mnemonic);
        if (!seed) {
            std::fputs("amarian-wallet: invalid mnemonic\n", stderr);
            return EXIT_FAILURE;
        }
        wallet.emplace(wallet_path, wallet_pass, params);
        if (!wallet->RestoreFromBackup(backup)) {
            std::fputs("amarian-wallet: restore failed\n", stderr);
            return EXIT_FAILURE;
        }
        std::printf("Wallet restored at %s\n", wallet_path.c_str());
        return EXIT_SUCCESS;
    } else {
        // Try to open existing wallet
        auto existing = wallet::Wallet::Open(wallet_path, wallet_pass, params);
        if (!existing) {
            std::fprintf(stderr, "amarian-wallet: cannot open wallet at %s (use 'create' first)\n",
                         wallet_path.c_str());
            return EXIT_FAILURE;
        }
        wallet.emplace(std::move(*existing));
    }

    // Parse RPC connection info
    const int64_t asked_port = parser.GetInt("rpcport", 0);
    const uint16_t port = asked_port ? static_cast<uint16_t>(asked_port) : params.default_rpc_port;
    const auto cookie_path = parser.Has("cookie")
        ? std::filesystem::path(parser.GetString("cookie"))
        : std::filesystem::path(*datadir) / ".cookie";
    const auto cookie = rpc::Cookie::Read(cookie_path);
    const bool is_online_command = (command == "getbalance" || command == "send");
    if (!cookie && is_online_command) {
        std::fprintf(stderr, "amarian-wallet: cannot read RPC cookie: %s\n",
                     cookie.error().c_str());
        // Non-fatal for offline operations
    }
    const auto timeout = std::chrono::seconds(parser.GetInt("timeout", 60));

    // Execute command
    if (command == "getnewaddress") {
        auto addr = wallet->GetNewAddress(0);
        std::printf("%s\n", addr.address.c_str());
        return EXIT_SUCCESS;
    } else if (command == "listaddresses") {
        auto addrs = wallet->ListAddresses(0);
        for (const auto& a : addrs) {
            std::printf("%s  (account %u, index %u)\n", a.address.c_str(), a.account_id, a.index);
        }
        return EXIT_SUCCESS;
    } else if (command == "getbalance") {
        if (cookie) {
            auto rpc_result = RpcCall(*cookie, port, "getbalance", json::array(), timeout);
            if (rpc_result && rpc_result->contains("result")) {
                const auto& res = (*rpc_result)["result"];
                long long confirmed = res.value("confirmed", 0LL);
                long long pending = res.value("pending", 0LL);
                long long total = res.value("total", 0LL);
                std::printf("Confirmed: %s AMR (%lld facets)\n", FormatAmr(confirmed).c_str(), confirmed);
                std::printf("Pending:   %s AMR (%lld facets)\n", FormatAmr(pending).c_str(), pending);
                std::printf("Total:     %s AMR (%lld facets)\n", FormatAmr(total).c_str(), total);
                return EXIT_SUCCESS;
            }
        }
        auto bal = wallet->GetBalance();
        std::printf("Confirmed: %s AMR (%lld facets)\n", FormatAmr(bal.confirmed).c_str(), static_cast<long long>(bal.confirmed));
        std::printf("Pending:   %s AMR (%lld facets)\n", FormatAmr(bal.pending).c_str(), static_cast<long long>(bal.pending));
        std::printf("Total:     %s AMR (%lld facets)\n", FormatAmr(bal.Total()).c_str(), static_cast<long long>(bal.Total()));
        return EXIT_SUCCESS;
    } else if (command == "send") {
        if (words.size() < 3) {
            std::fputs("amarian-wallet: send <address> <amount>\n", stderr);
            return EXIT_USAGE;
        }
        const auto amount = ParseAmount(words[2]);
        if (!amount.has_value()) {
            std::fprintf(stderr, "amarian-wallet: %s\n", amount.error().c_str());
            return EXIT_USAGE;
        }
        if (cookie) {
            auto rpc_result = RpcCall(*cookie, port, "sendtoaddress",
                                       json::array({words[1], *amount}), timeout);
            if (!rpc_result.has_value()) {
                std::fprintf(stderr, "amarian-wallet: RPC send failed: %s\n",
                             rpc_result.error().c_str());
                return EXIT_RPC_ERROR;
            }
            const auto result = rpc_result->find("result");
            if (result == rpc_result->end() || !result->is_object()) {
                std::fputs("amarian-wallet: RPC send returned an invalid result\n", stderr);
                return EXIT_RPC_ERROR;
            }
            {
                const auto& res = *result;
                std::string txid = res.value("txid", "");
                long long fee = res.value("fee", 0LL);
                std::printf("Sent. txid: %s, fee: %lld\n", txid.c_str(), fee);
                std::printf("Broadcast result: accepted\n");
                return EXIT_SUCCESS;
            }
        }
        auto result = wallet->Send(words[1], *amount);
        if (!result) {
            std::fputs("amarian-wallet: send failed\n", stderr);
            return EXIT_FAILURE;
        }
        std::printf("Sent. txid: %s, fee: %lld\n",
                    result->txid.ToHex().c_str(),
                    static_cast<long long>(result->fee));

        // Broadcast via RPC if available
        if (cookie) {
            auto rpc_result = RpcCall(*cookie, port, "sendrawtransaction",
                                       json::array({result->raw_tx_hex}), timeout);
            if (rpc_result) {
                std::printf("Broadcast result: accepted\n");
            } else {
                std::fprintf(stderr, "Broadcast: %s (tx still saved locally)\n",
                             rpc_result.error().c_str());
            }
        } else {
            std::fprintf(stderr, "Cannot broadcast: no RPC connection (tx saved locally)\n");
        }

        wallet::StoredTransaction stx;
        stx.txid = result->txid;
        stx.received = 0;
        stx.sent = *amount;
        stx.fee = result->fee;
        stx.height = -1;
        stx.raw_tx = ByteVec();
        wallet->AddTransaction(stx);
        return EXIT_SUCCESS;
    } else if (command == "listtransactions") {
        auto txs = wallet->ListTransactions();
        for (const auto& tx : txs) {
            std::printf("%s  height=%d  received=%lld  sent=%lld  fee=%lld\n",
                        tx.txid.ToHex().c_str(), tx.height,
                        static_cast<long long>(tx.received),
                        static_cast<long long>(tx.sent),
                        static_cast<long long>(tx.fee));
        }
        if (txs.empty()) std::puts("(no transactions)");
        return EXIT_SUCCESS;
    } else if (command == "backup") {
        auto backup = wallet->ExportBackup();
        std::printf("Mnemonic:\n%s\n\nMetadata:\n%s\n", JoinWords(backup.mnemonic).c_str(), ToHex(backup.metadata).c_str());
        return EXIT_SUCCESS;
    } else {
        std::fprintf(stderr, "amarian-wallet: unknown command '%s'. Try --help.\n", command.c_str());
        return EXIT_USAGE;
    }
}

}  // namespace
}  // namespace amarian

int main(int argc, char* argv[]) {
    return amarian::Run(argc, argv);
}
