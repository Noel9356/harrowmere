// test_war.cpp — task 6.2's pinned tests: the five casus belli each validate
// on their own merits (TestWar_DeclareRequiresCbOrRatio), the notice window
// keeps kills criminal while IN_WAR stays off and both flip at noticeEndsAt
// (the one warEffectivelyActive gate), the collapse and timeout resolution
// arms, the outcome effects (region war-outcome flags, title transfers, the
// claim rewiring) and the mandatory seven-day peace treaty. Two supporting
// tests pin the D-WAR-EVT-1 battle spawn (dual target groups) and the
// NPC-driven auto-declaration (the card's "NPC-driven by default").
#include <doctest.h>

#include <cstdint>
#include <vector>

#include "character/character.hpp"
#include "character/status.hpp"
#include "combat/death.hpp" // isCriminalAct (the war exemption's consumer)
#include "core/clock.hpp"
#include "core/constants.hpp"
#include "core/game_events.hpp" // PlayerSignal (the defender notice)
#include "core/types.hpp"
#include "events/engine.hpp"
#include "events/template.hpp"
#include "factions/claims.hpp"
#include "factions/factions.hpp"
#include "factions/war.hpp"
#include "justice/reputation.hpp"
#include "world/world.hpp"
#include "world/worldgen.hpp"

#include "test_justice_fixture.hpp"

namespace
{

using namespace harrowmere;
using namespace justice_test;
namespace fac = harrowmere::factions;
namespace ev  = harrowmere::events;

constexpr territoryId kKingdom  = JusticeFixture::kKingdom;
constexpr territoryId kCounty   = JusticeFixture::kCounty;
constexpr territoryId kVillageA = JusticeFixture::kVillageA;
constexpr territoryId kVillageB = JusticeFixture::kVillageB;
constexpr territoryId kXen      = JusticeFixture::kXenophobic;
constexpr regionId kRegionA     = JusticeFixture::kRegionA;
constexpr regionId kRegionB     = JusticeFixture::kRegionB;
constexpr regionId kRegionXen   = JusticeFixture::kRegionXen;

// The recording sink: the war publications (the defender's PlayerSignal and
// the TerritoryChanged facts) captured for the pinned assertions.
struct RecordingWarEvents final : public ISimEventSink
{
    std::vector<PlayerSignal> signals;
    std::vector<TerritoryChanged> territoryChanges;

    void onPlayerSignal(
        const PlayerSignal &ev
    ) override
    {
        signals.push_back(ev);
    }
    void onTerritoryChanged(
        const TerritoryChanged &ev
    ) override
    {
        territoryChanges.push_back(ev);
    }
};

// The war rig: the justice fixture's world (kVillageA/kVillageB under the
// county, kXenophobic under the kingdom) with TWO court factions seated so
// the two sides' territories split: court 1 holds the county (kVillageA's and
// kVillageB's jurisdiction), court 2 holds kXenophobic. Regions 0/1/2 are
// grid-adjacent in line, which the BorderDispute validation reads.
struct WarRig
{
    JusticeFixture f;
    ev::EventTemplateTable table;
    ev::EventEngine engine;
    fac::FactionStore factions;
    fac::WarStore wars;
    fac::ClaimStore claims;
    RecordingWarEvents recorder;
    fac::FactionQueries factionQueries;

    WarRig()
    {
        // The shipped templates (BATTLE included — the D-WAR-EVT-1 data).
        std::vector<data::DataError> errors;
        REQUIRE(ev::loadEventTemplates("mods/harrowmere_core/data/event_templates.json", table, errors));
        REQUIRE(table.byKey("BATTLE") != nullptr);
        REQUIRE(table.byKey("BATTLE")->specialDualTargetGroups);

        engine.bind(&table);
        engine.bindContext(&f.base.ctx);
        f.base.ctx.eventSpawner = &engine;
        f.base.ctx.simEvents    = &recorder;

        wars.bindFactions(&factions);
        wars.bindContext(&f.base.ctx);
        f.base.ctx.wars         = &wars; // the IWarRegistry face (IN_WAR reads it)
        f.base.ctx.warStore     = &wars; // the concrete registry (the cross-store reads)
        f.base.ctx.factionStore = &factions;
        f.base.ctx.claims       = &claims;

        factionQueries.bind(&factions, &f.base.ctx);
        f.base.ctx.factions = &factionQueries; // 3.3's isCriminalAct reads the real adapter

        // The region anchors the war logic reads (the fixture's hand-built
        // territories carry none — worldgen's runtime territories do, 1.5's
        // stage 2; the tests mirror that shape here).
        f.world.territory(kVillageA)->region = kRegionA;
        f.world.territory(kVillageB)->region = kRegionB;
        f.world.territory(kXen)->region      = kRegionXen;
    }

