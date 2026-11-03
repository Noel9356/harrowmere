#!/usr/bin/env bash
# Phase 4 one-command gate. The full three-game-year soak is opt-in for the
# nightly job; the PR gate runs the one-year soak registered in CTest.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-$ROOT_DIR/build}"
if [[ "$BUILD_DIR" != /* ]]; then BUILD_DIR="$ROOT_DIR/$BUILD_DIR"; fi

printf '%s\n' '== harrowmere ci: configure (Release) =='
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release

printf '%s\n' '== harrowmere ci: build (warnings-as-errors) =='
cmake --build "$BUILD_DIR" --parallel

printf '%s\n' '== harrowmere ci: core ctest (determinism, roundtrip/fuzz, balance, PR soak) =='
ctest --test-dir "$BUILD_DIR/mods/harrowmere_core/tests" --output-on-failure -LE nightly

printf '%s\n' '== harrowmere ci: UI ctest =='
ctest --test-dir "$BUILD_DIR/mods/harrowmere_ui/tests" --output-on-failure

printf '%s\n' '== harrowmere ci: bootstrap smoke and graph =='
"$ROOT_DIR/scripts/bootstrap_tests.sh" "$BUILD_DIR"

printf '%s\n' '== harrowmere ci: balance measurement (one game-year) =='
"$BUILD_DIR/runtime/harrowmere_soak" --balance --game-years 1 --seed 20261102

printf '%s\n' '== harrowmere ci: task 8.5 acceptance preflight =='
"$ROOT_DIR/scripts/acceptance.sh" --ci "$BUILD_DIR"

if [[ "${HARROWMERE_FULL_NIGHTLY:-0}" == 1 ]]; then
    printf '%s\n' '== harrowmere ci: full three-game-year nightly soak =='
    "$BUILD_DIR/runtime/harrowmere_soak" --game-years 3 --seed 20261102
fi

printf '%s\n' 'harrowmere ci: automated Phase 4 gates green (manual 8.5 sign-off remains separate)'
