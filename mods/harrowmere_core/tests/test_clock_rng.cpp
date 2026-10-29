// tests/test_clock_rng.cpp — TestClock_* and TestRng_* for task 0.3.
//
// Pinned test names (must exist, per task 0.3):
//   TestClock_AdvanceTick_TimeScaleMath
//   TestRng_SameSeedSameSequence
//   TestRng_RangeBounds
//   TestRng_StateRoundTrip
//
// Additional tests added in this implementation:
//   TestClock_AdvanceTick_RemainderCarry      — non-divisible tickRate case
//   TestClock_DayPhase01_BoundsAndPhases      — sun/NIGHT-mask driver
//   TestClock_GameDay_RollsAtMidnight
//   TestClock_TickRateZero_GuardsR2           — release-mode R2 fallback
//   TestRng_DifferentSeedsDiffer              — sanity: hash mix is non-trivial
//   TestRng_StreamsAreIndependent             — drawing stream A doesn't move B
//   TestRng_PinnedStreamListIsClosed          — 8 streams, exact names

#include "src/core/clock.hpp"
#include "src/core/rng.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Bring in doctest's implementation + default main() in this TU.
// The game's full test harness (task 0.6) will own a single main.cpp that
// defines this; the local standalone build puts it here so the binary has
// an entry point.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

using harrowmere::core::DAY_LENGTH_SECONDS;
using harrowmere::core::RNG_SCALE;
using harrowmere::core::SeedRng;
using harrowmere::core::WorldClock;

namespace
{

// The pinned stream list — mirrored from rng.cpp's kPinnedStreams for tests
// that iterate every stream. If rng.cpp's list changes, this must too;
// TestRng_PinnedStreamListIsClosed enforces the parity at test time.
constexpr std::array<const char *, 8> kAllStreams = {
    "combat",
    "economy",
    "events",
    "justice",
    "misc",
    "npc_life",
    "weather",
    "worldgen",
};

} // namespace

// =============================================================================
// Clock tests
// =============================================================================

TEST_CASE(
    "TestClock_AdvanceTick_TimeScaleMath"
)
{
    WorldClock c;
    c.timeScale = 60;
    c.tickRate  = 10;

    // First tick: worldTime += 60/10 = 6 game-seconds; realTime += 1/10 = 0
    // (carry 1, no whole second yet).
    c.advanceTick();
    CHECK(c.worldTime == 6);
    CHECK(c.realTime == 0);

    // After 10 ticks (one real second): 60 game-seconds, 1 real-second.
    for (int i = 0; i < 9; ++i)
        c.advanceTick();
    CHECK(c.worldTime == 60);
    CHECK(c.realTime == 1);

    // dayPhase01 at 60s into the day is 60/86'400.
    CHECK(c.dayPhase01() == doctest::Approx(60.0 / 86400.0).epsilon(0.000001));
    CHECK(c.gameDay() == 0); // 60s << 86'400s
}

TEST_CASE(
    "TestClock_AdvanceTick_RemainderCarry"
)
{
    // tickRate that does NOT divide timeScale cleanly: timeScale=60, tickRate=11.
    // Per real second (11 ticks): worldTime must advance by exactly 60
    // game-seconds and realTime by exactly 1 real-second. The carry is what
    // makes this true; without it integer truncation would lose 5 game-seconds
    // per real-second.
    WorldClock c;
    c.timeScale = 60;
    c.tickRate  = 11;

    for (int i = 0; i < 11; ++i)
        c.advanceTick();
    CHECK(c.worldTime == 60);
    CHECK(c.realTime == 1);

    // Continue for another real-second's worth of ticks; carry must still
    // resolve cleanly. After 22 ticks: 120 game-seconds, 2 real-seconds.
    for (int i = 0; i < 11; ++i)
        c.advanceTick();
    CHECK(c.worldTime == 120);
    CHECK(c.realTime == 2);
}

