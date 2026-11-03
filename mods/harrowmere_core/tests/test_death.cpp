// tests/test_death.cpp — Task 3.3 pinned tests.
// TestDeath_EnvironmentDebtHalfLevelCost, TestDeath_PvpVictimInstantPenalty,
// TestDeath_NpcKillTransfersXpAndDirtiesPower, TestDeath_KillerNpcGainsXp,
// TestCriminalAct_DuelFalse, TestCriminalAct_NonSapientFalse, TestCriminalAct_SafeZoneAlwaysTrue,
// TestCriminalAct_WarExempt, TestCriminalAct_LethalWarrantExonerates_NonLethalDoesNot
// (+ TestDeath_PlayerVictimOfNpc_NoEvidence — die()'s routing table, 3.3 pitfalls).
#include <doctest.h> // the single implement define lives in main.cpp (task 0.6)

#include "character/species.hpp"
#include "combat/justice_sink.hpp"
#include "combat/death.hpp"
#include "core/constants.hpp"
#include "core/constants_phase2.hpp"
#include "core/math.hpp"
#include "progression/progression.hpp"
#include "test_phase2_fixture.hpp"

namespace
{
// phase2_test already carries `using namespace harrowmere;` (fixture header):
// a second using-directive here makes the qualified name `harrowmere::`
// ambiguous for the TU (transitive lookup reaches the namespace by two paths).
using namespace phase2_test;
} // namespace

TEST_CASE(
    "TestDeath_EnvironmentDebtHalfLevelCost"
)
{
    Fixture f;
    const characterId p = spawnPlayer(f);
    Character &pc       = f.characters.get(p);
    REQUIRE(pc.progression.level == 1);

    pc.progression.availableXp = 100000;
    pc.progression.totalXp     = 100000;
    const std::int64_t before  = pc.progression.availableXp;

    onEnvironmentDeath(pc, f.ctx);

    // A-32: debt = 0.5x the victim's NEXT level cost, through the R4 clamp helper.
    const std::int64_t debt
        = static_cast<std::int64_t>(ENVIRONMENT_DEATH_PENALTY_FACTOR * levelCosts()[pc.progression.level] / SCALE);
    CHECK(debt == static_cast<std::int64_t>(5000 * levelCosts()[1] / SCALE)); // 0.5x level cost
    CHECK(pc.progression.availableXp == applyXpDelta(before, -debt));
    CHECK(pc.progression.availableXp == before - debt); // no clamp engaged here
    CHECK(pc.progression.totalXp == 100000);            // totalXp NEVER decreases
}

TEST_CASE(
    "TestDeath_PvpVictimInstantPenalty"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false); // NOT a safe zone: murder is criminal
    const characterId killer = spawnPlayer(f);
    const characterId victim = spawnPlayer(f);

    Character &vc              = f.characters.get(victim);
    vc.progression.availableXp = 100000;
    vc.progression.totalXp     = 100000;
    vc.body.currentHp          = 1;
    const std::int64_t before  = vc.progression.availableXp;

    f.ctx.now = 777;
    CHECK(applyDamage(DamageEvent{killer, victim, 10, DamageType::Melee}, f.ctx).isOk());

    // A-33: a full level cost, instant — "victim penalties stay instant".
    const std::int64_t loss = static_cast<std::int64_t>(
        PVP_VICTIM_PENALTY_FACTOR * levelCosts()[f.characters.get(victim).progression.level] / SCALE
    );
    CHECK(f.characters.get(victim).progression.availableXp == before - loss);
    CHECK(f.characters.get(victim).progression.totalXp == 100000);

    // Murder evidence recorded AT COMMIT TIME, before the corpse exists.
    REQUIRE(f.justice.crimes.size() == 1);
    CHECK(f.justice.crimes[0].type == CrimeType::MURDER);
    CHECK(f.justice.crimes[0].culprit == killer);
    CHECK(f.justice.crimes[0].victim == victim);
    CHECK(f.justice.crimes[0].worldTime == 777);

    // Corpse staged for step 11 with the victim's (empty) loot; player respawned.
    CHECK(f.corpses.pendingCount() == 1);
    const Character &respawned = f.characters.get(victim);
    CHECK(respawned.body.currentHp == respawned.body.maxHp);
    CHECK(respawned.lastCombatTime == 0);
    CHECK(respawned.body.position.x == f.world.safeSpawn.x); // safe-zone kingdom spawn point
    CHECK(respawned.body.position.z == f.world.safeSpawn.z);
}

