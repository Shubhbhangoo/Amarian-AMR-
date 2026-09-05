# Amarian library targets.
#
# The dependency order below is the architectural contract:
#
#   util  <-  crypto  <-  primitives  <-  consensus  <-  utxo  <-  chain  <-  storage
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
# Also the sole libsecp256k1 boundary, and the only layer permitted to call either.
# Both are PRIVATE so that a target linking amarian::crypto gets Amarian's interface
# and not OpenSSL's headers: a caller that could reach EVP directly would be a second
# place cryptography happens.
add_library(amarian_crypto STATIC
    crypto/hash.cpp
    crypto/signature.cpp
)
target_link_libraries(amarian_crypto
    PUBLIC amarian::util
    PRIVATE amarian::openssl amarian::secp256k1
)
add_library(amarian::crypto ALIAS amarian_crypto)

# --- primitives: consensus data and canonical encodings ---------------------
add_library(amarian_primitives STATIC
    primitives/block.cpp
    primitives/coin.cpp
    primitives/lock.cpp
    primitives/merkle.cpp
    primitives/outpoint.cpp
    primitives/sighash.cpp
    primitives/spend_condition.cpp
    primitives/transaction.cpp
    primitives/witness.cpp
)
target_link_libraries(amarian_primitives PUBLIC amarian::crypto)
add_library(amarian::primitives ALIAS amarian_primitives)

# --- consensus: the rules that decide validity ------------------------------
# Deliberately links nothing but primitives. No storage, no networking, no
# wallet, no RPC: the absence of those edges is what keeps the surface that
# decides which chain is real small enough to audit, and CMake is where that
# constraint is actually enforced rather than merely described.
add_library(amarian_consensus STATIC
    consensus/genesis.cpp
    consensus/target.cpp
    consensus/validation.cpp
    consensus/work.cpp
)
target_link_libraries(amarian_consensus PUBLIC amarian::primitives)
add_library(amarian::consensus ALIAS amarian_consensus)

# --- utxo: the set of unspent coins, and applying a block to it -------------
# Sits above consensus and below chain. It owns *which* coins exist and how a block
# moves the set forward or back; it decides no rules of its own beyond the ones a
# lookup can answer, and reports every rejection with consensus's vocabulary.
#
# Links only consensus, and so transitively only primitives: no storage engine, no
# networking. A persistent backend implements the `CoinsView` interface from the
# storage layer above rather than being reached down to from here, which is what keeps
# a database out of the code path that decides validity.
add_library(amarian_utxo STATIC
    utxo/coins.cpp
    utxo/connect.cpp
)
target_link_libraries(amarian_utxo PUBLIC amarian::consensus)
add_library(amarian::utxo ALIAS amarian_utxo)

# --- chain: which of the known blocks the node actually follows --------------
# The tree of known headers, the accumulated work on each branch, and the walk from one
# tip to another. Above utxo because activating a chain means applying blocks to the
# unspent output set; still below storage and networking, so the code that decides which
# chain is real can be tested without either.
add_library(amarian_chain STATIC
    chain/block_index.cpp
    chain/block_store.cpp
    chain/chain_state.cpp
)
target_link_libraries(amarian_chain PUBLIC amarian::utxo)
add_library(amarian::chain ALIAS amarian_chain)

# --- mining: deciding what the next block should be --------------------------
# The only layer that answers *what block should exist next*; every other layer answers
# *is this block allowed*. Deliberately a separate target from consensus rather than a
# corner of it: an assembler sharing a translation unit with the validator could produce a
# block that passes because both agree on a mistake, and keeping them apart is what makes
# the assembler's output something the rules judge rather than something they assume.
#
# Links chain, because the height, the previous hash and the target all come from the tip.
# Nothing links mining in return — a node builds blocks, and no rule consults a builder.
add_library(amarian_mining STATIC
    mining/block_assembler.cpp
)
target_link_libraries(amarian_mining PUBLIC amarian::chain)
add_library(amarian::mining ALIAS amarian_mining)

# --- storage: the state that survives a restart ------------------------------
# The first layer permitted to link a database, and the only one. Everything below it is
# written against the interfaces it implements — utxo::CoinsView, chain::BlockStore,
# chain::ChainSink — so a bug in RocksDB's option handling cannot reach the code that
# decides whether a block is valid, because that code cannot see RocksDB at all.
#
# amarian::rocksdb is PRIVATE, following the crypto precedent: a target linking
# amarian::storage gets Amarian's interface and not RocksDB's headers. The pimpl in
# chain_db.hpp is the other half of that boundary — one is the compile-time promise, the
# other is what makes it true of the header as well.
add_library(amarian_storage STATIC
    storage/chain_db.cpp
)
target_link_libraries(amarian_storage
    PUBLIC amarian::chain
    PRIVATE amarian::rocksdb
)
add_library(amarian::storage ALIAS amarian_storage)

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
target_link_libraries(amariand PRIVATE amarian::consensus amarian::util amarian::version)

# --- amarian-genesis: prints, checks and regenerates genesis parameters -----
# A tool rather than a test: it is how the recorded nonces were produced, and
# keeping it means a reviewer can reproduce a consensus constant instead of
# trusting it.
find_package(Threads REQUIRED)
add_executable(amarian-genesis
    tools/genesis_tool.cpp
)
target_link_libraries(amarian-genesis PRIVATE amarian::consensus Threads::Threads)
