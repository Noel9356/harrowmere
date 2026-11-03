// test_claims.cpp — task 6.3's pinned tests: the claimable classes (the
// unclaimed-frontier gate), the custom-law installation through lawsAt, the
// offline destruction protection with its active-war exception, the
// not-removable safe flag, the abandonment timer (validated minutes only, no
// AFK credit, the 14 REAL-day flag with its war pause) and the dismantle
// reversion that keeps the archive.
#include <doctest.h>

#include <cstdint>
#include <vector>

#include "character/character.hpp"
#include "core/clock.hpp"
#include "core/constants.hpp"
#include "core/types.hpp"
#include "data/laws.hpp"
#include "factions/claims.hpp"
#include "factions/factions.hpp"
#include "factions/war.hpp"
#include "world/world.hpp"
#include "world/worldgen.hpp"

#include "test_justice_fixture.hpp"

namespace
{

using namespace harrowmere;
using namespace justice_test;
namespace fac = harrowmere::factions;

constexpr territoryId kCounty   = JusticeFixture::kCounty;
constexpr territoryId kVillageA = JusticeFixture::kVillageA; // the SAFE village
constexpr territoryId kVillageB = JusticeFixture::kVillageB;
constexpr regionId kRegionA     = JusticeFixture::kRegionA;
constexpr regionId kRegionB     = JusticeFixture::kRegionB;
constexpr regionId kRegionXen   = JusticeFixture::kRegionXen;

// The frontier territory the claims tests claim: unclaimed (parent 0, no
// children, no laws), anchored to region 4 (worldgen never leaves unclaimed
// ground inside settled space — the tests hand-build it, the 1.4 shape).
constexpr territoryId kFrontier  = 8;
constexpr regionId kRegionFront  = 4;
constexpr std::uint64_t kDayReal = 24ull * 3'600ull; // the real-day conversion (the claims sweep's own)

// The claims rig: the war rig's two-court world (court 1 = the county seat,
// court 2 = the xenophobic village's seat) plus the frontier territory and a
// player with a purse.
struct ClaimRig
{
    JusticeFixture f;
    fac::FactionStore factions;
    fac::WarStore wars;
    fac::ClaimStore claims;
    fac::FactionQueries factionQueries;
    characterId player;
    characterId npc1; // faction 1's villager
    characterId npc2; // faction 2's marcher

    ClaimRig()
    {
        // The frontier: region 4 mapped to territory 8 (the 1.4 explicit
        // mapping), the FakeWorld mirror anchored at the region center.
        world::Region frontierRegion{};
        frontierRegion.id = kRegionFront;
        f.world.addRegion(frontierRegion);
        f.world.setRegionTerritory(kRegionFront, kFrontier);
        world::Territory frontier;
        frontier.id     = kFrontier;
        frontier.region = kRegionFront;
        f.world.addTerritory(frontier);
        f.base.addTerritory(
            kFrontier, world::regionCenter(kRegionFront).x, world::regionCenter(kRegionFront).z, kRegionFront, false
        );

        wars.bindFactions(&factions);
        wars.bindContext(&f.base.ctx);
        f.base.ctx.wars         = &wars;
        f.base.ctx.warStore     = &wars;
        f.base.ctx.factionStore = &factions;
        f.base.ctx.claims       = &claims;

        factionQueries.bind(&factions, &f.base.ctx);
        f.base.ctx.factions = &factionQueries;

        npc1         = phase2_test::spawnNpc(f.base, phase2_test::speciesIdByKey("human"), kVillageA, kRegionA, 30);
        npc2         = phase2_test::spawnNpc(f.base, phase2_test::speciesIdByKey("human"), kXenHome(), kRegionXen, 30);
        player       = phase2_test::spawnPlayer(f.base);
        Character &p = f.base.characters.get(player);
        p.body.position        = world::regionCenter(kRegionFront); // standing in the frontier
        p.progression.currency = 50'000;
        f.grid.insert(player, p.body.position);

        // The adoption joins the living home-anchored NPCs (spawns first,
        // then the membership walk — the 6.1 rig's explicit shape).
        factions.adoptWorldGen(testSeeds(), f.base.ctx);

        // The region anchors the war/claims logic reads (the fixture's
        // hand-built territories carry none — worldgen's runtime
        // territories do, 1.5's stage 2).
        f.world.territory(kVillageA)->region                   = kRegionA;
        f.world.territory(kVillageB)->region                   = kRegionB;
        f.world.territory(JusticeFixture::kXenophobic)->region = kRegionXen;
    }

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
        court2.courtTerritory = JusticeFixture::kXenophobic;
        court2.rivals         = {1};
        seeds.push_back(court2);
        return seeds;
    }

    static constexpr territoryId kXenHome()
    {
        return JusticeFixture::kXenophobic;
    }

    // The default custom set the install tests author (schema-constrained:
    // named, every threshold on the SCALE grid).
    static data::LawSet customLaws()
    {
        data::LawSet laws;
        laws.key = "player_charter";
        for (std::size_t i = 0; i < laws.convictionThreshold.size(); ++i)
        {
            laws.convictionThreshold[i] = 6000;
        }
        return laws;
    }

    // A declared + notice-ended war contesting the frontier's region: both
    // factions re-homed onto the frontier extract its stocked good (the
    // ResourceConflict cb names region 4), the powers set for the ratio.
    void makeWarOverFrontier()
    {
        f.base.npcs.get(npc1).homeTerritory          = kFrontier;
        f.base.npcs.get(npc2).homeTerritory          = kFrontier;
        f.base.economy.stock(kRegionFront, 1).supply = 50;
        f.base.characters.get(npc1).cachedPower      = 30'000;
        f.base.characters.get(npc2).cachedPower      = 21'000;
        fac::CasusBelli cb{fac::CasusBelliKind::ResourceConflict, world::kNoRegion, 0};
        REQUIRE(wars.declareWar(1, 2, cb, f.base.ctx).isOk());
        f.base.ctx.now += WAR_NOTICE_GAME_S + 1; // the war begins (effectively active)
    }

    // Validated (non-AFK) presence stamps, one per real minute.
    void validateMinutes(
        std::uint64_t &clock, int minutes, bool afk = false
    )
    {
        for (int i = 0; i < minutes; ++i)
        {
            clock += 60;
            claims.notePresence(kFrontier, player, afk, clock);
        }
    }
};

} // namespace

