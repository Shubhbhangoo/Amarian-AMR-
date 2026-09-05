/// \file
/// The scheme registry's implementation: a table, a size check, and a call into
/// somebody else's cryptography.
///
/// The three backends are reached through their own recommended interfaces —
/// libsecp256k1's static verification context, and OpenSSL 3.5's one-shot
/// `EVP_PKEY_verify_message` path for the two NIST schemes. Nothing here
/// reimplements, wraps, or "optimises" a primitive.

#include <amarian/crypto/signature.hpp>

#include <openssl/err.h>
#include <openssl/evp.h>

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace amarian::crypto {
namespace {

// --- The table ---------------------------------------------------------------
//
// Sizes are the values the implementations actually produce on this machine,
// measured by scripts/probe_signature_apis.sh rather than copied from a
// specification, and re-asserted by the crypto tests so a library upgrade that
// changed one would fail the build's tests rather than start rejecting valid
// signatures.

constexpr std::array<SchemeSpec, 3> SCHEMES{{
    SchemeSpec{.id = SCHEME_SCHNORR_SECP256K1,
               .name = "schnorr-secp256k1",
               .public_key_bytes = 32,
               .signature_bytes = 64,
               .scheme_class = SchemeClass::Classical,
               .backend = "libsecp256k1"},
    SchemeSpec{.id = SCHEME_ML_DSA_44,
               .name = "ml-dsa-44",
               .public_key_bytes = 1'312,
               .signature_bytes = 2'420,
               .scheme_class = SchemeClass::PostQuantum,
               .backend = "openssl"},
    SchemeSpec{.id = SCHEME_SLH_DSA_SHA2_128S,
               .name = "slh-dsa-sha2-128s",
               .public_key_bytes = 32,
               .signature_bytes = 7'856,
               .scheme_class = SchemeClass::PostQuantum,
               .backend = "openssl"},
}};

// Ascending and gapless from 1, which is what lets `FindScheme` be a bounds check
// and an index rather than a search, and what keeps the wire values dense.
static_assert(SCHEMES[0].id == 1);
static_assert(SCHEMES[1].id == 2);
static_assert(SCHEMES[2].id == 3);
static_assert(SCHEME_RESERVED == 0, "the reserved id must sit below the table");

// --- OpenSSL handles ---------------------------------------------------------

struct PkeyDeleter {
    void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
};
struct PkeyCtxDeleter {
    void operator()(EVP_PKEY_CTX* ctx) const noexcept { EVP_PKEY_CTX_free(ctx); }
};
struct SignatureAlgDeleter {
    void operator()(EVP_SIGNATURE* alg) const noexcept { EVP_SIGNATURE_free(alg); }
};

using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, PkeyCtxDeleter>;
using SignatureAlgPtr = std::unique_ptr<EVP_SIGNATURE, SignatureAlgDeleter>;

/// OpenSSL's own name for a scheme, or nullptr for one it does not answer for.
///
/// These strings are OpenSSL's algorithm names, not Amarian's: `SchemeSpec::name` is
/// what Amarian prints, and the two are separate so that renaming a display string
/// cannot break a provider lookup.
[[nodiscard]] const char* OpenSslName(uint16_t scheme) noexcept {
    switch (scheme) {
        case SCHEME_ML_DSA_44:
            return "ML-DSA-44";
        case SCHEME_SLH_DSA_SHA2_128S:
            return "SLH-DSA-SHA2-128s";
        default:
            return nullptr;
    }
}

/// The fetched algorithm object for `scheme`, or nullptr.
///
/// Fetched once per process and then shared. A fetch is a provider lookup, and doing
/// one per input would put a hash-table walk inside block validation; OpenSSL's
/// fetched objects are reference-counted and usable from several threads at once,
/// which is exactly what this needs. Destroyed at exit before OpenSSL's own cleanup,
/// because it is constructed after it.
[[nodiscard]] EVP_SIGNATURE* FetchedAlgorithm(uint16_t scheme) noexcept {
    static const SignatureAlgPtr ml_dsa_44{EVP_SIGNATURE_fetch(nullptr, "ML-DSA-44", nullptr)};
    static const SignatureAlgPtr slh_dsa{
        EVP_SIGNATURE_fetch(nullptr, "SLH-DSA-SHA2-128s", nullptr)};
    switch (scheme) {
        case SCHEME_ML_DSA_44:
            return ml_dsa_44.get();
        case SCHEME_SLH_DSA_SHA2_128S:
            return slh_dsa.get();
        default:
            return nullptr;
    }
}

// --- The three verifications -------------------------------------------------

[[nodiscard]] VerifyResult
VerifySchnorr(ByteSpan public_key, ByteSpan signature, const Hash256& message) noexcept {
    // A 32-byte string is not automatically an x coordinate on the curve. Parsing
    // separates "unusable key" from "signature does not verify", which matters because
    // the first is a malformed transaction and the second is a forged one.
    secp256k1_xonly_pubkey parsed{};
    if (secp256k1_xonly_pubkey_parse(secp256k1_context_static, &parsed, public_key.data()) != 1) {
        return VerifyResult::Malformed;
    }
    // The static context is verification-only and needs no randomisation: there is no
    // secret here to protect against a side channel, since every input is public.
    const int verified = secp256k1_schnorrsig_verify(
        secp256k1_context_static, signature.data(), message.Data(), Hash256::SIZE, &parsed);
    return verified == 1 ? VerifyResult::Valid : VerifyResult::Invalid;
}

[[nodiscard]] VerifyResult VerifyWithOpenSsl(uint16_t scheme,
                                             ByteSpan public_key,
                                             ByteSpan signature,
                                             const Hash256& message) noexcept {
    const char* algorithm = OpenSslName(scheme);
    EVP_SIGNATURE* fetched = FetchedAlgorithm(scheme);
    if (algorithm == nullptr || fetched == nullptr) {
        // Only reachable if this build's OpenSSL cannot provide an algorithm the table
        // claims. `FirstUnavailableScheme` exists so a node discovers that at startup;
        // reaching it here means the check was skipped, and the safe direction is to
        // refuse rather than to accept a signature nothing checked.
        return VerifyResult::Malformed;
    }

    const PkeyPtr key{EVP_PKEY_new_raw_public_key_ex(
        nullptr, algorithm, nullptr, public_key.data(), public_key.size())};
    if (!key) {
        ERR_clear_error();
        return VerifyResult::Malformed;
    }
    const PkeyCtxPtr ctx{EVP_PKEY_CTX_new_from_pkey(nullptr, key.get(), nullptr)};
    if (!ctx || EVP_PKEY_verify_message_init(ctx.get(), fetched, nullptr) <= 0) {
        ERR_clear_error();
        return VerifyResult::Malformed;
    }

    const int verified = EVP_PKEY_verify(
        ctx.get(), signature.data(), signature.size(), message.Data(), Hash256::SIZE);
    // A rejected signature leaves entries on OpenSSL's error queue. Left there they
    // would surface as a spurious failure in whatever calls OpenSSL next, and an
    // invalid signature is an ordinary event on a public network.
    if (verified != 1) {
        ERR_clear_error();
        return VerifyResult::Invalid;
    }
    return VerifyResult::Valid;
}

}  // namespace

std::span<const SchemeSpec> KnownSchemes() noexcept {
    return std::span<const SchemeSpec>{SCHEMES};
}

const SchemeSpec* FindScheme(uint16_t id) noexcept {
    const auto found = std::ranges::find_if(
        SCHEMES, [id](const SchemeSpec& spec) noexcept { return spec.id == id; });
    return found == SCHEMES.end() ? nullptr : &*found;
}

bool HasWellFormedSizes(uint16_t scheme,
                        size_t public_key_bytes,
                        size_t signature_bytes) noexcept {
    const SchemeSpec* spec = FindScheme(scheme);
    return spec != nullptr && spec->public_key_bytes == public_key_bytes &&
           spec->signature_bytes == signature_bytes;
}

uint16_t FirstUnavailableScheme() noexcept {
    for (const SchemeSpec& spec : SCHEMES) {
        if (OpenSslName(spec.id) != nullptr && FetchedAlgorithm(spec.id) == nullptr) {
            ERR_clear_error();
            return spec.id;
        }
    }
    // libsecp256k1 is linked, not fetched: if it were absent this would not have built.
    return SCHEME_RESERVED;
}

VerifyResult
Verify(uint16_t scheme, ByteSpan public_key, ByteSpan signature, const Hash256& message) noexcept {
    if (scheme == SCHEME_RESERVED) {
        return VerifyResult::Reserved;
    }
    const SchemeSpec* spec = FindScheme(scheme);
    if (spec == nullptr) {
        return VerifyResult::UnknownScheme;
    }
    // Lengths before anything else, so no external implementation is ever handed a
    // buffer of a size it does not expect. This is the only place these two lengths are
    // enforced, which is why the table is the single description of them.
    if (public_key.size() != spec->public_key_bytes ||
        signature.size() != spec->signature_bytes) {
        return VerifyResult::Malformed;
    }

    switch (scheme) {
        case SCHEME_SCHNORR_SECP256K1:
            return VerifySchnorr(public_key, signature, message);
        case SCHEME_ML_DSA_44:
        case SCHEME_SLH_DSA_SHA2_128S:
            return VerifyWithOpenSsl(scheme, public_key, signature, message);
        default:
            // Unreachable: the table and this switch are the same set, and a scheme
            // added to one without the other is caught by the size check above only by
            // accident, so it is refused rather than accepted.
            return VerifyResult::Malformed;
    }
}

}  // namespace amarian::crypto