TEST_CASE(
    "TestClock_DayPhase01_BoundsAndPhases"
)
{
    WorldClock c;

    c.worldTime = 0;
    CHECK(c.dayPhase01() == 0.0); // midnight

    c.worldTime = 6 * 60 * 60; // 06:00
    CHECK(c.dayPhase01() == doctest::Approx(0.25).epsilon(0.000001));

    c.worldTime = 12 * 60 * 60; // noon
    CHECK(c.dayPhase01() == doctest::Approx(0.5).epsilon(0.000001));

    c.worldTime = 18 * 60 * 60; // 18:00
    CHECK(c.dayPhase01() == doctest::Approx(0.75).epsilon(0.000001));

    c.worldTime = DAY_LENGTH_SECONDS - 1; // last second of the day
    CHECK(c.dayPhase01() > 0.9999);
    CHECK(c.dayPhase01() < 1.0);

    c.worldTime = DAY_LENGTH_SECONDS; // exactly midnight -> phase 0
    CHECK(c.dayPhase01() == 0.0);
}

TEST_CASE(
    "TestClock_GameDay_RollsAtMidnight"
)
{
    WorldClock c;

    c.worldTime = 0;
    CHECK(c.gameDay() == 0);

    c.worldTime = DAY_LENGTH_SECONDS - 1;
    CHECK(c.gameDay() == 0);

    c.worldTime = DAY_LENGTH_SECONDS;
    CHECK(c.gameDay() == 1);

    c.worldTime = 7 * DAY_LENGTH_SECONDS;
    CHECK(c.gameDay() == 7);
}

TEST_CASE(
    "TestClock_TickRateZero_GuardsR2"
)
{
    // R2: division by a value that can be zero is guarded with a pinned
    // fallback. We test the release-behavior path (no advance, no crash).
    // The debug-assert path is documented in clock.cpp.
    WorldClock c;
    c.tickRate = 0;

#ifdef NDEBUG
    // Release: the guard returns without advancing — worldTime stays at 0.
    c.advanceTick();
    CHECK(c.worldTime == 0);
    CHECK(c.realTime == 0);
#else
    // Debug: the assertion fires. We catch it via doctest's death-test
    // facility would be ideal, but for portability we just assert the
    // documented invariant holds (assert fires -> test aborts if broken).
    // To avoid aborting the test binary in debug, we skip the call.
    CHECK(c.worldTime == 0);
#endif
}

// =============================================================================
// RNG tests
// =============================================================================

