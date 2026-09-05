#pragma once

/// \file
/// The signature scheme registry: the single place where a scheme identifier
/// becomes an actual verification.
///
/// Amarian implements no signature algorithm. This layer is a table and a switch
/// over two external implementations — libsecp256k1 for BIP-340 Schnorr, OpenSSL
/// for FIPS 204 ML-DSA and FIPS 205 SLH-DSA — and its whole job is to make the
/// choice between them a *value on the wire* rather than a compile-time decision.
/// That is what cryptographic agility means in practice: adding a scheme is adding
/// a row here and a case in `Verify`, and no other file in the project changes.
///
/// ## Why a tri-state answer and not a bool
///
/// A verifier that returns `false` for both "this signature is forged" and "I have
/// never heard of this scheme" cannot be used to build a chain that upgrades. An
/// unknown scheme has to stay *valid-and-spendable* so a new algorithm can be
/// deployed by soft fork without splitting old nodes off the chain, while a forged
/// signature must be rejected — opposite verdicts from the same bool. `VerifyResult`
/// keeps them apart and forces the caller to write down which it means.
///
/// Scheme `0` is reserved and is never verifiable, which is a different thing again
/// from unknown: it exists so that a zero-filled or truncated-then-padded scheme
/// field does not name a real algorithm.
///
/// ## Hybrid authorisation needs nothing from this file
///
/// A "hybrid" spend — one classical signature *and* one post-quantum signature both
/// required — is not a scheme. It is a 2-of-2 `SpendCondition` holding one key of
/// each class, which the Phase 1 threshold form already expresses. There is
/// therefore no `SCHEME_HYBRID`, and adding one would be the mistake: it would fix
/// at the algorithm layer a policy that belongs to whoever creates the output.
///
/// ## Sizes are exact, and checked here
///
/// Every scheme in the table has one public key length and one signature length,
/// and a value of the wrong length is rejected by this layer before any external
/// library sees it. Consensus then does not depend on how a third party treats a
/// short buffer, which is the kind of dependency that turns a library upgrade into
/// a chain split.

#include <amarian/util/types.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace amarian::crypto {

// --- Scheme identifiers ------------------------------------------------------
//
// These are wire values: they appear in every `PublicKey` and `Signature`, are
// committed to by the signature hash, and can never be renumbered. They are
// deliberately small and dense so the registry can stay a flat table.
//
// The assignment is **not final until Phase 1 closes**: it is recorded in
// AMARIAN_PROTOCOL.md as provisional, and while no block exists there is no cost to
// changing it. Once a chain has one output locked to a scheme, the number is
// permanent.

/// Never valid. Reserved so that an all-zero scheme field names no algorithm.
///
/// The same protocol constant as `amarian::SCHEME_RESERVED` in the primitives layer,
/// which cannot be included from here — crypto sits below primitives. The consensus
/// layer sees both and `static_assert`s that they agree, so a divergence is a build
/// failure rather than a rule that disagrees with the codec.
inline constexpr uint16_t SCHEME_RESERVED = 0;

/// BIP-340 Schnorr over secp256k1: 32-byte x-only public key, 64-byte signature.
///
/// The classical scheme, and the reason it is Schnorr rather than ECDSA is
/// malleability: a BIP-340 signature is a fixed 64 bytes with no encoding freedom,
/// so there is no second valid encoding of one and therefore no second wtxid for a
/// transaction carrying it.
inline constexpr uint16_t SCHEME_SCHNORR_SECP256K1 = 1;

/// ML-DSA-44 (FIPS 204, lattice): 1 312-byte public key, 2 420-byte signature.
inline constexpr uint16_t SCHEME_ML_DSA_44 = 2;

/// SLH-DSA-SHA2-128s (FIPS 205, hash-based): 32-byte public key, 7 856-byte
/// signature.
///
/// Present alongside ML-DSA on purpose, and it is the reason the registry is worth
/// having at all: the two rest on unrelated hardness assumptions, so a break in
/// lattice cryptography does not take the chain's post-quantum option with it. Its
/// signature is three times ML-DSA's, which is the price of that independence.
inline constexpr uint16_t SCHEME_SLH_DSA_SHA2_128S = 3;