// ---------------------------------------------------------------------------
// TestClaims_UnclaimedOnlyClaimable (card 6.3, pinned name): only unclaimed
// frontier / lawless / abandoned territory claims; the gates around it (the
// player actor, the standing, the A-113 purse, the no-active-war rule) each
// refuse in turn.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestClaims_UnclaimedOnlyClaimable"
)
{
    ClaimRig rig;

    // The claim lands: the D-TERR-OWNER-1 write + the A-113 cost.
    CHECK(rig.claims.claim(kFrontier, rig.player, rig.f.base.ctx).isOk());
    CHECK(rig.f.world.territory(kFrontier)->owner == rig.player);
    CHECK(rig.f.base.characters.get(rig.player).progression.currency == 50'000 - CLAIM_COST);

    // A live owner holds it: the second claim refuses.
    CHECK(rig.claims.claim(kFrontier, rig.player, rig.f.base.ctx).error() == fac::ClaimError::AlreadyOwned);

    // Settled ground is not claimable (the village is neither unclaimed,
    // nor lawless, nor abandoned) — the player must stand there to try.
    rig.f.base.characters.get(rig.player).body.position = world::regionCenter(kRegionB);
    CHECK(rig.claims.claim(kVillageB, rig.player, rig.f.base.ctx).error() == fac::ClaimError::NotClaimable);
    rig.f.base.characters.get(rig.player).body.position = world::regionCenter(kRegionFront);

    // The standing gate: the interaction happens at the territory center.
    ClaimRig away;
    away.f.base.characters.get(away.player).body.position = world::regionCenter(kRegionA);
    CHECK(away.claims.claim(kFrontier, away.player, away.f.base.ctx).error() == fac::ClaimError::NotStandingThere);

    // The actor gate: only players claim (D-SP-1's NPC actors hold ground
    // through titles and the war outcome, never through this interaction).
    CHECK(rig.claims.claim(kFrontier, rig.npc1, rig.f.base.ctx).error() == fac::ClaimError::NotPlayer);
    ClaimRig npcClaim;
    npcClaim.f.base.characters.get(npcClaim.npc1).body.position = world::regionCenter(kRegionFront);
    CHECK(npcClaim.claims.claim(kFrontier, npcClaim.npc1, npcClaim.f.base.ctx).error() == fac::ClaimError::NotPlayer);

    // The purse gate (A-113 = 20000; the starter purse cannot cover it).
    ClaimRig poor;
    poor.f.base.characters.get(poor.player).body.position        = world::regionCenter(kRegionFront);
    poor.f.base.characters.get(poor.player).progression.currency = 100; // the A-41 starter purse
    CHECK(poor.claims.claim(kFrontier, poor.player, poor.f.base.ctx).error() == fac::ClaimError::InsufficientCurrency);

    // The war gate: a declared war contesting the frontier's region blocks
    // the claim, notice included.
    ClaimRig warRig;
    warRig.makeWarOverFrontier();
    CHECK(warRig.claims.claim(kFrontier, warRig.player, warRig.f.base.ctx).error() == fac::ClaimError::AtWar);
}

// ---------------------------------------------------------------------------
// TestClaims_InstallCustomLawsLawsAtReturnsThem (card 6.3, pinned name): the
// owner authors a LawSet-constrained set; installing writes territory.laws
// and lawsAt returns it (the doc walk, untouched). Non-owners and malformed
// sets refuse.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestClaims_InstallCustomLawsLawsAtReturnsThem"
)
{
    ClaimRig rig;
    REQUIRE(rig.claims.claim(kFrontier, rig.player, rig.f.base.ctx).isOk());

    // The installation: the store-owned copy, lawsAt resolving it.
    const data::LawSet laws = ClaimRig::customLaws();
    CHECK(rig.claims.installLaws(kFrontier, rig.player, laws, rig.f.base.ctx).isOk());
    const data::LawSet *installed = rig.claims.lawsInstalledOn(kFrontier);
    REQUIRE(installed != nullptr);
    CHECK(installed->key == "player_charter");
    CHECK(installed->convictionThreshold[static_cast<std::size_t>(CrimeType::MURDER)] == 6000);
    CHECK(rig.f.world.lawsAt(kFrontier).key == "player_charter"); // the doc walk reads the custom set

    // A non-owner refuses (an NPC outsider — SP has exactly one player,
    // the store's createPlayer is a singleton by contract).
    CHECK(rig.claims.installLaws(kFrontier, rig.npc1, laws, rig.f.base.ctx).error() == fac::ClaimError::NotOwner);

    // The schema constraint: an unnamed set and an off-grid threshold refuse.
    data::LawSet unnamed = laws;
    unnamed.key          = "";
    CHECK(rig.claims.installLaws(kFrontier, rig.player, unnamed, rig.f.base.ctx).error() == fac::ClaimError::BadLawSet);
    data::LawSet offGrid                                                    = laws;
    offGrid.convictionThreshold[static_cast<std::size_t>(CrimeType::THEFT)] = kScale + 1;
    CHECK(rig.claims.installLaws(kFrontier, rig.player, offGrid, rig.f.base.ctx).error() == fac::ClaimError::BadLawSet);
}

