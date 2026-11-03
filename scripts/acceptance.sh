#!/usr/bin/env bash
# Phase 4 / task 8.5 acceptance preflight and deterministic campaign fixture.
# This does not impersonate the two required human sign-offs.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${2:-$ROOT_DIR/build}"
if [[ "$BUILD_DIR" != /* ]]; then BUILD_DIR="$ROOT_DIR/$BUILD_DIR"; fi
MODE="${1:---ci}"
DOC="$ROOT_DIR/docs/acceptance_walkthrough.md"
BIN="$BUILD_DIR/runtime/harrowmere"
SOAK="$BUILD_DIR/runtime/harrowmere_soak"

fail() { printf 'acceptance: FAIL: %s\n' "$*" >&2; exit 1; }
pass() { printf 'acceptance: ok: %s\n' "$*"; }

[[ -f "$DOC" ]] || fail "missing $DOC"
COUNT="$(grep -c '^### A[0-9][0-9] — ' "$DOC" || true)"
[[ "$COUNT" == 40 ]] || fail "expected 40 scenario headings, found $COUNT"
for id in $(seq -w 1 40); do
    grep -Fq "### A${id} — " "$DOC" || fail "missing scenario A${id}"
done
for field in 'Setup:' 'Steps:' 'Expected:' 'Proof:' 'Result:'; do
    count="$(grep -c "^- \\*\\*${field}\\*\\*" "$DOC" || true)"
    [[ "$count" == 40 ]] || fail "expected 40 ${field} sections, found $count"
done
pass "40 acceptance scenarios have setup/steps/expected/proof/result fields"

if [[ "$MODE" == "--check-only" ]]; then
    exit 0
fi
[[ "$MODE" == "--ci" ]] || fail "usage: scripts/acceptance.sh [--ci|--check-only] [build-dir]"
[[ -x "$BIN" ]] || fail "runtime binary not found: $BIN (build first)"
[[ -x "$SOAK" ]] || fail "headless acceptance harness not found: $SOAK (build first)"

"$BIN" --validate
GRAPH="$(mktemp)"
SCENARIOS="$(mktemp)"
trap 'rm -f "$GRAPH" "$SCENARIOS"' EXIT
"$BIN" --print-graph >"$GRAPH"
grep -q 'harrowmere_core' "$GRAPH" || fail 'component graph omits harrowmere_core'
grep -q 'harrowmere_ui' "$GRAPH" || fail 'component graph omits harrowmere_ui'
if grep -q 'ui_imgui' "$GRAPH"; then
    fail 'trimmed ui_imgui unexpectedly joined the resolved graph'
fi
pass "assembly validates and graph contains the two game mods without ui_imgui"

"$SOAK" --list-scenarios >"$SCENARIOS"
[[ "$(grep -c '^A[0-9][0-9]$' "$SCENARIOS")" == 40 ]] || fail 'soak harness does not expose all 40 acceptance presets'
pass "headless harness exposes 40 deterministic acceptance presets"
"$SOAK" --scenario A36 --seed 20261102 --data-dir "$ROOT_DIR/mods/harrowmere_core/data"
pass "A36 three-contributor reward payout and deterministic remainder fixture passed"
"$SOAK" --scenario A40 --seed 20261102 --data-dir "$ROOT_DIR/mods/harrowmere_core/data"
pass "A40 ten-day/save-load/ten-day/territory-transfer/save-load campaign fixture passed"

printf '%s\n' 'acceptance: manual execution/sign-off remains a human gate; see docs/acceptance_walkthrough.md'
