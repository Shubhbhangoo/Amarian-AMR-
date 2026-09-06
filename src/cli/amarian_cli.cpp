/// \file
/// `amarian-cli` — the client that makes the node's interface usable by a person.
///
/// Everything that decides what a call *means* is in `rpc/client.hpp` and has no socket near
/// it. What is left here is the same short list the server's listener has: connect, write,
/// read, close. Plus one thing the server has no equivalent of, and it is the reason this
/// binary is worth having rather than a shell function around `curl`: finding the credential.
///
/// A person running this has a datadir, not a token. The node writes its cookie there at
/// startup and removes it on the way out, so reading it is both how the client authenticates
/// and how it knows there is something to talk to — a missing cookie is a node that is not
/// running, and saying so is more useful than a connection refused. That the file is
/// owner-only is what makes "running as this user" the requirement rather than "on this
/// machine", and it is why this client never takes a token on the command line: an argument is
/// visible in `ps` to every account on the box, which would undo the cookie's entire point.

#include <amarian/consensus/params.hpp>
#include <amarian/rpc/client.hpp>
#include <amarian/rpc/http.hpp>
#include <amarian/util/args.hpp>
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
/// A call that reached the node and came back an error. Distinct from `EXIT_FAILURE`, which
/// here means the call never got an answer: a script retrying on a dropped connection must not
/// also retry a block the rules refused.
constexpr int EXIT_RPC_ERROR = 1;
/// A transport failure: nothing was asked, or nothing came back.
constexpr int EXIT_TRANSPORT = 3;

void RegisterOptions(ArgsParser& parser) {
    parser.Add({.name = "help",
                .kind = ArgKind::Flag,
                .help = "Show this help and exit.",
                .short_name = 'h'});
    parser.Add({.name = "version",
                .kind = ArgKind::Flag,
                .help = "Print the version and exit.",
                .short_name = 'v'});
    parser.Add({.name = "chain",
                .kind = ArgKind::String,
                .value_hint = "<network>",
                .help = "One of mainnet, testnet, regtest. Default: mainnet."});
    parser.Add({.name = "datadir",
                .kind = ArgKind::String,
                .value_hint = "<dir>",
                .help = "The node's datadir, where its cookie is. Default: $HOME/.amarian/<network>."});
    parser.Add({.name = "rpcport",
                .kind = ArgKind::Integer,
                .value_hint = "<port>",
                .help = "Port the node serves on. Default: the network's (12501/12511/12521)."});
    parser.Add({.name = "cookie",
                .kind = ArgKind::String,
                .value_hint = "<file>",
                .help = "Read the credential from this file instead of <datadir>/.cookie."});
    parser.Add({.name = "timeout",
                .kind = ArgKind::Integer,
                .value_hint = "<seconds>",
                .help = "Give up on a call after this long. Default: 60."});
    parser.Add({.name = "raw",
                .kind = ArgKind::Flag,
                .help = "Print the reply envelope as the node sent it, unformatted."});
}

/// The methods, so `--help` can name them without a running node to ask.
///
/// Read from `rpc::MethodNames`, which is derived from the dispatch table itself, so this list
/// cannot drift from what the node actually serves — a client advertising a method that no
/// longer exists is worse than one advertising none.
void PrintMethods() {
    std::fputs("\nMethods (arguments are positional; ask the node for `help`):\n", stdout);
    for (const std::string_view name : rpc::MethodNames()) {
        std::fprintf(stdout, "  %.*s\n", static_cast<int>(name.size()), name.data());
    }
    std::fputs(
        "\nAn argument that parses as JSON is sent as that JSON, and one that does not is sent\n"
        "as a string. So `getblockhash 0` sends the number zero and `submitblock deadbeef`\n"
        "sends the string \"deadbeef\".\n",
        stdout);
}

/// Where the node's datadir is, by the same rule the daemon uses.
///
/// Duplicated deliberately rather than shared: this is a client, and if it ever disagreed with
/// the daemon about where a chain lives the symptom would be a confusing "no cookie" rather
/// than anything dangerous. Sharing it would mean the CLI linking the node.
std::optional<std::string> ResolveDataDir(const ArgsParser& parser, const ChainParams& params) {
    if (parser.Has("datadir")) {
        return parser.GetString("datadir");
    }
    const char* const home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        std::fputs("amarian-cli: HOME is not set, so --datadir must be given\n", stderr);
        return std::nullopt;
    }
    return std::string(home) + "/.amarian/" + std::string(params.name);
}

