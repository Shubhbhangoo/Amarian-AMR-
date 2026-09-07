#include <amarian/build_config.hpp>
#include <amarian/version.hpp>

#include <openssl/crypto.h>
#include <openssl/opensslv.h>

#include <string>
#include <string_view>

namespace amarian {
namespace {

/// Whether the OpenSSL providing ML-DSA / SLH-DSA at runtime is new enough.
/// FIPS 204 / 205 signatures arrived in the default provider in OpenSSL 3.5.
/// Reported rather than asserted here; the crypto layer enforces it.
constexpr unsigned long MIN_OPENSSL_RUNTIME = 0x30500000UL;

std::string_view Prerelease() {
    return AMARIAN_VERSION_PRERELEASE;
}

}  // namespace

std::string VersionString() {
    std::string version = AMARIAN_VERSION_STRING;
    if (!Prerelease().empty()) {
        version += '-';
        version += Prerelease();
    }
    return version;
}

std::string VersionStringLong() {
    std::string out = VersionString();
    out += " (";
    out += AMARIAN_GIT_COMMIT;
#if AMARIAN_GIT_DIRTY
    {
        out += ", modified working tree";
    }
#endif
    out += ')';
    return out;
}

std::string UserAgent() {
    return "/Amarian:" + VersionString() + "/";
}

std::string BuildInfoString() {
    const unsigned long openssl_runtime = OpenSSL_version_num();

    std::string out;
    out.reserve(768);

    const auto line = [&out](std::string_view key, std::string_view value) {
        out += "  ";
        out += key;
        for (size_t i = key.size(); i < 22; ++i) {
            out += ' ';
        }
        out += ": ";
        out += value;
        out += '\n';
    };

    line("version", VersionStringLong());
    line("protocol version", std::to_string(PROTOCOL_VERSION));
    line("compiler", std::string(AMARIAN_COMPILER_ID) + " " + AMARIAN_COMPILER_VERSION);
    line("C++ standard", std::to_string(__cplusplus));
    line("build type", AMARIAN_BUILD_TYPE);
    line("target", std::string(AMARIAN_TARGET_SYSTEM) + "/" + AMARIAN_TARGET_PROCESSOR);
    line("hardening", AMARIAN_HARDENING_ENABLED != 0 ? "enabled" : "DISABLED");

    // if constexpr, not a runtime conditional: AMARIAN_SANITIZER_STRING is a
    // compile-time constant, so a runtime branch here leaves dead code that
    // -Wunreachable-code-aggressive correctly objects to.
    constexpr std::string_view SANITIZER = AMARIAN_SANITIZER_STRING;
    if constexpr (SANITIZER.empty()) {
        line("sanitizers", "none");
    } else {
        line("sanitizers", SANITIZER);
    }

    line("libsecp256k1", AMARIAN_SECP256K1_VERSION);
    line("OpenSSL (build)", AMARIAN_OPENSSL_VERSION);

    std::string runtime = OpenSSL_version(OPENSSL_VERSION_STRING);
    if (openssl_runtime < MIN_OPENSSL_RUNTIME) {
        runtime += "  [too old for FIPS 204/205 signatures]";
    }
    line("OpenSSL (runtime)", runtime);

    return out;
}

}  // namespace amarian
