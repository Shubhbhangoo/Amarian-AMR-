/// \file
/// The Amarian node daemon.
///
/// Phase 0/1 scope: option handling, network selection, logging setup, build
/// identification, and two startup checks — that every consensus signature scheme is
/// actually usable in this build, and that this build's genesis parameters are the ones
/// the selected network uses. The chain, storage and networking layers
/// are introduced in later phases and wired in here. The binary deliberately does not
/// pretend to do more than it does — `--version`, `--build-info` and `--chain` are
/// real, and everything else reports honestly that it is not yet available.

#include <amarian/consensus/genesis.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/crypto/signature.hpp>
#include <amarian/util/args.hpp>
#include <amarian/util/logging.hpp>
#include <amarian/version.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

namespace amarian {
namespace {

constexpr int EXIT_USAGE = 2;

void Print(const std::string& text) {
    (void)std::fwrite(text.data(), 1, text.size(), stdout);
}

void RegisterOptions(ArgsParser& parser) {
    parser.Add({.name = "help",
                .kind = ArgKind::Flag,
                .help = "Show this help and exit.",
                .short_name = 'h'});
    parser.Add({.name = "version",
                .kind = ArgKind::Flag,
                .help = "Print the version and exit.",
                .short_name = 'v'});
    parser.Add({.name = "build-info",
                .kind = ArgKind::Flag,
                .help = "Print compiler, hardening and crypto library details, then exit."});
    parser.Add({.name = "chain",
                .kind = ArgKind::String,
                .value_hint = "<network>",
                .help = "One of mainnet, testnet, regtest. Default: mainnet."});
    parser.Add({.name = "log-level",
                .kind = ArgKind::String,
                .value_hint = "<level>",
                .help = "One of error, warn, info, debug, trace. Default: info."});
    parser.Add({.name = "log-categories",
                .kind = ArgKind::String,
                .value_hint = "<list>",
                .help = "Comma-separated categories, 'all', or 'none'. Default: general."});
    parser.Add({.name = "log-file",
                .kind = ArgKind::String,
                .value_hint = "<path>",
                .help = "Also append log output to this file."});
    parser.Add({.name = "log-deterministic",
                .kind = ArgKind::Flag,
                .help = "Omit timestamps so logs from separate nodes can be diffed."});
}

/// Applies logging options. Returns false after reporting a usage error.
bool ConfigureLogging(const ArgsParser& parser) {
    if (parser.Has("log-level")) {
        const std::string name = parser.GetString("log-level");
        log::Level level{};
        if (!log::ParseLevel(name, &level)) {
            std::fprintf(
                stderr,
                "amariand: unknown --log-level '%s' (expected error, warn, info, debug or trace)\n",
                name.c_str());
            return false;
        }
        log::SetLevel(level);
    }

    if (parser.Has("log-categories")) {
        const std::string csv = parser.GetString("log-categories");
        std::string unknown;
        const uint32_t mask = log::ParseCategories(csv, &unknown);
        if (!unknown.empty()) {
            std::fprintf(stderr,
                         "amariand: unknown log category '%s' (known: %s, all, none)\n",
                         unknown.c_str(),
                         log::CategoryNames().c_str());
            return false;
        }
        log::DisableCategories(log::ToBits(log::Category::All));
        log::EnableCategories(mask);
    }

    if (parser.Has("log-deterministic")) {
        log::SetDeterministicOutput(parser.GetBool("log-deterministic"));
    }

    if (parser.Has("log-file")) {
        const std::string path = parser.GetString("log-file");
        if (!log::AddFileSink(path)) {
            std::fprintf(stderr, "amariand: cannot open log file '%s'\n", path.c_str());
            return false;
        }
    }
    return true;
}

/// Resolves --chain. Returns nullopt after reporting a usage error.
///
/// An unrecognised name is an error and never a default: silently falling back to the
/// network that holds real value is the wrong way to be wrong.
std::optional<Network> SelectNetwork(const ArgsParser& parser) {
    if (!parser.Has("chain")) {
        return Network::Mainnet;
    }
    const std::string name = parser.GetString("chain");
    const std::optional<Network> network = NetworkFromName(name);
    if (!network.has_value()) {
        std::fprintf(stderr,
                     "amariand: unknown --chain '%s' (expected mainnet, testnet or regtest)\n",
                     name.c_str());
        return std::nullopt;
    }
    return network;
}

/// Verifies that every signature scheme consensus knows about is actually usable in this
/// build, and reports the list.
///
/// A refusal to start rather than a warning. The scheme table is consensus: an output
/// locked to ML-DSA-44 is spendable only if this binary can verify ML-DSA-44, and a node
/// whose OpenSSL cannot provide it would not reject those spends — it would report them as
/// a scheme it does not know and, by the soft-fork rule, accept them unchecked. That is
/// precisely the failure that must never happen silently: a validator that believes it is
/// verifying signatures while verifying nothing.
bool CheckSignatureBackends() {
    if (const uint16_t missing = crypto::FirstUnavailableScheme();
        missing != crypto::SCHEME_RESERVED) {
        const crypto::SchemeSpec* spec = crypto::FindScheme(missing);
        AMARIAN_ERROR(log::Category::General,
                      "signature scheme {} ({}) is not available from this build's {}: refusing to "
                      "start, because a node that cannot verify a consensus scheme would accept "
                      "spends under it without checking them",
                      missing,
                      spec != nullptr ? spec->name : "unknown",
                      spec != nullptr ? spec->backend : "backend");
        return false;
    }

    for (const crypto::SchemeSpec& scheme : crypto::KnownSchemes()) {
        AMARIAN_INFO(log::Category::General,
                     "signature scheme {} {} ({}, {} byte key, {} byte signature) via {}",
                     scheme.id,
                     scheme.name,
                     scheme.scheme_class == crypto::SchemeClass::PostQuantum ? "post-quantum"
                                                                            : "classical",
                     scheme.public_key_bytes,
                     scheme.signature_bytes,
                     scheme.backend);
    }
    return true;
}

/// Verifies that this build's genesis parameters are the selected network's, and
/// reports the identity a node operator needs to confirm they are on the right chain.
///
/// A node whose block 0 differs from the network's shares no history with it at all,
/// so this is a refusal to start rather than a warning.
bool ReportChainIdentity(const ChainParams& params) {
    if (const GenesisFault fault = CheckGenesis(params); fault != GenesisFault::None) {
        AMARIAN_ERROR(log::Category::General,
                      "genesis check failed for {}: {}",
                      params.name,
                      Describe(fault));
        return false;
    }

    AMARIAN_INFO(
        log::Category::General, "network {} (chain_id {})", params.name, params.chain_id.ToHex());
    AMARIAN_INFO(log::Category::General,
                 "magic {:02x}{:02x}{:02x}{:02x}, p2p port {}, rpc port {}",
                 params.magic[0],
                 params.magic[1],
                 params.magic[2],
                 params.magic[3],
                 params.default_p2p_port,
                 params.default_rpc_port);
    AMARIAN_INFO(log::Category::General, "genesis {}", params.genesis_hash.ToHex());
    return true;
}

int Run(int argc, char* argv[]) {
    ArgsParser parser("amariand", "[options]");
    RegisterOptions(parser);

    if (const auto parsed = parser.Parse(argc, argv); !parsed.has_value()) {
        std::fprintf(stderr, "amariand: %s\n", parsed.error().Message().c_str());
        std::fprintf(stderr, "Try 'amariand --help'.\n");
        return EXIT_USAGE;
    }

    if (parser.Has("help")) {
        Print("Amarian node daemon " + VersionString() + "\n\n" + parser.HelpText());
        return EXIT_SUCCESS;
    }
    if (parser.Has("version")) {
        Print(VersionStringLong() + "\n");
        return EXIT_SUCCESS;
    }
    if (parser.Has("build-info")) {
        Print("Amarian build information\n" + BuildInfoString());
        return EXIT_SUCCESS;
    }

    if (!parser.Positional().empty()) {
        std::fprintf(
            stderr, "amariand: unexpected argument '%s'\n", parser.Positional().front().c_str());
        return EXIT_USAGE;
    }

    if (!ConfigureLogging(parser)) {
        return EXIT_USAGE;
    }

    const std::optional<Network> network = SelectNetwork(parser);
    if (!network.has_value()) {
        return EXIT_USAGE;
    }

    AMARIAN_INFO(log::Category::General, "Amarian {} starting", VersionStringLong());
    AMARIAN_INFO(log::Category::General, "user agent {}", UserAgent());

    if (!CheckSignatureBackends()) {
        return EXIT_FAILURE;
    }
    if (!ReportChainIdentity(ParamsFor(*network))) {
        return EXIT_FAILURE;
    }

    // Phase 1 replaces this with chainstate initialisation, block index load and
    // the node event loop. Reporting the gap is preferable to a silent no-op that
    // looks like a successful node start.
    AMARIAN_ERROR(log::Category::General,
                  "no chain backend in this build: block storage, validation and the network "
                  "layer land in Phase 1. Run with --build-info to inspect this build.");
    return EXIT_FAILURE;
}

}  // namespace
}  // namespace amarian

int main(int argc, char* argv[]) {
    return amarian::Run(argc, argv);
}
