// tests/test_roles.cpp — Task 3.6 pinned tests.
// TestRoles_GuardDeathSchedulesReplacementAfterDelay, TestRoles_EligiblePromotesLowerRoleFirst,
// TestRoles_NoEligibleSpawnsMigrant, TestRoles_ChildAndNonSapientNeverAssigned,
// TestRoles_OneOpenSeatPerRole.
//
// The delayed closure captures the ROLE KEY and territory — never a reference to the
// dead NPC (3.6 pitfalls). Replacement is per-territory.
#include <doctest.h> // the single implement define lives in main.cpp (task 0.6)

#include "combat/death.hpp"
#include "core/constants.hpp"
#include "core/constants_phase2.hpp"
#include "npc/roles.hpp"
#include "test_phase2_fixture.hpp"

namespace
{
// phase2_test already carries `using namespace harrowmere;` (fixture header):
// a second using-directive here makes the qualified name `harrowmere::`
// ambiguous for the TU (transitive lookup reaches the namespace by two paths).
using namespace phase2_test;
} // namespace

TEST_CASE(
    "TestRoles_GuardDeathSchedulesReplacementAfterDelay"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId guard = spawnNpc(f, 1, 1, 1, 30, "guard");
    const characterId peer  = spawnNpc(f, 1, 1, 1, 30); // eligible roleless adult

    // Kill the guard: hp to 0, one death, one handler (environment-style silent death
    // for an NPC — the critical-role hook still fires).
    f.characters.get(guard).body.currentHp = 0;
    die(f.characters.get(guard), 0, f.ctx);

    CHECK_FALSE(f.characters.exists(guard));
    CHECK(f.roles.hasOpenSeat(1, "guard"));  // seat marked at commit time...
    CHECK(f.npcs.get(peer).roleKey.empty()); // ...but NOT filled yet

    // REPLACEMENT_DELAY (A-48 = 1 game-day) later, the first tick past the due time
    // fills the seat inside its scheduled step.
    f.ctx.now += REPLACEMENT_DELAY;
    f.scheduler.runTick(f.ctx);
    CHECK(f.npcs.get(peer).roleKey == "guard"); // functioning guard again
    CHECK_FALSE(f.roles.hasOpenSeat(1, "guard"));
}

TEST_CASE(
    "TestRoles_EligiblePromotesLowerRoleFirst"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId laborer = spawnNpc(f, 1, 1, 1, 30, "laborer"); // priority 2
    const characterId farmer  = spawnNpc(f, 1, 1, 1, 25, "farmer");  // priority 3

    // The guard seat opens: highest current-role priority wins (farmer 3 > laborer 2).
    CHECK(eligibleNpcExists("guard", 1, f.ctx));
    CHECK(selectEligibleNpc("guard", 1, f.ctx) == farmer);
    assignRole(farmer, "guard", f.ctx);
    CHECK(f.npcs.get(farmer).roleKey == "guard");
    CHECK(f.npcs.get(laborer).roleKey == "laborer");

    // Priority beats age: the laborer (priority 2) outranks both roleless peers (0).
    const characterId young = spawnNpc(f, 1, 1, 1, 20);
    const characterId old   = spawnNpc(f, 1, 1, 1, 45);
    CHECK(selectEligibleNpc("healer", 1, f.ctx) == laborer);
    assignRole(laborer, "healer", f.ctx);
    // The promoted healer (priority 5) is still eligible for vendor (5 < 6) and
    // outranks the roleless peers — priority-first again.
    CHECK(selectEligibleNpc("vendor", 1, f.ctx) == laborer);
    assignRole(laborer, "vendor", f.ctx);
    // Only roleless peers remain: the age tiebreak decides (oldest first, R7).
    CHECK(selectEligibleNpc("healer", 1, f.ctx) == old);
    assignRole(old, "healer", f.ctx);
    CHECK(f.npcs.get(young).roleKey.empty()); // each seat took one NPC, not the pool
}

