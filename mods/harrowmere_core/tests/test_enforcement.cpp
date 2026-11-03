// test_enforcement.cpp — task 4.6's pinned tests: the guard-power odds (both
// directions), the signal-before-force invariant, the evacuation + the
// enqueued manhunt spawn (through a recording spawner), the guard-power
// rollup, and the lawless lifecycle.
#include <doctest.h>

#include <cstdint>
#include <vector>

#include "core/constants.hpp"
#include "core/game_events.hpp"
#include "core/scheduler.hpp"
#include "justice/enforcement.hpp"
#include "justice/warrants.hpp"
#include "progression/power.hpp" // getCurrentPower + markPowerDirty
#include "npc/state.hpp"
#include "world/world.hpp"

#include "test_justice_fixture.hpp"

namespace
{

using namespace harrowmere;
using namespace justice_test;

// The recording spawner (the stub the card's test names — the manhunt spawn
// arrives ENQUEUED through the scheduler's step-5 slot).
struct RecordingSpawner final : public IEventSpawner
{
    std::vector<std::pair<std::string, ManhuntSeed>> spawns;

    bool spawnEvent(
        const char *templateKey, const ManhuntSeed &seed
    ) override
    {
        spawns.emplace_back(templateKey, seed);
        return true;
    }
};

// A guard/captain/militia garrison at one territory.
characterId spawnGuard(
    JusticeFixture &f, territoryId home, regionId region, const char *role, std::uint64_t strength = 0
)
{
    const characterId id = phase2_test::spawnNpc(f.base, phase2_test::speciesIdByKey("human"), home, region, 30, role);
    Character &c         = f.base.characters.get(id);
    c.body.position      = JusticeFixture::villageCenter(region);
    f.grid.insert(id, c.body.position);
    if (strength > 0)
    {
        for (auto &stat : c.progression.stats)
        {
            stat = strength;
        }
        markPowerDirty(c);
    }
    return id;
}

// A live warrant for the culprit at the given territory (the ASSAULT case
// shape; the identity arm rides one sighting-backed witness report).
std::uint64_t issueTestWarrant(
    JusticeFixture &f, characterId culprit, territoryId territory, regionId region
)
{
    justice::Evidence e;
    e.id            = f.store.nextEvidenceId();
    e.culpritId     = culprit;
    e.victimId      = 0;
    e.territory     = territory;
    e.worldTime     = f.base.ctx.now;
    e.type          = CrimeType::ASSAULT;
    e.status        = EvidenceStatus::UNKNOWN;
    e.physicalScore = 9000;
    justice::Report witness;
    witness.source        = f.spawnWitness(territory, region, "farmer", 9000, culprit);
    witness.weight        = 9000;
    witness.knowsIdentity = true;
    e.reports.push_back(witness);
    f.store.evidenceStore[territory].push_back(e);

    justice::Evidence &stored = f.store.evidenceStore[territory].back();
    justice::issueWarrant(stored, f.base.ctx);
    REQUIRE_FALSE(f.store.warrants.empty());
    return f.store.warrants.rbegin()->first;
}

struct EnforcementRig
{
    JusticeFixture f;
    RecordingSpawner spawner;

    EnforcementRig()
    {
        f.base.ctx.eventSpawner = &spawner;
    }
};

} // namespace

TEST_CASE(
    "TestEnforcement_GuardPowerSumsRoles"
)
{
    EnforcementRig rig;
    const territoryId terr = JusticeFixture::kVillageB;
    const regionId region  = JusticeFixture::kRegionB;

    // guard + captain + militia_veteran all roll into the sum; a farmer and
    // a guard of another territory do not.
    const characterId guardA  = spawnGuard(rig.f, terr, region, "guard", 100);
    const characterId captain = spawnGuard(rig.f, terr, region, "captain", 200);
    const characterId veteran = spawnGuard(rig.f, terr, region, "militia_veteran", 300);
    const characterId farmer  = spawnGuard(rig.f, terr, region, "farmer", 999);
    (void)farmer; // in the territory, but a civilian role never rolls into the garrison sum

    const std::uint64_t powerA
        = getCurrentPower(rig.f.base.characters.get(guardA), phase2_test::shippedRegistry(), rig.f.base.itemInstances);
    const std::uint64_t powerB
        = getCurrentPower(rig.f.base.characters.get(captain), phase2_test::shippedRegistry(), rig.f.base.itemInstances);
    const std::uint64_t powerC
        = getCurrentPower(rig.f.base.characters.get(veteran), phase2_test::shippedRegistry(), rig.f.base.itemInstances);

    CHECK(justice::localGuardPower(terr, rig.f.base.ctx) == powerA + powerB + powerC);
}

