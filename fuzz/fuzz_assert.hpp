#pragma once

/// \file
/// Assertions for fuzz harnesses.
///
/// `assert` is compiled out under NDEBUG, and the fuzz preset builds
/// RelWithDebInfo, so a harness that used `assert` for its correctness
/// properties would silently check nothing. FUZZ_CHECK always aborts, which is
/// what libFuzzer needs in order to record a crash and minimise the input.

#include <cstdio>
#include <cstdlib>

#define FUZZ_CHECK(cond, message)                                                       \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::fprintf(stderr,                                                        \
                         "FUZZ_CHECK failed at %s:%d\n  condition: %s\n  reason: %s\n", \
                         __FILE__,                                                      \
                         __LINE__,                                                      \
                         #cond,                                                         \
                         (message));                                                    \
            std::abort();                                                               \
        }                                                                               \
    } while (false)
