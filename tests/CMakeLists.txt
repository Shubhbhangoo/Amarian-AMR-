# Amarian test suite.
#
# Two tiers exist under ctest, and a third is planned:
#   unit/       - component behaviour, fast, no I/O
#   consensus/  - rule-by-rule validation and adversarial cases (the important ones)
#   integration/- multi-node, storage, sync; not built yet. The one multi-node check that
#                 exists is scripts/phase1_acceptance.sh, run deliberately rather than by
#                 ctest, because it costs two RocksDB directories and a mining run.
#
# A test may carry more than one label. `test_consensus` is in both tiers because it is the
# rule-by-rule suite that predates the consensus/ directory, and a release gate that ran the
# adversarial cases while skipping the rules they are adversarial about would be worse than
# no gate at all.

include(GoogleTest)

function(amarian_add_test name tiers)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE amarian_settings GTest::gtest GTest::gtest_main)
    # Sanitizer/hardening flags come from amarian_settings; nothing else to set here.
    #
    # The escaping is load-bearing. `gtest_discover_tests` writes PROPERTIES straight into a
    # generated `set_tests_properties` call, so a bare `LABELS unit;consensus` arrives there as
    # `LABELS unit consensus` — which CMake reads as LABELS=unit followed by a property named
    # `consensus`, and the second tier is silently dropped. Escaping the separator keeps it one
    # value. This was caught by `ctest -L consensus` matching seven tests instead of ninety-five.
    #
    # Changing a tier needs `build/<preset>/tests/**/*_tests.cmake` deleted to take effect. Under
    # DISCOVERY_MODE PRE_TEST those files cache the discovered test list, and editing PROPERTIES
    # here rewrites the `_include.cmake` that consults the cache without invalidating the cache
    # itself — so a corrected label reads as having changed nothing.
    string(REPLACE ";" "\\;" escaped_labels "${tiers}")
    gtest_discover_tests(${name}
        PROPERTIES LABELS "${escaped_labels}"
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 60)
endfunction()

add_subdirectory(unit)
add_subdirectory(consensus)