// --- The table ---------------------------------------------------------------

/// What a scheme's security rests on. Not a strength ordering: it is the question
/// "does a quantum adversary break this", which is the only distinction consensus
/// and a wallet actually need to reason about.
enum class SchemeClass : uint8_t {
    /// Broken by Shor's algorithm given a published public key.
    Classical,
    /// Believed to survive it, under the assumptions in PQ_CRYPTO.md.
    PostQuantum,
};

/// One row of the registry.
struct SchemeSpec {
    uint16_t id;

    /// Stable lowercase name for logs, RPC and wallet display. Never parsed by
    /// consensus, and never a substitute for `id`.
    std::string_view name;

    /// Exact lengths. A key or signature of any other length is malformed, not
    /// merely invalid, and is rejected without calling the implementation.
    size_t public_key_bytes;
    size_t signature_bytes;

    SchemeClass scheme_class;

    /// Which implementation answers for it, for the startup banner and for a bug
    /// report that needs to name the code that produced a verdict.
    std::string_view backend;
};

/// Every scheme this build can verify, ascending by `id`.
[[nodiscard]] std::span<const SchemeSpec> KnownSchemes() noexcept;

/// The row for `id`, or `nullptr` if this build does not know the scheme.
///
/// A null answer is not an error. It is the soft-fork case, and the caller has to
/// decide what it means — see `VerifyResult::UnknownScheme`.
[[nodiscard]] const SchemeSpec* FindScheme(uint16_t id) noexcept;

// --- Verification ------------------------------------------------------------

/// The outcome of one verification.
///
/// Five values rather than a bool, because consensus treats three of them
/// differently and a caller that cannot tell them apart cannot be correct.
enum class VerifyResult : uint8_t {
    /// The signature verifies under the key over the message.
    Valid,

    /// The implementation was called and said no. This is a forgery, a signature
    /// over a different message, or a signature under a different key.
    Invalid,

    /// Scheme 0. Never verifiable, under any condition version, forever.
    Reserved,

    /// This build has no implementation for the scheme. Consensus accepts such a
    /// witness — that is the soft-fork upgrade path — and a wallet must refuse to
    /// *create* one, because it cannot check what it is spending to.
    UnknownScheme,

    /// A known scheme, but the key or signature is not the length that scheme uses,
    /// or the key is not a valid encoding of a point or of an algorithm key. Not
    /// `Invalid`: nothing was verified, so a caller distinguishing "wrong" from
    /// "unusable" in a log or a fuzz oracle can still do so.
    Malformed,
};

/// Verifies `signature` over the 32-byte `message` under `public_key`.
///
/// The message is a `Hash256` and not a span because every signature in Amarian is
/// over a signature hash: there is no interface here through which a caller could
/// sign an arbitrary string, and so no way to be talked into signing one.
///
/// Deterministic and free of side effects. It reads no clock, touches no global
/// state a caller can set, and never allocates on behalf of the input's length —
/// lengths are checked against the table first. Thread-safe: libsecp256k1's static
/// verification context and OpenSSL's fetched algorithm objects are both shareable,
/// and everything per-call is stack- or arena-local.
[[nodiscard]] VerifyResult
Verify(uint16_t scheme, ByteSpan public_key, ByteSpan signature, const Hash256& message) noexcept;

/// Whether a scheme is known *and* its key and signature lengths are the ones it
/// uses. For a wallet or a mempool policy check that wants the structural answer
/// without paying for the verification.
[[nodiscard]] bool
HasWellFormedSizes(uint16_t scheme, size_t public_key_bytes, size_t signature_bytes) noexcept;

/// The first scheme in the table that this build cannot actually verify, or
/// `SCHEME_RESERVED` if every one of them is available.
///
/// A node calls this at startup and refuses to run if it is not zero. The reason it
/// is a startup check rather than a runtime fallback: a build whose OpenSSL lacks an
/// algorithm the table claims has two bad options at verification time — accept a
/// signature it cannot check, or reject a transaction the rest of the network
/// accepts — and one of those loses money while the other splits the chain. Finding
/// out before the first block is the only good option, and it costs one call.
[[nodiscard]] uint16_t FirstUnavailableScheme() noexcept;

}  // namespace amarian::crypto
