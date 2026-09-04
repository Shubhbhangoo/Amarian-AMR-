#include <amarian/util/args.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace amarian {
namespace {

/// Builds an argv-shaped array. argv[0] is always the program name, matching
/// what main() receives, so the tests exercise the same skipping logic.
class Argv {
public:
    explicit Argv(std::vector<std::string> args) : storage_(std::move(args)) {
        pointers_.reserve(storage_.size() + 1);
        pointers_.push_back("amariand");
        for (const std::string& a : storage_) {
            pointers_.push_back(a.c_str());
        }
    }

    [[nodiscard]] int Count() const { return static_cast<int>(pointers_.size()); }

    [[nodiscard]] const char* const* Data() const { return pointers_.data(); }

private:
    std::vector<std::string> storage_;
    std::vector<const char*> pointers_;
};

ArgsParser MakeParser() {
    ArgsParser parser("amariand", "[options]");
    parser.Add({.name = "help", .kind = ArgKind::Flag, .help = "help", .short_name = 'h'});
    parser.Add({.name = "mine", .kind = ArgKind::Flag, .help = "mine"});
    parser.Add({.name = "datadir",
                .kind = ArgKind::String,
                .value_hint = "<dir>",
                .help = "data directory",
                .short_name = 'd'});
    parser.Add({.name = "port", .kind = ArgKind::Integer, .value_hint = "<n>", .help = "port"});
    return parser;
}

testing::AssertionResult ParseOk(ArgsParser& parser, std::vector<std::string> args) {
    const Argv argv(std::move(args));
    const auto result = parser.Parse(argv.Count(), argv.Data());
    if (result.has_value()) {
        return testing::AssertionSuccess();
    }
    return testing::AssertionFailure() << result.error().Message();
}

std::string ParseError(ArgsParser& parser, std::vector<std::string> args) {
    const Argv argv(std::move(args));
    const auto result = parser.Parse(argv.Count(), argv.Data());
    if (result.has_value()) {
        return {};
    }
    return std::string(result.error().Context());
}

TEST(Args, EmptyCommandLineSetsNothing) {
    ArgsParser parser = MakeParser();
    EXPECT_TRUE(ParseOk(parser, {}));
    EXPECT_FALSE(parser.Has("help"));
    EXPECT_FALSE(parser.Has("datadir"));
    EXPECT_TRUE(parser.Positional().empty());
    EXPECT_EQ(parser.GetString("datadir", "/default"), "/default");
    EXPECT_EQ(parser.GetInt("port", 9333), 9333);
    EXPECT_FALSE(parser.GetBool("mine", false));
}

TEST(Args, AcceptsInlineAndSeparatedValues) {
    ArgsParser inlined = MakeParser();
    EXPECT_TRUE(ParseOk(inlined, {"--datadir=/tmp/a", "--port=1234"}));
    EXPECT_EQ(inlined.GetString("datadir"), "/tmp/a");
    EXPECT_EQ(inlined.GetInt("port"), 1234);

    ArgsParser separated = MakeParser();
    EXPECT_TRUE(ParseOk(separated, {"--datadir", "/tmp/b", "--port", "4321"}));
    EXPECT_EQ(separated.GetString("datadir"), "/tmp/b");
    EXPECT_EQ(separated.GetInt("port"), 4321);
}

TEST(Args, ShortNamesWork) {
    ArgsParser parser = MakeParser();
    EXPECT_TRUE(ParseOk(parser, {"-h", "-d", "/tmp/c"}));
    EXPECT_TRUE(parser.Has("help"));
    EXPECT_EQ(parser.GetString("datadir"), "/tmp/c");
}

TEST(Args, FlagsAcceptExplicitBooleansAndNegation) {
    ArgsParser on = MakeParser();
    EXPECT_TRUE(ParseOk(on, {"--mine"}));
    EXPECT_TRUE(on.GetBool("mine"));

    ArgsParser off = MakeParser();
    EXPECT_TRUE(ParseOk(off, {"--no-mine"}));
    EXPECT_TRUE(off.Has("mine"));
    EXPECT_FALSE(off.GetBool("mine"));

    ArgsParser word = MakeParser();
    EXPECT_TRUE(ParseOk(word, {"--mine=off"}));
    EXPECT_FALSE(word.GetBool("mine"));
}

// The parser must refuse rather than guess. Each of these silently accepted
// would put a node in a state its operator did not ask for.
TEST(Args, RejectsUnknownOption) {
    ArgsParser parser = MakeParser();
    EXPECT_EQ(ParseError(parser, {"--netwrok=regtest"}), "args.unknown_option");
    EXPECT_EQ(ParseError(parser, {"-x"}), "args.unknown_option");
    EXPECT_EQ(ParseError(parser, {"-hd"}), "args.unknown_option");
}

TEST(Args, RejectsDuplicateOption) {
    ArgsParser parser = MakeParser();
    EXPECT_EQ(ParseError(parser, {"--datadir=/a", "--datadir=/b"}), "args.duplicate_option");
    EXPECT_EQ(ParseError(parser, {"--mine", "--no-mine"}), "args.duplicate_option");
}

TEST(Args, RejectsMissingValue) {
    ArgsParser parser = MakeParser();
    EXPECT_EQ(ParseError(parser, {"--datadir"}), "args.missing_value");
    EXPECT_EQ(ParseError(parser, {"--port"}), "args.missing_value");
}

TEST(Args, RejectsMalformedInteger) {
    ArgsParser parser = MakeParser();
    EXPECT_EQ(ParseError(parser, {"--port=12x"}), "args.invalid_integer");
    EXPECT_EQ(ParseError(parser, {"--port="}), "args.invalid_integer");
    EXPECT_EQ(ParseError(parser, {"--port=99999999999999999999"}), "args.invalid_integer");
}

TEST(Args, RejectsMalformedBoolean) {
    ArgsParser parser = MakeParser();
    EXPECT_EQ(ParseError(parser, {"--mine=yse"}), "args.invalid_boolean");
}

TEST(Args, RejectsNegatingAValueOption) {
    ArgsParser parser = MakeParser();
    EXPECT_EQ(ParseError(parser, {"--no-datadir"}), "args.not_a_flag");
}

TEST(Args, AcceptsNegativeIntegers) {
    ArgsParser parser = MakeParser();
    EXPECT_TRUE(ParseOk(parser, {"--port=-1"}));
    EXPECT_EQ(parser.GetInt("port"), -1);
}

TEST(Args, ValuesMayContainDashesAndEquals) {
    ArgsParser parser = MakeParser();
    EXPECT_TRUE(ParseOk(parser, {"--datadir=--not-an-option=x"}));
    EXPECT_EQ(parser.GetString("datadir"), "--not-an-option=x");
}

TEST(Args, DoubleDashEndsOptionParsing) {
    ArgsParser parser = MakeParser();
    EXPECT_TRUE(ParseOk(parser, {"--mine", "--", "--datadir", "positional"}));
    EXPECT_TRUE(parser.GetBool("mine"));
    EXPECT_FALSE(parser.Has("datadir"));
    ASSERT_EQ(parser.Positional().size(), 2U);
    EXPECT_EQ(parser.Positional()[0], "--datadir");
    EXPECT_EQ(parser.Positional()[1], "positional");
}

TEST(Args, CollectsPositionalArguments) {
    ArgsParser parser = MakeParser();
    EXPECT_TRUE(ParseOk(parser, {"getblock", "--mine", "0000"}));
    ASSERT_EQ(parser.Positional().size(), 2U);
    EXPECT_EQ(parser.Positional()[0], "getblock");
    EXPECT_EQ(parser.Positional()[1], "0000");
}

TEST(Args, FailedParseLeavesNoPartialState) {
    ArgsParser parser = MakeParser();
    EXPECT_EQ(ParseError(parser, {"--datadir=/good", "--bogus"}), "args.unknown_option");
    EXPECT_FALSE(parser.Has("datadir"));
}

TEST(Args, HelpTextListsEveryRegisteredOption) {
    const ArgsParser parser = MakeParser();
    const std::string help = parser.HelpText();
    EXPECT_NE(help.find("Usage: amariand [options]"), std::string::npos);
    EXPECT_NE(help.find("-h, --help"), std::string::npos);
    EXPECT_NE(help.find("--datadir <dir>"), std::string::npos);
    EXPECT_NE(help.find("data directory"), std::string::npos);
    EXPECT_NE(help.find("--port <n>"), std::string::npos);
}

}  // namespace
}  // namespace amarian
