// test_citizen_justice.cpp — task 4.7's pinned tests: the strict violence
// gate (suspicion NEVER authorizes), the militia odds, the bounty claims
// (alive/dead, pay-once, capture fatigue, the below-conviction invariant),
// the lethality routing, the smuggling exclusion, and the village-slaughter
// marker (zero loot currency + the casus belli).
#include <doctest.h>

#include <cstdint>
#include <tuple>
#include <vector>

#include "combat/death.hpp"
#include "core/constants.hpp"
#include "core/game_events.hpp"
#include "core/scheduler.hpp"
#include "events/engine.hpp" // the event-engine adapter (the smuggling test)
#include "justice/citizen_justice.hpp"
#include "justice/suspicion.hpp" // the SHUNNED memory fill (the gate test)
#include "progression/power.hpp" // getCurrentPower + markPowerDirty
#include "justice/conviction.hpp"
#include "justice/warrants.hpp"
#include "npc/state.hpp"
#include "world/world.hpp"

#include "test_justice_fixture.hpp"

namespace
{

using namespace harrowmere;
using namespace justice_test;

// The recording war registry (6.2's frozen stub signature — D-SINK-1).
struct RecordingWars final : public IWarRegistry
{
    std::vector<std::tuple<territoryId, characterId, factionId>> casusBelli;

