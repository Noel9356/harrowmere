// tests/test_reputation.cpp — Task 4.4's pinned test suite.
//
// The card's Tests block (names verbatim — common.md §3.5):
//
//   * TestRep_DecaySignAware            (negative values decay UP toward zero)
//   * TestRep_PropagationHalvesPerLevel (village -1000 -> county -500 -> kingdom -250 ...)
//   * TestRep_ClampedAtMax
//   * TestRep_LazyDecayCatchUp          (10 idle days apply 10 steps exactly once)
//   * TestNotoriety_FloorsAtZero
//
// Plus the Done-when pins: no global periodic tick exists (the decay is lazy
// on every update/read) and all readers decay-before-read.
//
// Test harness: doctest v2.4.12 (vendored per task 0.6); no main here —
// tests/main.cpp owns DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.
#include <doctest/doctest.h>

#include <cstdint>

#include "core/constants.hpp" // A-84..A-88, SCALE
#include "justice/reputation.hpp"

#include "test_justice_fixture.hpp"

namespace
{
using namespace justice_test;
} // namespace

// ============================================================================
// Pinned test 1 — the sign-aware decay: negative values decay UP toward zero
// (the doc's `v -= sign(v) * min(|v|, DECAY * steps)` term), positives decay
// down, and a partial interval carries over.
// ============================================================================
TEST_CASE(
    "TestRep_DecaySignAware"
)
{
    justice::Reputation rep;
    rep.lastDecayTime = 0;

    // A hated name heals UP: -1000 after 2 full decay intervals lands at 0
    // (A-85: 500 per day, 2 steps = 1000 of healing, capped by the magnitude).
    rep.perTerritory[1] = -1000;
    justice::decayIfDue(rep, 2 * REPUTATION_DECAY_INTERVAL);
    CHECK(rep.perTerritory[1] == 0);

    // A beloved name heals DOWN, symmetrically: -800->+800 over 2 steps...
    rep.perTerritory[2] = 800;
    rep.lastDecayTime   = 0;
    justice::decayIfDue(rep, 2 * REPUTATION_DECAY_INTERVAL);
    CHECK(rep.perTerritory[2] == 0);

    // ...and a value short of the budget keeps its remainder sign.
    rep.perTerritory[3] = -300;
    rep.lastDecayTime   = 0;
    justice::decayIfDue(rep, REPUTATION_DECAY_INTERVAL); // one step: -300 + 500 -> 0 (clamped by |v|)
    CHECK(rep.perTerritory[3] == 0);

    rep.perTerritory[4] = -1200;
    rep.lastDecayTime   = 0;
    justice::decayIfDue(rep, REPUTATION_DECAY_INTERVAL); // one step: -1200 + 500 = -700 (still negative)
    CHECK(rep.perTerritory[4] == -700);

    // A partial interval applies nothing (the lazy quantum is whole days).
    rep.perTerritory[5] = -100;
    rep.lastDecayTime   = 0;
    justice::decayIfDue(rep, REPUTATION_DECAY_INTERVAL - 1);
    CHECK(rep.perTerritory[5] == -100);
    CHECK(rep.lastDecayTime == 0);
}

// ============================================================================
// Pinned test 2 — the propagation walk: the origin takes the delta at full
// weight, every ancestor at 0.5x per level (A-88). village -1000 -> county
// -500 -> kingdom -250 -> empire -125.
// ============================================================================
TEST_CASE(
    "TestRep_PropagationHalvesPerLevel"
)
{
    JusticeFixture f;
    const characterId c = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);

    justice::changeReputation(c, JusticeFixture::kVillageB, -1000, f.base.ctx);

    CHECK(f.store.reputations.at(c).perTerritory.at(JusticeFixture::kVillageB) == -1000);
    CHECK(f.store.reputations.at(c).perTerritory.at(JusticeFixture::kCounty) == -500);
    CHECK(f.store.reputations.at(c).perTerritory.at(JusticeFixture::kKingdom) == -250);
    CHECK(f.store.reputations.at(c).perTerritory.at(JusticeFixture::kEmpire) == -125);

    // The walk mutates EVERY ancestor INCLUDING the origin — and a second
    // crime at a sibling village stacks onto the shared chain (each level
    // takes the new delta at ITS OWN halved weight: county -500, kingdom
    // -250, empire -125).
    justice::changeReputation(c, JusticeFixture::kVillageA, -1000, f.base.ctx);
    CHECK(f.store.reputations.at(c).perTerritory.at(JusticeFixture::kVillageA) == -1000);
    CHECK(f.store.reputations.at(c).perTerritory.at(JusticeFixture::kCounty) == -1000); // -500 + -500
    CHECK(f.store.reputations.at(c).perTerritory.at(JusticeFixture::kKingdom) == -500); // -250 + -250
    CHECK(f.store.reputations.at(c).perTerritory.at(JusticeFixture::kEmpire) == -250);  // -125 + -125
}

