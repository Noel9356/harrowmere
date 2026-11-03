// test_matchmaking.cpp — task 6.5's pinned tests: the closest-score duel
// proposal (NEAR+MID only, ties to the lower id), the A-116 acceptance math,
// the two anti-abuse regressions (the peak watermark defeats sandbagging;
// the potential is tamper-proof because totalXp never decreases) and the
// event-difficulty seeding clamp — both the pure math and the live engine
// join through the IMatchmakingSink seam.
#include <doctest.h>

#include <cstdint>

#include "character/character.hpp"
#include "core/constants.hpp"
#include "core/math.hpp"
#include "events/engine.hpp"
#include "events/template.hpp"
#include "npc/state.hpp"
#include "progression/matchmaking.hpp"
#include "progression/power.hpp"

#include "test_justice_fixture.hpp"

namespace
{

using namespace harrowmere;
using namespace justice_test;
namespace ev = harrowmere::events;
// The fixture's pinned hierarchy ids (static members; local aliases keep the
// helpers readable).
constexpr territoryId kKingdom  = JusticeFixture::kKingdom;
constexpr territoryId kCounty   = JusticeFixture::kCounty;
constexpr territoryId kVillageA = JusticeFixture::kVillageA;
constexpr regionId kRegionA     = JusticeFixture::kRegionA;

// The rig: the justice fixture (full stores + a real engine-ready context)
// with the real matchmaking sink bound (the plugin's swap, headless).
struct MatchmakingRig
{
    JusticeFixture f;
    progression::MatchmakingSystem matchmaking;

    MatchmakingRig()
    {
        f.base.ctx.matchmaking = &matchmaking;
        matchmaking.bind(&f.base.ctx);
    }

    // One duelist with hand-set score parts (peakPower is the score's 8/10
    // weight, totalXp the 2/10 — the pipeline's own tests pin the formula;
    // these tests exercise the CONSUMERS, not the blend).
    characterId spawnDuelist(
        std::uint64_t peakPower, std::uint64_t totalXp, LodTier tier
    )
    {
        const characterId id
            = phase2_test::spawnNpc(f.base, phase2_test::speciesIdByKey("human"), kVillageA, kRegionA, 30);
        Character &c             = f.base.characters.get(id);
        c.peakPower              = peakPower;
        c.cachedPower            = peakPower;
        c.powerDirty             = false;
        c.progression.totalXp    = totalXp;
        f.base.npcs.get(id).tier = tier;
        return id;
    }

