// tests/test_combat.cpp — Task 3.2 pinned tests.
// TestDamage_ClampsToHp, TestDamage_StampsLastCombatBothSides, TestDamage_InvulnerableRejected,
// TestDamage_AssaultHookFiresOnUnconsentedSapient, TestDamage_NoAssaultForDuel,
// TestDamage_EventHookRoutesToContribution, TestCombat_HitChanceBounded,
// TestSpecies_GooBeastsReduceDamage (+ TestCombat_RawDamageFloorAtMin).
//
// Hook order inside applyDamage is pinned (3.2 pitfalls): evidence before death.
#include <doctest.h> // the single implement define lives in main.cpp (task 0.6)

#include "character/species.hpp"
#include "character/status.hpp"
#include "combat/attacks.hpp"
#include "combat/damage.hpp"
#include "core/constants.hpp"
#include "core/constants_phase2.hpp"
#include "data/registry.hpp"
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
    "TestDamage_ClampsToHp"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId attacker = spawnPlayer(f);
    const characterId victim   = spawnNpc(f, 1, 1, 1, 30); // human

    // Sub-lethal: applied == amount, hp reduced exactly.
    Character &v     = f.characters.get(victim);
    v.body.currentHp = 50;
    CHECK(applyDamage(DamageEvent{attacker, victim, 30, DamageType::Melee}, f.ctx).isOk());
    CHECK(f.characters.get(victim).body.currentHp == 20);

    // Lethal: applied clamps to currentHp (10), not the raw 1000 — visible through
    // the assault record (hook 4 runs BEFORE death routes, 3.2 pitfalls).
    Character &v2     = f.characters.get(victim);
    v2.body.currentHp = 10;
    REQUIRE(applyDamage(DamageEvent{attacker, victim, 1000, DamageType::Melee}, f.ctx).isOk());
    REQUIRE(f.justice.assaults.size() == 2);
    CHECK(std::get<2>(f.justice.assaults.back()) == 10); // clamped, not 1000
    // Victim is gone (died), the killing blow was recorded at commit time.
    CHECK_FALSE(f.characters.exists(victim));
}

TEST_CASE(
    "TestDamage_StampsLastCombatBothSides"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId attacker = spawnPlayer(f);
    const characterId victim   = spawnNpc(f, speciesIdByKey("wolf"), 1, 1, 3); // non-sapient: no justice noise

    f.ctx.now = 500;
    (void)applyDamage(DamageEvent{attacker, victim, 5, DamageType::Melee}, f.ctx);
    CHECK(f.characters.get(attacker).lastCombatTime == 500);
    CHECK(f.characters.get(victim).lastCombatTime == 500);
}

TEST_CASE(
    "TestDamage_InvulnerableRejected"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId attacker      = spawnPlayer(f);
    const characterId victim        = spawnNpc(f, 1, 1, 1, 30);
    f.npcs.get(victim).invulnerable = true;
    f.ctx.now                       = 100;

    CHECK(
        applyDamage(DamageEvent{attacker, victim, 50, DamageType::Melee}, f.ctx).error() == DamageError::Invulnerable
    );
    CHECK(f.characters.get(victim).body.currentHp == 100); // untouched
    CHECK(f.characters.get(victim).lastCombatTime == 0);   // never stamped
    CHECK(f.justice.assaults.empty());                     // no evidence either
}

TEST_CASE(
    "TestDamage_AssaultHookFiresOnUnconsentedSapient"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId attacker = spawnPlayer(f);
    const characterId victim   = spawnNpc(f, 1, 1, 1, 30); // sapient, no duel, no arena

    f.ctx.now = 42;
    CHECK(applyDamage(DamageEvent{attacker, victim, 7, DamageType::Melee}, f.ctx).isOk());
    REQUIRE(f.justice.assaults.size() == 1);
    CHECK(std::get<0>(f.justice.assaults[0]) == attacker);
    CHECK(std::get<1>(f.justice.assaults[0]) == victim);
    CHECK(std::get<2>(f.justice.assaults[0]) == 7);
}

TEST_CASE(
    "TestDamage_NoAssaultForDuel"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId attacker = spawnPlayer(f);
    const characterId victim   = spawnNpc(f, 1, 1, 1, 30);

    // 2.7's consent channel: mutual duelOpponent set -> consentsToPvp true (2.1).
    f.characters.get(victim).duelOpponent   = attacker;
    f.characters.get(attacker).duelOpponent = victim;

    CHECK(applyDamage(DamageEvent{attacker, victim, 9, DamageType::Melee}, f.ctx).isOk());
    CHECK(f.justice.assaults.empty()); // a lawful duel generates zero assault evidence
    CHECK(f.characters.get(victim).body.currentHp == 91);
}

