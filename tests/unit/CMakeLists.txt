# The `consensus` label is carried by the three layers where a bug is a chain split:
# consensus, utxo and chain. Not primitives, util or crypto — a canonical-encoding bug is
# just as fatal, but a gate that covers every layer is a gate that says nothing about which
# question it answered, and those layers are what `ctest -L unit` is for.
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

amarian_add_test(test_consensus "unit;consensus"
    consensus_asert_test.cpp
    consensus_genesis_test.cpp
    consensus_issuance_test.cpp
    consensus_params_test.cpp
    consensus_target_test.cpp
    consensus_validation_test.cpp
    consensus_work_test.cpp
)
# OpenSSL for the same reason test_crypto links it, and only for that reason: the spend
# authorisation rules cannot be tested against anything but real signatures, and the node
# deliberately cannot produce one. ML-DSA-44 is used rather than Schnorr so that this stays
# the single already-documented exception instead of also exposing libsecp256k1's signing
# API to a test.
target_link_libraries(test_consensus PRIVATE amarian::consensus amarian::openssl)

amarian_add_test(test_utxo "unit;consensus"
    utxo_coins_test.cpp
    utxo_connect_test.cpp
)
target_link_libraries(test_utxo PRIVATE amarian::utxo)

amarian_add_test(test_chain "unit;consensus"
    chain_block_index_test.cpp
)
target_link_libraries(test_chain PRIVATE amarian::chain)

# `unit` and not `unit;consensus`, by the criterion the label was given in DECISIONS.md: the
# consensus gate covers the layers where a bug is a chain split. Neither of these is one. A
# mempool that admits an illegal transaction, or an assembler that packs one, makes *this*
# node build a block every other node refuses — an orphan it paid for, which is expensive and
# entirely its own problem. No other node's view of validity moves.
amarian_add_test(test_mempool unit
    mempool_test.cpp
)
target_link_libraries(test_mempool PRIVATE amarian::mempool)

amarian_add_test(test_mining unit
    mining_test.cpp
)
target_link_libraries(test_mining PRIVATE amarian::mining)

# `unit` for the same reason, and with one more: the RPC surface is what an operator asks
# this node to do, and a node that mishandles a request has mishandled its own operator. It
# links only amarian::rpc — the chain, mempool and mining it needs arrive through that
# target's PUBLIC edges, which is the layering claim being exercised as much as the methods.
amarian_add_test(test_rpc unit
    rpc_test.cpp
)
target_link_libraries(test_rpc PRIVATE amarian::rpc)

amarian_add_test(test_version unit
    version_test.cpp
)
target_link_libraries(test_version PRIVATE amarian::version)
