#!/usr/bin/env bash
# Builds and tests every preset that has tests, and prints one line per preset:
#   <preset> build=<ok|FAIL> warnings=<n> tests=<ctest pass line>
# Logs go under the Windows temp directory so they survive the WSL VM stopping.
set -u

cd "/mnt/e/project Amarian" || exit 1
LOGS=/mnt/c/Users/Shubhkarman/AppData/Local/Temp/amarian_matrix
mkdir -p "$LOGS"

for preset in dev debug clang-dev asan tsan; do
    build_log="$LOGS/$preset.build.log"
    test_log="$LOGS/$preset.test.log"

    if [ ! -d "build/$preset" ]; then
        cmake --preset "$preset" > "$LOGS/$preset.configure.log" 2>&1
    fi

    if cmake --build "build/$preset" > "$build_log" 2>&1; then
        build=ok
    else
        build=FAIL
    fi
    warnings=$(grep -c "warning:" "$build_log")

    ctest --preset "$preset" > "$test_log" 2>&1
    tests=$(grep -E "tests passed, .* tests failed" "$test_log" | tail -1)
    [ -z "$tests" ] && tests="no pass line — see $test_log"

    printf '%-10s build=%-4s warnings=%-3s %s\n' "$preset" "$build" "$warnings" "$tests"
done
