// test_player_view_fill.cpp — Task 2.8's pinned test suite.
//
// The card's Tests block (the assertions verbatim, the names following the
// tree's Test<System>_<Behavior>_<Expected> convention, §3.5):
//
//   * a fresh store with an adopted counter -> the player's id ==
//     nextCharacterId (TestPlayerViewFill_AdoptedCounter_PlayerIdContinuesPlan);
//   * the filled view carries level 1, maxHp == BASE_HP + level * HP_PER_LEVEL
//     (A-42/A-43 -> 120), currentHp == maxHp (a fresh player is never 0/0),
//     currency == STARTER_CURRENCY (A-41), nextLevelCost == levelCosts[level]
//     (TestPlayerViewFill_FreshPlayer_CarriesPinnedStartingState);
//   * the regression pin for the ids: run the 3.4 bootstrap over an adopted
//     store and assert the store holds every plan id plus the player (no
//     `createNpc rejected` line in the captured log)
//     (TestPlayerViewFill_BootstrapOverAdoptedStore_NoRejectedLines).
//
// Plus the fill's remaining spec surface: the effectiveness/cap/cooldown/
// appearance-cost fields and the derived booleans (inCombat per A-4, inEvent
// from eventId), and the zero-value rule (fields with no data keep zero).
//
// Test harness: doctest v2.4.12 (vendored per task 0.6); no main here -
// tests/main.cpp owns DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "logging_capability.hpp" // engine::LoggerCapability — the log-capture seam (task 0.5)

#include "bridge/player_view.hpp"   // THE fill (the free function under test)
#include "character/appearance.hpp" // setCooldown helper rides status.hpp; A-22/A-23 constants
#include "character/status.hpp"     // setCooldown (2.1's cooldown slots)
#include "character/store.hpp"      // CharacterStore + adoptWorldGen (the runtime fix)
#include "core/constants.hpp"       // A-41/A-42/A-43, BASE_LEVEL_COST (A-7/A-8)
#include "core/enums.hpp"           // CooldownType, StatType, SkillType
#include "core/log.hpp"             // log::install/uninstall — the log-capture seam
#include "core/types.hpp"           // Position, characterId
#include "core/world_context.hpp"   // WorldContext (bootstrapPopulation's bundle)
#include "npc/lifecycle.hpp"        // bootstrapPopulation (3.4's spawn path)
#include "world/worldgen.hpp"       // NpcSeed (the plan)

#include "test_phase2_fixture.hpp" // the Phase-2 harness: FakeWorld + shippedRegistry

namespace
{

// A recording LoggerCapability: the id-collision regression asserts on the
// LOG (the card's exact words: "no `createNpc rejected` line in the captured
// log"), so the store's WARN path must actually run and be captured. The
// default INFO floor keeps WARN compiled in (log.hpp's level table).
struct CapturingLogger final : public engine::LoggerCapability
{
    std::vector<std::string> lines;

    void logMessage(
        engine::LogLevel, const std::string &message
    ) override
    {
        lines.push_back(message);
    }
};

harrowmere::world::NpcSeed makePlanSeed(
    harrowmere::characterId id
)
{
    harrowmere::world::NpcSeed seed{};
    seed.id            = id; // plan ids are identity (R10) — drawn in strict plan order
    seed.species       = 1;  // human
    seed.roleKey       = "farmer";
    seed.ageYears      = 25;
    seed.integrity     = 5000;
    seed.level         = 1;
    seed.homeTerritory = 7;
    seed.homeRegion    = 40;
    return seed;
}

} // namespace

// ============================================================================
// Pinned test 1 — the adopted counter: the player draws plan_end + 1, never
// the generator's FIRST_ID default (the runtime defect task 2.8 repairs).
// ============================================================================
TEST_CASE(
    "TestPlayerViewFill_AdoptedCounter_PlayerIdContinuesPlan"
)
{
    harrowmere::character::CharacterStore store;
    const harrowmere::Position spawn{12.0, 0.0, 34.0};
    store.adoptWorldGen(5, spawn); // worldgen drew ids 1..4 for the plan; 5 is next

    const harrowmere::characterId playerId = store.createPlayer(harrowmere::PlayerCreation{"wanderer", {}, ""});
    CHECK(playerId == 5); // == nextCharacterId: the plan's ids stay the plan's (R10)

    // And the sim-side player record spawns at the adopted spawn point (the
    // zero-default defect's second half).
    CHECK(store.get(playerId).body.position.x == doctest::Approx(12.0));
    CHECK(store.get(playerId).body.position.z == doctest::Approx(34.0));
}

