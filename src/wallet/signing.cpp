/// \file
/// Signing bridge: produces real ML-DSA-44 and Schnorr signatures.

#include <amarian/wallet/signing.hpp>

#include <amarian/crypto/random.hpp>
#include <amarian/crypto/signature.hpp>

#include <openssl/err.h>
#include <openssl/evp.h>

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

namespace amarian::wallet {

namespace {

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

EVP_SIGNATURE* Mldsa44Algorithm() noexcept {
    static const SignatureAlgPtr alg{EVP_SIGNATURE_fetch(NULL, "ML-DSA-44", NULL)};
    return alg.get();
}

secp256k1_context* SchnorrSigningContext() noexcept {
    static secp256k1_context* ctx = []() {
        return secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    }();
    return ctx;
}

}  // namespace

std::optional<ByteVec>
SignMldsa44(ByteSpan private_key, const Hash256& message) {
    EVP_SIGNATURE* alg = Mldsa44Algorithm();
    if (alg == nullptr) return std::nullopt;
    if (private_key.size() < 2550 || private_key.size() > 2600) return std::nullopt;

    const PkeyPtr key{EVP_PKEY_new_raw_private_key_ex(
        NULL, "ML-DSA-44", NULL, private_key.data(), private_key.size())};
    if (!key) { ERR_clear_error(); return std::nullopt; }

    const PkeyCtxPtr ctx{EVP_PKEY_CTX_new_from_pkey(NULL, key.get(), NULL)};
    if (!ctx) { ERR_clear_error(); return std::nullopt; }

    if (EVP_PKEY_sign_message_init(ctx.get(), alg, NULL) <= 0) {
        ERR_clear_error(); return std::nullopt;
    }

    size_t sig_len = 0;
    if (EVP_PKEY_sign(ctx.get(), NULL, &sig_len, message.Data(), Hash256::SIZE) <= 0) {
        ERR_clear_error(); return std::nullopt;
    }

    ByteVec sig(sig_len);
    if (EVP_PKEY_sign(ctx.get(), sig.data(), &sig_len,
                       message.Data(), Hash256::SIZE) <= 0) {
        ERR_clear_error(); return std::nullopt;
    }
    sig.resize(sig_len);
    return sig;
}

std::optional<ByteVec>
SignSchnorr(ByteSpan private_key, const Hash256& message) {
    if (private_key.size() != 32) return std::nullopt;

    secp256k1_context* ctx = SchnorrSigningContext();
    if (ctx == nullptr) return std::nullopt;

    secp256k1_keypair keypair;
    if (secp256k1_keypair_create(ctx, &keypair, private_key.data()) != 1) {
        return std::nullopt;
    }

    ByteVec sig(64, 0);
    if (secp256k1_schnorrsig_sign32(ctx, sig.data(), message.Data(),
                                     &keypair, NULL) != 1) {
        std::memset(&keypair, 0, sizeof(keypair));
        return std::nullopt;
    }

    std::memset(&keypair, 0, sizeof(keypair));
    return sig;
}

std::optional<KeyPair> GenerateMldsa44Key() {
    EVP_PKEY_CTX* genctx = EVP_PKEY_CTX_new_from_name(NULL, "ML-DSA-44", NULL);
    if (genctx == nullptr) return std::nullopt;
    PkeyCtxPtr genctx_ptr(genctx);

    if (EVP_PKEY_keygen_init(genctx_ptr.get()) <= 0) {
        ERR_clear_error(); return std::nullopt;
    }

    EVP_PKEY* pkey = nullptr;
    if (EVP_PKEY_keygen(genctx_ptr.get(), &pkey) <= 0) {
        ERR_clear_error(); return std::nullopt;
    }
    PkeyPtr key_ptr(pkey);

    KeyPair result;
    size_t priv_len = 0;
    EVP_PKEY_get_raw_private_key(key_ptr.get(), NULL, &priv_len);
    result.private_key.resize(priv_len);
    if (EVP_PKEY_get_raw_private_key(key_ptr.get(), result.private_key.data(), &priv_len) <= 0) {
        ERR_clear_error(); return std::nullopt;
    }
    result.private_key.resize(priv_len);

    size_t pub_len = 0;
    EVP_PKEY_get_raw_public_key(key_ptr.get(), NULL, &pub_len);
    result.public_key.resize(pub_len);
    if (EVP_PKEY_get_raw_public_key(key_ptr.get(), result.public_key.data(), &pub_len) <= 0) {
        ERR_clear_error(); return std::nullopt;
    }
    result.public_key.resize(pub_len);

    return result;
}

std::optional<KeyPair> GenerateSchnorrKey(ByteSpan seed) {
    if (seed.size() != 32) return std::nullopt;

    secp256k1_context* ctx = SchnorrSigningContext();
    if (ctx == nullptr) return std::nullopt;

    secp256k1_keypair keypair;
    if (secp256k1_keypair_create(ctx, &keypair, seed.data()) != 1) {
        return std::nullopt;
    }

    KeyPair result;
    result.private_key.assign(seed.begin(), seed.end());

    secp256k1_xonly_pubkey xonly;
    if (secp256k1_keypair_xonly_pub(ctx, &xonly, NULL, &keypair) != 1) {
        std::memset(&keypair, 0, sizeof(keypair));
        return std::nullopt;
    }

    result.public_key.resize(32);
    if (secp256k1_xonly_pubkey_serialize(ctx, result.public_key.data(), &xonly) != 1) {
        std::memset(&keypair, 0, sizeof(keypair));
        return std::nullopt;
    }

    std::memset(&keypair, 0, sizeof(keypair));
    return result;
}

}  // namespace amarian::wallet