/// \file
/// Fuzz target for the command-line parser.
///
/// argv is operator-supplied rather than attacker-supplied, so this harness is
/// about robustness, not remote attack surface. It earns its place anyway: the
/// parser decides which network a node joins and where its chainstate lives, and
/// it runs before any other subsystem exists to sanity-check the result. A parser
/// that mis-splits `--datadir=/x=y`, or silently drops an option it could not
/// understand, turns an operator typo into a second chainstate in the wrong place
/// or a node quietly on the wrong network.
///
/// The input is treated as NUL-separated argv fields, which is the shape argv
/// actually has coming out of execve, and gives libFuzzer a separator to mutate.
///
/// Properties checked on every input:
///
///   1. Parsing is deterministic, and never mutates the option schema.
///   2. A failed parse commits nothing: no option set, no positional kept.
///   3. An absent option reports the caller's fallback.
///   4. Flag values are normalised to "0"/"1", and GetBool agrees with them.
///   5. An integer Parse accepted is an integer GetInt can re-read.
///   6. Every stored value really came from argv rather than being fabricated.
///   7. An option-shaped argument only becomes positional after `--`.

#include <amarian/util/args.hpp>
#include <amarian/util/result.hpp>

#include "fuzz_assert.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using amarian::ArgKind;
using amarian::ArgsParser;
using amarian::ArgSpec;
using amarian::Status;

/// argv[0] is skipped by Parse(), so letting the fuzzer control it would only
/// waste mutation budget.
constexpr std::string_view kArgv0 = "fuzz_args";

/// Enough fields to reach every branch (duplicates, terminators, dangling
/// values) while keeping the O(fields) property checks cheap per input.
constexpr size_t kMaxFields = 64;

/// One option of each kind, with and without a short alias, plus two names where
/// one is a prefix of the other so that exact-match lookup is exercised.
///
/// "no-mine" is registered deliberately: it makes `--no-mine` ambiguous between a
/// literal option and the negation of "mine". The parser resolves that in favour
/// of the literal name, and this harness pins that it does so without crashing or
/// storing a malformed value. It is a schema a careless caller could write, not a
/// schema Amarian itself uses.
constexpr ArgSpec kSpecs[] = {
    {.name = "help", .kind = ArgKind::Flag, .help = "print help", .short_name = 'h'},
    {.name = "mine", .kind = ArgKind::Flag, .help = "mine blocks"},
    {.name = "no-mine", .kind = ArgKind::Flag, .help = "literal name shadowing a negation"},
    {.name = "datadir",
     .kind = ArgKind::String,
     .value_hint = "<dir>",
     .help = "data directory",
     .short_name = 'd'},
    {.name = "log", .kind = ArgKind::String, .value_hint = "<spec>", .help = "log categories"},
    {.name = "log-level", .kind = ArgKind::String, .value_hint = "<level>", .help = "log level"},
    {.name = "port",
     .kind = ArgKind::Integer,
     .value_hint = "<n>",
     .help = "listen port",
     .short_name = 'p'},
    {.name = "dbcache", .kind = ArgKind::Integer, .value_hint = "<mib>", .help = "cache size"},
};

ArgsParser BuildParser() {
    ArgsParser parser(kArgv0, "[options] [positional...]");
    for (const ArgSpec& spec : kSpecs) {
        parser.Add(spec);
    }
    return parser;
}

/// Splits the input into NUL-separated fields, prepending argv[0]. N separators
/// yield N+1 fields, so empty fields — which execve can genuinely produce — are
/// reachable.
std::vector<std::string> SplitFields(const uint8_t* data, size_t size) {
    std::vector<std::string> fields;
    fields.emplace_back(kArgv0);

    const char* text = reinterpret_cast<const char*>(data);
    size_t start = 0;
    for (size_t i = 0; i <= size && size > 0; ++i) {
        if (i != size && data[i] != 0) {
            continue;
        }
        if (fields.size() >= kMaxFields) {
            break;
        }
        fields.emplace_back(text + start, i - start);
        start = i + 1;
    }
    return fields;
}

