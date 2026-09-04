# Amarian library targets.
#
# The dependency order below is the architectural contract:
#
#   util  <-  crypto  <-  primitives  <-  consensus  <-  utxo  <-  chain
#                                            ^                       ^
#                                            |                       |
#                                    mempool / mining  ---------------+
#                                            ^
#                              net / wallet / rpc / node / cli
#
# consensus must never link storage, networking, wallet or RPC. Keeping that edge
# absent is what makes the consensus surface small enough to audit.

# --- util: bytes, hex, checked arithmetic, logging, CLI options -------------
add_library(amarian_util STATIC
    util/args.cpp
    util/hex.cpp
    util/serialize.cpp
    util/types.cpp
    util/logging.cpp
)
target_link_libraries(amarian_util PUBLIC amarian_settings)
add_library(amarian::util ALIAS amarian_util)

# --- crypto: the sole OpenSSL boundary for consensus hashing ----------------
add_library(amarian_crypto STATIC
    crypto/hash.cpp
)
target_link_libraries(amarian_crypto PUBLIC amarian::util PRIVATE amarian::openssl)
add_library(amarian::crypto ALIAS amarian_crypto)

# --- version: build identity ------------------------------------------------
# Queries the OpenSSL actually loaded at runtime, so it links OpenSSL. Kept in
# its own target so that nothing else acquires a dependency on OpenSSL just to
# print a version banner.
add_library(amarian_version STATIC
    version.cpp
)
target_link_libraries(amarian_version PUBLIC amarian_settings PRIVATE amarian::openssl)
add_library(amarian::version ALIAS amarian_version)

# --- amariand: the node daemon ---------------------------------------------
add_executable(amariand
    node/amariand.cpp
)
target_link_libraries(amariand PRIVATE amarian::util amarian::version)