    characterId spawnPlayer()
    {
        const characterId id  = f.base.characters.createPlayer(PlayerCreation{"scorer", {}, ""});
        f.base.world.playerId = id;
        return id;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// TestMatchmaking_ClosestScoreProposed (card 6.5, pinned name): the proposal
// is the NEAR+MID NPC with the closest score; ties -> lower id; FAR and
// non-candidates never propose.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestMatchmaking_ClosestScoreProposed"
)
{
    MatchmakingRig rig;
    const characterId player = rig.spawnPlayer();

    // Scores (8:2 blend, all from peakPower here): player 1'000'000.
    {
        Character &p  = rig.f.base.characters.get(player);
        p.peakPower   = 1'250'000;
        p.cachedPower = p.peakPower;
        p.powerDirty  = false;
    }
    // far_away: score 500'000, delta 500'000 — but FAR tier (never proposes).
    const characterId farAway = rig.spawnDuelist(625'000, 0, LodTier::FAR);
    // weak: score 600'000, delta 400'000 — NEAR.
    const characterId weak = rig.spawnDuelist(750'000, 0, LodTier::NEAR);
    // close: score 960'000, delta 40'000 — MID (the closest candidate).
    const characterId close = rig.spawnDuelist(1'200'000, 0, LodTier::MID);

    CHECK(progression::proposeDuelOpponent(player, rig.f.base.ctx) == close);
    (void)farAway;
    (void)weak;

    // The tie rule: a second NPC at exactly the close score loses to the
    // lower id (the scan is id-ascending and the strict < keeps the first).
    const characterId tie = rig.spawnDuelist(1'200'000, 0, LodTier::NEAR);
    CHECK(progression::proposeDuelOpponent(player, rig.f.base.ctx) == close);

    // The child gate: a NON-adult candidate is never proposed even at a
    // perfect score.
    const characterId child
        = phase2_test::spawnNpc(rig.f.base, phase2_test::speciesIdByKey("human"), kVillageA, kRegionA, 8);
    {
        Character &c  = rig.f.base.characters.get(child);
        c.peakPower   = 1'200'000;
        c.cachedPower = c.peakPower;
        c.powerDirty  = false;
    }
    rig.f.base.npcs.get(child).tier = LodTier::NEAR;
    CHECK(progression::proposeDuelOpponent(player, rig.f.base.ctx) == close);
    (void)tie;

    // No candidates at all: the proposal is 0 (the caller rejects
    // deterministically).
    MatchmakingRig empty;
    const characterId lonely = empty.spawnPlayer();
    CHECK(progression::proposeDuelOpponent(lonely, empty.f.base.ctx) == 0);
}

// ---------------------------------------------------------------------------
// TestMatchmaking_PeakWatermarkPreventsSandbagging (card 6.5, pinned name):
// unequipping gear cannot drop the score — the peak watermark persists and
// the score reads the watermark, never the weakened current power.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestMatchmaking_PeakWatermarkPreventsSandbagging"
)
{
    JusticeFixture f;
    const characterId player = f.base.characters.createPlayer(PlayerCreation{"sandbagger", {}, ""});
    Character &p             = f.base.characters.get(player);

    // Strong today: STRENGTH 100 computes a real power through the pipeline.
    p.progression.stats[static_cast<std::size_t>(StatType::STRENGTH)] = 100;
    const std::uint64_t strongPower = getCurrentPower(p, phase2_test::shippedRegistry(), f.base.itemInstances);
    CHECK(p.peakPower == strongPower);
    const std::uint64_t strongScore = getMatchmakingScore(p, phase2_test::shippedRegistry());

    // Sandbagging tonight: the gear "unequipped" (the stats dropped to the
    // floor). The current power falls; the watermark does not.
    p.progression.stats[static_cast<std::size_t>(StatType::STRENGTH)] = 0;
    p.powerDirty                                                      = true;
    const std::uint64_t weakPower = getCurrentPower(p, phase2_test::shippedRegistry(), f.base.itemInstances);
    CHECK(weakPower < strongPower);
    CHECK(p.peakPower == strongPower);                                            // the watermark never decreases
    CHECK(getMatchmakingScore(p, phase2_test::shippedRegistry()) == strongScore); // the score reads the peak
}

// ---------------------------------------------------------------------------
// TestMatchmaking_PotentialPowerMonotonic (card 6.5, pinned name): the
// potential is tamper-proof — totalXp never decreases (R4), so neither does
// the isqrt potential, whatever the XP debt does to availableXp.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestMatchmaking_PotentialPowerMonotonic"
)
{
    JusticeFixture f;
    const characterId id = f.base.characters.createPlayer(PlayerCreation{"debtor", {}, ""});
    Character &c         = f.base.characters.get(id);

    c.progression.totalXp           = 1'000'000;
    c.progression.availableXp       = 1'000'000;
    const std::uint64_t before      = getPotentialPower(c);
    const std::uint64_t scoreBefore = getMatchmakingScore(c, phase2_test::shippedRegistry());

    // The conviction debt: availableXp plunges (R4's floor), totalXp stands.
    c.progression.availableXp = applyXpDelta(c.progression.availableXp, -900'000);
    CHECK(c.progression.availableXp == 100'000);
    CHECK(getPotentialPower(c) == before);
    CHECK(getMatchmakingScore(c, phase2_test::shippedRegistry()) == scoreBefore);

    // Spending XP never rewinds the lifetime total either.
    c.progression.availableXp = applyXpDelta(c.progression.availableXp, -100'000);
    CHECK(getPotentialPower(c) == before);
    CHECK(getMatchmakingScore(c, phase2_test::shippedRegistry()) == scoreBefore);
}

// ---------------------------------------------------------------------------
// TestMatchmaking_EventSeedingClamped (card 6.5, pinned name): the seeding
// math clamps to the template's [0.5x, 2x] band, and the live engine join
// seeds through the IMatchmakingSink seam exactly once (the first, all-NPC
// join).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestMatchmaking_EventSeedingClamped"
)
{
    // ---- the pure math -------------------------------------------------------
    // clamp(meanScore / 1000, tpl/2, tpl*2):
    CHECK(progression::calibratedEventDifficulty(4'000'000, 5000) == 4000);    // inside the band
    CHECK(progression::calibratedEventDifficulty(400'000'000, 5000) == 10000); // clamps at 2x
    CHECK(progression::calibratedEventDifficulty(40'000, 5000) == 2500);       // clamps at 0.5x
    CHECK(progression::calibratedEventDifficulty(4'000'000, 0) == 0);          // a zero template keeps its zero band

    // ---- the live join through the seam ---------------------------------------
    MatchmakingRig rig;
    ev::EventTemplateTable table;
    ev::EventEngine engine;
    engine.bind(&table);
    engine.bindContext(&rig.f.base.ctx);

    ev::EventTemplate tpl;
    tpl.id                 = 900;
    tpl.key                = "seeding_test";
    tpl.global             = false;
    tpl.initialStateId     = 1;
    tpl.initialDifficulty  = 5000;
    tpl.initialSuccessRate = 5000;
    tpl.maxDurationDays    = 30;
    ev::TemplateState a;
    a.stateId = 1;
    ev::Transition toB;
    toB.to = 2;
    ev::Condition timer;
    timer.kind = ev::ConditionKind::Timer;
    timer.days = 30;
    toB.conditions.push_back(timer);
    a.transitions.push_back(toB);
    ev::TemplateState b;
    b.stateId = 2;
    ev::Action end;
    end.kind    = ev::ActionKind::EndEvent;
    end.success = true;
    b.entryActions.push_back(end);
    tpl.states.push_back(a);
    tpl.states.push_back(b);
    table.insert(tpl);

    // The all-NPC first join seeds the difficulty from the participant mean:
    // peakPower 5'000'000 (totalXp 0) -> score 4'000'000 -> seed 4000.
    const characterId npc       = rig.spawnDuelist(5'000'000, 0, LodTier::NEAR);
    const std::uint64_t eventId = engine.startEventByKey("seeding_test", kRegionA, 0, nullptr, rig.f.base.ctx);
    REQUIRE(eventId != 0);
    CHECK(engine.find(eventId)->currentState.difficulty == 5000); // before the join: the template stands
    engine.join(npc, eventId, rig.f.base.ctx);
    CHECK(engine.find(eventId)->currentState.difficulty == 4000); // seeded through the seam

    // A player's first join never seeds (the all-NPC condition fails the
    // moment a participant is a player).
    MatchmakingRig rig2;
    ev::EventTemplateTable table2;
    ev::EventEngine engine2;
    engine2.bind(&table2);
    engine2.bindContext(&rig2.f.base.ctx);
    table2.insert(tpl);
    const characterId human    = rig2.spawnPlayer();
    const std::uint64_t event2 = engine2.startEventByKey("seeding_test", kRegionA, 0, nullptr, rig2.f.base.ctx);
    REQUIRE(event2 != 0);
    engine2.join(human, event2, rig2.f.base.ctx);
    CHECK(engine2.find(event2)->currentState.difficulty == 5000); // the template stands

    // The null sink keeps the template difficulty (D-SINK-1's vocabulary).
    MatchmakingRig rig3;
    rig3.f.base.ctx.matchmaking = nullptr; // the pre-binding state
    ev::EventTemplateTable table3;
    ev::EventEngine engine3;
    engine3.bind(&table3);
    engine3.bindContext(&rig3.f.base.ctx);
    table3.insert(tpl);
    const characterId npc3     = rig3.spawnDuelist(5'000'000, 0, LodTier::NEAR);
    const std::uint64_t event3 = engine3.startEventByKey("seeding_test", kRegionA, 0, nullptr, rig3.f.base.ctx);
    REQUIRE(event3 != 0);
    engine3.join(npc3, event3, rig3.f.base.ctx);
    CHECK(engine3.find(event3)->currentState.difficulty == 5000);
}

// ---------------------------------------------------------------------------
// The A-116 acceptance math (the card's consumer 1 tail; the formula row is
// normative — the falloff, floor and ceiling are the pinned values).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestMatchmaking_AcceptanceFormulaAndRoll"
)
{
    // clamp(SCALE − |Δscore|/10, 1000, 9000):
    CHECK(progression::duelAcceptancePct(0) == 9000);      // SCALE clamps at the ceiling
    CHECK(progression::duelAcceptancePct(10'000) == 9000); // 10000 − 1000
    CHECK(progression::duelAcceptancePct(20'000) == 8000);
    CHECK(progression::duelAcceptancePct(80'000) == 2000);
    CHECK(progression::duelAcceptancePct(200'000) == 1000);   // the floor
    CHECK(progression::duelAcceptancePct(1'000'000) == 1000); // never below

    // The roll: one draw from the pinned "npc_life" stream (R6 — the same
    // seeded stream answers the same probability).
    MatchmakingRig rig;
    rig.f.base.rng.seedAll(7);
    const std::uint32_t sure = progression::duelAcceptancePct(0); // 9000/10000 — near-certain
    std::uint32_t accepted   = 0;
    for (int i = 0; i < 1000; ++i)
    {
        accepted += progression::npcAcceptsDuel(sure, rig.f.base.ctx) ? 1 : 0;
    }
    CHECK(accepted >= 850); // a 90% roll over 1000 draws: the band is generous, never exact (R6)

    // The volunteer join weight: proximity-weighted, [10%, 100%] of base.
    CHECK(progression::hunterJoinWeight(1'000'000, 1'000'000, 100) == 100); // identical: full weight
    CHECK(progression::hunterJoinWeight(0, 80'000, 100) == 20);             // the A-116 falloff at 20%
    CHECK(progression::hunterJoinWeight(0, 2'000'000, 100) == 10);          // the floor
}
