// test_factions.cpp — task 6.1's pinned tests: the bidirectional membership
// lockstep, the relation clamp + the standing flags, the A-108 founding cost
// with the D-SP-1 singleton, the leader-only dissolution with the rank-share
// distribution, and the deterministic monthly drift (rivalry pairs harden or
// soften by the leaders' reputation).
#include <doctest.h>

#include <cstdint>

#include "character/character.hpp"
#include "core/constants.hpp"
#include "core/types.hpp"
#include "factions/factions.hpp"
#include "justice/reputation.hpp"
#include "world/worldgen.hpp"

#include "test_justice_fixture.hpp"

namespace
{

using namespace harrowmere;
using namespace justice_test;
namespace fac = harrowmere::factions;

// The fixture's pinned hierarchy ids (static members; local aliases keep the
// seeds and helpers readable).
constexpr territoryId kKingdom  = JusticeFixture::kKingdom;
constexpr territoryId kCounty   = JusticeFixture::kCounty;
constexpr territoryId kVillageA = JusticeFixture::kVillageA;
constexpr regionId kRegionA     = JusticeFixture::kRegionA;

// The seed set the tests share: two kingdom courts (1, 2 — rivals of each
// other) and the frontier free company (3). Court 1 owns the fixture's
// kingdom (kKingdom); court 2 owns the county (no residents in these tests).
std::vector<world::FactionSeed> testSeeds()
{
    std::vector<world::FactionSeed> seeds;
    world::FactionSeed court1;
    court1.id             = 1;
    court1.key            = "court_kingdom_1";
    court1.name           = "Court of the First Kingdom";
    court1.courtTerritory = kKingdom;
    court1.rivals         = {2};
    seeds.push_back(court1);

    world::FactionSeed court2;
    court2.id             = 2;
    court2.key            = "court_kingdom_2";
    court2.name           = "Court of the Second Kingdom";
    court2.courtTerritory = kCounty;
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

struct FactionRig
{
    JusticeFixture f;
    fac::FactionStore store;

    void adopt()
    {
        store.adoptWorldGen(testSeeds(), f.base.ctx);
    }

    // A kingdom resident (joins court 1 through the home-territory walk) and
    // a frontier wanderer (joins the free company).
    characterId spawnKingdomNpc()
    {
        return phase2_test::spawnNpc(f.base, phase2_test::speciesIdByKey("human"), kVillageA, kRegionA, 30);
    }
    characterId spawnFrontierNpc()
    {
        return phase2_test::spawnNpc(f.base, phase2_test::speciesIdByKey("human"), 0, world::kNoRegion, 30);
    }
    characterId spawnPlayer()
    {
        const characterId id  = f.base.characters.createPlayer(PlayerCreation{"founder", {}, ""});
        f.base.world.playerId = id;
        Character &p          = f.base.characters.get(id);
        p.body.position       = world::regionCenter(kRegionA); // a live territory presence
        return id;
    }
    // Court 1's leader (the adoption's rank ladder makes the first member
    // the rank-9 when no royal exists — deterministic: the lowest id).
    characterId courtLeader()
    {
        for (const auto &[member, rank] : store.find(1)->ranks)
        {
            if (rank >= 9)
                return member;
        }
        return 0;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// TestFaction_MembershipBidirectional (card 6.1, pinned name): the adoption
// wires every side — the Character.faction field, the Faction.members set
// and the store's index — and the reads agree.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestFaction_MembershipBidirectional"
)
{
    FactionRig rig;
    const characterId kingdomNpc  = rig.spawnKingdomNpc();
    const characterId frontierNpc = rig.spawnFrontierNpc();
    rig.adopt();

    // The court walk: the village NPC's home territory sits under kKingdom,
    // so court 1 owns them; the wanderer joins the free company.
    CHECK(rig.f.base.characters.get(kingdomNpc).faction == 1);
    CHECK(rig.f.base.characters.get(frontierNpc).faction == 3);
    REQUIRE(rig.store.find(1) != nullptr);
    CHECK(rig.store.find(1)->members.count(kingdomNpc) == 1);
    REQUIRE(rig.store.find(3) != nullptr);
    CHECK(rig.store.find(3)->members.count(frontierNpc) == 1);
    CHECK(rig.store.factionOfCharacter(kingdomNpc) == 1);
    CHECK(rig.store.factionOfCharacter(frontierNpc) == 3);
    // The rank ladder: court 1's first member leads (no royal in the plan).
    CHECK(rig.store.find(1)->ranks.at(kingdomNpc) == 9);

    // The player never auto-joins.
    const characterId player = rig.spawnPlayer();
    CHECK(rig.f.base.characters.get(player).faction == 0);
    CHECK(rig.store.factionOfCharacter(player) == 0);

    // The seeded relations opened BOTH directions (rivals 1<->2 at the
    // seeded score, the rivalry flag set).
    REQUIRE(rig.store.relationBetween(1, 2) != nullptr);
    CHECK(rig.store.relationBetween(1, 2)->score == FACTION_SEEDED_RIVALRY_SCORE);
    CHECK(rig.store.relationBetween(1, 2)->rivalry);
    REQUIRE(rig.store.relationBetween(2, 1) != nullptr);
    CHECK(rig.store.relationBetween(2, 1)->rivalry);

    // isAtWar: the interface exists and stubs FALSE until 6.2 (the card's
    // Done-when) — even between hard rivals.
    CHECK_FALSE(rig.store.isAtWar(1, 2, rig.f.base.ctx));
}

// ---------------------------------------------------------------------------
// TestFaction_RelationClampAndAllianceFlag (card 6.1, pinned name): SetRelation
// clamps to the ±SCALE grid, writes both directions symmetrically, and the
// standing flags re-derive at the seeded magnitude.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestFaction_RelationClampAndAllianceFlag"
)
{
    FactionRig rig;
    rig.adopt();

    // Over the ceiling: clamps to +SCALE, the alliance flag flips.
    CHECK(rig.store.setRelation(1, 2, 50000, rig.f.base.ctx).isOk());
    REQUIRE(rig.store.relationBetween(1, 2) != nullptr);
    CHECK(rig.store.relationBetween(1, 2)->score == static_cast<std::int32_t>(SCALE));
    CHECK(rig.store.relationBetween(1, 2)->alliance);
    CHECK_FALSE(rig.store.relationBetween(1, 2)->rivalry);
    // Symmetric: the mirror row reads the same.
    REQUIRE(rig.store.relationBetween(2, 1) != nullptr);
    CHECK(rig.store.relationBetween(2, 1)->alliance);

    // Under the floor: clamps to -SCALE, the rivalry flag flips.
    CHECK(rig.store.setRelation(1, 2, -50000, rig.f.base.ctx).isOk());
    CHECK(rig.store.relationBetween(1, 2)->score == -static_cast<std::int32_t>(SCALE));
    CHECK(rig.store.relationBetween(1, 2)->rivalry);
    CHECK_FALSE(rig.store.relationBetween(1, 2)->alliance);

    // The middle band: neither flag stands.
    CHECK(rig.store.setRelation(1, 2, 3000, rig.f.base.ctx).isOk());
    CHECK(rig.store.relationBetween(1, 2)->score == 3000);
    CHECK_FALSE(rig.store.relationBetween(1, 2)->alliance);
    CHECK_FALSE(rig.store.relationBetween(1, 2)->rivalry);

    // Degenerate targets refuse (unknown faction, self-relation).
    CHECK(rig.store.setRelation(1, 99, 0, rig.f.base.ctx).isErr());
    CHECK(rig.store.setRelation(1, 1, 0, rig.f.base.ctx).isErr());
}

// ---------------------------------------------------------------------------
// TestFaction_PlayerFoundCostAndSingleton (card 6.1, pinned name): the A-108
// cost leaves the founder's purse, the presence and the D-SP-1 singleton
// gate, and the founder leads at rank 9.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestFaction_PlayerFoundCostAndSingleton"
)
{
    FactionRig rig;
    rig.adopt();
    const characterId player = rig.spawnPlayer();
    Character &p             = rig.f.base.characters.get(player);
    p.progression.currency   = 10'000; // twice the founding cost

    // The founding: exactly A-108 leaves the purse; the founder leads.
    CHECK(rig.store.foundPlayerFaction(player, "The Harrow Company", rig.f.base.ctx).isOk());
    CHECK(p.progression.currency == 10'000 - FACTION_FOUND_COST);
    const factionId founded = rig.store.factionOfCharacter(player);
    REQUIRE(founded != 0);
    REQUIRE(rig.store.find(founded) != nullptr);
    CHECK(rig.store.find(founded)->playerFounded);
    CHECK(rig.store.find(founded)->ranks.at(player) == 9);
    CHECK(rig.store.find(founded)->capital == kVillageA); // the presence territory

    // The membership gate: the founder already stands in a faction.
    CHECK(
        rig.store.foundPlayerFaction(player, "The Second Company", rig.f.base.ctx).error()
        == fac::FactionError::AlreadyMember
    );
    CHECK(p.progression.currency == 10'000 - FACTION_FOUND_COST);

    // The singleton: a DIFFERENT founder is refused (D-SP-1's at most one
    // playerFounded faction) and spends nothing.
    const characterId rival                               = rig.spawnFrontierNpc();
    rig.f.base.characters.get(rival).progression.currency = 10'000;
    rig.f.base.characters.get(rival).body.position        = world::regionCenter(kRegionA);
    CHECK(
        rig.store.foundPlayerFaction(rival, "The Rival Company", rig.f.base.ctx).error()
        == fac::FactionError::PlayerFactionExists
    );
    CHECK(rig.f.base.characters.get(rival).progression.currency == 10'000);

    // The purse gate: a founder who cannot cover A-108 is refused.
    FactionRig poor;
    poor.adopt();
    const characterId poorFounder                                = poor.spawnPlayer();
    poor.f.base.characters.get(poorFounder).progression.currency = 4'999;
    CHECK(
        poor.store.foundPlayerFaction(poorFounder, "The Beggar Band", poor.f.base.ctx).error()
        == fac::FactionError::InsufficientCurrency
    );
    CHECK(poor.store.factionOfCharacter(poorFounder) == 0);

    // The presence gate: no territory underfoot, no founding (a bare
    // fixture — no anchored territories resolve).
    phase2_test::Fixture bare;
    fac::FactionStore store2;
    const characterId wanderer = bare.characters.createPlayer(PlayerCreation{"drifter", {}, ""});
    bare.characters.get(wanderer).progression.currency = 10'000; // position stays (0,0,0): no territory anywhere
    CHECK(
        store2.foundPlayerFaction(wanderer, "The Landless", bare.ctx).error() == fac::FactionError::NoTerritoryPresence
    );
}

// ---------------------------------------------------------------------------
// TestFaction_DissolutionDistributesByRank (card 6.1, pinned name): the
// leader-only gate, the rank-share distribution (weight = rank + 1) with the
// flooring remainder to the top weight, and the full teardown.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestFaction_DissolutionDistributesByRank"
)
{
    FactionRig rig;
    rig.adopt();
    const characterId player                               = rig.spawnPlayer();
    rig.f.base.characters.get(player).progression.currency = 10'000;
    CHECK(rig.store.foundPlayerFaction(player, "The Share-Out", rig.f.base.ctx).isOk());
    const factionId founded = rig.store.factionOfCharacter(player);

    // An invited member: the roll is rigged by affinity (a strong positive
    // affinity pins the acceptance at the ceiling — and the seeded stream's
    // first draw answers it deterministically).
    rig.f.base.rng.seedAll(7);
    const characterId member                     = rig.spawnKingdomNpc();
    rig.f.base.npcs.get(member).affinity[player] = SCALE; // +10000 -> pct hits the ceiling
    const fac::FactionResult invite              = rig.store.inviteMember(player, member, rig.f.base.ctx);
    CHECK(invite.isOk());
    CHECK(rig.store.factionOfCharacter(member) == founded);
    CHECK(rig.store.find(founded)->ranks.at(member) == 0); // a grunt joins

    // Rank assignment: the leader ranks the member; the band is closed.
    CHECK(rig.store.setMemberRank(player, member, 4, rig.f.base.ctx).isOk());
    CHECK(rig.store.find(founded)->ranks.at(member) == 4);
    CHECK(rig.store.setMemberRank(player, member, 10, rig.f.base.ctx).error() == fac::FactionError::BadRank);

    // The shared purse: 1100 currency (goodId 0) + a goods row that splits
    // even over the 10/5 weights.
    rig.store.find(founded)->sharedResources[0] = 1100;
    rig.store.find(founded)->sharedResources[4] = 60;

    // A non-leader cannot dissolve.
    CHECK(rig.store.dissolveFaction(member, rig.f.base.ctx).error() == fac::FactionError::NotLeader);

    // The leader dissolves: weights 10 (rank 9) vs 5 (rank 4) over 15.
    // Currency: player 1100*10/15 = 733, member 366, paid 1099, the flooring
    // remainder 1 to the top weight (the player). Goods: 40 / 20 (even).
    const std::int64_t playerBefore = rig.f.base.characters.get(player).progression.currency;
    const std::int64_t memberBefore = rig.f.base.characters.get(member).progression.currency;
    CHECK(rig.store.dissolveFaction(player, rig.f.base.ctx).isOk());
    CHECK(rig.f.base.characters.get(player).progression.currency == playerBefore + 733 + 1);
    CHECK(rig.f.base.characters.get(member).progression.currency == memberBefore + 366);
    // The goods row dispersed into the members' held-goods ledger by the
    // same weights (3.7's addGoodsHeld — the member-side goods home).
    CHECK(rig.f.base.economy.goodsHeld(player, 4) == 40);
    CHECK(rig.f.base.economy.goodsHeld(member, 4) == 20);
    // The faction and every membership row are gone; the people keep theirs.
    CHECK(rig.store.find(founded) == nullptr);
    CHECK(rig.store.factionOfCharacter(player) == 0);
    CHECK(rig.store.factionOfCharacter(member) == 0);
    CHECK(rig.f.base.characters.get(player).faction == 0);
    CHECK(rig.f.base.characters.get(member).faction == 0);
}

// ---------------------------------------------------------------------------
// TestFaction_NpcDriftDeterministic (card 6.1, pinned name): the lazy month
// gate applies each month exactly once; the seeded rivalry pair hardens
// under a reviled leader and softens under a respected one; two identical
// runs agree.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestFaction_NpcDriftDeterministic"
)
{
    // ---- the hardening arm ----------------------------------------------------
    FactionRig rig;
    const characterId resident = rig.spawnKingdomNpc(); // the court's first member IS its leader
    (void)resident;
    rig.adopt();
    const characterId leader = rig.courtLeader();
    REQUIRE(leader != 0);
    const std::uint64_t month1 = 30 * core::DAY_LENGTH_SECONDS + 5;
    rig.f.base.ctx.now         = month1;
    // The reviled leader: negative reputation AT the drift time (the lazy
    // decay reads the same clock — writing first keeps the read honest).
    // The walk writes villageA -4000 -> county -2000 -> kingdom -1000.
    justice::changeReputation(leader, kVillageA, -4000, rig.f.base.ctx);
    rig.store.monthlyDrift(rig.f.base.ctx);
    const std::int32_t afterFirstMonth = rig.store.relationBetween(1, 2)->score;
    CHECK(afterFirstMonth == FACTION_SEEDED_RIVALRY_SCORE - FACTION_RIVALRY_STEP_PER_MONTH); // hardened

    // The lazy gate: same month, more calls — no further drift.
    rig.store.monthlyDrift(rig.f.base.ctx);
    rig.store.monthlyDrift(rig.f.base.ctx);
    CHECK(rig.store.relationBetween(1, 2)->score == afterFirstMonth);

    // Next month hardens again (the reputation re-written at the new month;
    // a month of lazy decay would otherwise eat the stale grudge first).
    rig.f.base.ctx.now = month1 + 30 * core::DAY_LENGTH_SECONDS;
    justice::changeReputation(leader, kVillageA, -4000, rig.f.base.ctx);
    rig.store.monthlyDrift(rig.f.base.ctx);
    CHECK(rig.store.relationBetween(1, 2)->score == afterFirstMonth - FACTION_RIVALRY_STEP_PER_MONTH);

    // ---- the softening arm -----------------------------------------------------
    FactionRig soft;
    const characterId likedResident = soft.spawnKingdomNpc();
    (void)likedResident;
    soft.adopt();
    const characterId liked = soft.courtLeader();
    soft.f.base.ctx.now     = 30 * core::DAY_LENGTH_SECONDS + 5;
    justice::changeReputation(liked, kVillageA, +4000, soft.f.base.ctx);
    soft.store.monthlyDrift(soft.f.base.ctx);
    CHECK(soft.store.relationBetween(1, 2)->score == FACTION_SEEDED_RIVALRY_SCORE + FACTION_RIVALRY_STEP_PER_MONTH);

    // ---- the determinism arm ---------------------------------------------------
    FactionRig replay;
    const characterId replayResident = replay.spawnKingdomNpc();
    (void)replayResident;
    replay.adopt();
    const characterId replayLeader = replay.courtLeader();
    replay.f.base.ctx.now          = month1;
    justice::changeReputation(replayLeader, kVillageA, -4000, replay.f.base.ctx);
    replay.store.monthlyDrift(replay.f.base.ctx);
    CHECK(replay.store.relationBetween(1, 2)->score == afterFirstMonth); // the same inputs, the same drift
}
