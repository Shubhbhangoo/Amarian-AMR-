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
    util/types.cpp
    util/logging.cpp
)
target_link_libraries(amarian_util PUBLIC amarian_settings)
add_library(amarian::util ALIAS amarian_util)
