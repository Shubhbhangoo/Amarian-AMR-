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
    crypto/random.cpp
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
    consensus/asert.cpp
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

# --- mempool: unconfirmed transactions this node might mine -------------------
# Above consensus/utxo and below mining; not part of consensus. See mempool.hpp.
add_library(amarian_mempool STATIC
    mempool.cpp
)
target_link_libraries(amarian_mempool PUBLIC amarian::consensus amarian::utxo)
add_library(amarian::mempool ALIAS amarian_mempool)

# --- mining: deciding what the next block should be --------------------------
# The only layer that answers *what block should exist next*; every other layer answers
# *is this block allowed*. Deliberately a separate target from consensus rather than a
# corner of it: an assembler sharing a translation unit with the validator could produce a
# block that passes because both agree on a mistake, and keeping them apart is what makes
# the assembler's output something the rules judge rather than something they assume.
#
# Links chain, because the height, the previous hash and the target all come from the tip,
# and mempool, because that is where a template's transactions come from. Nothing links
# mining in return — a node builds blocks, and no rule consults a builder.
add_library(amarian_mining STATIC
    mining/block_assembler.cpp
)
target_link_libraries(amarian_mining PUBLIC amarian::chain amarian::mempool)
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

# --- rpc: the operations a client can ask a node to perform -------------------
# The typed half of the JSON-RPC surface plus everything the transport decides about a
# request — and no socket, no port and no thread. That split is the point, and it is drawn
# where it is because of what each half can be wrong about.
#
# `submitblock` must run a submitted block through exactly the validation an arriving block
# gets, and `getblocktemplate` must describe exactly the block this node would mine. Both are
# properties of function calls, so they live in a target with no transport in it and are
# exercised as function calls.
#
# Framing, screening and the cookie are here for the same reason rather than the opposite one:
# whether a request is refused, and on what evidence, is a property of its bytes. A DNS-rebound
# browser request and a legitimate miner's request differ in a header, not in a socket, so the
# check that tells them apart is testable without one and belongs next to the code it guards.
# What is left for the listener target is reading, writing, and closing.
#
# rpc/client.cpp is the other end of those same bytes, and it is in this target rather than one
# of its own on purpose: the client must agree with the server about the credential's format and
# about how a body is delimited, and the way to guarantee that is for both to be the same code.
# A client target with its own copy of the cookie format would be a second implementation of the
# thing that decides who may mine, and the two would drift silently.
#
# amarian::crypto is PRIVATE and only the cookie needs it: 32 bytes from the platform CSPRNG.
# Naming it here rather than reaching for OpenSSL directly is what keeps `amarian::crypto` the
# sole cryptographic boundary even when the layer that needs randomness is the top one.
#
# amarian::json is PUBLIC because the method results *are* `nlohmann::json` values, so a
# caller cannot use this header without it. That is only tolerable here: rpc is a top
# layer, nothing below it links rpc, and so no JSON type can reach the code that decides
# whether a block is valid. The dependency file says the same thing from the other side —
# "JSON-RPC serialisation only (never consensus)".
add_library(amarian_rpc STATIC
    rpc/client.cpp
    rpc/dispatch.cpp
    rpc/http.cpp
)
target_link_libraries(amarian_rpc
    PUBLIC amarian::chain amarian::mining amarian::mempool amarian::json
    PRIVATE amarian::crypto
)
add_library(amarian::rpc ALIAS amarian_rpc)

# --- rpc_server: the socket the interface is reachable through ------------------
# A separate target from amarian::rpc so that the networking library stops here. Nothing
# but this file and the daemon links Asio, which is the same boundary drawn around OpenSSL
# and RocksDB: a target that wanted to open its own socket would have to be given the
# ability, and the CMake graph is where that is refused.
#
# amarian::asio is PRIVATE, and the pimpl in server.hpp is the other half of that promise —
# one is the link-time boundary, the other keeps Asio's headers out of anything that
# includes ours.
add_library(amarian_rpc_server STATIC
    rpc/server.cpp
)
target_link_libraries(amarian_rpc_server
    PUBLIC amarian::rpc
    PRIVATE amarian::asio
)
add_library(amarian::rpc_server ALIAS amarian_rpc_server)

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
# Links storage and mining, which is the whole shape of what a node is today: it keeps a chain
# on disk, it can build the next block, and it can answer a miner asking for one. It links no
# peer-to-peer layer because there is not one yet — Phase 4 — and until then blocks move
# between nodes through a flat file.
#
# amarian::rpc_server is the only path by which Asio reaches this binary, and it reaches it
# PRIVATEly and through a pimpl: the daemon opens no socket of its own.
add_executable(amariand
    node/amariand.cpp
)
target_link_libraries(amariand
    PRIVATE amarian::storage amarian::mining amarian::rpc_server amarian::consensus
            amarian::util amarian::version
)

# --- amarian-cli: the client an operator actually types -----------------------
# Links Asio directly, which is the one place besides the listener that happens, and for the
# symmetrical reason: this is a program whose entire job is a socket. It does not link storage,
# chain or mining — a client that could open the chainstate would be a second thing writing to
# it, and every question it can ask is one the node answers.
#
# It does link amarian::rpc, and that is the point rather than an accident: the credential
# format, the request framing and the argument rule are shared with the server instead of
# reimplemented, so the two cannot disagree about who may mine.
add_executable(amarian-cli
    cli/amarian_cli.cpp
)
target_link_libraries(amarian-cli
    PRIVATE amarian::rpc amarian::util amarian::version amarian::asio
)

# --- amarian-genesis: prints, checks and regenerates genesis parameters -----
# A tool rather than a test: it is how the recorded nonces were produced, and
# keeping it means a reviewer can reproduce a consensus constant instead of
# trusting it.
find_package(Threads REQUIRED)
add_executable(amarian-genesis
    tools/genesis_tool.cpp
)
target_link_libraries(amarian-genesis PRIVATE amarian::consensus Threads::Threads)
# --- amarian-retarget-sim: the Phase 3 difficulty simulation ----------------
# Links the real consensus retarget (chain::NextTargetBits) and drives it with a
# synthetic hashrate; the acceptance evidence for the difficulty choice lives in
# the numbers it prints, recorded in DEVELOPMENT_STATUS.md and DECISIONS.md.
add_executable(amarian-retarget-sim
    tools/retarget_sim.cpp
)
target_link_libraries(amarian-retarget-sim PRIVATE amarian::chain)

