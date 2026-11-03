// test_suspicion.cpp — task 4.5's pinned tests: the memory (cap/eviction),
// the ladder, the (act, context) evaluation, the darkvision gate, the AFK
// exemption, the never-evidence hard rule, and the decay/relief rules.
#include <doctest.h>

#include <cstdint>
#include <vector>

#include "core/constants.hpp"
#include "core/constants_phase2.hpp"
#include "core/enums.hpp"
#include "justice/citizen_justice.hpp" // npcMayViolate (the never-violence pin)
#include "justice/suspicion.hpp"
#include "npc/state.hpp"
#include "player/controller.hpp"

#include "test_justice_fixture.hpp"

namespace
{

using namespace harrowmere;
using namespace justice_test;

// A fresh observer NPC at villageB's anchor (an adult with a role, so the
// weight paths have an honest role row to read).
struct SuspicionRig
{
    JusticeFixture f;
    player::PlayerActions acts;
    characterId observer = 0;
    characterId player   = 0;

    SuspicionRig()
    {
        // The fixture's territories carry no region anchor (the case tests
        // read through runCaseTicks' own path); the suspicion evaluation
        // derives the settlement center from the region, so wire them.
        f.world.territory(JusticeFixture::kVillageB)->region = JusticeFixture::kRegionB;
        f.world.territory(JusticeFixture::kVillageA)->region = JusticeFixture::kRegionA;

        observer = f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "farmer", 8000);
        player   = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);

        f.base.ctx.playerActions = &acts;
        f.base.ctx.now           = core::DAY_LENGTH_SECONDS / 2; // noon — the NIGHT bit is off
        // The observer sees the player (same anchor; the grid has both).
        f.base.lod.activeNear.push_back(observer);
    }

    NpcState &observerState()
    {
        return f.base.npcs.get(observer);
    }
};

} // namespace

TEST_CASE(
    "TestSuspicion_MaskedInMarketAccumulates"
)
{
    SuspicionRig rig;
    rig.acts.masked = true; // the MASKED act (row: SETTLEMENT+MARKET, weight 5000)

    std::vector<SuspicionAct> scratch;
    justice::evaluateObserver(rig.f.base.ctx, rig.observer, WITNESS_RADIUS_M, scratch);

    const justice::SuspicionMemoryEntry entry = justice::memoryEntry(rig.observerState(), rig.player);
    // The player stands at the settlement anchor: SETTLEMENT+MARKET both
    // hold, the MASKED row fires once, and 5000 lands the DISTRUSTED rung.
    CHECK(entry.score == 5000);
    CHECK(justice::tierFor(entry.score) == TrustTier::DISTRUSTED);
}

TEST_CASE(
    "TestSuspicion_UnlitOnlyForNoDarkvision"
)
{
    SuspicionRig rig;
    rig.acts.torchLit = false; // UNLIT (row: NIGHT+SETTLEMENT, weight 2500)

    // Night in the settlement: the human-like player (species 0, no
    // darkvision) draws the UNLIT row.
    rig.f.base.ctx.now = core::DAY_LENGTH_SECONDS + 20 * 3600; // day 2, 20:00
    std::vector<SuspicionAct> scratch;
    justice::evaluateObserver(rig.f.base.ctx, rig.observer, WITNESS_RADIUS_M, scratch);
    CHECK(justice::memoryEntry(rig.observerState(), rig.player).score == 2500);

    // The darkvision gate: the observed's species row decides. A beastkin
    // observed (darkvision true, species.json) exposes UNLIT to nobody.
    Character &player               = rig.f.base.characters.get(rig.player);
    const speciesId humanSpecies    = player.species;
    const speciesId beastkinSpecies = phase2_test::speciesIdByKey("beastkin");
    player.species                  = beastkinSpecies;
    scratch.clear();
    justice::evaluateObserver(rig.f.base.ctx, rig.observer, WITNESS_RADIUS_M, scratch);
    CHECK(justice::memoryEntry(rig.observerState(), rig.player).score == 2500); // no second grant

    // The gate reopens when the species lacks darkvision again.
    player.species = humanSpecies;
    scratch.clear();
    justice::evaluateObserver(rig.f.base.ctx, rig.observer, WITNESS_RADIUS_M, scratch);
    CHECK(justice::memoryEntry(rig.observerState(), rig.player).score == 5000);
}