TEST_CASE(
    "TestDeath_NpcKillTransfersXpAndDirtiesPower"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId killer = spawnPlayer(f);
    const characterId victim = spawnNpc(f, speciesIdByKey("wolf"), 1, 1, 3); // a wolf: hunting is no crime

    Character &vc                           = f.characters.get(victim);
    vc.progression.level                    = 5;
    vc.body.currentHp                       = 1;
    f.npcs.get(victim).inventory.stacks[42] = 3; // some bag loot

    Character &kc              = f.characters.get(killer);
    kc.progression.totalXp     = 1000;
    kc.progression.availableXp = 1000;
    kc.powerDirty              = false;

    CHECK(applyDamage(DamageEvent{killer, victim, 10, DamageType::Melee}, f.ctx).isOk());

    // A-34/A-35: loss = 2.0x level cost (sunk with the wolf), gain = 0.8x of it.
    const std::uint64_t loss = NPC_VICTIM_PENALTY_FACTOR * levelCosts()[5] / SCALE;
    const std::uint64_t gain = loss * NPC_KILL_XP_GAIN / SCALE;
    CHECK(kc.progression.totalXp == 1000 + gain); // faucet only goes up (R4)
    CHECK(kc.progression.availableXp == 1000 + static_cast<std::int64_t>(gain));
    CHECK(kc.powerDirty);            // onNpcKill marks power dirty (2.5)
    CHECK(f.justice.crimes.empty()); // non-sapient kill: no crime
    CHECK_FALSE(f.characters.exists(victim));
    CHECK_FALSE(f.npcs.contains(victim));
    REQUIRE(f.corpses.pendingCount() == 1); // loot = equipped + bag (v1)
}

TEST_CASE(
    "TestDeath_KillerNpcGainsXp"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    // NPC-vs-NPC: the routing table's onNpcKill path — NPCs gain XP dropped by what
    // they kill (design rule), with no evidence either way.
    const characterId killer = spawnNpc(f, speciesIdByKey("wolf"), 1, 1, 4); // wolf
    const characterId victim = spawnNpc(f, speciesIdByKey("wolf"), 1, 1, 3); // deer-tier wolf

    Character &vc              = f.characters.get(victim);
    vc.progression.level       = 5;
    vc.body.currentHp          = 1;
    Character &kc              = f.characters.get(killer);
    kc.progression.totalXp     = 0;
    kc.progression.availableXp = 0;

    CHECK(applyDamage(DamageEvent{killer, victim, 10, DamageType::Melee}, f.ctx).isOk());

    const std::uint64_t loss = NPC_VICTIM_PENALTY_FACTOR * levelCosts()[5] / SCALE;
    const std::uint64_t gain = loss * NPC_KILL_XP_GAIN / SCALE;
    CHECK(kc.progression.totalXp == gain);
    CHECK(kc.progression.availableXp == static_cast<std::int64_t>(gain));
    CHECK(kc.powerDirty);
    CHECK(f.justice.crimes.empty());
    CHECK(f.justice.assaults.empty());
}