// ============================================================================
// Pinned test 2 — the filled view of a FRESH player carries the pinned
// starting state (never a placeholder zero a store field could fill).
// ============================================================================
TEST_CASE(
    "TestPlayerViewFill_FreshPlayer_CarriesPinnedStartingState"
)
{
    harrowmere::character::CharacterStore store;
    const harrowmere::Position spawn{5.0, 0.0, 5.0};
    store.adoptWorldGen(9, spawn);
    const harrowmere::characterId playerId = store.createPlayer(harrowmere::PlayerCreation{"wanderer", {}, ""});

    constexpr std::uint64_t kWorldTime     = 10u * 3600u; // the A-129 epoch open
    const harrowmere::views::PlayerView pv = harrowmere::bridge::buildPlayerView(
        store.get(playerId),
        store.playerCreation(),
        phase2_test::shippedRegistry(),
        spawn, // the controller's authoritative transform
        kWorldTime,
        0,    // realTime
        false // afk
    );

    CHECK(pv.level == 1);
    CHECK(pv.maxHp == harrowmere::BASE_HP + static_cast<std::uint64_t>(pv.level) * harrowmere::HP_PER_LEVEL);
    CHECK(pv.maxHp == 120);                             // A-42/A-43 at level 1 — the HUD's pinned reading
    CHECK(pv.currentHp == pv.maxHp);                    // a fresh player is never 0/0
    CHECK(pv.currency == harrowmere::STARTER_CURRENCY); // A-41
    CHECK(pv.nextLevelCost == 115);                     // levelCosts[1]: the first purchase (DEVIATIONS 2026-10-31)
    CHECK(pv.totalXp == 0);
    CHECK(pv.availableXp == 0);
    CHECK(pv.name == "wanderer"); // the creation record's name

    // The caps (A-13..A-16 at level 1: 10 + 1/5 = 10) and the appearance cost
    // (A-22/A-23: 50 + 10 * 1).
    CHECK(pv.statCap == 10);
    CHECK(pv.skillCap == 10);
    CHECK(pv.appearanceCostNext == 60);

    // Zero-value rules: a fresh player has bought nothing and armed nothing.
    for (std::size_t i = 0; i < pv.stats.size(); ++i)
    {
        CHECK(pv.stats[i] == 0);
        CHECK(pv.statEffectiveness[i] == 0);
    }
    for (std::size_t i = 0; i < pv.skills.size(); ++i)
    {
        CHECK(pv.skills[i] == 0);
        CHECK(pv.skillEffectiveness[i] == 0);
    }
    for (std::size_t i = 0; i < pv.cooldowns.size(); ++i)
    {
        CHECK(pv.cooldowns[i] == 0); // 0 = expired (the panels compare against worldTime)
    }
    CHECK(pv.cachedPower == 0); // the 2.5 cache has not run — the store's honest zero
    CHECK(pv.peakPower == 0);
    CHECK(pv.inCombat == false);
    CHECK(pv.inEvent == false);

    // The controller slice rides through unchanged.
    CHECK(pv.worldTime == kWorldTime);
    CHECK(pv.afk == false);
    CHECK(pv.position.x == doctest::Approx(spawn.x));
    CHECK(pv.position.z == doctest::Approx(spawn.z));
}