// ---------------------------------------------------------------------------
// TestClaims_IndestructibleOfflineExceptWar (card 6.3, pinned name): a
// player-owned territory refuses destruction while offline — and only an
// effectively-active war lifts the protection (D-SP-WAR-1: from
// noticeEndsAt).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestClaims_IndestructibleOfflineExceptWar"
)
{
    ClaimRig rig;
    REQUIRE(rig.claims.claim(kFrontier, rig.player, rig.f.base.ctx).isOk());
    CHECK(rig.claims.isProtectedFromDestruction(kFrontier, rig.f.base.ctx));

    // Unowned and NPC-held ground answers false (the protection class is
    // player-owned).
    CHECK_FALSE(rig.claims.isProtectedFromDestruction(kVillageB, rig.f.base.ctx));
    rig.claims.noteOwnershipTransfer(kVillageB, rig.npc1, rig.f.base.ctx.realTimeSeconds);
    CHECK_FALSE(rig.claims.isProtectedFromDestruction(kVillageB, rig.f.base.ctx));

    // The active-war exception: the notice window's protections still hold
    // (the suspension starts at noticeEndsAt, D-SP-WAR-1)...
    ClaimRig warRig;
    REQUIRE(warRig.claims.claim(kFrontier, warRig.player, warRig.f.base.ctx).isOk());
    warRig.f.base.npcs.get(warRig.npc1).homeTerritory     = kFrontier;
    warRig.f.base.npcs.get(warRig.npc2).homeTerritory     = kFrontier;
    warRig.f.base.economy.stock(kRegionFront, 1).supply   = 50;
    warRig.f.base.characters.get(warRig.npc1).cachedPower = 30'000;
    warRig.f.base.characters.get(warRig.npc2).cachedPower = 21'000;
    fac::CasusBelli cb{fac::CasusBelliKind::ResourceConflict, world::kNoRegion, 0};
    REQUIRE(warRig.wars.declareWar(1, 2, cb, warRig.f.base.ctx).isOk());
    CHECK(warRig.claims.isProtectedFromDestruction(kFrontier, warRig.f.base.ctx)); // notice: protected

    // ...and from noticeEndsAt the protection is suspended.
    warRig.f.base.ctx.now += WAR_NOTICE_GAME_S + 1;
    CHECK_FALSE(warRig.claims.isProtectedFromDestruction(kFrontier, warRig.f.base.ctx));
}

