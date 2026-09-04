#include <amarian/util/args.hpp>

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <span>
#include <system_error>
#include <utility>

namespace amarian {
namespace {

/// Accepted spellings for boolean values on a Flag option. Anything else is an
/// error rather than a silent false, so `--mine=yse` cannot quietly disable mining.
constexpr std::string_view kTrueWords[] = {"1", "true", "yes", "on"};
constexpr std::string_view kFalseWords[] = {"0", "false", "no", "off"};

[[nodiscard]] bool Contains(std::span<const std::string_view> words, std::string_view value) {
    return std::ranges::find(words, value) != words.end();
}

}  // namespace

ArgsParser::ArgsParser(std::string_view program, std::string_view usage_line)
    : program_(program), usage_(usage_line) {}

void ArgsParser::Add(const ArgSpec& spec) {
    assert(!spec.name.empty() && "option must have a long name");
    assert(FindLong(spec.name) == nullptr && "duplicate option registration");
    assert((spec.short_name == '\0' || FindShort(spec.short_name) == nullptr) &&
           "duplicate short option registration");
    specs_.push_back(spec);
}

const ArgSpec* ArgsParser::FindLong(std::string_view name) const {
    const auto it =
        std::ranges::find_if(specs_, [name](const ArgSpec& s) { return s.name == name; });
    return it == specs_.end() ? nullptr : &*it;
}

const ArgSpec* ArgsParser::FindShort(char c) const {
    if (c == '\0') {
        return nullptr;
    }
    const auto it =
        std::ranges::find_if(specs_, [c](const ArgSpec& s) { return s.short_name == c; });
    return it == specs_.end() ? nullptr : &*it;
}

const ArgsParser::Parsed* ArgsParser::Find(std::string_view name) const {
    const auto it =
        std::ranges::find_if(parsed_, [name](const Parsed& p) { return p.name == name; });
    return it == parsed_.end() ? nullptr : &*it;
}

bool ArgsParser::Has(std::string_view name) const {
    assert(FindLong(name) != nullptr && "queried an unregistered option");
    return Find(name) != nullptr;
}

std::string ArgsParser::GetString(std::string_view name, std::string_view fallback) const {
    assert(FindLong(name) != nullptr && "queried an unregistered option");
    const Parsed* p = Find(name);
    return p == nullptr ? std::string(fallback) : p->value;
}

int64_t ArgsParser::GetInt(std::string_view name, int64_t fallback) const {
    assert(FindLong(name) != nullptr && "queried an unregistered option");
    const Parsed* p = Find(name);
    if (p == nullptr) {
        return fallback;
    }
    // Parse validity was already established during Parse().
    int64_t out = fallback;
    const char* begin = p->value.data();
    const char* end = begin + p->value.size();
    const auto result = std::from_chars(begin, end, out);
    if (result.ec != std::errc{} || result.ptr != end) {
        return fallback;
    }
    return out;
}

bool ArgsParser::GetBool(std::string_view name, bool fallback) const {
    assert(FindLong(name) != nullptr && "queried an unregistered option");
    const Parsed* p = Find(name);
    if (p == nullptr) {
        return fallback;
    }
    return Contains(kTrueWords, p->value);
}

Status ArgsParser::Parse(int argc, const char* const* argv) {
    std::vector<Parsed> parsed;
    std::vector<std::string> positional;
    bool options_ended = false;

    const auto commit = [&](const ArgSpec& spec, std::string value) -> Status {
        const auto dup =
            std::ranges::find_if(parsed, [&spec](const Parsed& p) { return p.name == spec.name; });
        if (dup != parsed.end()) {
            return Fail("args.duplicate_option", "--" + std::string(spec.name));
        }
        parsed.push_back(Parsed{std::string(spec.name), std::move(value)});
        return Ok();
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        if (options_ended || arg.empty() || arg.front() != '-' || arg == "-") {
            positional.emplace_back(arg);
            continue;
        }
        if (arg == "--") {
            options_ended = true;
            continue;
        }

        // Split off an inline `=value` before any name lookup, so that a value
        // containing dashes or equals signs is never mistaken for an option.
        std::string_view token = arg;
        std::string_view inline_value;
        bool has_inline = false;
        if (const size_t eq = token.find('='); eq != std::string_view::npos) {
            inline_value = token.substr(eq + 1);
            token = token.substr(0, eq);
            has_inline = true;
        }

        const ArgSpec* spec = nullptr;
        bool negated = false;

        if (token.starts_with("--")) {
            std::string_view name = token.substr(2);
            spec = FindLong(name);
            if (spec == nullptr && name.starts_with("no-")) {
                spec = FindLong(name.substr(3));
                negated = spec != nullptr;
            }
        } else {
            const std::string_view shorts = token.substr(1);
            if (shorts.size() != 1) {
                return Fail("args.unknown_option", std::string(arg));
            }
            spec = FindShort(shorts.front());
        }

        if (spec == nullptr) {
            return Fail("args.unknown_option", std::string(token));
        }

        switch (spec->kind) {
            case ArgKind::Flag: {
                if (negated && has_inline) {
                    return Fail("args.conflicting_value", std::string(arg));
                }
                std::string value = negated ? "0" : "1";
                if (has_inline) {
                    if (Contains(kTrueWords, inline_value)) {
                        value = "1";
                    } else if (Contains(kFalseWords, inline_value)) {
                        value = "0";
                    } else {
                        return Fail("args.invalid_boolean", std::string(arg));
                    }
                }
                AMARIAN_TRY(commit(*spec, std::move(value)));
                break;
            }
            case ArgKind::String:
            case ArgKind::Integer: {
                if (negated) {
                    return Fail("args.not_a_flag", std::string(token));
                }
                std::string value;
                if (has_inline) {
                    value = inline_value;
                } else {
                    if (i + 1 >= argc) {
                        return Fail("args.missing_value", "--" + std::string(spec->name));
                    }
                    ++i;
                    value = argv[i];
                }
                if (spec->kind == ArgKind::Integer) {
                    int64_t probe = 0;
                    const char* begin = value.data();
                    const char* end = begin + value.size();
                    const auto r = std::from_chars(begin, end, probe);
                    if (r.ec != std::errc{} || r.ptr != end) {
                        return Fail("args.invalid_integer",
                                    "--" + std::string(spec->name) + "=" + value);
                    }
                }
                AMARIAN_TRY(commit(*spec, std::move(value)));
                break;
            }
        }
    }

    parsed_ = std::move(parsed);
    positional_ = std::move(positional);
    return Ok();
}

std::string ArgsParser::HelpText() const {
    std::string out;
    out += "Usage: ";
    out += program_;
    out += ' ';
    out += usage_;
    out += "\n\nOptions:\n";

    size_t width = 0;
    std::vector<std::string> left;
    left.reserve(specs_.size());
    for (const ArgSpec& s : specs_) {
        std::string entry;
        if (s.short_name != '\0') {
            entry += '-';
            entry += s.short_name;
            entry += ", ";
        } else {
            entry += "    ";
        }
        entry += "--";
        entry += s.name;
        if (!s.value_hint.empty()) {
            entry += ' ';
            entry += s.value_hint;
        }
        width = std::max(width, entry.size());
        left.push_back(std::move(entry));
    }

    for (size_t i = 0; i < specs_.size(); ++i) {
        out += "  ";
        out += left[i];
        out.append(width - left[i].size(), ' ');
        out += "  ";
        out += specs_[i].help;
        out += '\n';
    }
    return out;
}

}  // namespace amarian