TEST_CASE(
    "TestRoles_NoEligibleSpawnsMigrant"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false, 2, 10); // template min 2: migrants allowed
    const characterId guard = spawnNpc(f, 1, 1, 1, 30, "guard");
    spawnNpc(f, 1, 1, 1, 8, CHILD_ROLE_KEY); // a child: never eligible
    spawnNpc(f, 1, 1, 1, 8, CHILD_ROLE_KEY);
    CHECK(f.world.territoryPopulation(1) == 3);

    f.characters.get(guard).body.currentHp = 0;
    die(f.characters.get(guard), 0, f.ctx);
    CHECK(f.roles.hasOpenSeat(1, "guard"));

    // No eligible adult -> the replacement migrates a fresh NPC from the frontier
    // pool (1.5's seed path), marked migrated = true, already in the role.
    const std::size_t before = f.npcs.all().size();
    f.ctx.now += REPLACEMENT_DELAY;
    f.scheduler.runTick(f.ctx);

    CHECK(f.npcs.all().size() == before + 1);
    CHECK_FALSE(f.roles.hasOpenSeat(1, "guard"));
    bool foundMigrant = false;
    for (const auto &[id, npc] : f.npcs.all())
    {
        if (npc.migrated)
        {
            foundMigrant = true;
            CHECK(npc.roleKey == "guard");
        }
    }
    CHECK(foundMigrant);
}

TEST_CASE(
    "TestRoles_ChildAndNonSapientNeverAssigned"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false, 2, 10);
    const characterId guard = spawnNpc(f, 1, 1, 1, 30, "guard");
    const characterId kid   = spawnNpc(f, 1, 1, 1, 8, CHILD_ROLE_KEY);
    const characterId wolf  = spawnNpc(f, speciesIdByKey("wolf"), 1, 1, 20); // adult but non-sapient

    CHECK_FALSE(eligibleNpcExists("guard", 1, f.ctx));
    CHECK(selectEligibleNpc("guard", 1, f.ctx) == 0);

    f.characters.get(guard).body.currentHp = 0;
    die(f.characters.get(guard), 0, f.ctx);
    f.ctx.now += REPLACEMENT_DELAY;
    f.scheduler.runTick(f.ctx); // falls through to the migrant path

    CHECK(f.npcs.get(kid).roleKey == CHILD_ROLE_KEY); // never promoted
    CHECK(f.npcs.get(wolf).roleKey.empty());          // animals never
}

TEST_CASE(
    "TestRoles_OneOpenSeatPerRole"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId guardA = spawnNpc(f, 1, 1, 1, 30, "guard");
    const characterId guardB = spawnNpc(f, 1, 1, 1, 32, "guard");
    const characterId peerA  = spawnNpc(f, 1, 1, 1, 30);
    const characterId peerB  = spawnNpc(f, 1, 1, 1, 31);

    // Both guards die the same tick: ONE open seat per role per territory, and only
    // one delayed replacement (population/time gating, 3.6).
    f.characters.get(guardA).body.currentHp = 0;
    die(f.characters.get(guardA), 0, f.ctx);
    f.characters.get(guardB).body.currentHp = 0;
    die(f.characters.get(guardB), 0, f.ctx);

    CHECK(f.roles.openSeatsOf(1).size() == 1);
    CHECK(f.roles.openSeatsOf(1).count("guard") == 1);

    f.ctx.now += REPLACEMENT_DELAY;
    f.scheduler.runTick(f.ctx);

    // Exactly one promotion: the seat closed, the second eligible peer stayed put.
    int promotions = 0;
    for (const characterId id : {peerA, peerB})
    {
        if (f.npcs.get(id).roleKey == "guard")
            ++promotions;
    }
    CHECK(promotions == 1);
    CHECK_FALSE(f.roles.hasOpenSeat(1, "guard"));

    // A different role's seat coexists per-territory (a village guard death never
    // touches the capital's captain).
    const characterId vendor                = spawnNpc(f, 1, 1, 1, 40, "vendor");
    f.characters.get(vendor).body.currentHp = 0;
    die(f.characters.get(vendor), 0, f.ctx);
    CHECK(f.roles.openSeatsOf(1).size() == 1);
    CHECK(f.roles.openSeatsOf(1).count("vendor") == 1);
}