std::optional<Network> SelectNetwork(const ArgsParser& parser) {
    if (!parser.Has("chain")) {
        return Network::Mainnet;
    }
    const std::string name = parser.GetString("chain");
    if (name == "mainnet") {
        return Network::Mainnet;
    }
    if (name == "testnet") {
        return Network::Testnet;
    }
    if (name == "regtest") {
        return Network::Regtest;
    }
    std::fprintf(stderr,
                 "amarian-cli: unknown --chain '%s' (expected mainnet, testnet or regtest)\n",
                 name.c_str());
    return std::nullopt;
}

/// One request, one reply, over one connection.
///
/// Asio's asynchronous operations driven by `run_for`, which is what gives the whole exchange
/// a single deadline rather than one per step: an operator waiting on `generate` cares how
/// long the call takes, not how long the connect took separately from the read. When the
/// budget runs out the socket is closed, which is what cancels whatever was pending.
///
/// IPv4 loopback only. The server binds both families, so this reaches it either way, and
/// naming one address means there is no resolver in this path — nothing for a hosts file or a
/// DNS server to have an opinion about, which is the same reason the server does not resolve
/// the `Host` it is sent.
[[nodiscard]] std::expected<rpc::ParsedResponse, std::string> Call(uint16_t port,
                                                                   std::string_view request,
                                                                   std::chrono::seconds timeout) {
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
        std::error_code ignored;
        socket.close(ignored);
        if (!failure) {
            return std::unexpected("timed out connecting to 127.0.0.1:" + std::to_string(port));
        }
        // The message an operator most often needs, and the one most worth being specific
        // about: a refused connection on a loopback port means nothing is listening, which
        // almost always means the node is not running or is on another port.
        return std::unexpected("cannot reach a node at 127.0.0.1:" + std::to_string(port) + ": " +
                               failure.message());
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
        std::error_code ignored;
        socket.close(ignored);
        return std::unexpected(failure ? "cannot send the request: " + failure.message()
                                       : "timed out sending the request");
    }

    // Read until the reply is whole, or until the peer closes and `FinishResponse` says whether
    // what arrived was a whole reply after all. The two entry points exist for exactly this
    // loop: a reply with no announced length is only complete once the connection is not.
    std::string buffer;
    std::array<char, 8192> chunk{};
    for (;;) {
        const rpc::ParsedResponse parsed = rpc::ParseResponse(buffer);
        if (parsed.state == rpc::ResponseState::Complete) {
            return parsed;
        }
        if (parsed.state == rpc::ResponseState::Malformed) {
            return std::unexpected(parsed.failure);
        }

        io.restart();
        size_t got = 0;
        bool eof = false;
        bool read = false;
        socket.async_read_some(asio::buffer(chunk), [&](const std::error_code& ec, size_t bytes) {
            got = bytes;
            eof = ec == asio::error::eof;
            failure = ec;
            read = !ec || eof;
        });
        io.run_for(timeout);
        if (!read) {
            std::error_code ignored;
            socket.close(ignored);
            return std::unexpected(failure ? "cannot read the reply: " + failure.message()
                                           : "timed out waiting for a reply");
        }
        buffer.append(chunk.data(), got);
        if (eof) {
            const rpc::ParsedResponse finished = rpc::FinishResponse(buffer);
            if (finished.state == rpc::ResponseState::Complete) {
                return finished;
            }
            return std::unexpected(finished.failure);
        }
        if (buffer.size() > rpc::MAX_RESPONSE_BYTES) {
            std::error_code ignored;
            socket.close(ignored);
            return std::unexpected("the node's reply is larger than this client will read");
        }
    }
}

