// mods/harrowmere_core/tests/main.cpp
// Task 0.6 — Test harness entry point.
//
// Defines the doctest main and the single Phase-1 sanity test
// `TestHarness_SanityAlwaysPasses`. This is the *only* test that ships in
// Phase 1: it guards against a silently-empty test run (if it is the only
// test that ran, the harness wiring is broken). CI asserts test count > 20
// from Layer 2 onward — see scripts/ci.sh.
//
// Test-name conventions follow §3.5: `Test<System>_<Behavior>_<Expected>`.
// Later tasks add their tests in `tests/test_<system>.cpp` files alongside
// this main; doctest's `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` here is the
// single registration point — every other test file just `#include`s the
// header and defines `TEST_CASE`s.
//
// The binary links harrowmere_core's *objects* (not the runtime mod
// library), so tests run without the engine's plugin scanner. See
// `tests/CMakeLists.txt` for the linking contract.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

// ---------------------------------------------------------------------------
// TestHarness_SanityAlwaysPasses
//
// Guards against a silently-empty test run. If this is the only test that
// executed, the harness wiring is broken somewhere (a TEST_CASE failed to
// compile, a test file failed to link, CMake forgot to register the
// source). The CI script (scripts/ci.sh) parses ctest's report and asserts
// the test count exceeds 20 once Layer 2 lands.
//
// Trivial assertions only — the point is presence, not arithmetic.
// ---------------------------------------------------------------------------
TEST_CASE("TestHarness_SanityAlwaysPasses") {
    // The doctest framework initialised and ran this case.
    CHECK(true);

    // Basic arithmetic — catches a broken toolchain (e.g. UB-san trap on
    // integer overflow would have aborted before this line).
    CHECK(1 + 1 == 2);

    // The doctest version we vendored is the pinned v2.4.12.
    CHECK_EQ(DOCTEST_VERSION_MAJOR, 2);
    CHECK_EQ(DOCTEST_VERSION_MINOR, 4);
    CHECK_EQ(DOCTEST_VERSION_PATCH, 12);
}
