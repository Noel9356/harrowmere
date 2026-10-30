#!/usr/bin/env bash
# scripts/ci.sh - Harrowmere's one-command local CI (task 0.6).
#
#   1. configure (Release; the game targets carry warnings-as-errors)
#   2. build (app + both mod packages + the unit-test binaries)
#   3. ctest (harrowmere_core_tests + harrowmere_ui_tests; discovery is
#      directory-local - see each mods/<id>/tests/CMakeLists.txt)
#   4. the engine's validate() dry run (configuration, manifests, graph,
#      permissions - no backend), exit 0 expected
#
# Any failure exits non-zero. This script is the acceptance gate every PR
# runs locally (the repo has no CI provider configured).
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-$ROOT_DIR/build}"

echo "== harrowmere ci: configure (Release) =="
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release

echo "== harrowmere ci: build =="
cmake --build "$BUILD_DIR" --parallel

echo "== harrowmere ci: ctest (harrowmere_core_tests) =="
ctest --test-dir "$BUILD_DIR/mods/harrowmere_core/tests" --output-on-failure

echo "== harrowmere ci: ctest (harrowmere_ui_tests) =="
ctest --test-dir "$BUILD_DIR/mods/harrowmere_ui/tests" --output-on-failure

echo "== harrowmere ci: engine validate dry run =="
"$BUILD_DIR/runtime/harrowmere" --validate

echo "harrowmere ci: all green"