// ---------------------------------------------------------------------------
// TestClaims_SafeFlagNotRemovableByOwner (card 6.3, pinned name): a claimed
// territory that was safe keeps its safe flag whatever the owner installs —
// safe zones are spawn points (worldgen-owned; the install path never reads
// or writes the flag).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestClaims_SafeFlagNotRemovableByOwner"
)
{
    ClaimRig rig;
    // The safe village reaches claim-holding through the war-transfer path
    // (settled ground never claims — the transfer is the doc's other door):
    // the war writes Territory.owner, the ledger row follows.
    rig.f.world.territory(kVillageA)->owner = rig.player;
    rig.claims.noteOwnershipTransfer(kVillageA, rig.player, rig.f.base.ctx.realTimeSeconds);
    REQUIRE(rig.f.world.territory(kVillageA)->safe);
    const data::LawSet laws = ClaimRig::customLaws(); // a safe=false set by shape
    CHECK(rig.claims.installLaws(kVillageA, rig.player, laws, rig.f.base.ctx).isOk());
    CHECK(rig.f.world.territory(kVillageA)->safe); // the spawn-point flag stands
    CHECK(rig.f.world.isSafeTerritory(kVillageA)); // the ward-report rule intact

    // And the mirror arm: installing a safe=true set on unclaimed-turned-
    // claimed ground does NOT mint a safe zone.
    REQUIRE(rig.claims.claim(kFrontier, rig.player, rig.f.base.ctx).isOk());
    data::LawSet safeSet = ClaimRig::customLaws();
    safeSet.key          = "player_safe_charter";
    safeSet.safe         = true;
    CHECK(rig.claims.installLaws(kFrontier, rig.player, safeSet, rig.f.base.ctx).isOk());
    CHECK_FALSE(rig.f.world.territory(kFrontier)->safe);
}

// ---------------------------------------------------------------------------
// TestAbandon_TimerNeedsValidatedMinutes (card 6.3, pinned name): a 29-minute
// validated session does NOT reset the timer; the 30th validated minute
// (A-114) does.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestAbandon_TimerNeedsValidatedMinutes"
)
{
    // ---- the 29-minute arm: the timer survives ------------------------------
    ClaimRig rig;
    rig.f.base.ctx.realTimeSeconds = 1'000;
    REQUIRE(rig.claims.claim(kFrontier, rig.player, rig.f.base.ctx).isOk());
    std::uint64_t clock = 1'000;
    rig.validateMinutes(clock, 29); // 29 validated minutes — one short of A-114
    CHECK_FALSE(rig.claims.isAbandoned(kFrontier));

    clock += ABANDON_DAYS_REAL * kDayReal + 1; // 14 real days later
    rig.f.base.ctx.realTimeSeconds = clock;
    rig.claims.tick(rig.f.base.ctx);
    CHECK(rig.claims.isAbandoned(kFrontier)); // the short session never reset the timer

    // ---- the 30-minute arm: the timer resets ---------------------------------
    ClaimRig good;
    good.f.base.ctx.realTimeSeconds = 1'000;
    REQUIRE(good.claims.claim(kFrontier, good.player, good.f.base.ctx).isOk());
    std::uint64_t goodClock = 1'000;
    good.validateMinutes(goodClock, 30); // the qualifying session (A-114): the timer re-arms at clock 2800
    // The ORIGINAL 14-day horizon (from the claim) is now behind us, but the
    // mid-session reset re-armed the timer 30 minutes before the horizon —
    // the claim survives its first 14 days.
    good.f.base.ctx.realTimeSeconds = 1'000 + ABANDON_DAYS_REAL * kDayReal + 1;
    good.claims.tick(good.f.base.ctx);
    CHECK_FALSE(good.claims.isAbandoned(kFrontier)); // the reset pushed the horizon out

    // 14 real days past the RESET: abandoned.
    good.f.base.ctx.realTimeSeconds = goodClock + ABANDON_DAYS_REAL * kDayReal + 1;
    good.claims.tick(good.f.base.ctx);
    CHECK(good.claims.isAbandoned(kFrontier));
}

