#pragma once

/// \file
/// Schema-driven command-line parsing.
///
/// Options are declared up front rather than probed ad hoc. That buys two things
/// a node genuinely needs:
///
///   * `--help` is generated from the same data the parser uses, so it cannot
///     drift out of date.
///   * An unrecognised or malformed option is a hard error. Silently ignoring a
///     mistyped `--network=regtest` would start a node on mainnet, and silently
///     ignoring a mistyped `--datadir` would quietly build a second chainstate
///     in the wrong place. Neither failure is acceptable, so the parser refuses
///     rather than guesses.
///
/// Duplicate options are also an error, not last-wins: `--network=regtest
/// --network=mainnet` is an operator mistake and the safe response is to stop.

#include <amarian/util/result.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace amarian {

enum class ArgKind : uint8_t {
    /// Presence-only. Accepts `--flag`, `--flag=true`, `--flag=0`, `--no-flag`.
    Flag,
    /// Requires a value: `--key=value` or `--key value`.
    String,
    /// Requires a value parsable as a signed 64-bit decimal integer.
    Integer,
};

struct ArgSpec {
    /// Long name without leading dashes, e.g. "datadir". Required.
    std::string_view name;
    ArgKind kind = ArgKind::Flag;
    /// Placeholder shown in help for value-taking options, e.g. "<dir>".
    std::string_view value_hint = {};
    std::string_view help = {};
    /// Optional single-character alias, '\0' for none.
    char short_name = '\0';
};

class ArgsParser {
public:
    ArgsParser(std::string_view program, std::string_view usage_line);

    /// Registers an option. Names must be unique; a duplicate registration is a
    /// programming error and terminates via assertion in debug builds.
    void Add(const ArgSpec& spec);

    /// Parses argv, skipping argv[0]. `--` ends option parsing; everything after
    /// it is positional. On failure nothing is committed and the parser stays
    /// usable only for HelpText().
    [[nodiscard]] Status Parse(int argc, const char* const* argv);

    [[nodiscard]] bool Has(std::string_view name) const;

    /// Value accessors. Asking for a name that was never registered is a
    /// programming error and asserts, because a typo in code must not silently
    /// read as "absent".
    [[nodiscard]] std::string GetString(std::string_view name,
                                        std::string_view fallback = {}) const;
    [[nodiscard]] int64_t GetInt(std::string_view name, int64_t fallback = 0) const;
    [[nodiscard]] bool GetBool(std::string_view name, bool fallback = false) const;

    [[nodiscard]] const std::vector<std::string>& Positional() const noexcept {
        return positional_;
    }

    /// Full `--help` body, generated from the registered specs.
    [[nodiscard]] std::string HelpText() const;

private:
    struct Parsed {
        std::string name;
        std::string value;
    };

    [[nodiscard]] const ArgSpec* FindLong(std::string_view name) const;
    [[nodiscard]] const ArgSpec* FindShort(char c) const;
    [[nodiscard]] const Parsed* Find(std::string_view name) const;

    std::string program_;
    std::string usage_;
    std::vector<ArgSpec> specs_;
    std::vector<Parsed> parsed_;
    std::vector<std::string> positional_;
};

}  // namespace amarian
