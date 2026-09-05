#!/usr/bin/env bash
# Probes the two signature APIs Amarian intends to bind, before binding them.
#
# The point is to find out what OpenSSL 3.5's one-shot ML-DSA verification call
# actually looks like on this machine, and that libsecp256k1's static context is
# usable for BIP-340 verification, rather than writing the registry against a
# remembered signature and discovering the difference in a build error.
set -euo pipefail

WORK="${TMPDIR:-/tmp}/amarian_sig_probe"
mkdir -p "$WORK"
cat >"$WORK/probe.c" <<'EOF'
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <secp256k1_extrakeys.h>
#include <stdio.h>
#include <string.h>

/* BIP-340 test vector 0: a 32-byte x-only key, a 32-byte message, a 64-byte sig. */
static const unsigned char PK[32] = {
    0xF9,0x30,0x8A,0x01,0x92,0x58,0xC3,0x10,0x49,0x34,0x4F,0x85,0xF8,0x9D,0x52,0x29,
    0xB5,0x31,0xC8,0x45,0x83,0x6F,0x99,0xB0,0x86,0x01,0xF1,0x13,0xBC,0xE0,0x36,0xF9};
static const unsigned char MSG[32] = {0};
static const unsigned char SIG[64] = {
    0xE9,0x07,0x83,0x1F,0x80,0x84,0x8D,0x10,0x69,0xA5,0x37,0x1B,0x40,0x24,0x10,0x36,
    0x4B,0xDF,0x1C,0x5F,0x83,0x07,0xB0,0x08,0x4C,0x55,0xF1,0xCE,0x2D,0xCA,0x82,0x15,
    0x25,0xF6,0x6A,0x4A,0x85,0xEA,0x8B,0x71,0xE4,0x82,0xA7,0x4F,0x38,0x2D,0x2C,0xE5,
    0xEB,0xEE,0xE8,0xFD,0xB2,0x17,0x2F,0x47,0x7D,0xF4,0x90,0x0D,0x31,0x05,0x36,0xC0};

static int probe_schnorr(void) {
    secp256k1_xonly_pubkey pk;
    if (!secp256k1_xonly_pubkey_parse(secp256k1_context_static, &pk, PK)) {
        printf("schnorr: xonly_pubkey_parse FAILED\n");
        return 1;
    }
    int ok = secp256k1_schnorrsig_verify(secp256k1_context_static, SIG, MSG, sizeof(MSG), &pk);
    printf("schnorr: static-context verify of BIP-340 vector 0 = %d (expect 1)\n", ok);

    unsigned char bad[64];
    memcpy(bad, SIG, sizeof(bad));
    bad[0] ^= 1;
    ok = secp256k1_schnorrsig_verify(secp256k1_context_static, bad, MSG, sizeof(MSG), &pk);
    printf("schnorr: mutated signature = %d (expect 0)\n", ok);
    return 0;
}

static int probe_mldsa(const char *alg) {
    /* Generate, sign, verify. Amarian only needs verify, but a self-produced
       signature is the only way to check the verify path without a vector file. */
    EVP_PKEY *key = NULL;
    EVP_PKEY_CTX *gen = EVP_PKEY_CTX_new_from_name(NULL, alg, NULL);
    if (gen == NULL || EVP_PKEY_keygen_init(gen) <= 0 || EVP_PKEY_generate(gen, &key) <= 0) {
        printf("%s: keygen FAILED\n", alg);
        return 1;
    }
    EVP_PKEY_CTX_free(gen);

    size_t publen = 0;
    EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, NULL, 0, &publen);
    unsigned char pub[8192];
    if (publen > sizeof(pub) ||
        !EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, pub, sizeof(pub), &publen)) {
        printf("%s: raw public key export FAILED\n", alg);
        return 1;
    }
    printf("%s: public key = %zu bytes\n", alg, publen);

    EVP_SIGNATURE *sigalg = EVP_SIGNATURE_fetch(NULL, alg, NULL);
    if (sigalg == NULL) { printf("%s: EVP_SIGNATURE_fetch FAILED\n", alg); return 1; }

    EVP_PKEY_CTX *sctx = EVP_PKEY_CTX_new_from_pkey(NULL, key, NULL);
    size_t siglen = 0;
    if (EVP_PKEY_sign_message_init(sctx, sigalg, NULL) <= 0 ||
        EVP_PKEY_sign(sctx, NULL, &siglen, MSG, sizeof(MSG)) <= 0) {
        printf("%s: sign_message_init/size FAILED\n", alg);
        return 1;
    }
    static unsigned char sig[16384];
    if (siglen > sizeof(sig) || EVP_PKEY_sign(sctx, sig, &siglen, MSG, sizeof(MSG)) <= 0) {
        printf("%s: sign FAILED\n", alg);
        return 1;
    }
    printf("%s: signature = %zu bytes\n", alg, siglen);
    EVP_PKEY_CTX_free(sctx);

    /* Now the path Amarian actually needs: raw public key bytes in, verdict out. */
    EVP_PKEY *imported =
        EVP_PKEY_new_raw_public_key_ex(NULL, alg, NULL, pub, publen);
    if (imported == NULL) {
        printf("%s: EVP_PKEY_new_raw_public_key_ex FAILED\n", alg);
        return 1;
    }
    EVP_PKEY_CTX *vctx = EVP_PKEY_CTX_new_from_pkey(NULL, imported, NULL);
    int ok = 0;
    if (EVP_PKEY_verify_message_init(vctx, sigalg, NULL) > 0) {
        ok = EVP_PKEY_verify(vctx, sig, siglen, MSG, sizeof(MSG));
    } else {
        printf("%s: verify_message_init FAILED\n", alg);
    }
    printf("%s: verify of own signature = %d (expect 1)\n", alg, ok);

    sig[0] ^= 1;
    EVP_PKEY_CTX_free(vctx);
    vctx = EVP_PKEY_CTX_new_from_pkey(NULL, imported, NULL);
    EVP_PKEY_verify_message_init(vctx, sigalg, NULL);
    ok = EVP_PKEY_verify(vctx, sig, siglen, MSG, sizeof(MSG));
    printf("%s: mutated signature = %d (expect 0)\n", alg, ok);

    EVP_PKEY_CTX_free(vctx);
    EVP_PKEY_free(imported);
    EVP_PKEY_free(key);
    EVP_SIGNATURE_free(sigalg);
    return 0;
}

int main(void) {
    printf("OpenSSL: %s\n", OpenSSL_version(OPENSSL_VERSION_STRING));
    if (probe_schnorr() != 0) { return 1; }
    if (probe_mldsa("ML-DSA-44") != 0) { return 1; }
    if (probe_mldsa("SLH-DSA-SHA2-128s") != 0) { return 1; }
    return 0;
}
EOF

cc -O1 -o "$WORK/probe" "$WORK/probe.c" \
    $(pkg-config --cflags --libs libsecp256k1) -lcrypto
"$WORK/probe"
