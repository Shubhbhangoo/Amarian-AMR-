/// \file
/// The Amarian node daemon.
///
/// Phase 0 scope: option handling, logging setup, and build identification.
/// The chain, storage and networking layers are introduced in later phases and
/// wired in here. The binary deliberately does not pretend to do more than it
/// does — `amariand --version` and `--build-info` are real, and everything else
/// reports honestly that it is not yet available.

#include <amarian/util/args.hpp>
#include <amarian/util/logging.hpp>
#include <amarian/version.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace amarian {
namespace {

constexpr int EXIT_USAGE = 2;

void Print(const std::string& text) {
    (void)std::fwrite(text.data(), 1, text.size(), stdout);
}

void RegisterOptions(ArgsParser& parser) {
    parser.Add({.name = "help", .kind = ArgKind::Flag, .help = "Show this help and exit.",
                .short_name = 'h'});
    parser.Add({.name = "version",
                .kind = ArgKind::Flag,
                .help = "Print the version and exit.",
                .short_name = 'v'});
    parser.Add({.name = "build-info",
                .kind = ArgKind::Flag,
                .help = "Print compiler, hardening and crypto library details, then exit."});
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
            std::fprintf(stderr, "amariand: unknown --log-level '%s' (expected error, warn, info, debug or trace)\n",
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
            std::fprintf(stderr, "amariand: unknown log category '%s' (known: %s, all, none)\n",
                         unknown.c_str(), log::CategoryNames().c_str());
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
        std::fprintf(stderr, "amariand: unexpected argument '%s'\n",
                     parser.Positional().front().c_str());
        return EXIT_USAGE;
    }

    if (!ConfigureLogging(parser)) {
        return EXIT_USAGE;
    }

    AMARIAN_INFO(log::Category::General, "Amarian {} starting", VersionStringLong());
    AMARIAN_INFO(log::Category::General, "user agent {}", UserAgent());

    // Phase 1 replaces this with chainstate initialisation, block index load and
    // the node event loop. Reporting the gap is preferable to a silent no-op that
    // looks like a successful node start.
    AMARIAN_ERROR(log::Category::General,
                  "no chain backend in this build: the consensus, storage and network layers "
                  "land in Phase 1. Run with --build-info to inspect this build.");
    return EXIT_FAILURE;
}

}  // namespace
}  // namespace amarian

int main(int argc, char* argv[]) {
    return amarian::Run(argc, argv);
}