// ---------------------------------------------------------------------------
// TestAbandon_AfkTimeNotCounted (card 6.3, pinned name): AFK stamps add no
// validated seconds (D-AFK-1) and a stamp gap longer than the AFK timeout
// (A-58 — the player left) restarts the stint.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestAbandon_AfkTimeNotCounted"
)
{
    // ---- pure AFK hours never validate ---------------------------------------
    ClaimRig rig;
    rig.f.base.ctx.realTimeSeconds = 1'000;
    REQUIRE(rig.claims.claim(kFrontier, rig.player, rig.f.base.ctx).isOk());
    std::uint64_t clock = 1'000;
    rig.validateMinutes(clock, 60, /*afk=*/true); // an hour of AFK
    CHECK_FALSE(rig.claims.isAbandoned(kFrontier));
    rig.f.base.ctx.realTimeSeconds = clock + ABANDON_DAYS_REAL * kDayReal + 1;
    rig.claims.tick(rig.f.base.ctx);
    CHECK(rig.claims.isAbandoned(kFrontier)); // the AFK hour counted for nothing

    // ---- the mixed arm: 20 active + 40 AFK is one short session ---------------
    ClaimRig mixed;
    mixed.f.base.ctx.realTimeSeconds = 1'000;
    REQUIRE(mixed.claims.claim(kFrontier, mixed.player, mixed.f.base.ctx).isOk());
    std::uint64_t mixedClock = 1'000;
    mixed.validateMinutes(mixedClock, 20);               // 20 validated minutes
    mixed.validateMinutes(mixedClock, 40, /*afk=*/true); // 40 AFK minutes — no credit
    mixed.f.base.ctx.realTimeSeconds = mixedClock + ABANDON_DAYS_REAL * kDayReal + 1;
    mixed.claims.tick(mixed.f.base.ctx);
    CHECK(mixed.claims.isAbandoned(kFrontier));

    // ---- the gap arm: a lapse longer than A-58 restarts the stint -------------
    ClaimRig gap;
    gap.f.base.ctx.realTimeSeconds = 1'000;
    REQUIRE(gap.claims.claim(kFrontier, gap.player, gap.f.base.ctx).isOk());
    std::uint64_t gapClock = 1'000;
    gap.validateMinutes(gapClock, 20);   // 20 validated minutes banked
    gapClock += AFK_TIMEOUT_REAL_S + 60; // the player left, then returned
    gap.claims.notePresence(kFrontier, gap.player, /*afk=*/false, gapClock);
    gap.validateMinutes(gapClock, 29); // 29 more validated minutes
    gap.f.base.ctx.realTimeSeconds = gapClock + ABANDON_DAYS_REAL * kDayReal + 1;
    gap.claims.tick(gap.f.base.ctx);
    CHECK(gap.claims.isAbandoned(kFrontier)); // the banked 20 died with the gap
}

// ---------------------------------------------------------------------------
// TestAbandon_FourteenRealDaysFlags (card 6.3, pinned name): 14 REAL days
// with no qualifying session flag the claim abandoned — paused while a war
// effectively involves the territory (the game-time half of the two-clocks
// rule, D-TIME-1).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestAbandon_FourteenRealDaysFlags"
)
{
    ClaimRig rig;
    rig.f.base.ctx.realTimeSeconds = 1'000;
    REQUIRE(rig.claims.claim(kFrontier, rig.player, rig.f.base.ctx).isOk());

    // One second short of the horizon: not yet.
    rig.f.base.ctx.realTimeSeconds = 1'000 + ABANDON_DAYS_REAL * kDayReal - 1;
    rig.claims.tick(rig.f.base.ctx);
    CHECK_FALSE(rig.claims.isAbandoned(kFrontier));

    // At the horizon: flagged.
    rig.f.base.ctx.realTimeSeconds = 1'000 + ABANDON_DAYS_REAL * kDayReal;
    rig.claims.tick(rig.f.base.ctx);
    CHECK(rig.claims.isAbandoned(kFrontier));

    // ---- the war pause: an effectively-active war contesting the region ------
    ClaimRig warRig;
    warRig.f.base.ctx.realTimeSeconds = 1'000;
    REQUIRE(warRig.claims.claim(kFrontier, warRig.player, warRig.f.base.ctx).isOk());
    warRig.makeWarOverFrontier(); // declared + notice-ended, contesting region 4
    warRig.f.base.ctx.realTimeSeconds = 1'000 + 5 * ABANDON_DAYS_REAL * kDayReal;
    warRig.claims.tick(warRig.f.base.ctx);
    CHECK_FALSE(warRig.claims.isAbandoned(kFrontier)); // the war keeps the claim alive

    // The war ends (the defender collapses); the timer resumes its count.
    warRig.f.base.characters.get(warRig.npc2).cachedPower = 100;
    warRig.wars.tick(warRig.f.base.ctx);
    CHECK(warRig.wars.activeWarBetween(1, 2) == nullptr);
    warRig.f.base.ctx.realTimeSeconds = 1'000 + 6 * ABANDON_DAYS_REAL * kDayReal;
    warRig.claims.tick(warRig.f.base.ctx);
    CHECK(warRig.claims.isAbandoned(kFrontier));
}

