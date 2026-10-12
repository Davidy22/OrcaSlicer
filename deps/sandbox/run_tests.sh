#!/usr/bin/env bash
# Run the purge-inner-wall acceptance suite plus regression tests.
# Run after configure_main.sh has built libslic3r + fff_print_tests + libslic3r_tests.
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD=$REPO/build
LOG=$REPO/deps/build/log
mkdir -p "$LOG"

cd "$BUILD/tests" || { echo "no build dir — run configure_main.sh first"; exit 1; }

echo "== acceptance: purge inner walls =="
ctest -R "PurgeInnerWalls|PurgeInnerWallPlan" --output-on-failure 2>&1 | tee "$LOG/ctest_purge.log" | tail -15

echo "== regressions =="
ctest -R "test_wipe|test_mixed_filament|test_multi_nozzle_utils|test_tree_support" --output-on-failure 2>&1 | tee "$LOG/ctest_regression.log" | tail -15

echo "== summary =="
grep -h "tests passed" "$LOG/ctest_purge.log" "$LOG/ctest_regression.log"