/// True when `value` is a suffix of any argv field. A stored value is either a
/// whole field or the part after an inline '=', and both are suffixes; anything
/// else means the parser fabricated or truncated it.
bool IsSuffixOfSomeField(const std::vector<std::string>& fields, std::string_view value) {
    return std::ranges::any_of(fields, [value](const std::string& field) {
        return std::string_view(field).ends_with(value);
    });
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::vector<std::string> fields = SplitFields(data, size);

    std::vector<const char*> argv;
    argv.reserve(fields.size());
    for (const std::string& field : fields) {
        argv.push_back(field.c_str());
    }
    const int argc = static_cast<int>(argv.size());

    ArgsParser parser = BuildParser();
    const std::string help = parser.HelpText();
    const Status status = parser.Parse(argc, argv.data());

    // Property 1: Parse reads the schema and must not write to it. A change here
    // would mean Parse corrupted specs_, which HelpText renders verbatim.
    FUZZ_CHECK(parser.HelpText() == help, "Parse mutated the option schema");

    ArgsParser again = BuildParser();
    const Status status_again = again.Parse(argc, argv.data());
    FUZZ_CHECK(status_again.has_value() == status.has_value(),
               "parse outcome is not deterministic");
    FUZZ_CHECK(again.Positional() == parser.Positional(),
               "positional arguments are not deterministic");
    for (const ArgSpec& spec : kSpecs) {
        FUZZ_CHECK(again.Has(spec.name) == parser.Has(spec.name),
                   "option presence is not deterministic");
        FUZZ_CHECK(again.GetString(spec.name) == parser.GetString(spec.name),
                   "option value is not deterministic");
    }

    if (!status.has_value()) {
        // Property 2: a rejected command line leaves the parser empty, so a
        // caller that ignores the Status cannot act on half-applied options.
        FUZZ_CHECK(parser.Positional().empty(), "failed parse kept positional arguments");
        for (const ArgSpec& spec : kSpecs) {
            FUZZ_CHECK(!parser.Has(spec.name), "failed parse committed an option");
        }
        FUZZ_CHECK(!status.error().Context().empty(), "parse failure carried no error code");
        return 0;
    }

    for (const ArgSpec& spec : kSpecs) {
        if (!parser.Has(spec.name)) {
            // Property 3: absent means absent, not empty-string or zero.
            FUZZ_CHECK(parser.GetString(spec.name, "fallback") == "fallback",
                       "absent option ignored the string fallback");
            FUZZ_CHECK(parser.GetInt(spec.name, 4242) == 4242,
                       "absent option ignored the integer fallback");
            FUZZ_CHECK(parser.GetBool(spec.name, true), "absent option ignored the bool fallback");
            continue;
        }

        const std::string value = parser.GetString(spec.name);
        switch (spec.kind) {
            case ArgKind::Flag:
                // Property 4: `--mine`, `--mine=yes` and `--no-mine=0` must all
                // collapse to the same two spellings, or downstream code that reads
                // the raw string would see spurious distinctions.
                FUZZ_CHECK(value == "0" || value == "1", "flag stored something other than 0 or 1");
                FUZZ_CHECK(parser.GetBool(spec.name) == (value == "1"),
                           "GetBool disagrees with the stored flag");
                break;
            case ArgKind::Integer:
                // Property 5: Parse and GetInt must use the same acceptance rule.
                // Two different fallbacks can only agree if neither was needed.
                FUZZ_CHECK(parser.GetInt(spec.name, std::numeric_limits<int64_t>::min()) ==
                               parser.GetInt(spec.name, std::numeric_limits<int64_t>::max()),
                           "Parse accepted an integer GetInt cannot re-read");
                [[fallthrough]];
            case ArgKind::String:
                // Property 6.
                FUZZ_CHECK(IsSuffixOfSomeField(fields, value),
                           "stored value is not present in argv");
                break;
        }
    }

    // Property 7: only an explicit `--` may push option-shaped arguments into the
    // positional list. Checked conservatively — if `--` appears anywhere, even as
    // an option value, the check is skipped rather than risking a false report.
    if (std::ranges::find(fields, "--") == fields.end()) {
        for (const std::string& arg : parser.Positional()) {
            FUZZ_CHECK(arg.empty() || arg == "-" || arg.front() != '-',
                       "an option-shaped argument was silently treated as positional");
        }
    }

    return 0;
}
