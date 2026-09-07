#!/usr/bin/env bash
# Check repository-controlled Phase 13 evidence. This is not a launch approval.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${AMARIAN_BUILD_DIR:-$ROOT/build/dev}"
required=(
    "$ROOT/docs/PHASE13_MAINNET_READINESS.md"
    "$ROOT/docs/PHASE11_RELEASE.md"
    "$ROOT/docs/PHASE12_MEASUREMENTS.md"
    "$ROOT/scripts/phase4_acceptance.sh"
    "$ROOT/scripts/phase5_acceptance.sh"
    "$ROOT/scripts/phase11_acceptance.sh"
    "$ROOT/scripts/phase12_full_measure.sh"
    "$ROOT/SECURITY.md"
    "$ROOT/LICENSE"
)

for path in "${required[@]}"; do
    [[ -f "$path" ]] || { echo "missing evidence file: $path" >&2; exit 1; }
done

for binary in amariand amarian-cli amarian-wallet; do
    [[ -x "$BUILD/src/$binary" ]] || {
        echo "missing local build artifact: $BUILD/src/$binary" >&2
        exit 1
    }
done

git -C "$ROOT" diff --check
ctest --test-dir "$BUILD" --output-on-failure

echo "LOCAL PHASE 13 AUDIT: pass"
echo "External security review, long-running testnet evidence, operational sign-off,"
echo "release signing, and final launch approval remain open gates."