TEST_CASE(
    "TestEnforcement_WeakCulpritArrested"
)
{
    EnforcementRig rig;
    const territoryId terr = JusticeFixture::kVillageB;
    const regionId region  = JusticeFixture::kRegionB;

    // A strong garrison faces a powerless culprit: the odds hold.
    spawnGuard(rig.f, terr, region, "guard", 3000);
    const characterId culprit = spawnGuard(rig.f, terr, region, "farmer", 0);

    const std::uint64_t warrantId   = issueTestWarrant(rig.f, culprit, terr, region);
    const justice::Warrant &warrant = rig.f.store.warrants.at(warrantId);

    // The signal-before-force order: the case machine signalled on the
    // WANTED transition; the enforcement driver then responds.
    {
        justice::Evidence *casePtr = rig.f.firstEvidence(terr);
        REQUIRE(casePtr != nullptr);
        justice::notifyCaseStatusChanged(*casePtr, static_cast<int>(EvidenceStatus::UNKNOWN), rig.f.base.ctx);
    }
    REQUIRE(rig.f.store.signalledCulprits.count(culprit) == 1);

    const justice::EnforcementDecision decision = justice::respondToWarrant(warrant, rig.f.base.ctx);
    CHECK(decision == justice::EnforcementDecision::Arrested);

    // The NPC auto-submit converted the warrant to a conviction: the
    // standard sentence ran, the warrant left BOTH maps (the invariant).
    CHECK(rig.f.store.warrants.count(warrantId) == 0);
    const bool warrantGoneFromBothMaps = rig.f.store.activeWarrantsByCulprit.count(culprit) == 0
                                      || rig.f.store.activeWarrantsByCulprit.at(culprit).empty();
    CHECK(warrantGoneFromBothMaps);
    CHECK(justice::isArrested(rig.f.store, culprit, rig.f.base.ctx.now));
    CHECK(rig.f.firstEvidence(terr)->status == EvidenceStatus::CONVICTED);
}

TEST_CASE(
    "TestEnforcement_StrongCulpritTriggersEvacuationAndManhunt"
)
{
    EnforcementRig rig;
    const territoryId terr = JusticeFixture::kVillageB;
    const regionId region  = JusticeFixture::kRegionB;

    // A powerless garrison faces an overpowering culprit: evacuate + manhunt.
    const characterId civilianA = spawnGuard(rig.f, terr, region, "farmer", 0);
    const characterId civilianB = spawnGuard(rig.f, terr, region, "laborer", 0);
    const characterId culprit   = spawnGuard(rig.f, terr, region, "farmer", 9000);
    CHECK_FALSE(rig.f.base.npcs.get(civilianA).fleeing);

    const std::uint64_t warrantId   = issueTestWarrant(rig.f, culprit, terr, region);
    const justice::Warrant &warrant = rig.f.store.warrants.at(warrantId);
    {
        justice::Evidence *casePtr = rig.f.firstEvidence(terr);
        REQUIRE(casePtr != nullptr);
        justice::notifyCaseStatusChanged(*casePtr, static_cast<int>(EvidenceStatus::UNKNOWN), rig.f.base.ctx);
    }

    const justice::EnforcementDecision decision = justice::respondToWarrant(warrant, rig.f.base.ctx);
    CHECK(decision == justice::EnforcementDecision::Evacuated);

    // The civilians flee toward home/safe zone for the manhunt duration;
    // the warrant itself is untouched (the manhunt owns the resolution).
    CHECK(rig.f.base.npcs.get(civilianA).fleeing);
    CHECK(rig.f.base.npcs.get(civilianA).fleeingUntil == warrant.expiresTime);
    CHECK(rig.f.base.npcs.get(civilianB).fleeing);
    CHECK(rig.f.store.warrants.count(warrantId) == 1);

    // The spawn was ENQUEUED — it lands in the NEXT tick's step 5 (the
    // reentrancy rule), never synchronously inside step 6.
    CHECK(rig.spawner.spawns.empty());
    rig.f.base.scheduler.runTick(rig.f.base.ctx);
    REQUIRE(rig.spawner.spawns.size() == 1);
    CHECK(rig.spawner.spawns.front().first == "MANHUNT");
    CHECK(rig.spawner.spawns.front().second.culprit == culprit);
    CHECK(rig.spawner.spawns.front().second.territory == terr);
}

TEST_CASE(
    "TestEnforcement_NoArrestWithoutSignal"
)
{
    EnforcementRig rig;
    const territoryId terr = JusticeFixture::kVillageB;
    const regionId region  = JusticeFixture::kRegionB;

    spawnGuard(rig.f, terr, region, "guard", 3000);
    const characterId culprit       = spawnGuard(rig.f, terr, region, "farmer", 0);
    const std::uint64_t warrantId   = issueTestWarrant(rig.f, culprit, terr, region);
    const justice::Warrant &warrant = rig.f.store.warrants.at(warrantId);

    // The doc invariant: never arrest with zero prior signals — the attempt
    // refuses (a deterministic return value, not a debug-only assert).
    const justice::EnforcementDecision decision = justice::respondToWarrant(warrant, rig.f.base.ctx);
    CHECK(decision == justice::EnforcementDecision::NoSignal);
    CHECK(rig.f.store.warrants.count(warrantId) == 1); // the warrant stands
    CHECK_FALSE(justice::isArrested(rig.f.store, culprit, rig.f.base.ctx.now));
}