/// Prints what a successful call returned, and returns the exit status.
///
/// A string result is printed bare rather than quoted, because the results that are strings
/// are hashes and hex, and an operator piping a block hash into another command should not
/// have to strip quotes this client only added to prove it was JSON. Everything else is
/// printed as indented JSON, which is what a person reads.
[[nodiscard]] int PrintReply(const json& envelope, bool raw) {
    if (raw) {
        std::fprintf(stdout, "%s\n", envelope.dump().c_str());
        // Still an error exit when the envelope carries one: `--raw` changes what is printed,
        // not what happened, and a script checking the status must get the same answer either
        // way.
        const auto error = envelope.find("error");
        return (error != envelope.end() && !error->is_null()) ? EXIT_RPC_ERROR : EXIT_SUCCESS;
    }

    const auto error = envelope.find("error");
    if (error != envelope.end() && !error->is_null()) {
        // Both halves of what the node said, and `data` first, because that is the specific one.
        // JSON-RPC's `message` is a short description of the *class* of error — the node renders
        // it from an enum — while `data` carries what this call actually got wrong. A client that
        // printed only `message` would answer "the arguments are not of the shape this method
        // takes" to a `getblockhash` past the tip, when the node had computed "this chain has no
        // block at that height" and put it right there. Reading one and discarding the other is
        // how an operator ends up debugging the wrong thing.
        const auto detail = error->find("data");
        const auto message = error->find("message");
        const auto code = error->find("code");
        std::string text;
        if (detail != error->end() && detail->is_string()) {
            text = detail->get<std::string>();
        } else if (message != error->end() && message->is_string()) {
            text = message->get<std::string>();
        } else {
            text = error->dump();
        }
        // Only the one line. The class is not printed alongside the specific reason, because in
        // every case the node produces it is a strictly weaker statement of the same thing —
        // "expected a non-negative integer" against "the arguments are not of the shape this
        // method takes" — and a second clause an operator learns to skip is a second clause that
        // makes the first harder to see. The code is printed because that is what a caller
        // greps for and it is not a restatement of anything.
        //
        // stderr, so a caller redirecting stdout gets an empty result rather than an error
        // mixed into its data.
        if (code != error->end() && code->is_number_integer()) {
            std::fprintf(stderr, "amarian-cli: %s (code %lld)\n", text.c_str(),
                         static_cast<long long>(code->get<int64_t>()));
        } else {
            std::fprintf(stderr, "amarian-cli: %s\n", text.c_str());
        }
        return EXIT_RPC_ERROR;
    }

    const auto result = envelope.find("result");
    if (result == envelope.end() || result->is_null()) {
        // A null result is a successful call that had nothing to say. Printed as nothing
        // rather than as "null", because that is what it means.
        return EXIT_SUCCESS;
    }
    if (result->is_string()) {
        std::fprintf(stdout, "%s\n", result->get<std::string>().c_str());
    } else {
        std::fprintf(stdout, "%s\n", result->dump(2).c_str());
    }
    return EXIT_SUCCESS;
}

