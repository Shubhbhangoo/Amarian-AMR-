# External dependency resolution for Amarian.
#
# Policy: Amarian does not implement its own cryptographic primitives. Classical
# signatures come from libsecp256k1 (the implementation Bitcoin Core uses) and
# post-quantum signatures come from OpenSSL's FIPS 204 / FIPS 205 implementations.

find_package(Threads REQUIRED)

# --- libsecp256k1: ECDSA + BIP-340 Schnorr, x-only pubkeys ------------------
if(WIN32)
    find_package(libsecp256k1 CONFIG REQUIRED)
    add_library(amarian::secp256k1 ALIAS libsecp256k1::secp256k1)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SECP256K1 REQUIRED IMPORTED_TARGET libsecp256k1)
    set(AMARIAN_SECP256K1_VERSION "${SECP256K1_VERSION}" CACHE INTERNAL "")
    add_library(amarian::secp256k1 ALIAS PkgConfig::SECP256K1)
endif()

# --- OpenSSL: SHA-256, ML-DSA (FIPS 204), SLH-DSA (FIPS 205), CSPRNG --------
find_package(OpenSSL 3.5 REQUIRED COMPONENTS Crypto)
add_library(amarian_openssl INTERFACE)
target_link_libraries(amarian_openssl INTERFACE OpenSSL::Crypto)
add_library(amarian::openssl ALIAS amarian_openssl)

# --- RocksDB: chainstate and index storage ---------------------------------
if(WIN32)
    find_package(RocksDB CONFIG REQUIRED)
    add_library(amarian::rocksdb ALIAS RocksDB::rocksdb)
else()
    find_path(AMARIAN_ROCKSDB_INCLUDE_DIR rocksdb/db.h)
    find_library(AMARIAN_ROCKSDB_LIBRARY NAMES rocksdb)
    if(NOT AMARIAN_ROCKSDB_INCLUDE_DIR OR NOT AMARIAN_ROCKSDB_LIBRARY)
        message(FATAL_ERROR "RocksDB not found (install librocksdb-dev)")
    endif()
    add_library(amarian_rocksdb INTERFACE)
    target_include_directories(amarian_rocksdb SYSTEM INTERFACE "${AMARIAN_ROCKSDB_INCLUDE_DIR}")
    target_link_libraries(amarian_rocksdb INTERFACE "${AMARIAN_ROCKSDB_LIBRARY}" Threads::Threads)
    add_library(amarian::rocksdb ALIAS amarian_rocksdb)
endif()

# --- Asio (standalone, header-only): P2P transport -------------------------
find_path(AMARIAN_ASIO_INCLUDE_DIR asio.hpp)
if(NOT AMARIAN_ASIO_INCLUDE_DIR)
    message(FATAL_ERROR "standalone Asio not found (install libasio-dev)")
endif()
add_library(amarian_asio INTERFACE)
target_include_directories(amarian_asio SYSTEM INTERFACE "${AMARIAN_ASIO_INCLUDE_DIR}")
target_compile_definitions(amarian_asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
if(WIN32)
    target_compile_definitions(amarian_asio INTERFACE _WIN32_WINNT=0x0601)
endif()
target_link_libraries(amarian_asio INTERFACE Threads::Threads)
add_library(amarian::asio ALIAS amarian_asio)

# --- nlohmann/json: JSON-RPC serialisation only (never consensus) ----------
find_package(nlohmann_json 3.11 REQUIRED)
add_library(amarian_json INTERFACE)
target_link_libraries(amarian_json INTERFACE nlohmann_json::nlohmann_json)
add_library(amarian::json ALIAS amarian_json)

# --- Test / benchmark frameworks ------------------------------------------
if(AMARIAN_BUILD_TESTS)
    find_package(GTest REQUIRED)
endif()
if(AMARIAN_BUILD_BENCH)
    find_package(benchmark REQUIRED)
endif()
