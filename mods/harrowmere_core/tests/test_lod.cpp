// tests/test_lod.cpp — Task 3.5 pinned tests.
// TestLod_HysteresisEnterNearBelowNearTimesMargin, TestLod_HysteresisExitsNearAboveNearOverMargin,
// TestLod_NoFlickerUnderJitter, TestLod_FarUntouchedPerTick.
//
// The margin comparisons are asymmetric on purpose (A-45/A-46/A-47): ENTER NEAR below
// NEAR x MARGIN (62.5 m), LEAVE NEAR above NEAR / MARGIN (40 m). Both directional
// tests pin both comparisons at the card's samples (62.5/60 and 40/50 m).
//
// NOTE for the lead (full analysis in MERGE_NOTES.md): in the design-reference block
// as written, the MID-entry branch also catches NEAR-tier NPCs at any distance below
// MID x MARGIN, so a NEAR NPC re-evaluates to MID whenever the leave comparison does
// not fire. The jitter test therefore pins its "at most one change" placement inside
// the bands that are stable for the shipped block AND for the candidate corrected
// block, and the deviation-protocol flag is raised rather than silently patched.
#include <doctest.h> // the single implement define lives in main.cpp (task 0.6)

#include <cmath>

#include "character/character.hpp"
#include "core/constants_phase2.hpp"
#include "npc/lifecycle.hpp"
#include "npc/lod.hpp"
#include "test_phase2_fixture.hpp"

namespace
{
// phase2_test already carries `using namespace harrowmere;` (fixture header):
// a second using-directive here makes the qualified name `harrowmere::`
// ambiguous for the TU (transitive lookup reaches the namespace by two paths).
using namespace phase2_test;
} // namespace

TEST_CASE(
    "TestLod_HysteresisEnterNearBelowNearTimesMargin"
)
{
    NpcState npc;
    npc.tier = LodTier::FAR;

    // 60 m < 50 * 1.25 = 62.5 -> NEAR (the card's pinned sample).
    updateNpcSimulation(npc, 60.0);
    CHECK(npc.tier == LodTier::NEAR);

    // ...and the entry comparison is strict: at exactly NEAR x MARGIN the NPC is not
    // NEAR yet (the block's next branch places it MID).
    NpcState atThreshold;
    atThreshold.tier = LodTier::FAR;
    updateNpcSimulation(atThreshold, NEAR_DISTANCE_M * TIER_MARGIN);
    CHECK(atThreshold.tier == LodTier::MID);
}

TEST_CASE(
    "TestLod_HysteresisExitsNearAboveNearOverMargin"
)
{
    NpcState npc;
    npc.tier = LodTier::NEAR;

    // 50 m > 50 / 1.25 = 40 -> MID (the card's pinned sample).
    updateNpcSimulation(npc, 50.0);
    CHECK(npc.tier == LodTier::MID);

    // Documented verbatim-block behavior (flagged in MERGE_NOTES.md, deviation
    // protocol): the MID-entry branch's tier guard is `!= MID`, so it also catches
    // NEAR-tier NPCs — a NEAR evaluation below MID x MARGIN that does not leave via
    // the 40 m comparison re-tiers to MID. Pinned here so any future correction of
    // the block is a conscious, reviewed change, not an accident.
    NpcState docBehavior;
    docBehavior.tier = LodTier::NEAR;
    updateNpcSimulation(docBehavior, 30.0);
    CHECK(docBehavior.tier == LodTier::MID);
}

TEST_CASE(
    "TestLod_NoFlickerUnderJitter"
)
{
    // ±2 m of distance noise must produce at most ONE tier change in 100 samples.
    // Placements sit in bands that are stable under the shipped block (and under the
    // candidate corrected block — see the header note), so the noise cannot churn the
    // active set.
    auto countChanges = [](LodTier start, double base) {
        NpcState npc;
        npc.tier    = start;
        int changes = 0;
        for (int i = 0; i < 100; ++i)
        {
            const double noise   = (i % 2 == 0) ? -2.0 : 2.0; // deterministic ±2 m
            const LodTier before = npc.tier;
            updateNpcSimulation(npc, base + noise);
            if (npc.tier != before)
                ++changes;
        }
        return changes;
    };

    // Deep-FAR: 298..302 m is outside every enter threshold — zero changes.
    CHECK(countChanges(LodTier::FAR, 300.0) == 0);
    // Mid band: 98..102 m sits between the NEAR-enter (62.5) and FAR-leave (160)
    // comparisons — a latched MID (or a NEAR passing through) settles at once.
    CHECK(countChanges(LodTier::MID, 100.0) == 0);
    CHECK(countChanges(LodTier::NEAR, 100.0) <= 1);
}

TEST_CASE(
    "TestLod_FarUntouchedPerTick"
)
{
    Fixture f;
    // Territory center far from the beginner spawn (5,0,5) so its resident sits
    // genuinely outside the MID band of the fallback anchor.
    f.addTerritory(1, 1000.0, 1000.0, 1, false);
    const characterId farNpc = spawnNpc(f, 1, 1, 1, 30);
    f.npcs.get(farNpc).tier  = LodTier::FAR;

    // No player in-world: the anchor is the beginner spawn (D-LOD-1) and the FAR NPC
    // is outside its NEAR/MID bands — the per-tick NPC work must never touch it.
    CHECK(f.world.playerId == 0);
    f.ctx.lod->farTouches = 0;
    npcPerTickWork(f.ctx);
    CHECK(f.ctx.lod->farTouches == 0);
    CHECK(f.characters.get(farNpc).body.position.x == f.world.settlementCenter(1).x);
    CHECK(f.npcs.get(farNpc).needs.hunger == SCALE); // needs untouched per tick, too

    // The step-3 system likewise: the FAR NPC is tiered but never listed as active.
    struct SingleSource final : public ILodSpatialSource
    {
        characterId id = 0;
        void forEachCharacterInRadius(
            const Position &, double, const std::function<void(characterId)> &fn
        ) const override
        {
            if (id != 0)
                fn(id);
        }
    } source;
    source.id = farNpc;
    LodSystem lodSystem(source);
    lodSystem.run(f.ctx);
    CHECK(f.ctx.lod->activeNear.empty());
    CHECK(f.ctx.lod->activeMid.empty());
}