// ============================================================================
// Pinned test 3 — the clamp: every level clamps to ±MAX_REPUTATION (A-87) on
// the i64 grid before storing back into the i32 map.
// ============================================================================
TEST_CASE(
    "TestRep_ClampedAtMax"
)
{
    JusticeFixture f;
    const characterId c = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);

    // A monstrous crime clamps at the origin; each ancestor clamps its own
    // halved sum (county -25000, kingdom -12500 — both over the bound).
    justice::changeReputation(c, JusticeFixture::kVillageB, -50'000, f.base.ctx);
    CHECK(f.store.reputations.at(c).perTerritory.at(JusticeFixture::kVillageB) == -MAX_REPUTATION);
    CHECK(f.store.reputations.at(c).perTerritory.at(JusticeFixture::kCounty) == -MAX_REPUTATION);
    CHECK(f.store.reputations.at(c).perTerritory.at(JusticeFixture::kKingdom) == -MAX_REPUTATION);
    CHECK(f.store.reputations.at(c).perTerritory.at(JusticeFixture::kEmpire) == -6250);

    // ...and sainthood clamps the same way, upward.
    justice::changeReputation(c, JusticeFixture::kVillageB, +50'000, f.base.ctx);
    CHECK(f.store.reputations.at(c).perTerritory.at(JusticeFixture::kVillageB) == MAX_REPUTATION);

    // The clamped level stays put: another push cannot exceed the bound.
    justice::changeReputation(c, JusticeFixture::kVillageB, +5'000, f.base.ctx);
    CHECK(f.store.reputations.at(c).perTerritory.at(JusticeFixture::kVillageB) == MAX_REPUTATION);
}

// ============================================================================
// Pinned test 4 — the lazy catch-up: 10 idle days apply exactly 10 decay
// steps on the FIRST read, and the second read applies nothing new.
// ============================================================================
TEST_CASE(
    "TestRep_LazyDecayCatchUp"
)
{
    JusticeFixture f;
    const characterId c = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    justice::changeReputation(c, JusticeFixture::kVillageB, -10'000, f.base.ctx);
    REQUIRE(f.store.reputations.at(c).perTerritory.at(JusticeFixture::kVillageB) == -MAX_REPUTATION);

    // 10 game-days pass with no sim tick touching the record — the READ does
    // the catch-up (the doc: "lazy decay replaces global ticks").
    const std::uint64_t now = 10 * REPUTATION_DECAY_INTERVAL;
    const std::int32_t read = justice::getReputationIn(f.store, c, JusticeFixture::kVillageB, now);
    CHECK(read == -10'000 + 10 * static_cast<std::int32_t>(REPUTATION_DECAY)); // -10000 + 5000 = -5000

    // The stamp advanced by exactly the applied steps: an immediate second
    // read applies nothing (no double decay — the reader-decays rule).
    CHECK(justice::getReputationIn(f.store, c, JusticeFixture::kVillageB, now) == read);
    CHECK(f.store.reputations.at(c).lastDecayTime == now);
}

// ============================================================================
// Pinned test 5 — notoriety floors at zero (A-86) and the readers decay
// before they answer.
// ============================================================================
TEST_CASE(
    "TestNotoriety_FloorsAtZero"
)
{
    JusticeFixture f;
    const characterId c = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);

    justice::addNotoriety(f.store, c, 500);
    CHECK(justice::getNotoriety(f.store, c, 0) == 500);

    // Two decay steps of A-86 (1000/day) against a 500 pool: the floor holds.
    CHECK(justice::getNotoriety(f.store, c, 2 * REPUTATION_DECAY_INTERVAL) == 0);

    // The writer only adds; the decay belongs to the readers. Adding again
    // after the decayed read starts from the CURRENT record.
    justice::addNotoriety(f.store, c, 1500);
    CHECK(f.store.reputations.at(c).notoriety == 1500);
    CHECK(justice::getNotoriety(f.store, c, 2 * REPUTATION_DECAY_INTERVAL + REPUTATION_DECAY_INTERVAL) == 500);

    // D-REP-2: a pariah's multiplier grows; a neutral name reads exactly
    // SCALE (the conviction math's neutral case).
    justice::changeReputation(c, JusticeFixture::kVillageB, -2000, f.base.ctx);
    const std::uint64_t multiplier = justice::getReputationMultiplier(
        f.store, c, JusticeFixture::kVillageB, 2 * REPUTATION_DECAY_INTERVAL + REPUTATION_DECAY_INTERVAL
    );
    CHECK(multiplier == static_cast<std::uint64_t>(SCALE) + 400); // max(0, 2000) * 2000 / 10000
}