TEST_CASE(
    "TestDamage_EventHookRoutesToContribution"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId attacker = spawnPlayer(f);
    const characterId victim   = spawnNpc(f, speciesIdByKey("wolf"), 1, 1, 3); // non-sapient: justice stays quiet

    f.characters.get(attacker).eventId = 7;
    CHECK(applyDamage(DamageEvent{attacker, victim, 13, DamageType::Melee}, f.ctx).isOk());
    REQUIRE(f.events.damages.size() == 1);
    CHECK(f.events.damages[0].source == attacker);
    CHECK(f.events.damages[0].target == victim);
    CHECK(f.events.damages[0].amount == 13); // the APPLIED amount (5.3's contribution)

    // Either side in an event routes; target-side membership alone does too.
    f.events.damages.clear();
    f.characters.get(attacker).eventId = 0;
    f.characters.get(victim).eventId   = 3;
    (void)applyDamage(DamageEvent{attacker, victim, 4, DamageType::Melee}, f.ctx);
    CHECK(f.events.damages.size() == 1);
}

TEST_CASE(
    "TestCombat_HitChanceBounded"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId a = spawnPlayer(f);
    // SP has exactly one player (D-STORE-1: a second createPlayer returns the
    // same id) — the defender is an NPC (human: AGILITY multiplier 1.0).
    const characterId d = spawnNpc(f, speciesIdByKey("human"), 1, 1, 30);
    Character &atk      = f.characters.get(a);
    Character &def      = f.characters.get(d);

    // Equal effectiveness: SCALE/2 exactly (A-28 verbatim).
    CHECK(hitChance(shippedRegistry(), atk, def) == SCALE / 2);

    // The agility term is (effA - effD) * 100 / SCALE — signed delta (R1), integer math.
    atk.progression.stats[static_cast<std::size_t>(StatType::AGILITY)] = 10000; // eff = 50 + isqrt(9950) = 149
    CHECK(hitChance(shippedRegistry(), atk, def) == SCALE / 2 + 149 * 100 / static_cast<int>(SCALE)); // 5001

    // Bounds hold at every extreme the caps allow (clamp [1000, 9000], A-28).
    def.progression.stats[static_cast<std::size_t>(StatType::AGILITY)] = 10000;
    CHECK(hitChance(shippedRegistry(), atk, def) >= HIT_CHANCE_MIN);
    CHECK(hitChance(shippedRegistry(), atk, def) <= HIT_CHANCE_MAX);
    def.progression.stats[static_cast<std::size_t>(StatType::AGILITY)] = 1000000;
    atk.progression.stats[static_cast<std::size_t>(StatType::AGILITY)] = 0;
    const std::uint64_t chance                                         = hitChance(shippedRegistry(), atk, def);
    CHECK(chance >= HIT_CHANCE_MIN);
    CHECK(chance <= HIT_CHANCE_MAX);
}

TEST_CASE(
    "TestCombat_RawDamageFloorAtMin"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId a = spawnPlayer(f);
    const characterId d = spawnNpc(f, speciesIdByKey("human"), 1, 1, 30); // SP: one player
    Character &atk      = f.characters.get(a);
    Character &def      = f.characters.get(d);

    const data::ItemData *sword = shippedRegistry().itemById(1);
    REQUIRE(sword != nullptr);

    // A-29/A-30 verbatim with the iron sword (weaponFactor 1.5 on the per-mille grid):
    // str eff 0 * 1500/1000 - def eff 0 * 800/1000 = 0 -> floor MIN_DAMAGE.
    CHECK(rawDamage(shippedRegistry(), atk, def, sword) == MIN_DAMAGE);

    // STR 149 eff: 149*1500/1000 = 223; the fresh defender's DEF eff is 0.
    atk.progression.stats[static_cast<std::size_t>(StatType::STRENGTH)] = 10000;
    CHECK(rawDamage(shippedRegistry(), atk, def, sword) == 149 * 1500 / 1000 - 0);
    def.progression.stats[static_cast<std::size_t>(StatType::DEFENCE)] = 10000; // DEF eff 149: 149*800/1000 = 119
    CHECK(rawDamage(shippedRegistry(), atk, def, sword) == 149 * 1500 / 1000 - 149 * 800 / 1000);

    // Unarmed weapon factor (A-29's 800) applies when no weapon is passed.
    def.progression.stats[static_cast<std::size_t>(StatType::DEFENCE)] = 0;
    CHECK(rawDamage(shippedRegistry(), atk, def, nullptr) == 149 * WEAPON_FACTOR_UNARMED / 1000);
}

TEST_CASE(
    "TestSpecies_GooBeastsReduceDamage"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId goo = spawnNpc(f, speciesIdByKey("goo_beast"), 1, 1, 10);
    Character &c          = f.characters.get(goo);
    c.body.maxHp          = 500;
    c.body.currentHp      = 500;

    // 2.2's pinned damageTakenMultiplier (goo 2500) applies BEFORE clamping (pipeline
    // step 3): 1000 raw -> 250 applied.
    CHECK(applyDamage(DamageEvent{0, goo, 1000, DamageType::Exposure}, f.ctx).isOk());
    CHECK(f.characters.get(goo).body.currentHp == 250);

    // Human baseline: no reduction (kept sub-lethal so the store keeps the body).
    const characterId human = spawnNpc(f, 1, 1, 1, 10);
    Character &h            = f.characters.get(human);
    h.body.maxHp            = 1500;
    h.body.currentHp        = 1500;
    (void)applyDamage(DamageEvent{0, human, 1000, DamageType::Exposure}, f.ctx);
    CHECK(f.characters.get(human).body.currentHp == 500); // full 1000 applied
}