TEST_CASE(
    "TestSuspicion_AfkExempt"
)
{
    SuspicionRig rig;
    rig.acts.crouched = true; // the CROUCH act (row: SETTLEMENT+MARKET, weight 2000)
    rig.acts.afk      = true; // D-AFK-1: crouch held while AFK is an artifact

    std::vector<SuspicionAct> scratch;
    justice::evaluateObserver(rig.f.base.ctx, rig.observer, WITNESS_RADIUS_M, scratch);
    CHECK(justice::memoryEntry(rig.observerState(), rig.player).score == 0);

    // Presence resumes: the same crouch now draws the row.
    rig.acts.afk = false;
    justice::evaluateObserver(rig.f.base.ctx, rig.observer, WITNESS_RADIUS_M, scratch);
    CHECK(justice::memoryEntry(rig.observerState(), rig.player).score == 2000);
    CHECK(justice::tierFor(2000) == TrustTier::WATCHED);
}

TEST_CASE(
    "TestSuspicion_MemoryCapEvictsLowestTierStrangerFirst"
)
{
    SuspicionRig rig;
    NpcState &observer = rig.observerState();

    // Fill the memory to the cap with strangers of DESCENDING scores — the
    // weakest row (the last, score 1) is the eviction candidate.
    for (std::uint32_t i = 0; i < MEMORY_CAP; ++i)
    {
        justice::addSuspicion(observer, 1000 + i, 9000 - i * 10, rig.f.base.ctx.now);
    }
    CHECK(observer.trust.perCharacterScore.size() == MEMORY_CAP);

    // One more stranger with a mid score: the memory stays capped and the
    // LOWEST-tier entry (id 1000 + 15, score 9000 - 150) is gone — the
    // lowest tier goes first, never the newcomer by insertion order.
    justice::addSuspicion(observer, 9999, 4000, rig.f.base.ctx.now);
    CHECK(observer.trust.perCharacterScore.size() == MEMORY_CAP);
    CHECK(observer.trust.perCharacterScore.count(1000 + (MEMORY_CAP - 1)) == 0);
    CHECK(observer.trust.perCharacterScore.count(9999) == 1);
    CHECK(observer.trust.perCharacterScore.count(1000) == 1); // the strongest stays
}

TEST_CASE(
    "TestSuspicion_AcquaintanceOutranksStrangerUnderCap"
)
{
    SuspicionRig rig;
    NpcState &observer = rig.observerState();

    // An acquaintance (affinity >= A-54) with the weakest score in memory.
    const characterId friendId = 4242;
    npc::addAffinityEvent(observer, friendId, ACQUAINTANCE_AFFINITY);
    justice::addSuspicion(observer, friendId, 1, rig.f.base.ctx.now);
    for (std::uint32_t i = 1; i < MEMORY_CAP; ++i)
    {
        justice::addSuspicion(observer, 1000 + i, 5000, rig.f.base.ctx.now);
    }
    CHECK(observer.trust.perCharacterScore.size() == MEMORY_CAP);

    // A stranger arrives at the cap: the STRANGER entries outrank the
    // acquaintance for eviction — the weakest stranger goes, the
    // acquaintance survives with its score-1 memory.
    justice::addSuspicion(observer, 9999, 5000, rig.f.base.ctx.now);
    CHECK(observer.trust.perCharacterScore.size() == MEMORY_CAP);
    CHECK(observer.trust.perCharacterScore.count(friendId) == 1);
    CHECK(observer.trust.perCharacterScore.at(friendId) == 1);
    CHECK(observer.trust.perCharacterScore.count(9999) == 1);     // the newcomer
    CHECK(observer.trust.perCharacterScore.count(1000 + 1) == 0); // a stranger left
}

TEST_CASE(
    "TestSuspicion_NeverCreatesEvidence"
)
{
    SuspicionRig rig;
    // Mass-suspicious behavior: every act, repeatedly, across observers.
    rig.acts.masked   = true;
    rig.acts.crouched = true;
    std::vector<SuspicionAct> scratch;
    for (int pass = 0; pass < 10; ++pass)
    {
        justice::evaluateObserver(rig.f.base.ctx, rig.observer, WITNESS_RADIUS_M, scratch);
        rig.f.base.ctx.now += GAME_HOUR_SECONDS;
    }
    // The doc's hard rule: suspicion NEVER generates evidence.
    CHECK(rig.f.store.evidenceStore.empty());
    // ...and never justifies violence (the gate reads nowhere here — the
    // tier is SHUNNED by now and the gate function still answers from its
    // own three arms only).
    CHECK(justice::tierOf(rig.observerState(), rig.player) == TrustTier::SHUNNED);
    CHECK_FALSE(justice::npcMayViolate(rig.observer, rig.player, rig.f.base.ctx));
}

