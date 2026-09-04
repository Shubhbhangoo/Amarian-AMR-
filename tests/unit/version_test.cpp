#include <amarian/version.hpp>

#include <gtest/gtest.h>

#include <string>

namespace amarian {
namespace {

TEST(Version, VersionStringIsNonEmptyAndDotted) {
    const std::string version = VersionString();
    ASSERT_FALSE(version.empty());
    EXPECT_NE(version.find('.'), std::string::npos);
}

TEST(Version, LongVersionCarriesTheCommit) {
    const std::string long_version = VersionStringLong();
    EXPECT_NE(long_version.find(VersionString()), std::string::npos);
    EXPECT_NE(long_version.find('('), std::string::npos);
    EXPECT_NE(long_version.find(')'), std::string::npos);
}

// The user agent is network-visible. Peers parse it, so its shape is part of the
// protocol surface even though its contents are not consensus-critical.
TEST(Version, UserAgentIsBip14Shaped) {
    const std::string agent = UserAgent();
    ASSERT_GE(agent.size(), 4U);
    EXPECT_EQ(agent.front(), '/');
    EXPECT_EQ(agent.back(), '/');
    EXPECT_TRUE(agent.starts_with("/Amarian:"));
    // A commit hash or build metadata must not leak into what peers see.
    EXPECT_EQ(agent.find('('), std::string::npos);
    EXPECT_LE(agent.size(), 64U);
}

// Release version and protocol version are deliberately independent: bumping a
// release must never move the wire protocol. Pinned as compile-time assertions so
// a change to either constant has to be made deliberately.
static_assert(MIN_PEER_PROTOCOL_VERSION <= PROTOCOL_VERSION,
              "cannot require peers to speak a protocol newer than this build's");
static_assert(PROTOCOL_VERSION == 1,
              "bumping PROTOCOL_VERSION is a wire-protocol change: update "
              "the handshake compatibility tests and docs/NETWORK.md");

TEST(Version, BuildInfoReportsTheThingsABugReportNeeds) {
    const std::string info = BuildInfoString();
    for (const char* key : {"version",
                            "compiler",
                            "build type",
                            "hardening",
                            "sanitizers",
                            "libsecp256k1",
                            "OpenSSL (build)",
                            "OpenSSL (runtime)"}) {
        EXPECT_NE(info.find(key), std::string::npos) << "missing: " << key;
    }
}

// Guards against shipping a build whose OpenSSL predates FIPS 204/205 support in
// the default provider. BuildInfoString annotates that case; if the annotation
// appears here, the test environment cannot support post-quantum signatures.
TEST(Version, RuntimeOpenSslSupportsPostQuantumSignatures) {
    EXPECT_EQ(BuildInfoString().find("too old for FIPS 204/205"), std::string::npos);
}

}  // namespace
}  // namespace amarian