// ---------------------------------------------------------------------------
// TestDismantle_RevertsAndKeepsArchive (card 6.3, pinned name): the abandoned
// territory dismantles back to unclaimed — owner 0, custom laws discarded,
// the ARCHIVE PRESERVED (world memory) — and re-claims at half cost.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestDismantle_RevertsAndKeepsArchive"
)
{
    ClaimRig rig;
    rig.f.base.ctx.realTimeSeconds = 1'000;
    REQUIRE(rig.claims.claim(kFrontier, rig.player, rig.f.base.ctx).isOk());
    const data::LawSet laws = ClaimRig::customLaws();
    REQUIRE(rig.claims.installLaws(kFrontier, rig.player, laws, rig.f.base.ctx).isOk());

    // The archive rows are world memory: written by 4.1's compactRecord, read
    // by 4.2's burn — the dismantle touches neither.
    world::Territory *territory = rig.f.world.territory(kFrontier);
    territory->archive.push_back(
        world::CompactRecord{42, 500, static_cast<std::uint32_t>(CrimeType::THEFT), rig.npc1, rig.player}
    );

    // A live claim refuses the dismantle.
    CHECK(
        rig.claims.dismantleAbandoned(kFrontier, rig.player, rig.f.base.ctx).error() == fac::ClaimError::NotAbandoned
    );

    // The 14 real days pass: abandoned.
    rig.f.base.ctx.realTimeSeconds = 1'000 + ABANDON_DAYS_REAL * kDayReal + 1;
    rig.claims.tick(rig.f.base.ctx);
    REQUIRE(rig.claims.isAbandoned(kFrontier));

    // The dismantle (the owner standing there — SP has exactly one player;
    // the doc's "dismantled" arm): the reversion with the archive intact.
    CHECK(rig.claims.dismantleAbandoned(kFrontier, rig.player, rig.f.base.ctx).isOk());
    CHECK(rig.f.world.territory(kFrontier)->owner == 0);
    CHECK(rig.f.world.territory(kFrontier)->laws == nullptr); // the custom set discarded
    CHECK(rig.claims.lawsInstalledOn(kFrontier) == nullptr);
    REQUIRE(rig.f.world.territory(kFrontier)->archive.size() == 1); // the archive preserved
    CHECK(rig.f.world.territory(kFrontier)->archive.front().evidenceId == 42);

    // The re-claim after the dismantle is a plain claim at FULL cost (the
    // abandoned flag went with the dismantled row).
    rig.f.base.ctx.realTimeSeconds += 60;
    rig.f.base.characters.get(rig.player).progression.currency = 50'000;
    CHECK(rig.claims.claim(kFrontier, rig.player, rig.f.base.ctx).isOk());
    CHECK(rig.f.base.characters.get(rig.player).progression.currency == 50'000 - CLAIM_COST);

    // The half-cost arm is the ABANDONED claim's: claim, abandon, re-claim —
    // the doc's "claimable at half cost" without the dismantle in between.
    ClaimRig half;
    half.f.base.ctx.realTimeSeconds = 1'000;
    REQUIRE(half.claims.claim(kFrontier, half.player, half.f.base.ctx).isOk());
    half.f.base.ctx.realTimeSeconds = 1'000 + ABANDON_DAYS_REAL * kDayReal + 1;
    half.claims.tick(half.f.base.ctx);
    REQUIRE(half.claims.isAbandoned(kFrontier));
    half.f.base.characters.get(half.player).progression.currency = 50'000;
    CHECK(half.claims.claim(kFrontier, half.player, half.f.base.ctx).isOk());
    CHECK(half.f.base.characters.get(half.player).progression.currency == 50'000 - CLAIM_COST / 2);
}
