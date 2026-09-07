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

# Amarian uses headers, not C++20 modules. Left on, CMake adds a dependency-scan
# step per source file and compiles everything with GCC's experimental
# -fmodules-ts; neither buys anything here, and consensus code should not be built
# with an experimental front-end mode. Revisit if the project adopts modules.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

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
option(AMARIAN_BUILD_GUI   "Build the native Windows operator GUI" OFF)
option(AMARIAN_BUILD_GPU_MINER "Build the CUDA testnet-only GPU miner" OFF)
option(AMARIAN_WERROR      "Treat warnings as errors" ON)
option(AMARIAN_HARDENING   "Enable hardening flags and libstdc++ assertions" ON)
set(AMARIAN_SANITIZER "" CACHE STRING "Sanitizer: '', address, undefined, address+undefined, thread, memory")

include(AmarianWarnings)
include(AmarianHardening)
include(AmarianSanitizers)
include(AmarianDependencies)
include(AmarianBuildInfo)
include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Interface target carrying the project-wide compile settings.
# ---------------------------------------------------------------------------
add_library(amarian_settings INTERFACE)
target_include_directories(amarian_settings INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(amarian_settings INTERFACE cxx_std_23)
if(MSVC)
    # MSVC otherwise leaves __cplusplus at the legacy 199711 value, which makes
    # the build-info report contradict the actual C++23 compiler mode.
    target_compile_options(amarian_settings INTERFACE /Zc:__cplusplus)
endif()
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

install(TARGETS amariand amarian-cli amarian-wallet amarian-genesis amarian-retarget-sim
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET amarian-gui)
    install(TARGETS amarian-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(TARGET amarian-wallet-gui)
    install(TARGETS amarian-wallet-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(TARGET amarian-miner)
    install(TARGETS amarian-miner RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs
        FILES_MATCHING PATTERN "*.md")

set(CPACK_PACKAGE_NAME "amarian")
set(CPACK_PACKAGE_VENDOR "Amarian Project")
set(CPACK_PACKAGE_CONTACT "Amarian Project")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Amarian proof-of-work blockchain node and wallet")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "amarian-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TGZ;DEB")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Amarian Project")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)

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
