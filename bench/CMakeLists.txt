# Amarian microbenchmarks.
#
# Purpose is Phase 12 — "profile before optimising" — plus regression detection.
# Two rules keep the numbers honest:
#
#   * Benchmarks build in the shipping configuration. The bench preset leaves
#     AMARIAN_HARDENING on, so _GLIBCXX_ASSERTIONS and _FORTIFY_SOURCE=3 are
#     active exactly as they are in a release build. Numbers from an unhardened
#     build would describe a binary nobody runs.
#   * Any figure quoted in documentation must come from an actual run of these
#     targets, with the machine and build stated alongside it. Benchmark results
#     are measured, never estimated.
#
# Grouped one executable per layer so a single run covers a whole layer, matching
# how tests/ groups its targets.

function(amarian_add_bench name)
    add_executable(${name} ${ARGN})
    # benchmark_main supplies main(); amarian_settings supplies the include path,
    # warning set and hardening flags.
    target_link_libraries(${name} PRIVATE amarian_settings benchmark::benchmark_main)
endfunction()

amarian_add_bench(bench_util
    util_hex_bench.cpp
    util_overflow_bench.cpp
)
target_link_libraries(bench_util PRIVATE amarian::util)
