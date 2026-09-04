# Amarian — root build definition.
#
# Layering is enforced by target dependencies: consensus code may not depend on
# storage, networking, wallet or RPC. See docs/ARCHITECTURE.md.

cmake_minimum_required(VERSION 3.28)

project(amarian
    VERSION 0.1.0
    DESCRIPTION "Amarian: an independent proof-of-work, UTXO, hard-capped, post-quantum-ready blockchain"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Pre-release qualifier appended to PROJECT_VERSION. Cleared for a tagged release.
# Kept separate from PROJECT_VERSION so CMake's version comparisons stay numeric.
set(AMARIAN_VERSION_PRERELEASE "dev" CACHE STRING "Pre-release qualifier, empty on release")

option(AMARIAN_BUILD_TESTS "Build the test suite" ON)
option(AMARIAN_BUILD_BENCH "Build microbenchmarks" OFF)
option(AMARIAN_BUILD_FUZZ  "Build libFuzzer targets (requires clang)" OFF)
option(AMARIAN_WERROR      "Treat warnings as errors" ON)
option(AMARIAN_HARDENING   "Enable hardening flags and libstdc++ assertions" ON)
set(AMARIAN_SANITIZER "" CACHE STRING "Sanitizer: '', address, undefined, address+undefined, thread, memory")

include(AmarianWarnings)
include(AmarianHardening)
include(AmarianSanitizers)
include(AmarianDependencies)
include(AmarianBuildInfo)

# ---------------------------------------------------------------------------
# Interface target carrying the project-wide compile settings.
# ---------------------------------------------------------------------------
add_library(amarian_settings INTERFACE)
target_include_directories(amarian_settings INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(amarian_settings INTERFACE cxx_std_23)
amarian_apply_warnings(amarian_settings)
amarian_apply_hardening(amarian_settings)
amarian_apply_sanitizers(amarian_settings)
amarian_configure_build_info()

add_subdirectory(src)

if(AMARIAN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(AMARIAN_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(AMARIAN_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif()

message(STATUS "")
message(STATUS "Amarian ${PROJECT_VERSION} configuration")
message(STATUS "  compiler       : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "  sanitizer      : ${AMARIAN_SANITIZER}")
message(STATUS "  hardening      : ${AMARIAN_HARDENING}")
message(STATUS "  warnings-as-err: ${AMARIAN_WERROR}")
message(STATUS "  tests          : ${AMARIAN_BUILD_TESTS}")
message(STATUS "  benchmarks     : ${AMARIAN_BUILD_BENCH}")
message(STATUS "  fuzzers        : ${AMARIAN_BUILD_FUZZ}")
message(STATUS "  libsecp256k1   : ${AMARIAN_SECP256K1_VERSION}")
message(STATUS "  OpenSSL        : ${OPENSSL_VERSION}")
message(STATUS "  RocksDB        : ${AMARIAN_ROCKSDB_INCLUDE_DIR}")
message(STATUS "")