    // The adoption runs AFTER the tests spawn their NPCs (the membership
    // walk joins living home-anchored NPCs — the 6.1 rig's explicit shape).
    void adopt()
    {
        factions.adoptWorldGen(testSeeds(), f.base.ctx);
    }

    // Two kingdom courts (1 = the county seat, 2 = the xenophobic village's
    // seat) as rivals of each other, plus the frontier free company.
    static std::vector<world::FactionSeed> testSeeds()
    {
        std::vector<world::FactionSeed> seeds;
        world::FactionSeed court1;
        court1.id             = 1;
        court1.key            = "court_county";
        court1.name           = "Court of the County";
        court1.courtTerritory = kCounty;
        court1.rivals         = {2};
        seeds.push_back(court1);

        world::FactionSeed court2;
        court2.id             = 2;
        court2.key            = "court_xen";
        court2.name           = "Court of the Outer March";
        court2.courtTerritory = kXen;
        court2.rivals         = {1};
        seeds.push_back(court2);

        world::FactionSeed freeCompany;
        freeCompany.id             = 3;
        freeCompany.key            = "frontier_free_company";
        freeCompany.name           = "The Frontier Free Company";
        freeCompany.courtTerritory = 0;
        seeds.push_back(freeCompany);
        return seeds;
    }

    // Faction 1's villager (home under the county's jurisdiction) and
    // faction 2's marcher (home kXenophobic). cachedPower stays 0 — every
    // test sets the powers it needs (the PowerRatio fallback must stay
    // silent in the non-PowerRatio cases).
    characterId spawnFaction1Npc(
        territoryId home, regionId region
    )
    {
        return phase2_test::spawnNpc(f.base, phase2_test::speciesIdByKey("human"), home, region, 30);
    }
    characterId spawnFaction2Npc()
    {
        return phase2_test::spawnNpc(f.base, phase2_test::speciesIdByKey("human"), kXen, kRegionXen, 30);
    }
    characterId spawnPlayer()
    {
        const characterId id                    = phase2_test::spawnPlayer(f.base);
        f.base.characters.get(id).body.position = world::regionCenter(kRegionA);
        f.grid.insert(id, f.base.characters.get(id).body.position);
        return id;
    }

    // A title under `faction`'s control (the control definition's noble seat).
    void grantTitle(
        territoryId t, characterId holder
    )
    {
        world::Territory *territory = f.world.territory(t);
        REQUIRE(territory != nullptr);
        territory->titles[holder] = SocialClassId::NOBLE;
    }

    void setPower(
        characterId who, std::uint64_t power
    )
    {
        f.base.characters.get(who).cachedPower = power;
    }

    fac::WarResult declareOk(
        factionId attacker, factionId defender, fac::CasusBelli cb
    )
    {
        return wars.declareWar(attacker, defender, cb, f.base.ctx); // the local cb is the in-out lvalue
    }
};

// The PowersRig: the attacker fields 30000 aggregate, the defender 21000 —
// a ratio UNDER the A-109 1.5x line, so the PowerRatio fallback never fires
// in the cb-specific cases; the PowerRatio test re-points the numbers.
struct PowersRig
{
    WarRig rig;
    characterId a1;
    characterId a2;
    characterId d1;

