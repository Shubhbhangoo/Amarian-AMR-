amarian_add_test(test_util unit
    util_args_test.cpp
    util_hex_test.cpp
    util_logging_test.cpp
    util_overflow_test.cpp
    util_serialize_test.cpp
)
target_link_libraries(test_util PRIVATE amarian::util)

amarian_add_test(test_crypto unit
    crypto_hash_test.cpp
    crypto_signature_test.cpp
)
# The only target outside src/crypto permitted to reach OpenSSL, and only because the
# post-quantum verification path cannot be tested without producing a signature: the
# node never signs, so amarian::crypto deliberately exposes no signing interface. The
# alternative is leaving ML-DSA and SLH-DSA verification with no test at all.
target_link_libraries(test_crypto PRIVATE amarian::crypto amarian::openssl)

amarian_add_test(test_primitives unit
    primitives_block_test.cpp
    primitives_lock_test.cpp
    primitives_merkle_test.cpp
    primitives_outpoint_test.cpp
    primitives_sighash_test.cpp
    primitives_spend_condition_test.cpp
    primitives_transaction_test.cpp
)
target_link_libraries(test_primitives PRIVATE amarian::primitives)

amarian_add_test(test_consensus unit
    consensus_genesis_test.cpp
    consensus_issuance_test.cpp
    consensus_params_test.cpp
    consensus_target_test.cpp
)
target_link_libraries(test_consensus PRIVATE amarian::consensus)

amarian_add_test(test_version unit
    version_test.cpp
)
target_link_libraries(test_version PRIVATE amarian::version)
