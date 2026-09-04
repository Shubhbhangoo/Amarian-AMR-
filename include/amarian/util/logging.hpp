#pragma once

/// \file
/// Minimal level- and category-based logging.
///
/// Deliberately small: a node's log is an operational tool and, during incident
/// response, evidence. It must never allocate unpredictably on the validation hot path,
/// so category checks are a single atomic load and formatting only happens if enabled.

#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace amarian::log {

enum class Level : uint8_t { Error = 0, Warn = 1, Info = 2, Debug = 3, Trace = 4 };

/// Log categories are a bitmask so `-debug=net,validation` can enable subsets.
enum class Category : uint32_t {
    None = 0,
    General = 1U << 0U,
    Validation = 1U << 1U,
    Net = 1U << 2U,
    Mempool = 1U << 3U,
    Mining = 1U << 4U,
    Wallet = 1U << 5U,
    Db = 1U << 6U,
    Rpc = 1U << 7U,
    Bench = 1U << 8U,
    Reorg = 1U << 9U,
    All = 0xFFFFFFFFU,
};

[[nodiscard]] constexpr uint32_t ToBits(Category c) noexcept {
    return static_cast<uint32_t>(c);
}

/// Global sink configuration. Set once at startup, before threads spawn.
void SetLevel(Level level);
void EnableCategories(uint32_t mask);
void DisableCategories(uint32_t mask);
[[nodiscard]] Level CurrentLevel() noexcept;
[[nodiscard]] uint32_t EnabledCategories() noexcept;

/// Appends to `path` in addition to stderr. Returns false if the file cannot be opened.
bool AddFileSink(std::string_view path);
void CloseFileSink();

/// Makes every subsequent line prefix deterministic (no wall-clock timestamp), which
/// keeps golden-output tests and multi-node log diffs stable.
void SetDeterministicOutput(bool on);

[[nodiscard]] bool WillLog(Level level, Category category) noexcept;

/// Emits one already-formatted line. Prefer the AMARIAN_LOG* macros.
void Emit(Level level, Category category, std::string_view message);

/// Resolves a comma-separated list such as "net,validation" or "all".
/// Returns 0 and sets `unknown` when a token is not recognised.
[[nodiscard]] uint32_t ParseCategories(std::string_view csv, std::string* unknown);

[[nodiscard]] std::string_view LevelName(Level level) noexcept;
[[nodiscard]] std::string_view CategoryName(Category category) noexcept;

/// Resolves a level name ("error".."trace"). Returns false on an unknown name
/// rather than defaulting, so a mistyped `--log-level` cannot silently discard
/// the diagnostics an operator was trying to enable.
[[nodiscard]] bool ParseLevel(std::string_view name, Level* out) noexcept;

/// Comma-separated list of every category name, for `--help` text.
[[nodiscard]] std::string CategoryNames();

}  // namespace amarian::log

/// Formatting is skipped entirely when the level/category is disabled.
#define AMARIAN_LOG(level, category, ...)                                                          \
    do {                                                                                           \
        if (::amarian::log::WillLog((level), (category))) {                                        \
            ::amarian::log::Emit((level), (category), std::format(__VA_ARGS__));                   \
        }                                                                                          \
    } while (false)

#define AMARIAN_ERROR(cat, ...) AMARIAN_LOG(::amarian::log::Level::Error, (cat), __VA_ARGS__)
#define AMARIAN_WARN(cat, ...) AMARIAN_LOG(::amarian::log::Level::Warn, (cat), __VA_ARGS__)
#define AMARIAN_INFO(cat, ...) AMARIAN_LOG(::amarian::log::Level::Info, (cat), __VA_ARGS__)
#define AMARIAN_DEBUG(cat, ...) AMARIAN_LOG(::amarian::log::Level::Debug, (cat), __VA_ARGS__)
#define AMARIAN_TRACE(cat, ...) AMARIAN_LOG(::amarian::log::Level::Trace, (cat), __VA_ARGS__)
