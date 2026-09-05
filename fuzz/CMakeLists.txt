# Fuzz targets.
#
# Two kinds of harness live here, and the distinction matters when triaging a
# finding:
#
#   * Attacker-facing parsers — anything reachable from P2P, RPC or a block file.
#     A crash on malformed input is a remote denial of service, so these must
#     survive arbitrary bytes without aborting, reading out of bounds, or
#     allocating unboundedly.
#   * Operator-facing parsers such as the command line. Not remote attack
#     surface, but they decide which network a node joins and where its
#     chainstate lives, so silent misinterpretation is the bug being hunted
#     rather than a crash.
#
# Both kinds also assert correctness properties (round-tripping, determinism,
# no partial state) that a unit test can only check on the cases someone thought
# of. Those use FUZZ_CHECK, not assert: the fuzz preset is RelWithDebInfo, so
# assert is compiled out and would check nothing.
#
# Reproducible crashes get promoted into tests/vectors/ as explicit regression
# cases rather than living in a corpus directory.

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "AMARIAN_BUILD_FUZZ requires Clang for libFuzzer. Use the 'fuzz' preset.")
endif()

# Shared settings for every libFuzzer target. The sanitizer flags themselves come
# from amarian_settings via AMARIAN_SANITIZER, so only the fuzzer driver and
# coverage instrumentation are added here.
add_library(amarian_fuzz_settings INTERFACE)
target_compile_options(amarian_fuzz_settings INTERFACE
    -fsanitize=fuzzer-no-link)
target_link_options(amarian_fuzz_settings INTERFACE
    -fsanitize=fuzzer)

function(amarian_add_fuzzer name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE amarian_fuzz_settings ${ARGN})

    # A second executable over the same harness with a plain main instead of
    # libFuzzer's. Needed to replay a saved input under gdb or valgrind, and to
    # separate the harness's memory behaviour from the fuzzer's. Built with
    # coverage instrumentation but without the fuzzer driver, so it links a real
    # main.
    add_executable(standalone_${name} ${source} standalone_main.cpp)
    target_compile_options(standalone_${name} PRIVATE -fsanitize=fuzzer-no-link)
    target_link_libraries(standalone_${name} PRIVATE ${ARGN})
endfunction()

# Attacker-facing: hex arrives from RPC arguments and config files.
amarian_add_fuzzer(fuzz_hex hex_fuzz.cpp amarian::util)

# Attacker-facing, and the widest surface in the project: every byte a node acts
# on arrives through the Reader.
amarian_add_fuzzer(fuzz_serialize serialize_fuzz.cpp amarian::util)

# Operator-facing: robustness and no-silent-misinterpretation for the CLI parser.
amarian_add_fuzzer(fuzz_args args_fuzz.cpp amarian::util)