    PowersRig()
        : a1(rig.spawnFaction1Npc(kVillageA, kRegionA)), a2(rig.spawnFaction1Npc(kVillageB, kRegionB)),
          d1(rig.spawnFaction2Npc())
    {
        rig.adopt(); // the spawns first, then the membership walk joins them
        rig.setPower(a1, 15'000);
        rig.setPower(a2, 15'000);
        rig.setPower(d1, 21'000);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// TestWar_DeclareRequiresCbOrRatio (card 6.2, pinned name): each of the five
// casus belli validates on its own merits; an invalid cb falls back to the
// PowerRatio arm (the card's "— or PowerRatio") and with the powers equal the
// declaration refuses. Degenerate targets refuse.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWar_DeclareRequiresCbOrRatio"
)
{
    // ---- BorderDispute: adjacent controlled territories --------------------
    {
        PowersRig p;
        // Faction 1's title on kVillageA (region 0), faction 2's on
        // kXenophobic (region 2) — regions 0 and 2 are NOT adjacent; the
        // equal powers keep the PowerRatio fallback silent.
        p.rig.grantTitle(kVillageA, p.a1);
        p.rig.grantTitle(kXen, p.d1);
        CHECK(p.rig.declareOk(1, 2, fac::CasusBelli{fac::CasusBelliKind::BorderDispute, world::kNoRegion, 0}).isErr());
        // Faction 2's title moves next door (region 1 — adjacent to 0).
        p.rig.f.world.territory(kXen)->titles.erase(p.d1);
        p.rig.grantTitle(kVillageB, p.d1);
        CHECK(p.rig.declareOk(1, 2, fac::CasusBelli{fac::CasusBelliKind::BorderDispute, world::kNoRegion, 0}).isOk());
    }

    // ---- ResourceConflict: both extract the same good from one region -----
    {
        PowersRig p;
        // Faction 2's marcher re-homed into kVillageB (region 1): both
        // factions' members extract there, and the region's stock holds
        // supply (3.7's production ledger IS the extraction evidence).
        p.rig.f.base.npcs.get(p.d1).homeTerritory      = kVillageB;
        p.rig.f.base.economy.stock(kRegionB, 1).supply = 100;
        fac::CasusBelli cb{fac::CasusBelliKind::ResourceConflict, world::kNoRegion, 0};
        CHECK(p.rig.wars.declareWar(1, 2, cb, p.rig.f.base.ctx).isOk());
        CHECK(cb.region == kRegionB); // the validator filled the provenance region

        // No co-resident extraction (the stock empty): refused.
        PowersRig q;
        q.rig.f.base.npcs.get(q.d1).homeTerritory = kVillageB;
        CHECK(
            q.rig.declareOk(1, 2, fac::CasusBelli{fac::CasusBelliKind::ResourceConflict, world::kNoRegion, 0}).isErr()
        );
    }

    // ---- Reputation: the defender leader's reputation below the floor -----
    {
        PowersRig p;
        // Faction 2's leader (its first member) is reviled in faction 1's
        // capital (the county).
        justice::changeReputation(p.d1, kCounty, -5000, p.rig.f.base.ctx);
        CHECK(p.rig.declareOk(1, 2, fac::CasusBelli{fac::CasusBelliKind::Reputation, world::kNoRegion, 0}).isOk());
        // A respected leader gives no casus belli.
        PowersRig q;
        justice::changeReputation(q.d1, kCounty, +4000, q.rig.f.base.ctx);
        CHECK(q.rig.declareOk(1, 2, fac::CasusBelli{fac::CasusBelliKind::Reputation, world::kNoRegion, 0}).isErr());
    }

    // ---- Massacre: 4.7's marker on the declarer's ground -------------------
    {
        PowersRig p;
        // Faction 1 controls kVillageA; the marker names faction 2 as the
        // culprit's faction.
        p.rig.grantTitle(kVillageA, p.a1);
        p.rig.wars.registerCasusBelli(kVillageA, p.d1, 2);
        CHECK(p.rig.declareOk(1, 2, fac::CasusBelli{fac::CasusBelliKind::Massacre, world::kNoRegion, 0}).isOk());
        // The consumed marker cannot declare a second Massacre war.
        PowersRig q;
        q.rig.grantTitle(kVillageA, q.a1);
        CHECK(q.rig.declareOk(1, 2, fac::CasusBelli{fac::CasusBelliKind::Massacre, world::kNoRegion, 0}).isErr());
    }

    // ---- PowerRatio: the attacker at 1.5x the defender ----------------------
    {
        PowersRig p;
        p.rig.setPower(p.a1, 30'000); // 2x the defender
        CHECK(p.rig.declareOk(1, 2, fac::CasusBelli{fac::CasusBelliKind::PowerRatio, world::kNoRegion, 0}).isOk());
        // Below the 1.5x ratio: refused (the pinned A-109 band). The
        // aggregate counts BOTH faction-1 members: 3000 + 15000 = 18000
        // against 21000 — far under the ratio.
        PowersRig q;
        q.rig.setPower(q.a1, 3'000);
        CHECK(q.rig.declareOk(1, 2, fac::CasusBelli{fac::CasusBelliKind::PowerRatio, world::kNoRegion, 0}).isErr());
        // The zero guard: a memberless faction never wars (0 >= 0 must not pass).
        PowersRig r;
        CHECK(r.rig.declareOk(1, 3, fac::CasusBelli{fac::CasusBelliKind::PowerRatio, world::kNoRegion, 0}).isErr());
    }

    // ---- the degenerate refusals --------------------------------------------
    PowersRig p;
    CHECK(p.rig.declareOk(1, 99, fac::CasusBelli{}).error() == fac::WarError::UnknownFaction);
    CHECK(p.rig.declareOk(1, 1, fac::CasusBelli{}).error() == fac::WarError::SameFaction);
    // A brought cb that validates nowhere, with equal powers: NoCasusBelli.
    CHECK(
        p.rig.declareOk(1, 2, fac::CasusBelli{fac::CasusBelliKind::BorderDispute, world::kNoRegion, 0}).error()
        == fac::WarError::NoCasusBelli
    );
    // A pending war blocks a second declaration (notice included).
    p.rig.setPower(p.a1, 30'000);
    CHECK(p.rig.declareOk(1, 2, fac::CasusBelli{}).isOk());
    CHECK(p.rig.declareOk(1, 2, fac::CasusBelli{}).error() == fac::WarError::AlreadyAtWar);
    CHECK(p.rig.declareOk(2, 1, fac::CasusBelli{}).error() == fac::WarError::AlreadyAtWar); // both directions
}

// ---------------------------------------------------------------------------
// TestWar_NoticeWindow_KillsDuringNoticeAreCrimes (card 6.2, pinned name):
// kills during the notice window are still crimes — 3.3's war exemption
// applies only after noticeEndsAt.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWar_NoticeWindow_KillsDuringNoticeAreCrimes"
)
{
    PowersRig p;
    p.rig.setPower(p.a1, 30'000);
    REQUIRE(p.rig.declareOk(1, 2, fac::CasusBelli{}).isOk());

    Character &culprit    = p.rig.f.base.characters.get(p.a1);
    Character &victim     = p.rig.f.base.characters.get(p.d1);
    culprit.body.position = world::regionCenter(kRegionB); // not a safe zone
    victim.body.position  = world::regionCenter(kRegionB);
    const Position &pos   = victim.body.position;

    // During the notice: no exemption — the kill is criminal.
    CHECK(harrowmere::isCriminalAct(CrimeType::MURDER, culprit, victim, pos, p.rig.f.base.ctx));

    // Past noticeEndsAt: the war exemption applies (same ground, same pair).
    p.rig.f.base.ctx.now += WAR_NOTICE_GAME_S + 1;
    CHECK_FALSE(harrowmere::isCriminalAct(CrimeType::MURDER, culprit, victim, pos, p.rig.f.base.ctx));
}

// ---------------------------------------------------------------------------
// TestWar_InWarFlipsAtNoticeEnd (card 6.2, pinned name): IN_WAR (2.1) is not
// set during the notice and flips at noticeEndsAt for both factions'
// members — the derived status reads the registry's warEffectivelyActive.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWar_InWarFlipsAtNoticeEnd"
)
{
    PowersRig p;
    p.rig.setPower(p.a1, 30'000);
    REQUIRE(p.rig.declareOk(1, 2, fac::CasusBelli{}).isOk());

    const Character &attacker = p.rig.f.base.characters.get(p.a1);
    const Character &defender = p.rig.f.base.characters.get(p.d1);
    CHECK_FALSE(harrowmere::hasStatus(attacker, StatusType::IN_WAR, p.rig.f.base.ctx));
    CHECK_FALSE(harrowmere::hasStatus(defender, StatusType::IN_WAR, p.rig.f.base.ctx));

    p.rig.f.base.ctx.now += WAR_NOTICE_GAME_S;
    CHECK(harrowmere::hasStatus(attacker, StatusType::IN_WAR, p.rig.f.base.ctx));
    CHECK(harrowmere::hasStatus(defender, StatusType::IN_WAR, p.rig.f.base.ctx));

    // A non-belligerent is never a combatant (the free company stands aside).
    const characterId bystander = p.rig.spawnPlayer();
    CHECK_FALSE(harrowmere::hasStatus(p.rig.f.base.characters.get(bystander), StatusType::IN_WAR, p.rig.f.base.ctx));
}

// ---------------------------------------------------------------------------
// TestWar_OutcomeSetsRegionFlags (card 6.2, pinned name): the winner sets the
// war-outcome bits on the contested regions — the data-defined "war_torn"
// bit from territory_templates.json's regionFlags table.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWar_OutcomeSetsRegionFlags"
)
{
    PowersRig p;
    // Faction 2 controls kVillageB (region 1) — the war's contested region.
    p.rig.grantTitle(kVillageB, p.d1);
    p.rig.setPower(p.a1, 30'000);
    p.rig.setPower(p.a2, 30'000);
    REQUIRE(p.rig.declareOk(1, 2, fac::CasusBelli{}).isOk());

    // The flag bit is data (the shipped regionFlags table).
    const auto &flagTable = p.rig.f.base.ctx.data->territoryTemplates().regionFlags();
    REQUIRE(flagTable.find("war_torn") != flagTable.end());
    const std::uint32_t warTorn = flagTable.at("war_torn");
    const world::Region *before = p.rig.f.world.region(kRegionB);
    REQUIRE(before != nullptr);
    const std::uint32_t flagsBefore = before->flags;

    // The defender collapses: the war ends, the winner sets the bits.
    p.rig.setPower(p.d1, 1000); // far under 30% of 60000
    p.rig.f.base.ctx.now += WAR_NOTICE_GAME_S + 1;
    p.rig.wars.tick(p.rig.f.base.ctx);

    REQUIRE(p.rig.wars.activeWarBetween(1, 2) == nullptr);
    const world::Region *after = p.rig.f.world.region(kRegionB);
    REQUIRE(after != nullptr);
    CHECK((after->flags & warTorn) != 0);
    CHECK(after->flags == (flagsBefore | warTorn)); // set, never cleared
}

// ---------------------------------------------------------------------------
// TestWar_TitlesTransferOnLoss (card 6.2, pinned name): the losing side's
// NPC-ruled territories change titles — the loser's noble replaced by the
// winner's appointee, and socialClassOf reads the new titles naturally.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWar_TitlesTransferOnLoss"
)
{
    PowersRig p;
    p.rig.grantTitle(kXen, p.d1); // faction 2's noble seat on kXenophobic
    p.rig.setPower(p.a1, 30'000);
    p.rig.setPower(p.a2, 30'000);
    REQUIRE(p.rig.declareOk(1, 2, fac::CasusBelli{}).isOk());
    REQUIRE(p.rig.f.world.socialClassOf(p.d1, kXen) == SocialClassId::NOBLE); // the seat reads pre-war

    p.rig.setPower(p.d1, 100); // the defender collapses
    p.rig.f.base.ctx.now += WAR_NOTICE_GAME_S + 1;
    p.rig.wars.tick(p.rig.f.base.ctx);
    REQUIRE(p.rig.wars.activeWarBetween(1, 2) == nullptr);

    // The winner's leader (faction 1's first member) holds the seat now, at
    // the same social class; the loser holds nothing.
    const characterId winnerLeader = p.rig.factions.find(1)->ranks.begin()->first;
    CHECK(p.rig.f.world.territory(kXen)->titles.find(p.d1) == p.rig.f.world.territory(kXen)->titles.end());
    REQUIRE(p.rig.f.world.territory(kXen)->titles.find(winnerLeader) != p.rig.f.world.territory(kXen)->titles.end());
    CHECK(p.rig.f.world.territory(kXen)->titles.at(winnerLeader) == SocialClassId::NOBLE);
    CHECK(p.rig.f.world.socialClassOf(winnerLeader, kXen) == SocialClassId::NOBLE);
    CHECK(p.rig.f.world.socialClassOf(p.d1, kXen) == SocialClassId::COMMONER);
}

// ---------------------------------------------------------------------------
// TestWar_TreatyBlocksRedeclareSevenDays (card 6.2, pinned name): after
// endWar the pair may not re-declare for PEACE_TREATY_DAYS (7 game-days) —
// both directions.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWar_TreatyBlocksRedeclareSevenDays"
)
{
    PowersRig p;
    p.rig.setPower(p.a1, 30'000);
    REQUIRE(p.rig.declareOk(1, 2, fac::CasusBelli{}).isOk());
    p.rig.setPower(p.d1, 100);
    p.rig.f.base.ctx.now += WAR_NOTICE_GAME_S + 1;
    p.rig.wars.tick(p.rig.f.base.ctx);
    REQUIRE(p.rig.wars.activeWarBetween(1, 2) == nullptr);
    const std::uint64_t endedAt = p.rig.f.base.ctx.now;

    // The treaty stamps both relation rows at the pinned seven days.
    const std::uint64_t treatyEnd = endedAt + PEACE_TREATY_DAYS * core::DAY_LENGTH_SECONDS;
    REQUIRE(p.rig.factions.relationBetween(1, 2) != nullptr);
    CHECK(p.rig.factions.relationBetween(1, 2)->treatyUntil == treatyEnd);
    REQUIRE(p.rig.factions.relationBetween(2, 1) != nullptr);
    CHECK(p.rig.factions.relationBetween(2, 1)->treatyUntil == treatyEnd);

    // The re-declaration refuses while the treaty stands (both directions).
    CHECK(p.rig.declareOk(1, 2, fac::CasusBelli{}).error() == fac::WarError::TreatyUntil);
    CHECK(p.rig.declareOk(2, 1, fac::CasusBelli{}).error() == fac::WarError::TreatyUntil);

    // The day after the treaty's end: the pair may war again.
    p.rig.f.base.ctx.now = treatyEnd + 1;
    CHECK(p.rig.declareOk(1, 2, fac::CasusBelli{}).isOk());
}

// ---------------------------------------------------------------------------
// TestWar_CollapseEndsAtThirtyPercent (card 6.2, pinned name): the war ends
// when one side's aggregate power falls below 30% of the other's (A-111) —
// and at the WAR_MAX_DURATION_DAYS timeout the attacker loses.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWar_CollapseEndsAtThirtyPercent"
)
{
    // ---- the collapse arm (the defender falls below 30%) -------------------
    PowersRig p;
    p.rig.setPower(p.a1, 30'000);
    p.rig.setPower(p.a2, 30'000); // the attacker aggregates 60000
    p.rig.setPower(p.d1, 20'000); // 33% — still above the collapse line
    REQUIRE(p.rig.declareOk(1, 2, fac::CasusBelli{}).isOk());
    p.rig.f.base.ctx.now += WAR_NOTICE_GAME_S + 1;
    p.rig.wars.tick(p.rig.f.base.ctx);
    REQUIRE(p.rig.wars.activeWarBetween(1, 2) != nullptr); // 33% fights on

    p.rig.setPower(p.d1, 17'000); // 28.3% — under the A-111 line
    // The collapse arm rides the progression sweep's cadence (the 8.3
    // as-built, WAR_CHECK_INTERVAL_GAME_S): the first tick after the notice
    // consumed this sweep, so the crossing lands on the next one.
    p.rig.f.base.ctx.now += WAR_CHECK_INTERVAL_GAME_S;
    p.rig.wars.tick(p.rig.f.base.ctx);
    REQUIRE(p.rig.wars.activeWarBetween(1, 2) == nullptr);
    const fac::War *ended = p.rig.wars.find(1);
    REQUIRE(ended != nullptr);
    CHECK_FALSE(ended->active);
    CHECK(ended->outcome == fac::WarOutcome::AttackerWon);
    CHECK(ended->endedAt == p.rig.f.base.ctx.now);

    // ---- the timeout arm (14 game-days: the attacker loses) -----------------
    PowersRig q;
    q.rig.setPower(q.a1, 30'000);
    q.rig.setPower(q.d1, 30'000);
    REQUIRE(q.rig.declareOk(1, 2, fac::CasusBelli{}).isOk());
    q.rig.f.base.ctx.now += WAR_NOTICE_GAME_S + 1;
    q.rig.wars.tick(q.rig.f.base.ctx);
    REQUIRE(q.rig.wars.activeWarBetween(1, 2) != nullptr); // even powers never collapse

    q.rig.f.base.ctx.now += WAR_MAX_DURATION_DAYS * core::DAY_LENGTH_SECONDS;
    q.rig.wars.tick(q.rig.f.base.ctx);
    REQUIRE(q.rig.wars.activeWarBetween(1, 2) == nullptr);
    REQUIRE_FALSE(q.rig.wars.all().empty()); // the fresh store's first (and only) war
    const fac::War &timedOut = q.rig.wars.all().begin()->second;
    CHECK_FALSE(timedOut.active);
    CHECK(timedOut.outcome == fac::WarOutcome::DefenderWon); // the attacker loses on timeout
}

// ---------------------------------------------------------------------------
// TestWar_BattleSpawnsDualTargetGroups (supporting, not a pinned name): the
// D-WAR-EVT-1 spawn — one live BATTLE per contested region, both sides'
// living members joined as the two target groups and as targets of each
// other; the scale-uniqueness gate throttles the second spawn.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWar_BattleSpawnsDualTargetGroups"
)
{
    PowersRig p;
    p.rig.grantTitle(kVillageB, p.d1); // the contested region 1
    p.rig.setPower(p.a1, 30'000);
    p.rig.setPower(p.a2, 30'000);
    p.rig.setPower(p.d1, 29'000); // above the collapse line — the war stands
    REQUIRE(p.rig.declareOk(1, 2, fac::CasusBelli{}).isOk());
    p.rig.f.base.ctx.now += WAR_NOTICE_GAME_S + 1;

    p.rig.wars.tick(p.rig.f.base.ctx);
    const ev::Event *battle = p.rig.engine.findActiveByKey("BATTLE");
    REQUIRE(battle != nullptr);
    CHECK(battle->region == kRegionB);

    // The two target groups: the attacker side (group 0) and the defender
    // side (group 1), each a target of the other (the shared targetIds).
    const std::uint64_t battleId           = p.rig.engine.active().rbegin()->first;
    const std::set<characterId> *attackers = p.rig.engine.targetGroup(battleId, 0);
    const std::set<characterId> *defenders = p.rig.engine.targetGroup(battleId, 1);
    REQUIRE(attackers != nullptr);
    REQUIRE(defenders != nullptr);
    CHECK(attackers->count(p.a1) == 1);
    CHECK(attackers->count(p.a2) == 1);
    CHECK(defenders->count(p.d1) == 1);
    CHECK(battle->targetIds.count(p.a1) == 1);
    CHECK(battle->targetIds.count(p.d1) == 1);
    CHECK(battle->participants.size() == 3); // both sides joined as participants

    // The throttle: a second tick in the same region refuses (one live
    // BATTLE per region — the scale-uniqueness gate).
    p.rig.f.base.ctx.now += WAR_CHECK_INTERVAL_GAME_S; // the next progression sweep (the 8.3 as-built cadence)
    p.rig.wars.tick(p.rig.f.base.ctx);
    std::size_t battles = 0;
    for (const auto &[id, ev] : p.rig.engine.active())
    {
        (void)id;
        (void)ev;
        if (p.rig.engine.targetGroup(id, 0) != nullptr)
        {
            ++battles;
        }
    }
    CHECK(battles == 1);
}

// ---------------------------------------------------------------------------
// TestWar_NpcRivalsAutoDeclare (supporting, not a pinned name): "NPC-driven
// by default" — the lazy monthly sweep declares for rival pairs from a valid
// casus belli (the BorderDispute the seeded titles make adjacent), once, and
// the treaty + the active-war gates hold it after.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWar_NpcRivalsAutoDeclare"
)
{
    PowersRig p;
    // Faction 1's villager + faction 2's marcher hold adjacent seats
    // (kVillageA region 0, kVillageB region 1) — the BorderDispute reads.
    p.rig.grantTitle(kVillageA, p.a1);
    p.rig.grantTitle(kVillageB, p.d1);

    p.rig.f.base.ctx.now = 30 * core::DAY_LENGTH_SECONDS + 5; // the first month boundary
    p.rig.wars.tick(p.rig.f.base.ctx);
    const fac::War *declared = p.rig.wars.activeWarBetween(1, 2);
    REQUIRE(declared != nullptr);
    CHECK(declared->cb.kind == fac::CasusBelliKind::BorderDispute);
    CHECK(declared->attacker == 1); // the lower id drives (R7's pair scan)

    // The lazy month gate: the same month never declares twice (the
    // active-war gate would refuse anyway — the sweep is a no-op).
    p.rig.f.base.ctx.now += core::DAY_LENGTH_SECONDS;
    p.rig.wars.tick(p.rig.f.base.ctx);
    CHECK(p.rig.wars.all().size() == 1);
}