TEST_CASE(
    "TestLawless_FlagAfterDays_NoTicks"
)
{
    EnforcementRig rig;
    // The xenophobic village: its own law set makes the lawsAt distinction
    // observable (lawless resolves the DEFAULT set; restored, the walk
    // returns the village's own rows).
    const territoryId terr = JusticeFixture::kXenophobic;
    const regionId region  = JusticeFixture::kRegionXen;

    // No garrison at all: the watch starts, the promotion lands after A-96.
    spawnGuard(rig.f, terr, region, "farmer", 0);
    const characterId culprit     = spawnGuard(rig.f, terr, region, "farmer", 0);
    const std::uint64_t warrantId = issueTestWarrant(rig.f, culprit, terr, region);
    (void)warrantId;

    CHECK_FALSE(rig.f.world.isLawless(terr));
    justice::updateLawlessness(rig.f.base.ctx);
    CHECK(rig.f.world.lawlessWatchStart(terr) == rig.f.base.ctx.now);
    CHECK_FALSE(rig.f.world.isLawless(terr));

    rig.f.base.ctx.now += 2 * core::DAY_LENGTH_SECONDS;
    justice::updateLawlessness(rig.f.base.ctx);
    CHECK_FALSE(rig.f.world.isLawless(terr)); // 2 days: not yet

    rig.f.base.ctx.now += core::DAY_LENGTH_SECONDS; // 3 days == A-96
    justice::updateLawlessness(rig.f.base.ctx);
    CHECK(rig.f.world.isLawless(terr));

    // lawsAt resolves the DEFAULT set (the doc rule — 1.4's walk is bypassed).
    const data::LawSet &laws = rig.f.world.lawsAt(terr);
    CHECK(&laws == &phase2_test::shippedRegistry().defaultLaws());

    // Case ticks skip the lawless territory (4.6: "enforcement/case
    // ticks/suppression do not run — no one to enforce"): a player standing
    // inside the territory changes nothing — the A-76 cadence stamp never
    // lands and the case is untouched (the sweep is the only backstop).
    const characterId resident                        = phase2_test::spawnPlayer(rig.f.base);
    rig.f.base.characters.get(resident).body.position = JusticeFixture::villageCenter(region);
    justice::Evidence *e                              = rig.f.firstEvidence(terr);
    REQUIRE(e != nullptr);
    const EvidenceStatus statusBefore = e->status; // WANTED — issueWarrant's state
    rig.f.base.ctx.now += GAME_HOUR_SECONDS * 2;
    justice::runCaseTicks(rig.f.base.ctx);
    e = rig.f.firstEvidence(terr);
    REQUIRE(e != nullptr);
    CHECK(rig.f.store.caseLastTick.count(e->id) == 0); // never ticked: the lawless skip
    CHECK(e->status == statusBefore);                  // untouched by the tick pass

    // Enforcement refuses to respond in a lawless territory.
    const justice::EnforcementDecision decision
        = justice::respondToWarrant(rig.f.store.warrants.at(warrantId), rig.f.base.ctx);
    CHECK(decision == justice::EnforcementDecision::LawlessRefused);
}

TEST_CASE(
    "TestLawless_ReconquestRestoresEnforcement"
)
{
    EnforcementRig rig;
    const territoryId terr = JusticeFixture::kXenophobic;
    const regionId region  = JusticeFixture::kRegionXen;

    justice::updateLawlessness(rig.f.base.ctx);
    rig.f.base.ctx.now += 5 * core::DAY_LENGTH_SECONDS;
    justice::updateLawlessness(rig.f.base.ctx);
    REQUIRE(rig.f.world.isLawless(terr));

    // The reconquest seam (5.5's world-effect calls clearLawless — here the
    // garrison returns and the driver clears the flag itself).
    spawnGuard(rig.f, terr, region, "guard", 1000);
    justice::updateLawlessness(rig.f.base.ctx);
    CHECK_FALSE(rig.f.world.isLawless(terr));
    CHECK(rig.f.world.lawlessWatchStart(terr) == 0);

    // The restored territory enforces again: lawsAt walks the tree (the
    // village's own law set is back) and a warrant response no longer
    // refuses.
    CHECK(&rig.f.world.lawsAt(terr) == &phase2_test::shippedRegistry().lawSets().at("xenophobic_village"));

    const characterId culprit     = spawnGuard(rig.f, terr, region, "farmer", 0);
    const std::uint64_t warrantId = issueTestWarrant(rig.f, culprit, terr, region);
    {
        justice::Evidence *casePtr = rig.f.firstEvidence(terr);
        REQUIRE(casePtr != nullptr);
        justice::notifyCaseStatusChanged(*casePtr, static_cast<int>(EvidenceStatus::UNKNOWN), rig.f.base.ctx);
    }
    const justice::EnforcementDecision decision
        = justice::respondToWarrant(rig.f.store.warrants.at(warrantId), rig.f.base.ctx);
    CHECK(decision != justice::EnforcementDecision::LawlessRefused);
}
