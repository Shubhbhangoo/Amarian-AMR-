# Amarian test suite.
#
# Three tiers, all under ctest:
#   unit/       - component behaviour, fast, no I/O
#   consensus/  - rule-by-rule validation and adversarial cases (the important ones)
#   integration/- multi-node, storage, sync; slower, may use temp directories
#
# Consensus tests are labelled so `ctest -L consensus` can gate a release.

include(GoogleTest)

function(amarian_add_test name tier)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE amarian_settings GTest::gtest GTest::gtest_main)
    # Sanitizer/hardening flags come from amarian_settings; nothing else to set here.
    gtest_discover_tests(${name}
        PROPERTIES LABELS "${tier}"
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 60)
endfunction()

add_subdirectory(unit)
