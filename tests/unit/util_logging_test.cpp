#include <amarian/util/logging.hpp>

#include <gtest/gtest.h>

#include <string>

namespace amarian::log {
namespace {

/// Saves and restores the global sink configuration so tests do not leak state
/// into each other or into other test binaries sharing the process.
class LogStateGuard {
public:
    LogStateGuard() : level_(CurrentLevel()), categories_(EnabledCategories()) {}

    LogStateGuard(const LogStateGuard&) = delete;
    LogStateGuard& operator=(const LogStateGuard&) = delete;
    LogStateGuard(LogStateGuard&&) = delete;
    LogStateGuard& operator=(LogStateGuard&&) = delete;

    ~LogStateGuard() {
        SetLevel(level_);
        DisableCategories(ToBits(Category::All));
        EnableCategories(categories_);
        SetDeterministicOutput(false);
    }

private:
    Level level_;
    uint32_t categories_;
};

TEST(Logging, ParseLevelAcceptsKnownNamesOnly) {
    Level level = Level::Info;
    EXPECT_TRUE(ParseLevel("error", &level));
    EXPECT_EQ(level, Level::Error);
    EXPECT_TRUE(ParseLevel("trace", &level));
    EXPECT_EQ(level, Level::Trace);

    // A typo must not silently resolve to a default: an operator who asked for
    // debug output and got info would draw wrong conclusions from the log.
    EXPECT_FALSE(ParseLevel("verbose", &level));
    EXPECT_FALSE(ParseLevel("", &level));
    EXPECT_FALSE(ParseLevel("ERROR", &level));
    EXPECT_EQ(level, Level::Trace);
}

TEST(Logging, LevelNamesRoundTrip) {
    for (const Level level : {Level::Error, Level::Warn, Level::Info, Level::Debug, Level::Trace}) {
        Level parsed{};
        ASSERT_TRUE(ParseLevel(LevelName(level), &parsed)) << LevelName(level);
        EXPECT_EQ(parsed, level);
    }
}

TEST(Logging, ParseCategoriesBuildsMask) {
    std::string unknown;
    EXPECT_EQ(ParseCategories("net", &unknown), ToBits(Category::Net));
    EXPECT_TRUE(unknown.empty());

    EXPECT_EQ(ParseCategories("net,validation", &unknown),
              ToBits(Category::Net) | ToBits(Category::Validation));
    EXPECT_EQ(ParseCategories("all", &unknown), ToBits(Category::All));
    EXPECT_EQ(ParseCategories("none", &unknown), 0U);
    EXPECT_EQ(ParseCategories("", &unknown), 0U);
    EXPECT_TRUE(unknown.empty());
}

TEST(Logging, ParseCategoriesReportsUnknownToken) {
    std::string unknown;
    EXPECT_EQ(ParseCategories("net,nosuchcategory,db", &unknown), 0U);
    EXPECT_EQ(unknown, "nosuchcategory");
}

TEST(Logging, EveryCategoryNameParses) {
    const std::string names = CategoryNames();
    EXPECT_NE(names.find("validation"), std::string::npos);
    EXPECT_NE(names.find("reorg"), std::string::npos);

    std::string unknown;
    EXPECT_NE(ParseCategories(names, &unknown), 0U);
    EXPECT_TRUE(unknown.empty()) << unknown;
}

TEST(Logging, LevelGatesOutput) {
    const LogStateGuard guard;
    EnableCategories(ToBits(Category::All));

    SetLevel(Level::Error);
    EXPECT_TRUE(WillLog(Level::Error, Category::General));
    EXPECT_FALSE(WillLog(Level::Info, Category::General));

    SetLevel(Level::Debug);
    EXPECT_TRUE(WillLog(Level::Info, Category::General));
    EXPECT_TRUE(WillLog(Level::Debug, Category::General));
    EXPECT_FALSE(WillLog(Level::Trace, Category::General));
}

TEST(Logging, CategoryGatesOutputButNeverErrorsOrWarnings) {
    const LogStateGuard guard;
    SetLevel(Level::Trace);
    DisableCategories(ToBits(Category::All));
    EnableCategories(ToBits(Category::Net));

    EXPECT_TRUE(WillLog(Level::Info, Category::Net));
    EXPECT_FALSE(WillLog(Level::Info, Category::Mempool));

    // A consensus failure or a disk error must reach the log regardless of which
    // debug categories the operator happened to enable.
    EXPECT_TRUE(WillLog(Level::Error, Category::Mempool));
    EXPECT_TRUE(WillLog(Level::Warn, Category::Mempool));
}

}  // namespace
}  // namespace amarian::log