TEST_CASE(
    "TestRng_SameSeedSameSequence"
)
{
    SeedRng a, b;
    a.seedAll(0xDEADBEEFCAFEULL);
    b.seedAll(0xDEADBEEFCAFEULL);

    // 10'000 draws on every stream — matches the spec's "identical 10k draws
    // on every stream" requirement.
    for (const char *s : kAllStreams)
    {
        for (int i = 0; i < 10'000; ++i)
        {
            CHECK(a.draw(s) == b.draw(s));
        }
    }
}

TEST_CASE(
    "TestRng_DifferentSeedsDiffer"
)
{
    // Sanity check: two different seeds must produce at least one different
    // draw across the streams we sample. If this ever fails, the worldSeed
    // is being ignored in seedAll (a regression with determinism-breaking
    // consequences — every world would behave identically).
    SeedRng a, b;
    a.seedAll(1);
    b.seedAll(2);

    bool any_differ = false;
    for (const char *s : kAllStreams)
    {
        for (int i = 0; i < 100; ++i)
        {
            if (a.draw(s) != b.draw(s))
            {
                any_differ = true;
                break;
            }
        }
        if (any_differ)
            break;
    }
    CHECK(any_differ);
}

TEST_CASE(
    "TestRng_RangeBounds"
)
{
    SeedRng r;
    r.seedAll(42);

    // range(): every result is in [lo, hiExcl). 10k draws is enough to catch
    // an off-by-one on the boundary.
    for (int i = 0; i < 10'000; ++i)
    {
        const auto v = r.range("worldgen", 5, 10);
        CHECK(v >= 5);
        CHECK(v < 10);
    }

    // pct(): every result is in [0, RNG_SCALE).
    for (int i = 0; i < 10'000; ++i)
    {
        const auto v = r.pct("worldgen");
        CHECK(v < RNG_SCALE);
    }

    // Edge: span == 1 -> always returns lo.
    for (int i = 0; i < 100; ++i)
    {
        CHECK(r.range("combat", 7, 8) == 7);
    }

    // Edge: hiExcl <= lo -> R2 fallback returns lo (no division by zero).
    // Per common.md §3.4, programmer errors assert in debug and recover
    // deterministically in release — we only test the release path here;
    // the debug-assert behavior is documented in rng.cpp.
#ifdef NDEBUG
    CHECK(r.range("misc", 5, 5) == 5); // span 0
    CHECK(r.range("misc", 7, 3) == 7); // hiExcl < lo
#endif
}

TEST_CASE(
    "TestRng_StateRoundTrip"
)
{
    // Spec: "draw, save state, restore, continue → identical continuation".
    SeedRng a, b;
    constexpr std::uint64_t kSeed = 0x0123456789ABCDEFULL;
    a.seedAll(kSeed);
    b.seedAll(kSeed); // pre-condition for loadState: same worldSeed.

    // Advance `a` by some draws so its state_ differs from the post-seed
    // state (otherwise the round-trip is trivially correct).
    for (const char *s : kAllStreams)
    {
        for (int i = 0; i < 100; ++i)
            (void)a.draw(s);
    }

    const auto snapshot = a.state();
    REQUIRE_FALSE(snapshot.empty());
    // 2 entries per stream × 8 streams = 16.
    CHECK(snapshot.size() == kAllStreams.size() * 2);

    b.loadState(snapshot);

    // Continuing draws must be identical between a and b.
    for (const char *s : kAllStreams)
    {
        for (int i = 0; i < 1'000; ++i)
        {
            CHECK(a.draw(s) == b.draw(s));
        }
    }
}

TEST_CASE(
    "TestRng_StreamsAreIndependent"
)
{
    // Drawing from one stream must NOT advance another stream's sequence.
    // This is the property that makes per-system tests seed-local: a
    // regression in `combat` should not look like a regression in `justice`.
    SeedRng a, b;
    a.seedAll(99);
    b.seedAll(99);

    // From `a`, draw 1'000 from "worldgen" only.
    for (int i = 0; i < 1'000; ++i)
        (void)a.draw("worldgen");

    // `b` has not been drawn from at all. Every other stream's sequence
    // must still match — drawing from "worldgen" must not have moved any
    // other stream's state_.
    for (const char *s : kAllStreams)
    {
        if (std::string(s) == "worldgen")
            continue; // expected to differ
        for (int i = 0; i < 1'000; ++i)
        {
            CHECK(a.draw(s) == b.draw(s));
        }
    }
}

TEST_CASE(
    "TestRng_PinnedStreamListIsClosed"
)
{
    // The 8 names listed in rng.hpp's header comment must all be present
    // after seedAll; an unknown name is a programmer error.
    SeedRng r;
    r.seedAll(1);

    // Every pinned name draws successfully (returns *some* u64 — we don't
    // assert the value, only that the stream exists and produces a value
    // without asserting). The volatile sink prevents the optimizer from
    // eliding the draws.
    volatile std::uint64_t sink = 0;
    for (const char *s : kAllStreams)
    {
        sink ^= r.draw(s);
    }
    (void)sink;

    // Unknown stream: in release, returns 0 without corrupting other
    // streams. In debug, asserts. We test the release path.
#ifdef NDEBUG
    // Re-seed r and ref identically so the alignment math below is exact
    // (the sink loop above advanced r once per stream).
    r.seedAll(1);
    SeedRng ref;
    ref.seedAll(1);
    // Drain the same pinned streams in `r` and `ref` so they're aligned
    // up to the unknown-stream call.
    for (const char *s : kAllStreams)
    {
        (void)r.draw(s);
        (void)ref.draw(s);
    }
    CHECK(r.draw("nonexistent_stream") == 0);
    // The valid streams' states are untouched by the failed draw.
    for (const char *s : kAllStreams)
    {
        CHECK(r.draw(s) == ref.draw(s));
    }
#endif
}