// ============================================================================
// The fill's remaining spec surface: effectiveness, caps, cooldowns by
// ordinal, the appearance cost, and the derived booleans.
// ============================================================================
TEST_CASE(
    "TestPlayerViewFill_FillsEffectivenessCooldownsCostsAndDerivedStatuses"
)
{
    harrowmere::character::CharacterStore store;
    const harrowmere::Position spawn{1.0, 0.0, 1.0};
    store.adoptWorldGen(harrowmere::IdGenerator::FIRST_ID, spawn);
    const harrowmere::characterId playerId = store.createPlayer(harrowmere::PlayerCreation{"tester", {}, ""});

    constexpr std::uint64_t kNow  = 10u * 3600u;
    harrowmere::Character &player = store.get(playerId);

    // Bought state: three STR points (human multiplier 1.0 -> effectiveness 3),
    // an armed APPEARANCE cooldown, fresh combat (A-4: within 30 game-seconds)
    // and an event membership.
    player.progression.stats[static_cast<std::size_t>(harrowmere::StatType::STRENGTH)] = 3;
    player.progression.skills[static_cast<std::size_t>(harrowmere::SkillType::MELEE)]  = 2;
    player.cooldowns[static_cast<std::size_t>(harrowmere::CooldownType::APPEARANCE)]   = kNow + 12345;
    player.lastCombatTime = kNow - 10; // inside A-4's 30-game-second window
    player.eventId        = 7;

    const harrowmere::views::PlayerView pv = harrowmere::bridge::buildPlayerView(
        player, store.playerCreation(), phase2_test::shippedRegistry(), spawn, kNow, 0, false
    );

    CHECK(pv.statEffectiveness[static_cast<std::size_t>(harrowmere::StatType::STRENGTH)] == 3);
    CHECK(pv.skillEffectiveness[static_cast<std::size_t>(harrowmere::SkillType::MELEE)] == 2);
    CHECK(pv.cooldowns[static_cast<std::size_t>(harrowmere::CooldownType::APPEARANCE)] == kNow + 12345);
    CHECK(pv.cooldowns[static_cast<std::size_t>(harrowmere::CooldownType::RESPEC_STAT)] == 0);
    CHECK(pv.inCombat == true); // A-4 from lastCombatTime
    CHECK(pv.inEvent == true);  // eventId != 0
    CHECK(pv.appearanceCostNext == 60);

    // Outside the A-4 window the flag clears (the doc's derivation, not a
    // stored bit — D-CHAR-1).
    const harrowmere::views::PlayerView cooled = harrowmere::bridge::buildPlayerView(
        player,
        store.playerCreation(),
        phase2_test::shippedRegistry(),
        spawn,
        kNow + harrowmere::COMBAT_TIMEOUT_S,
        0,
        false
    );
    CHECK(cooled.inCombat == false);
    CHECK(cooled.inEvent == true); // event membership is not combat-windowed
}

// ============================================================================
// Pinned test 3 — the id-collision regression: the 3.4 bootstrap over an
// ADOPTED store holds every plan id plus the player, and the captured log
// carries no `createNpc rejected` line (the exact defect every new game
// logged before the fix).
// ============================================================================
TEST_CASE(
    "TestPlayerViewFill_BootstrapOverAdoptedStore_NoRejectedLines"
)
{
    phase2_test::Fixture fixture;

    // The population plan: ids drawn in strict plan order 1..12 (worldgen's
    // per-seed draw contract).
    constexpr harrowmere::characterId kPlanLast = 12;
    for (harrowmere::characterId id = 1; id <= kPlanLast; ++id)
    {
        fixture.world.plannedSeeds.push_back(makePlanSeed(id));
    }

    CapturingLogger logger;
    harrowmere::log::install(&logger); // capture the store's WARN/INFO lines

    // The runtime wiring under test: adopt BEFORE the first create (task
    // 2.8's repair — the counter lands past the plan, the player draws
    // plan_end + 1), then the 3.4 spawn path over the same store.
    const harrowmere::Position spawn{3.0, 0.0, 3.0};
    fixture.characters.adoptWorldGen(kPlanLast + 1, spawn);
    const harrowmere::characterId playerId
        = fixture.characters.createPlayer(harrowmere::PlayerCreation{"wanderer", {}, ""});
    fixture.world.playerId = playerId;

    fixture.ctx.now = 10u * 3600u;
    harrowmere::bootstrapPopulation(fixture.ctx);

    harrowmere::log::uninstall();

    // Every plan id plus the player exist — the world is not one person short.
    for (harrowmere::characterId id = 1; id <= kPlanLast; ++id)
    {
        MESSAGE("plan id ", static_cast<unsigned long long>(id), " exists: ", fixture.characters.exists(id));
        CHECK(fixture.characters.exists(id));
    }
    CHECK(fixture.characters.exists(playerId));
    CHECK(playerId == kPlanLast + 1);
    CHECK(fixture.characters.population() == kPlanLast + 1);

    // The bootstrap carries the seed's integrity draw into the NpcState (the
    // 8.4 balance probe's find: the worldgen draw 4 never reached the
    // bootstrap build, leaving every living witness's report weight at zero
    // — DEVIATIONS.md row 2026-11-02/8.4).
    for (harrowmere::characterId id = 1; id <= kPlanLast; ++id)
    {
        if (fixture.npcs.contains(id))
        {
            CHECK(fixture.npcs.get(id).integrity == 5000);
        }
    }

    // And the captured log is clean of the rejection line.
    std::size_t rejectedLines = 0;
    for (const std::string &line : logger.lines)
    {
        if (line.find("rejected") != std::string::npos)
        {
            ++rejectedLines;
            MESSAGE("captured rejection: ", line);
        }
    }
    CHECK(rejectedLines == 0);
}
