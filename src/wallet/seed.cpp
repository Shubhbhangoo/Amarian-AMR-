/// \file
/// Key derivation implementation: HKDF-SHA256 from the master seed.

#include <amarian/wallet/seed.hpp>
#include <amarian/util/serialize.hpp>

#include <amarian/crypto/hash.hpp>
#include <amarian/crypto/random.hpp>
#include <amarian/util/types.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace amarian::wallet {
namespace {

// --- HKDF-Expand (RFC 5869 §2.3) -------------------------------------------
//
// HKDF-Extract is always HMAC-SHA256(salt=domain_string, ikm=master_seed).
// The result is a 32-byte pseudorandom key (PRK). Then HKDF-Expand produces
// the desired number of output bytes.

/// HMAC-SHA256(key, data) -> 32 bytes.
[[nodiscard]] Hash256 HmacSha256(ByteSpan key, ByteSpan data) {
    // We use TaggedHash with a different construction: HMAC isn't directly
    // available from the hash layer. DoubleSha256( key XOR ipad || data )
    // and DoubleSha256( key XOR opad || hash ) is the HMAC construction.
    // For simplicity, use Sha256 inside a custom HMAC that doesn't require
    // OpenSSL's EVP_MAC.
    //
    // HMAC-SHA256(key, data) = SHA256( (k0 XOR opad) || SHA256( (k0 XOR ipad) || data ) )
    // where k0 = key padded to 64 bytes.

    std::array<uint8_t, 64> k0{};
    if (key.size() <= 64) {
        std::memcpy(k0.data(), key.data(), key.size());
    }
    // If key > 64 bytes we should hash it first, but our key is always <= 32 bytes.

    std::array<uint8_t, 64> ipad{};
    std::array<uint8_t, 64> opad{};
    for (size_t i = 0; i < 64; ++i) {
        ipad[i] = k0[i] ^ 0x36;
        opad[i] = k0[i] ^ 0x5C;
    }

    // Inner: SHA256(ipad || data)
    Writer inner_writer(64 + data.size());
    inner_writer.WriteBytes(ipad);
    inner_writer.WriteBytes(data);
    const Hash256 inner = Sha256(inner_writer.Bytes());

    // Outer: SHA256(opad || inner)
    Writer outer_writer(64 + Hash256::SIZE);
    outer_writer.WriteBytes(opad);
    outer_writer.WriteHash256(inner);
    return Sha256(outer_writer.Bytes());
}

/// HKDF-Extract: PRK = HMAC-SHA256(salt, IKM)
[[nodiscard]] Hash256 HkdfExtract(ByteSpan salt, ByteSpan ikm) {
    return HmacSha256(salt, ikm);
}

/// HKDF-Expand: output of length `L` from PRK and info.
void HkdfExpand(const Hash256& prk, ByteSpan info, MutableByteSpan output) {
    // T(1) || T(2) || ... where T(i) = HMAC(PRK, T(i-1) || info || i)
    constexpr size_t HASH_LEN = Hash256::SIZE;
    std::array<uint8_t, HASH_LEN> prev{};
    size_t produced = 0;
    uint8_t counter = 1;

    while (produced < output.size()) {
        Writer writer(prev.size() + info.size() + 1);
        if (produced > 0) {
            writer.WriteBytes(ByteSpan(prev.data(), prev.size()));
        }
        writer.WriteBytes(info);
        writer.WriteU8(counter++);

        const Hash256 block = HmacSha256(prk.Array(), writer.Bytes());
        std::memcpy(prev.data(), block.Data(), HASH_LEN);

        const size_t to_copy = std::min(HASH_LEN, output.size() - produced);
        std::memcpy(output.data() + produced, prev.data(), to_copy);
        produced += to_copy;
    }
}

constexpr std::string_view DERIVATION_DOMAIN = "Amarian/KeyDeriv/v1";

/// Builds the HKDF salt for a given scheme name.
std::array<uint8_t, 64> BuildSalt(std::string_view scheme_name) {
    const size_t total = DERIVATION_DOMAIN.size() + 1 + scheme_name.size();
    Writer writer(total);
    writer.WriteBytes(ByteSpan(reinterpret_cast<const uint8_t*>(DERIVATION_DOMAIN.data()),
                               DERIVATION_DOMAIN.size()));
    writer.WriteU8(static_cast<uint8_t>(0));  // separator
    writer.WriteBytes(ByteSpan(reinterpret_cast<const uint8_t*>(scheme_name.data()),
                               scheme_name.size()));
    std::array<uint8_t, 64> salt{};
    const ByteVec& bytes = writer.Bytes();
    std::memcpy(salt.data(), bytes.data(), std::min(bytes.size(), salt.size()));
    return salt;
}

}  // namespace

void DeriveKeyBytes(const MasterSeed& seed, std::string_view scheme_name,
                    uint32_t account, uint32_t index, MutableByteSpan out_bytes) {
    const std::array<uint8_t, 64> salt = BuildSalt(scheme_name);
    const Hash256 prk = HkdfExtract(salt, ByteSpan(seed.data(), seed.size()));

    Writer info_writer;
    info_writer.WriteU32(account);
    info_writer.WriteU32(index);

    HkdfExpand(prk, info_writer.Bytes(), out_bytes);
}

void DeriveSecp256k1Key(const MasterSeed& seed, uint32_t account, uint32_t index,
                        MutableByteSpan out_key) {
    // Derive 32 bytes and reduce into secp256k1 scalar range.
    // The secp256k1 order is 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141.
    // We derive and then reduce mod (order) by trying until a valid key is found.
    // A simpler approach: derive 32 bytes, that's what libsecp256k1's
    // `secp256k1_keypair_create` expects — it does its own range check internally.
    DeriveKeyBytes(seed, "schnorr-secp256k1", account, index, out_key);
}

void DeriveMldsa44Seed(const MasterSeed& seed, uint32_t account, uint32_t index,
                       MutableByteSpan out_seed) {
    DeriveKeyBytes(seed, "ml-dsa-44", account, index, out_seed);
}

void DeriveSlhDsaSeeds(const MasterSeed& seed, uint32_t account, uint32_t index,
                       MutableByteSpan sk_seed, MutableByteSpan sk_prf,
                       MutableByteSpan pk_seed) {
    // Derive 48 bytes total: 16 + 16 + 16.
    std::array<uint8_t, 48> all{};
    MutableByteSpan all_span(all);
    DeriveKeyBytes(seed, "slh-dsa-sha2-128s", account, index, all_span);
    std::memcpy(sk_seed.data(), all.data(), 16);
    std::memcpy(sk_prf.data(), all.data() + 16, 16);
    std::memcpy(pk_seed.data(), all.data() + 32, 16);
}

MasterSeed GenerateSeed() {
    MasterSeed seed{};
    static_cast<void>(crypto::RandomBytes(MutableByteSpan(seed.data(), seed.size())));
    return seed;
}

}  // namespace amarian::wallet