TEST_CASE(
    "TestSuspicion_DecayAndDonations"
)
{
    SuspicionRig rig;
    NpcState &observer = rig.observerState();

    // A watched stranger decays 100/game-day toward zero (A-91): one day
    // already slips UNDER the WATCHED rung (the A-90 ladder reads >= 2000 —
    // the score is the pinned fact, the tier follows the ladder).
    justice::addSuspicion(observer, rig.player, 2000, rig.f.base.ctx.now);
    justice::decaySuspicion(observer, rig.f.base.ctx.now + core::DAY_LENGTH_SECONDS);
    CHECK(justice::memoryEntry(observer, rig.player).score == 1900);
    CHECK(justice::tierOf(observer, rig.player) == TrustTier::STRANGER);

    // The donation relief (A-92): one act of service is ~10 days of decay.
    justice::relieveSuspicion(observer, rig.player);
    CHECK(justice::memoryEntry(observer, rig.player).score == 900);

    // The stranger memory lapses after STRANGER_MEMORY_S (A-56) out of
    // sight once the score faded to zero; the lazy quantum applies the whole
    // stale span at once — the decay stamp already consumed 12 days of the
    // 45, so the remaining 33 steps land here (the 4.4 lazy-decay
    // bookkeeping).
    justice::decaySuspicion(observer, rig.f.base.ctx.now + core::DAY_LENGTH_SECONDS * 45);
    CHECK(justice::memoryEntry(observer, rig.player).score == 0);
    CHECK(observer.trust.perCharacterScore.count(rig.player) == 0); // lapsed

    // The acquaintance memory is permanent: the same lapse window keeps it.
    const characterId friendId = 4242;
    npc::addAffinityEvent(observer, friendId, ACQUAINTANCE_AFFINITY);
    justice::addSuspicion(observer, friendId, 3000, rig.f.base.ctx.now);
    justice::decaySuspicion(observer, rig.f.base.ctx.now + core::DAY_LENGTH_SECONDS * 75);
    CHECK(observer.trust.perCharacterScore.count(friendId) == 1); // never lapses
    CHECK(justice::memoryEntry(observer, friendId).score == 0);   // but decays to zero
}

TEST_CASE(
    "TestSuspicion_SellReceiptTierModifier"
)
{
    SuspicionRig rig;
    // A vendor resident judges the seller through its own memory (A-93).
    const characterId vendor = rig.f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "vendor", 8000);
    NpcState &vendorState    = rig.f.base.npcs.get(vendor);

    CHECK(justice::sellReceiptModifierFor(rig.f.base.ctx, rig.player, JusticeFixture::kVillageB) == SCALE);

    justice::addSuspicion(vendorState, rig.player, TRUST_TIER_WATCHED, rig.f.base.ctx.now);
    CHECK(
        justice::sellReceiptModifierFor(rig.f.base.ctx, rig.player, JusticeFixture::kVillageB)
        == TIER_RECEIPT_MODIFIER_WATCHED
    );

    justice::addSuspicion(vendorState, rig.player, TRUST_TIER_SHUNNED - TRUST_TIER_WATCHED, rig.f.base.ctx.now);
    CHECK(
        justice::sellReceiptModifierFor(rig.f.base.ctx, rig.player, JusticeFixture::kVillageB)
        == TIER_RECEIPT_MODIFIER_SHUNNED
    );
}

TEST_CASE(
    "TestSuspicion_AppearanceSignalWeightsOnce"
)
{
    SuspicionRig rig;
    // The 2.6 seam's A-94 weight lands on the NEAR observers' memories.
    justice::applyAppearanceSignal(rig.f.base.ctx, rig.observer, rig.player, rig.f.base.ctx.now);
    CHECK(justice::memoryEntry(rig.observerState(), rig.player).score == APPEARANCE_SUSPICION_WEIGHT);
    // On its own the weight clears the WATCHED rung and stops short of
    // DISTRUSTED (the A-90 ladder decides: 3000 -> WATCHED).
    CHECK(justice::tierOf(rig.observerState(), rig.player) == TrustTier::WATCHED);
}