    void registerCasusBelli(
        territoryId origin, characterId culprit, factionId culpritFaction
    ) override
    {
        casusBelli.emplace_back(origin, culprit, culpritFaction);
    }
};

// The store's total evidence ROWS (the evidenceStore maps territory ->
// vector<Evidence>; .size() would count territory KEYS, which only move when
// a case lands in a previously case-less territory — the row count is the
// honest "how many cases exist" read).
std::size_t totalEvidenceRows(
    const justice::JusticeStore &store
)
{
    std::size_t rows = 0;
    for (const auto &[terr, cases] : store.evidenceStore)
    {
        (void)terr;
        rows += cases.size();
    }
    return rows;
}

// A live warrant for one culprit (the THEFT shape — non-lethal by data).
std::uint64_t issueTestWarrant(
    JusticeFixture &f,
    characterId culprit,
    characterId victim,
    territoryId territory,
    regionId region,
    CrimeType type = CrimeType::THEFT
)
{
    justice::Evidence e;
    e.id            = f.store.nextEvidenceId();
    e.culpritId     = culprit;
    e.victimId      = victim;
    e.territory     = territory;
    e.worldTime     = f.base.ctx.now;
    e.type          = type;
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

characterId spawnVillager(
    JusticeFixture &f, territoryId home, regionId region, const char *role = "farmer", std::uint64_t strength = 0
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

struct CitizenRig
{
    JusticeFixture f;
    RecordingWars wars;

    CitizenRig()
    {
        f.base.ctx.wars = &wars;
    }
};

} // namespace

TEST_CASE(
    "TestGate_SuspicionNeverAuthorizesViolence"
)
{
    CitizenRig rig;
    const regionId region      = JusticeFixture::kRegionB;
    const characterId observer = spawnVillager(rig.f, JusticeFixture::kVillageB, region);
    const characterId target   = spawnVillager(rig.f, JusticeFixture::kVillageB, region);

    // The observer's trust memory maxes out at SHUNNED — suspicion at its
    // strongest, and the gate still answers from its own three arms only.
    NpcState &observerState = rig.f.base.npcs.get(observer);
    justice::addSuspicion(observerState, target, TRUST_TIER_SHUNNED, rig.f.base.ctx.now);
    REQUIRE(justice::tierOf(observerState, target) == TrustTier::SHUNNED);

    CHECK_FALSE(justice::npcMayViolate(observer, target, rig.f.base.ctx));

    // Each arm alone authorizes: an in-progress assault the NPC personally
    // witnesses (the 3.2 accumulator + the spatial seam)...
    rig.f.store.assaultDamage[target][999] = 50;                     // an unconsented accumulation in progress
    CHECK(justice::npcMayViolate(observer, target, rig.f.base.ctx)); // the NPC sees the target
    rig.f.store.assaultDamage.clear();
    // ...an active warrant in the NPC's territory...
    issueTestWarrant(rig.f, target, observer, JusticeFixture::kVillageB, region);
    CHECK(justice::npcMayViolate(observer, target, rig.f.base.ctx));
}

TEST_CASE(
    "TestMilitia_EngagesOnlyAtOdds"
)
{
    CitizenRig rig;
    const territoryId terr = JusticeFixture::kVillageB;
    const regionId region  = JusticeFixture::kRegionB;

    const characterId victim  = spawnVillager(rig.f, terr, region);
    const characterId culprit = spawnVillager(rig.f, terr, region, "farmer", 0);
    const characterId veteran = spawnVillager(rig.f, terr, region, "militia_veteran", 1000);
    const characterId citizen = spawnVillager(rig.f, terr, region, "laborer", 0);

    // The willing citizen: affinity to the victim >= A-54, integrity >= the
    // card's floor (the veteran joins by role).
    rig.f.base.npcs.get(citizen).integrity = MILITIA_CITIZEN_INTEGRITY;
    npc::addAffinityEvent(rig.f.base.npcs.get(citizen), victim, ACQUAINTANCE_AFFINITY);

    const std::uint64_t warrantId   = issueTestWarrant(rig.f, culprit, victim, terr, region);
    const justice::Warrant &warrant = rig.f.store.warrants.at(warrantId);

    // The powerless culprit: the posse's summed power reaches the A-97
    // ratio (2.0x of ~nothing) -> engages.
    justice::Posse posse = justice::formPosse(warrant, rig.f.base.ctx);
    REQUIRE(posse.members.size() == 2); // the veteran + the willing citizen
    const bool veteranInPosse = posse.members[0] == veteran || posse.members[1] == veteran;
    CHECK(veteranInPosse);
    CHECK(posse.engages);

    // The overpowering culprit: the same posse shadows & reports.
    for (auto &stat : rig.f.base.characters.get(culprit).progression.stats)
    {
        stat = 9000;
    }
    markPowerDirty(rig.f.base.characters.get(culprit));
    posse = justice::formPosse(warrant, rig.f.base.ctx);
    CHECK_FALSE(posse.engages); // "veteran militias engage only at favorable odds"
}

TEST_CASE(
    "TestCapture_AliveFullDeadPartial"
)
{
    CitizenRig rig;
    const territoryId terr = JusticeFixture::kVillageB;
    const regionId region  = JusticeFixture::kRegionB;

    const characterId processor = spawnVillager(rig.f, terr, region, "guard", 0); // the claim's processor
    const characterId culprit   = spawnVillager(rig.f, terr, region);
    const characterId claimant = spawnVillager(rig.f, terr, region, "hunter" /* unknown role reads as a civilian */, 0);
    (void)processor;

    const std::uint64_t warrantId = issueTestWarrant(rig.f, culprit, 0, terr, region);
    const std::uint64_t bounty    = rig.f.store.warrants.at(warrantId).bounty; // ASSAULT/THEFT severity x 5
    REQUIRE(bounty > 0);

    // Alive capture pays FULL.
    rig.f.base.characters.get(claimant).progression.currency = 0;
    const justice::BountyClaim aliveClaim
        = justice::claimBounty(warrantId, claimant, /*capturedAlive=*/true, rig.f.base.ctx);
    CHECK(aliveClaim == justice::BountyClaim::Paid);
    const std::int64_t purseAfterAlive = rig.f.base.characters.get(claimant).progression.currency;
    CHECK(purseAfterAlive == static_cast<std::int64_t>(bounty));

    // Dead pays PARTIAL (A-98: 0.6x) — a fresh warrant for the same culprit.
    const std::uint64_t secondWarrant = issueTestWarrant(rig.f, culprit, 0, terr, region);
    const justice::BountyClaim deadClaim
        = justice::claimBounty(secondWarrant, claimant, /*capturedAlive=*/false, rig.f.base.ctx);
    CHECK(deadClaim == justice::BountyClaim::Paid);
    const std::uint64_t deadPayout    = bounty * DEAD_BOUNTY_RATIO / SCALE;
    const std::int64_t purseAfterDead = rig.f.base.characters.get(claimant).progression.currency;
    CHECK(purseAfterDead == static_cast<std::int64_t>(bounty + deadPayout));
}

TEST_CASE(
    "TestCapture_PaidOnce"
)
{
    CitizenRig rig;
    const territoryId terr = JusticeFixture::kVillageB;
    const regionId region  = JusticeFixture::kRegionB;

    spawnVillager(rig.f, terr, region, "guard", 0);
    const characterId culprit  = spawnVillager(rig.f, terr, region);
    const characterId claimant = spawnVillager(rig.f, terr, region, "hunter", 0);

    const std::uint64_t warrantId = issueTestWarrant(rig.f, culprit, 0, terr, region);
    CHECK(justice::claimBounty(warrantId, claimant, true, rig.f.base.ctx) == justice::BountyClaim::Paid);
    // "Warrants pay once" — per WARRANT (the paid flag's store-level form):
    // the second claim finds the warrant gone and is refused.
    CHECK(justice::claimBounty(warrantId, claimant, true, rig.f.base.ctx) == justice::BountyClaim::UnknownWarrant);
}

TEST_CASE(
    "TestCapture_FatigueCompounds"
)
{
    CitizenRig rig;
    const territoryId terr = JusticeFixture::kVillageB;
    const regionId region  = JusticeFixture::kRegionB;

    spawnVillager(rig.f, terr, region, "guard", 0);
    const characterId culprit  = spawnVillager(rig.f, terr, region);
    const characterId claimant = spawnVillager(rig.f, terr, region, "hunter", 0);

    // Three self-captures inside the A-99 window pay 1.0x -> 0.5x -> 0.25x
    // of one bounty (the A-100 compounding; the doc's exploit ceiling).
    std::vector<std::uint64_t> payouts;
    for (int i = 0; i < 3; ++i)
    {
        const std::uint64_t warrantId = issueTestWarrant(rig.f, culprit, 0, terr, region);
        const std::uint64_t bounty    = rig.f.store.warrants.at(warrantId).bounty;
        const std::uint64_t expected  = bounty / (1u << i); // 1.0x -> 0.5x -> 0.25x
        const std::uint64_t actual = justice::bountyPayoutFor(rig.f.store.warrants.at(warrantId), true, rig.f.base.ctx);
        CHECK(actual == expected);
        CHECK(justice::claimBounty(warrantId, claimant, true, rig.f.base.ctx) == justice::BountyClaim::Paid);
        payouts.push_back(bounty);
    }
    CHECK(payouts.size() == 3);

    // A capture outside the window starts the cycle fresh at 1.0x.
    rig.f.base.ctx.now += (CAPTURE_FATIGUE_WINDOW_DAYS + 1) * core::DAY_LENGTH_SECONDS;
    const std::uint64_t laterWarrant = issueTestWarrant(rig.f, culprit, 0, terr, region);
    const std::uint64_t freshPayout
        = justice::bountyPayoutFor(rig.f.store.warrants.at(laterWarrant), true, rig.f.base.ctx);
    CHECK(freshPayout == payouts.back()); // the window passed: the cycle starts at 1.0x again
}

TEST_CASE(
    "TestBounty_BelowConvictionPenalty"
)
{
    CitizenRig rig;
    const territoryId terr = JusticeFixture::kVillageB;
    const regionId region  = JusticeFixture::kRegionB;

    // The max-severity shipped row (MURDER, severity 9000): the D-BOUNTY-1
    // formula pays severity x 5 = 45 000 currency (the A-73 cap bites only
    // at severity 10000); the invariant check below keeps it below the
    // conviction's currency penalty.
    const characterId culprit       = spawnVillager(rig.f, terr, region);
    const std::uint64_t warrantId   = issueTestWarrant(rig.f, culprit, 0, terr, region, CrimeType::MURDER);
    const justice::Warrant &warrant = rig.f.store.warrants.at(warrantId);
    CHECK(warrant.bounty == 9000ull * BOUNTY_CURRENCY_PER_POINT); // D-BOUNTY-1 on the shipped row

    const justice::Evidence *e = rig.f.firstEvidence(terr);
    REQUIRE(e != nullptr);
    const std::int64_t fine = justice::fineFor(*e, rig.f.base.ctx);
    CHECK(fine > 0);
    CHECK(static_cast<std::int64_t>(warrant.bounty) <= fine); // the doc invariant
}

TEST_CASE(
    "TestLethality_NonLethalWarrantKillIsMurder"
)
{
    CitizenRig rig;
    const territoryId terr = JusticeFixture::kVillageB;
    const regionId region  = JusticeFixture::kRegionB;

    const characterId killer                        = phase2_test::spawnPlayer(rig.f.base);
    rig.f.base.characters.get(killer).body.position = JusticeFixture::villageCenter(region);
    rig.f.grid.insert(killer, rig.f.base.characters.get(killer).body.position);

    // The non-lethal warrant (THEFT data) for the target: killing them is
    // MURDER — the doc's exact rule (isCriminalAct's hasLethalWarrantIn arm).
    const characterId victim = spawnVillager(rig.f, terr, region);
    issueTestWarrant(rig.f, victim, 0, terr, region, CrimeType::THEFT);

    const std::size_t casesBefore = totalEvidenceRows(rig.f.store);
    Character &victimChar         = rig.f.base.characters.get(victim);
    victimChar.body.currentHp     = 0;
    die(victimChar, killer, rig.f.base.ctx);
    CHECK(totalEvidenceRows(rig.f.store) == casesBefore + 1); // the fresh MURDER case
    bool murderFound = false;
    for (auto &[terrId, cases] : rig.f.store.evidenceStore)
    {
        for (const justice::Evidence &e : cases)
        {
            if (e.type == CrimeType::MURDER && e.culpritId == killer && e.victimId == victim)
            {
                murderFound = true;
            }
        }
    }
    CHECK(murderFound);

    // The lethal-warrant twin: the kill is LAWFUL (no evidence).
    const characterId target2 = spawnVillager(rig.f, terr, region);
    issueTestWarrant(rig.f, target2, 0, terr, region, CrimeType::MURDER); // MURDER data: lethalForce true
    const std::size_t casesMid = totalEvidenceRows(rig.f.store);
    Character &victim2         = rig.f.base.characters.get(target2);
    victim2.body.currentHp     = 0;
    die(victim2, killer, rig.f.base.ctx);
    CHECK(totalEvidenceRows(rig.f.store) == casesMid); // nothing recorded: lawful execution
}

TEST_CASE(
    "TestSmuggling_EvidenceAndNoReward"
)
{
    CitizenRig rig;
    events::EventTemplateTable table;
    std::vector<data::DataError> loadErrors;
    REQUIRE(events::loadEventTemplates("mods/harrowmere_core/data/event_templates.json", table, loadErrors));
    events::EventEngine engine;
    engine.bind(&table);
    engine.bindContext(&rig.f.base.ctx);
    rig.f.base.ctx.eventExclusion = &engine;

    const characterId culprit = phase2_test::spawnPlayer(rig.f.base);
    const characterId helper  = spawnVillager(rig.f, JusticeFixture::kVillageB, JusticeFixture::kRegionB);

    // A manhunt with the culprit as the target side; the helper joins it.
    ManhuntSeed seed;
    seed.warrantId = 5;
    seed.culprit   = culprit;
    seed.territory = JusticeFixture::kVillageB;
    seed.region    = JusticeFixture::kRegionB;
    REQUIRE(engine.spawnEvent("MANHUNT", seed));
    REQUIRE_FALSE(engine.active().empty());
    const std::uint64_t eventId = engine.active().rbegin()->first;
    REQUIRE(engine.find(eventId) != nullptr);
    CHECK(engine.find(eventId)->targetIds.count(culprit) == 1);
    engine.join(helper, eventId, rig.f.base.ctx);

    // The aid trigger: SMUGGLING evidence vs the helper + the reward
    // exclusion (5.3 respects the set).
    justice::recordSmuggling(helper, culprit, rig.f.base.ctx);
    bool smugglingFound = false;
    for (auto &[terrId, cases] : rig.f.store.evidenceStore)
    {
        for (const justice::Evidence &e : cases)
        {
            if (e.type == CrimeType::SMUGGLING && e.culpritId == helper)
            {
                smugglingFound = true;
            }
        }
    }
    CHECK(smugglingFound);
    CHECK(engine.find(eventId)->excludedFromRewards.count(helper) == 1);

    // Aiding a NON-target-side participant is NOT smuggling: the shared
    // event alone does not make it (the target-side rule gates the rule).
    // (An NPC: the fixture's store carries exactly one player — D-STORE-1.)
    const characterId innocent = spawnVillager(rig.f, JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    engine.join(innocent, eventId, rig.f.base.ctx);
    const characterId bystander = spawnVillager(rig.f, JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    engine.join(bystander, eventId, rig.f.base.ctx);
    const std::size_t casesBefore = totalEvidenceRows(rig.f.store);
    justice::recordSmuggling(bystander, innocent, rig.f.base.ctx);
    CHECK(totalEvidenceRows(rig.f.store) == casesBefore); // no evidence: the aided one is not the target side
    CHECK(engine.find(eventId)->excludedFromRewards.count(bystander) == 0);
}

TEST_CASE(
    "TestMassacre_ZeroLootAndCasusBelli"
)
{
    CitizenRig rig;
    const territoryId terr = JusticeFixture::kVillageB;
    const regionId region  = JusticeFixture::kRegionB;

    const characterId killer                        = phase2_test::spawnPlayer(rig.f.base);
    rig.f.base.characters.get(killer).body.position = JusticeFixture::villageCenter(region);
    rig.f.grid.insert(killer, rig.f.base.characters.get(killer).body.position);

    // Five sapient villagers, each with a purse and a bag item (loot items
    // remain; the loot CURRENCY zeroes from the marker on).
    std::vector<characterId> victims;
    for (int i = 0; i < 5; ++i)
    {
        const characterId victim                               = spawnVillager(rig.f, terr, region);
        rig.f.base.characters.get(victim).progression.currency = 50;
        rig.f.base.npcs.get(victim).inventory.stacks[3]        = 2; // items.json id 3 (bandage)
        victims.push_back(victim);
    }

    for (std::size_t i = 0; i < victims.size(); ++i)
    {
        Character &victim     = rig.f.base.characters.get(victims[i]);
        victim.body.currentHp = 0;
        die(victim, killer, rig.f.base.ctx); // the sink fires BEFORE the loot staging
    }
    rig.f.base.corpses.materializePending();

    // The first four corpses carry the purse; the marker rose at the fifth
    // kill — that corpse's loot currency is zeroed and the items remain.
    for (std::size_t i = 0; i + 1 < victims.size(); ++i)
    {
        const Corpse *corpse = rig.f.base.corpses.find(victims[i]);
        REQUIRE(corpse != nullptr);
        CHECK(corpse->lootCurrency == 50);
    }
    const Corpse *last = rig.f.base.corpses.find(victims.back());
    REQUIRE(last != nullptr);
    CHECK(last->lootCurrency == 0);  // "corpse loot currency zeroed"
    CHECK_FALSE(last->loot.empty()); // "loot items remain"

    // The casus belli: the parent (county) + the sibling village, never the
    // massacred territory itself — registered once per crossing.
    REQUIRE(rig.wars.casusBelli.size() == 2);
    CHECK(std::get<0>(rig.wars.casusBelli[0]) == JusticeFixture::kCounty);
    CHECK(std::get<1>(rig.wars.casusBelli[0]) == killer);
    CHECK(std::get<0>(rig.wars.casusBelli[1]) == JusticeFixture::kVillageA);

    // The JusticeSystem sink answers the same rule for the death path's
    // staging read.
    CHECK(justice::massacreLootCurrency(killer, terr, 123, rig.f.base.ctx) == 0);
    CHECK(justice::massacreLootCurrency(killer, JusticeFixture::kVillageA, 123, rig.f.base.ctx) == 123);
}
