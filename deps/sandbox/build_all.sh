#!/usr/bin/env bash
# Single-turn end-to-end pipeline:
#   dependency chains (A + B in parallel) -> configure + build
#   (libslic3r + fff_print_tests + libslic3r_tests) -> acceptance + regression
#   tests.
#
# IMPORTANT: this MUST run inside ONE turn. The sandbox — including
# deps/build — is wiped when a turn ends (processes die, non-workspace files
# are dropped), so nothing in this pipeline may rely on surviving a turn
# boundary. Launch it (e.g. via start_process) and poll deps/build/log/
# PIPELINE_RESULT.txt until it says PIPELINE OK or FAILED.
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO" || exit 1
L=$REPO/deps/build/log
mkdir -p "$L"
R=$L/PIPELINE_RESULT.txt
: > "$R"

step() { echo "[$(date +%H:%M:%S)] === $* ===" | tee -a "$R"; }
fail() { echo "[$(date +%H:%M:%S)] FAILED: $*" | tee -a "$R"; exit 1; }

step "dependency chain A (C/C++ deps)"
bash deps/sandbox/chain_a.sh >> "$L/chain_a.out" 2>&1 &
PA=$!
step "dependency chain B (CPython, parallel with A)"
bash deps/sandbox/chain_b.sh >> "$L/chain_b.out" 2>&1 &
PB=$!
wait "$PA" || fail "chain_a (see $L/chain_a.out and $L/<dep>.log)"
wait "$PB" || fail "chain_b (see $L/chain_b.out and $L/python.log)"
step "configure + build libslic3r + test binaries"
bash deps/sandbox/configure_main.sh >> "$L/main.out" 2>&1 || fail "configure_main (see $L/main.out, main_configure.log, main_build.log)"
step "acceptance + regression tests"
bash deps/sandbox/run_tests.sh >> "$L/tests.out" 2>&1 || fail "run_tests (see $L/tests.out, ctest_purge.log, ctest_regression.log)"
echo "[$(date +%H:%M:%S)] PIPELINE OK" | tee -a "$R"
