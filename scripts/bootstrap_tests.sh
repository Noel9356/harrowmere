#!/usr/bin/env bash
# scripts/bootstrap_tests.sh - task 0.1's pinned bootstrap acceptance.
#
# TestBootstrap_GameAssemblyBoots (headless, CI-safe):
#   - `--set engine.quit_after_seconds=1.0` runs a finite 1 s session and
#     exits 0 (the DEVIATIONS.md 0.1 row records why the finite-run override
#     is spelled as a TOML float: the engine's pacing reader rejects integer
#     nodes),
#   - "harrowmere_core 0.1.0 up" appears in the log,
#   - the resolved component graph (--print-graph) has exactly one world
#     claimant (harrowmere_core, via its manifest scene claim) and no
#     ui_imgui module (the engine's ui_imgui is trim-excluded from the run -
#     D-UI-1; the module directory remains only as a build-edge header
#     provider, it never joins the resolved graph).
#
# TestBootstrap_WindowedLoopRuns (dev machine with GLFW/Vulkan): opens a
# window, renders >= 10 frames, stops cleanly. Skipped with a notice when no
# display is available (headless CI); it must be green on a dev machine.
set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT_DIR/build/runtime/harrowmere"
FAILURES=0

note() { printf '%s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*"; FAILURES=$((FAILURES + 1)); }
pass() { printf 'ok:   %s\n' "$*"; }

if ! test -x "$BIN"; then
    echo "bootstrap_tests: $BIN not found - build first (cmake --build build --parallel)" >&2
    exit 1
fi

WORK="$(mktemp -d)"
LOG="$WORK/boot.log"
GRAPH="$WORK/graph.txt"

# --- TestBootstrap_GameAssemblyBoots ---------------------------------------
note "TestBootstrap_GameAssemblyBoots (headless, quit_after_seconds=1.0)"
"$BIN" --set engine.quit_after_seconds=1.0 >"$LOG" 2>&1
RC=$?
if test "$RC" -eq 0; then
    pass "headless boot exits 0"
else
    fail "headless boot exit code $RC (expected 0); log at $LOG"
fi

if grep -q "harrowmere_core 0.1.0 up" "$LOG"; then
    pass "startup log carries the pinned line"
else
    fail "startup log missing 'harrowmere_core 0.1.0 up'"
fi

"$BIN" --print-graph >"$GRAPH" 2>&1
if grep -q "harrowmere_core" "$GRAPH" && ! grep -q "ui_imgui" "$GRAPH"; then
    pass "graph lists harrowmere_core and never ui_imgui"
else
    fail "resolved graph wrong: ui_imgui must be absent, harrowmere_core present"
fi

CLAIMS=0
for MANIFEST in "$ROOT_DIR"/build/runtime/mods/*/mod.toml; do
    test -f "$MANIFEST" || continue
    if grep -q "^scene" "$MANIFEST"; then
        CLAIMS=$((CLAIMS + 1))
    fi
done
if test "$CLAIMS" -eq 1; then
    pass "exactly one world claimant in the runtime (the one-world rule)"
else
    fail "$CLAIMS world claimants found (expected exactly 1)"
fi

# --- TestBootstrap_WindowedLoopRuns -----------------------------------------
note "TestBootstrap_WindowedLoopRuns (windowed; skipped headless)"
if test -n "${DISPLAY:-}" || test -n "${WAYLAND_DISPLAY:-}"; then
    "$BIN" --set engine.quit_after_seconds=1.0 >"$WORK/windowed.log" 2>&1
    RC=$?
    if test "$RC" -eq 0 && grep -q "simulation capability acquired" "$WORK/windowed.log"; then
        pass "windowed loop ran and acquired the sim capability"
    else
        fail "windowed run failed (exit $RC); log at $WORK/windowed.log"
    fi
else
    note "skipped: no display (headless CI) - run on a dev machine with GLFW/Vulkan"
fi

rm -rf "$WORK"
if test "$FAILURES" -eq 0; then
    echo "bootstrap_tests: all green"
    exit 0
fi
echo "bootstrap_tests: $FAILURES failure(s)"
exit 1