int Run(int argc, char* argv[]) {
    ArgsParser parser("amarian-cli",
                      "amarian-cli [options] <method> [argument...]\n"
                      "\n"
                      "Calls a running Amarian node over its loopback JSON-RPC interface. The\n"
                      "credential comes from the node's cookie file, which means this works\n"
                      "with no configuration when run by the account the node runs as.");
    RegisterOptions(parser);

    if (const auto parsed = parser.Parse(argc, argv); !parsed.has_value()) {
        std::fprintf(stderr, "amarian-cli: %s\n", parsed.error().Message().c_str());
        std::fputs("Try 'amarian-cli --help'.\n", stderr);
        return EXIT_USAGE;
    }

    if (parser.Has("help")) {
        std::fputs(parser.HelpText().c_str(), stdout);
        PrintMethods();
        return EXIT_SUCCESS;
    }
    if (parser.Has("version")) {
        std::fprintf(stdout, "%s\n", VersionString().c_str());
        return EXIT_SUCCESS;
    }

    const std::vector<std::string>& words = parser.Positional();
    if (words.empty()) {
        std::fputs("amarian-cli: no method given. Try 'amarian-cli --help'.\n", stderr);
        return EXIT_USAGE;
    }

    const std::optional<Network> network = SelectNetwork(parser);
    if (!network.has_value()) {
        return EXIT_USAGE;
    }
    const ChainParams& params = ParamsFor(*network);

    const int64_t asked_port = parser.GetInt("rpcport", 0);
    if (asked_port < 0 || asked_port > 65535) {
        std::fprintf(stderr, "amarian-cli: --rpcport must be between 0 and 65535\n");
        return EXIT_USAGE;
    }
    const uint16_t port =
        asked_port != 0 ? static_cast<uint16_t>(asked_port) : params.default_rpc_port;

    const int64_t asked_timeout = parser.GetInt("timeout", 60);
    if (asked_timeout <= 0 || asked_timeout > 86400) {
        std::fprintf(stderr, "amarian-cli: --timeout must be between 1 and 86400 seconds\n");
        return EXIT_USAGE;
    }

    // The cookie's path, and with it the answer to whether a node is even running. Resolved
    // before the socket is opened so that "the node is not running" is reported as that rather
    // than as a connection refused, which is the same fact with the useful part removed.
    std::filesystem::path cookie_path;
    if (parser.Has("cookie")) {
        cookie_path = parser.GetString("cookie");
    } else {
        const std::optional<std::string> datadir = ResolveDataDir(parser, params);
        if (!datadir.has_value()) {
            return EXIT_USAGE;
        }
        cookie_path = std::filesystem::path(*datadir) / ".cookie";
    }
    const std::expected<rpc::Cookie, std::string> cookie = rpc::Cookie::Read(cookie_path);
    if (!cookie.has_value()) {
        std::fprintf(stderr, "amarian-cli: %s\n", cookie.error().c_str());
        return EXIT_TRANSPORT;
    }

    // Arguments after the method, each turned into JSON by the rule the header states. Sent as
    // an array rather than an object because a positional call is what a command line is.
    json args = json::array();
    for (size_t i = 1; i < words.size(); ++i) {
        args.push_back(rpc::ParseArgument(words[i]));
    }

    // A fixed id. There is one request on this connection, so an id that varied would prove
    // nothing extra; what it is for is catching a reply that did not come from this call at
    // all, and any value it is checked against does that.
    constexpr int64_t REQUEST_ID = 1;
    const std::string body = rpc::RenderCallBody(words[0], args, REQUEST_ID);
    // `127.0.0.1` and not `localhost`, so the name in the header is the address connected to
    // and the server's check is comparing what it should.
    const std::string request =
        rpc::RenderCall(cookie->Authorization(), "127.0.0.1", port, body);

    const std::expected<rpc::ParsedResponse, std::string> reply =
        Call(port, request, std::chrono::seconds(asked_timeout));
    if (!reply.has_value()) {
        std::fprintf(stderr, "amarian-cli: %s\n", reply.error().c_str());
        return EXIT_TRANSPORT;
    }

    json envelope = json::parse(reply->body, nullptr, false);
    if (envelope.is_discarded() || !envelope.is_object()) {
        // A refusal answers in plain text and never in JSON-RPC's envelope, which is a
        // deliberate property of the server: a request refused before it reached a method may
        // not have been a JSON-RPC request at all. So the status and the body are what there is
        // to report, and reporting them is more use than complaining about the shape.
        std::fprintf(stderr, "amarian-cli: the node refused the request with HTTP %d\n",
                     reply->status);
        if (!reply->body.empty()) {
            std::fprintf(stderr, "  %s", reply->body.c_str());
            if (reply->body.back() != '\n') {
                std::fputc('\n', stderr);
            }
        }
        if (reply->status == static_cast<int>(rpc::HttpStatus::Unauthorized)) {
            // The one refusal with an obvious cause worth naming: a cookie from a previous run
            // authenticates against nothing, and the node rewrites it at every start.
            std::fprintf(stderr,
                         "  the credential in '%s' is not the one this node is using; it is "
                         "rewritten each time the node starts\n",
                         cookie_path.string().c_str());
        }
        return EXIT_TRANSPORT;
    }

    if (const auto id = envelope.find("id");
        id == envelope.end() || !id->is_number_integer() || id->get<int64_t>() != REQUEST_ID) {
        // Not a reply to this call. With one request per connection that should be impossible,
        // which is exactly why it is worth refusing rather than printing: something other than
        // this node's interface answered.
        std::fputs("amarian-cli: the reply does not match the request that was sent\n", stderr);
        return EXIT_TRANSPORT;
    }

    return PrintReply(envelope, parser.Has("raw"));
}

}  // namespace
}  // namespace amarian

int main(int argc, char* argv[]) {
    return amarian::Run(argc, argv);
}