TEST_CASE(
    "TestDeath_PlayerVictimOfNpc_NoEvidence"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId victim = spawnPlayer(f);
    const characterId killer = spawnNpc(f, speciesIdByKey("wolf"), 1, 1, 4); // wolf kills the player

    Character &vc              = f.characters.get(victim);
    vc.progression.availableXp = 100000;
    vc.body.currentHp          = 1;
    const std::int64_t before  = vc.progression.availableXp;

    CHECK(applyDamage(DamageEvent{killer, victim, 10, DamageType::Melee}, f.ctx).isOk());

    // PvP victim penalty applies whichever species killed; evidence does NOT —
    // "only players generate evidence" (die()'s routing table, 3.3 pitfalls).
    const std::int64_t loss = static_cast<std::int64_t>(
        PVP_VICTIM_PENALTY_FACTOR * levelCosts()[f.characters.get(victim).progression.level] / SCALE
    );
    CHECK(f.characters.get(victim).progression.availableXp == before - loss);
    CHECK(f.justice.crimes.empty());
    CHECK(f.justice.assaults.empty());
}

TEST_CASE(
    "TestCriminalAct_DuelFalse"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId culprit             = spawnPlayer(f);
    const characterId victim              = spawnPlayer(f);
    f.characters.get(victim).duelOpponent = culprit; // 2.1's only consent check
    CHECK_FALSE(
        isCriminalAct(CrimeType::MURDER, f.characters.get(culprit), f.characters.get(victim), Position{0, 0, 0}, f.ctx)
    );
}

TEST_CASE(
    "TestCriminalAct_NonSapientFalse"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId culprit = spawnPlayer(f);
    const characterId deer    = spawnNpc(f, speciesIdByKey("wolf"), 1, 1, 3); // hunting creates no crime
    CHECK_FALSE(
        isCriminalAct(CrimeType::MURDER, f.characters.get(culprit), f.characters.get(deer), Position{0, 0, 0}, f.ctx)
    );
}

TEST_CASE(
    "TestCriminalAct_SafeZoneAlwaysTrue"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, true); // safe zone
    const characterId culprit = spawnPlayer(f);
    const characterId victim  = spawnPlayer(f);
    f.factions.atWar          = true; // even war exemption comes LATER
    CHECK(
        isCriminalAct(CrimeType::MURDER, f.characters.get(culprit), f.characters.get(victim), Position{0, 0, 0}, f.ctx)
    );
}

TEST_CASE(
    "TestCriminalAct_WarExempt"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false); // not a safe zone
    const characterId culprit = spawnPlayer(f);
    const characterId victim  = spawnPlayer(f);
    f.factions.atWar          = true;
    CHECK_FALSE(
        isCriminalAct(CrimeType::MURDER, f.characters.get(culprit), f.characters.get(victim), Position{0, 0, 0}, f.ctx)
    );
    // ...and the exemption is faction-bound: peace restores the crime.
    f.factions.atWar = false;
    CHECK(
        isCriminalAct(CrimeType::MURDER, f.characters.get(culprit), f.characters.get(victim), Position{0, 0, 0}, f.ctx)
    );
}

TEST_CASE(
    "TestCriminalAct_LethalWarrantExonerates_NonLethalDoesNot"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId culprit = spawnPlayer(f);
    const characterId victim  = spawnPlayer(f);
    const Character &cu       = f.characters.get(culprit);
    const Character &vi       = f.characters.get(victim);
    const Position pos{0, 0, 0};

    // Lethal warrant on the VICTIM in this territory: execution is licensed.
    f.justice.lethalWarrant = true;
    CHECK_FALSE(isCriminalAct(CrimeType::MURDER, cu, vi, pos, f.ctx));

    // A NON-lethal warrant licenses arrest, never killing: murder still criminal,
    // while the non-lethal crime type it covers is not.
    f.justice.lethalWarrant = false;
    f.justice.warrant       = true;
    CHECK(isCriminalAct(CrimeType::MURDER, cu, vi, pos, f.ctx));
    CHECK_FALSE(isCriminalAct(CrimeType::ASSAULT, cu, vi, pos, f.ctx));

    // No warrant at all: both criminal.
    f.justice.warrant = false;
    CHECK(isCriminalAct(CrimeType::MURDER, cu, vi, pos, f.ctx));
    CHECK(isCriminalAct(CrimeType::ASSAULT, cu, vi, pos, f.ctx));
}
