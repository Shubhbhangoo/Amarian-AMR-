#!/usr/bin/env bash
# Probe the libstdc++ <charconv> UBSan integer finding and the proposed fix.
#
# The finding is an implicit char -> unsigned char conversion inside
# __from_chars_alnum. That conversion is well-defined in C++ (modular), so this is
# UBSan's "suspicious behaviour" integer check firing on third-party stdlib code,
# not undefined behaviour and not an Amarian defect. This script proves four
# things before the build system is changed:
#
#   1. the report reproduces from a correct from_chars call on arbitrary bytes,
#   2. -fno-sanitize-recover=integer makes such a finding fail the run instead of
#      printing a line and continuing,
#   3. which ignorelist syntax actually silences it (the section header is a glob
#      over sanitizer names, so a comma-separated list silently matches nothing),
#   4. that the same check still fires on our own translation units afterwards.
set -uo pipefail

CXX=clang++-21
INT_FLAGS="-fsanitize=integer -fno-sanitize=unsigned-integer-overflow -fno-sanitize=unsigned-shift-base"
WORK=/tmp/amarian_ubsan_probe
rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK" || exit 1

cat >probe.cpp <<'EOF'
// Correct from_chars usage over attacker-supplied bytes, exactly as
// ArgsParser::GetInt does it: whole-range parse, error and endpoint both checked.
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <system_error>

int main() {
    const std::string text = "\xad" "7";
    int64_t out = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto r = std::from_chars(begin, end, out);
    std::printf("stdlib from_chars rejected=%d\n", r.ec != std::errc{});
    return 0;
}
EOF

cat >ours.cpp <<'EOF'
// The same conversion written in our own code. This one must keep firing: an
// unnoticed sign change in a validator is a consensus bug.
#include <cstdio>
volatile char g = -83;

int main() {
    unsigned char c = g;  // implicit-integer-sign-change, in our translation unit
    std::printf("ours=%u\n", static_cast<unsigned>(c));
    return 0;
}
EOF

run() {
    local label="$1" src="$2"
    shift 2
    if ! $CXX -std=c++23 -O1 -g "$@" "$src" -o bin 2>cc.log; then
        printf '  %-42s COMPILE FAILED: %s\n' "$label" "$(head -2 cc.log | tr '\n' ' ')"
        return
    fi
    local out rc reports
    out=$(./bin 2>&1)
    rc=$?
    reports=$(printf '%s\n' "$out" | grep -c "runtime error")
    printf '  %-42s exit=%s reports=%s\n' "$label" "$rc" "$reports"
}

echo "--- 1. baseline: does it reproduce, and is it recoverable? ---"
run "stdlib, recoverable" probe.cpp $INT_FLAGS

echo "--- 2. with -fno-sanitize-recover=integer ---"
run "stdlib, fatal" probe.cpp $INT_FLAGS -fno-sanitize-recover=integer
run "our code, fatal" ours.cpp $INT_FLAGS -fno-sanitize-recover=integer

echo "--- 3. which ignorelist syntax silences the stdlib report? ---"
syntax() {
    printf '%b' "$2" >list.txt
    run "$1" probe.cpp $INT_FLAGS -fno-sanitize-recover=integer -fsanitize-ignorelist=list.txt
}
syntax "comma-separated sections (suspected no-op)" \
    '[implicit-integer-sign-change,implicit-signed-integer-truncation]\nsrc:*include/c++/*\n'
syntax "repeated single-name sections" \
    '[implicit-integer-sign-change]\nsrc:*include/c++/*\n[implicit-signed-integer-truncation]\nsrc:*include/c++/*\n'
syntax "brace alternation" \
    '[{implicit-integer-sign-change|implicit-signed-integer-truncation}]\nsrc:*include/c++/*\n'
syntax "wildcard section implicit-*" '[implicit-*]\nsrc:*include/c++/*\n'
syntax "no section header" 'src:*include/c++/*\n'
syntax "mainfile: instead of src:" '[implicit-*]\nmainfile:*include/c++/*\n'
syntax "exact absolute path" '[implicit-*]\nsrc:/usr/include/c++/15/charconv\n'
syntax "section [*]" '[*]\nsrc:*include/c++/*\n'

echo "--- 4. does the winning syntax leave our own code instrumented? ---"
printf '%b' '[implicit-*]\nsrc:*include/c++/*\n' >list.txt
run "our code, fatal + implicit-* ignorelist" ours.cpp $INT_FLAGS \
    -fno-sanitize-recover=integer -fsanitize-ignorelist=list.txt

echo
echo "Wanted: (1) reports>=1 exit=0. (2) both exit!=0. (3) at least one syntax with"
echo "exit=0 reports=0. (4) still exit!=0, or the check has been thrown away."
