#include <amarian/util/logging.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace amarian::log {

namespace {

struct CategoryEntry {
    std::string_view name;
    Category value;
};

constexpr std::array<CategoryEntry, 10> CATEGORY_TABLE{{
    {"general", Category::General},
    {"validation", Category::Validation},
    {"net", Category::Net},
    {"mempool", Category::Mempool},
    {"mining", Category::Mining},
    {"wallet", Category::Wallet},
    {"db", Category::Db},
    {"rpc", Category::Rpc},
    {"bench", Category::Bench},
    {"reorg", Category::Reorg},
}};

std::atomic<Level> g_level{Level::Info};
std::atomic<uint32_t> g_categories{ToBits(Category::General)};
std::atomic<bool> g_deterministic{false};

std::mutex& SinkMutex() {
    static std::mutex m;
    return m;
}

std::FILE*& FileSink() {
    static std::FILE* f = nullptr;
    return f;
}

[[nodiscard]] std::string Timestamp() {
    if (g_deterministic.load(std::memory_order_relaxed)) {
        return {};
    }
    const auto now = std::chrono::system_clock::now();
    return std::format("{:%Y-%m-%dT%H:%M:%S}Z ",
                       std::chrono::floor<std::chrono::milliseconds>(
                           std::chrono::time_point_cast<std::chrono::milliseconds>(now)));
}

}  // namespace

void SetLevel(Level level) {
    g_level.store(level, std::memory_order_relaxed);
}

void EnableCategories(uint32_t mask) {
    g_categories.fetch_or(mask, std::memory_order_relaxed);
}

void DisableCategories(uint32_t mask) {
    g_categories.fetch_and(~mask, std::memory_order_relaxed);
}

Level CurrentLevel() noexcept {
    return g_level.load(std::memory_order_relaxed);
}

uint32_t EnabledCategories() noexcept {
    return g_categories.load(std::memory_order_relaxed);
}

void SetDeterministicOutput(bool on) {
    g_deterministic.store(on, std::memory_order_relaxed);
}

bool AddFileSink(std::string_view path) {
    const std::lock_guard lock(SinkMutex());
    CloseFileSink();
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    std::FILE* f = std::fopen(std::string(path).c_str(), "ae");
    if (f == nullptr) {
        return false;
    }
    FileSink() = f;
    return true;
}

void CloseFileSink() {
    std::FILE*& f = FileSink();
    if (f != nullptr) {
        (void)std::fclose(f);
        f = nullptr;
    }
}

bool WillLog(Level level, Category category) noexcept {
    if (static_cast<uint8_t>(level) > static_cast<uint8_t>(CurrentLevel())) {
        return false;
    }
    // Errors and warnings are never filtered by category: they must always be visible.
    if (level == Level::Error || level == Level::Warn) {
        return true;
    }
    return (EnabledCategories() & ToBits(category)) != 0U;
}

void Emit(Level level, Category category, std::string_view message) {
    const std::string line = std::format(
        "{}[{}] [{}] {}\n", Timestamp(), LevelName(level), CategoryName(category), message);
    const std::lock_guard lock(SinkMutex());
    (void)std::fwrite(line.data(), 1, line.size(), stderr);
    if (std::FILE* f = FileSink(); f != nullptr) {
        (void)std::fwrite(line.data(), 1, line.size(), f);
        (void)std::fflush(f);
    }
}

uint32_t ParseCategories(std::string_view csv, std::string* unknown) {
    uint32_t mask = 0;
    size_t pos = 0;
    while (pos <= csv.size()) {
        const size_t comma = csv.find(',', pos);
        const std::string_view token =
            csv.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        if (!token.empty()) {
            if (token == "all" || token == "1") {
                mask |= ToBits(Category::All);
            } else if (token == "none" || token == "0") {
                mask = 0;
            } else {
                bool found = false;
                for (const auto& entry : CATEGORY_TABLE) {
                    if (entry.name == token) {
                        mask |= ToBits(entry.value);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    if (unknown != nullptr) {
                        *unknown = std::string(token);
                    }
                    return 0;
                }
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    return mask;
}

std::string_view LevelName(Level level) noexcept {
    switch (level) {
        case Level::Error:
            return "error";
        case Level::Warn:
            return "warn";
        case Level::Info:
            return "info";
        case Level::Debug:
            return "debug";
        case Level::Trace:
            return "trace";
    }
    return "?";
}

std::string_view CategoryName(Category category) noexcept {
    for (const auto& entry : CATEGORY_TABLE) {
        if (entry.value == category) {
            return entry.name;
        }
    }
    return "general";
}

bool ParseLevel(std::string_view name, Level* out) noexcept {
    constexpr std::array<std::pair<std::string_view, Level>, 5> LEVEL_TABLE{{
        {"error", Level::Error},
        {"warn", Level::Warn},
        {"info", Level::Info},
        {"debug", Level::Debug},
        {"trace", Level::Trace},
    }};
    for (const auto& [text, value] : LEVEL_TABLE) {
        if (text == name) {
            if (out != nullptr) {
                *out = value;
            }
            return true;
        }
    }
    return false;
}

std::string CategoryNames() {
    std::string out;
    for (const auto& entry : CATEGORY_TABLE) {
        if (!out.empty()) {
            out += ',';
        }
        out += entry.name;
    }
    return out;
}

}  // namespace amarian::log
