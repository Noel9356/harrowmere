// tests/test_npc_lifecycle.cpp — Task 3.4 pinned tests.
// TestNpc_AgeAdvancesAndDiesAtLifespan, TestNpc_ChildRoleUntilAdult,
// TestNpc_NeedsDecayGenerateIntents, TestNpc_MigrationRateAndPullMath,
// TestNpc_NaturalDeathNoEvidence, TestNpc_CriticalDeathTriggersReplacement
// (+ TestNpc_PopulationBoundedTwoGameYears — the Done-when soak precursor).
//
// Tests drive game time directly (D-TICK-1): one day = one lifecycleDailySweep with
// ctx.now advanced by DAY_LENGTH_SECONDS.
#include <doctest.h> // the single implement define lives in main.cpp (task 0.6)

#include <algorithm>

#include "character/species.hpp"
#include "core/constants.hpp"
#include "core/constants_phase2.hpp"
#include "npc/lifecycle.hpp"
#include "npc/roles.hpp"
#include "progression/progression.hpp"
#include "test_phase2_fixture.hpp"

namespace
{
// phase2_test already carries `using namespace harrowmere;` (fixture header):
// a second using-directive here makes the qualified name `harrowmere::`
// ambiguous for the TU (transitive lookup reaches the namespace by two paths).
using namespace phase2_test;
constexpr std::uint64_t kDay = core::DAY_LENGTH_SECONDS;
} // namespace

TEST_CASE(
    "TestNpc_AgeAdvancesAndDiesAtLifespan"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);

    // A-5 extends the species row's lifespan with level: lifespan * (SCALE +
    // level*100) / SCALE (level 1 -> +1%, level 5 -> +5%). The expectations read
    // the shipped human row (the tests are the data's customers).
    const std::uint64_t humanLifespan         = shippedRegistry().species(speciesIdByKey("human")).lifespan_years;
    const characterId elder                   = spawnNpc(f, speciesIdByKey("human"), 1, 1, humanLifespan - 1);
    const characterId hardy                   = spawnNpc(f, speciesIdByKey("human"), 1, 1, humanLifespan - 1);
    f.characters.get(hardy).progression.level = 5; // lifespan -> +5%

    CHECK(effectiveLifespan(shippedRegistry(), f.characters.get(elder)) == humanLifespan);
    CHECK(effectiveLifespan(shippedRegistry(), f.characters.get(hardy)) == humanLifespan * (SCALE + 5 * 100) / SCALE);

    // A year earlier: both alive.
    lifecycleDailySweep(f.ctx);
    CHECK(f.npcs.contains(elder));
    CHECK(f.npcs.contains(hardy));

    // Past the elder's lifespan, the level-extended hardy survives.
    f.ctx.now += 1 * kDay;
    lifecycleDailySweep(f.ctx);
    for (int i = 0; i < 365; ++i)
    {
        f.ctx.now += kDay;
        lifecycleDailySweep(f.ctx);
    }
    CHECK_FALSE(f.npcs.contains(elder)); // silent removal
    CHECK(f.npcs.contains(hardy));       // level-extended lifespan

    // Past the level-extended lifespan: hardy goes too.
    for (int i = 0; i < 365 * 5 + 2; ++i)
    {
        f.ctx.now += kDay;
        lifecycleDailySweep(f.ctx);
    }
    CHECK_FALSE(f.npcs.contains(hardy));
}

TEST_CASE(
    "TestNpc_ChildRoleUntilAdult"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId kid = spawnNpc(f, 1, 1, 1, 15, CHILD_ROLE_KEY); // A-44

    lifecycleDailySweep(f.ctx); // age 15: still a child
    CHECK(f.npcs.get(kid).roleKey == CHILD_ROLE_KEY);

    f.ctx.now += 366 * kDay; // the 16th year begins (ageYears truncates at 16)
    lifecycleDailySweep(f.ctx);
    CHECK(f.npcs.get(kid).roleKey.empty()); // adult: roleless until the role market (3.6)
}

TEST_CASE(
    "TestNpc_NeedsDecayGenerateIntents"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId id = spawnNpc(f, 1, 1, 1, 30);
    NpcState &npc        = f.npcs.get(id);

    // A-37 decay is hourly and carry-safe: a full hour drops hunger by exactly 120.
    applyNeedsDecay(npc, 3600);
    CHECK(npc.needs.hunger == SCALE - NEED_DECAY_HUNGER_PER_HOUR);
    applyNeedsDecay(npc, 3599); // carry holds the remainder...
    CHECK(npc.needs.hunger == SCALE - NEED_DECAY_HUNGER_PER_HOUR);
    applyNeedsDecay(npc, 1); // ...and the next second pays it
    CHECK(npc.needs.hunger == SCALE - 2 * NEED_DECAY_HUNGER_PER_HOUR);

    // Crisis threshold (need < 2500) generates the matching intent. The subject must
    // be NEAR-tier: per-tick work never touches FAR NPCs (3.5's contract).
    f.npcs.get(id).tier  = LodTier::NEAR;
    f.npcs.get(id).needs = Needs{}; // all SCALE: no crisis
    f.ctx.lod->activeNear.push_back(id);
    npcPerTickWork(f.ctx);
    CHECK(f.npcs.get(id).currentIntent.kind == NpcIntentKind::None);

    f.npcs.get(id).needs.hunger = 2000;
    npcPerTickWork(f.ctx);
    CHECK(f.npcs.get(id).currentIntent.kind == NpcIntentKind::Eat);

    f.npcs.get(id).needs        = Needs{};
    f.npcs.get(id).needs.social = 2000;
    const characterId buddy     = spawnNpc(f, 1, 1, 1, 30);
    f.npcs.get(buddy).tier      = LodTier::NEAR;
    f.ctx.lod->activeNear.push_back(buddy);
    f.npcs.get(id).affinity[buddy] = 100;
    npcPerTickWork(f.ctx);
    CHECK(f.npcs.get(id).currentIntent.kind == NpcIntentKind::Socialize);
    CHECK(f.npcs.get(id).currentIntent.target == buddy);

    // Eat resolves toward a NEAR vendor when one is active.
    f.npcs.get(id).needs        = Needs{};
    f.npcs.get(id).needs.hunger = 2000;
    const characterId vendor    = spawnNpc(f, 1, 1, 1, 40, "vendor");
    f.npcs.get(vendor).tier     = LodTier::NEAR;
    f.ctx.lod->activeNear.push_back(vendor);
    npcPerTickWork(f.ctx);
    CHECK(f.npcs.get(id).currentIntent.kind == NpcIntentKind::Eat);
    CHECK(f.npcs.get(id).currentIntent.target == vendor);
}

TEST_CASE(
    "TestNpc_MigrationRateAndPullMath"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);           // home
    f.addTerritory(2, 500.0, 0.0, 2, false, 0, 100); // destination with room
    const characterId id = spawnNpc(f, 1, 1, 1, 30);
    NpcState &npc        = f.npcs.get(id);
    Character &c         = f.characters.get(id);

    // pull = clamp(pressure + bias + openings - homeAffinity + climatePull, 0, 3*SCALE) / 3
    npc.needs = Needs{}; // satisfied: pressure 0
    npc.affinity.clear();
    f.world.fakeCultureBias = 0;
    f.world.climatePull     = 0;
    CHECK(migrationPull(npc, c, 2, f.ctx) == SCALE / 3); // openings alone

    npc.needs.hunger = npc.needs.rest = npc.needs.wealth = npc.needs.social = 0; // pressure SCALE
    CHECK(migrationPull(npc, c, 2, f.ctx) == 2 * SCALE / 3);

    npc.needs               = Needs{};
    f.world.fakeCultureBias = static_cast<std::int64_t>(SCALE); // loves the destination culture
    CHECK(migrationPull(npc, c, 2, f.ctx) == 2 * SCALE / 3);

    f.world.fakeCultureBias = 0;
    npc.affinity[999]       = -static_cast<std::int64_t>(SCALE); // hates home
    CHECK(migrationPull(npc, c, 2, f.ctx) == 2 * SCALE / 3);

    npc.affinity.clear();
    f.world.climatePull = MIGRATION_WINTER_PULL; // 3.8's frozen hook (+1000)
    CHECK(migrationPull(npc, c, 2, f.ctx) == (SCALE + MIGRATION_WINTER_PULL) / 3);

    // The clamp: everything maxed pins at the SCALE ceiling.
    npc.needs.hunger = npc.needs.rest = npc.needs.wealth = npc.needs.social = 0;
    f.world.fakeCultureBias                                                 = static_cast<std::int64_t>(SCALE);
    CHECK(migrationPull(npc, c, 2, f.ctx) == SCALE);
    f.world.fakeCultureBias = 0;
    f.world.climatePull     = 0;

    // The roll: pct = 3000 x pull / SCALE, one draw on the "migration" stream; the
    // outcome is pinned by learning the draw first (same seed -> same draw, R6).
    // Needs sit at 0, so the sweep's FAR decay cannot shift the pull either.
    npc.needs.hunger = npc.needs.rest = npc.needs.wealth = npc.needs.social = 0;
    CHECK(npc.needs.lowest() == 0);             // post-sweep FAR decay saturates at 0 regardless
    f.ctx.now                      = 31 * kDay; // the monthly cadence is due
    npc.lastMigrationRollWorldTime = 0;
    const std::uint64_t pull       = migrationPull(npc, c, 2, f.ctx);
    const std::uint64_t pct        = MIGRATION_ROLL_PCT_BASE * pull / SCALE;

    f.rng.seedAll(12345); // seedAll pins every stream; "npc_life" is deterministic
    const std::uint64_t learned = f.rng.pct("npc_life");
    f.rng.seedAll(12345); // seedAll pins every stream; "npc_life" is deterministic // rewind: the sweep redraws it

    lifecycleDailySweep(f.ctx);
    CHECK((f.npcs.get(id).homeTerritory == 2) == (learned < pct));
    if (learned < pct)
    {
        CHECK(f.npcs.get(id).homeTerritory == 2);
        CHECK(f.npcs.get(id).currentRegion == 2);
        CHECK(samePos(f.characters.get(id).body.position, f.world.settlementCenter(2))); // FAR teleports
    } else
    {
        CHECK(f.npcs.get(id).homeTerritory == 1);
    }

    // Rate bound: a destination with no openings yields pull 0 -> no roll, no move,
    // whatever the stream says. Fill territory 2 to its cap of 1.
    if (f.npcs.get(id).homeTerritory == 1)
    {
        const characterId squatter = spawnNpc(f, 1, 2, 2, 30);
        (void)squatter;
        f.ctx.now += 31 * kDay;
        lifecycleDailySweep(f.ctx);
        CHECK(f.npcs.get(id).homeTerritory == 1); // full destination: never moves
    }
}

TEST_CASE(
    "TestNpc_NaturalDeathNoEvidence"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId elder = spawnNpc(f, 1, 1, 1, 80); // at lifespan

    lifecycleDailySweep(f.ctx);
    CHECK_FALSE(f.npcs.contains(elder));
    CHECK_FALSE(f.characters.exists(elder));
    CHECK(f.justice.crimes.empty()); // silent removal: no evidence
    CHECK(f.justice.assaults.empty());
    f.corpses.materializePending();            // step 11 would run this
    REQUIRE(f.corpses.find(elder) != nullptr); // corpse without loot
    CHECK(f.corpses.find(elder)->loot.empty());
}

TEST_CASE(
    "TestNpc_CriticalDeathTriggersReplacement"
)
{
    Fixture f;
    f.addTerritory(1, 0.0, 0.0, 1, false);
    const characterId guard = spawnNpc(f, 1, 1, 1, 90, "guard"); // past lifespan
    const characterId peer  = spawnNpc(f, 1, 1, 1, 30);          // eligible roleless adult

    lifecycleDailySweep(f.ctx); // guard dies of age
    CHECK_FALSE(f.npcs.contains(guard));
    CHECK(f.roles.hasOpenSeat(1, "guard")); // seat marked, NOT yet filled
    CHECK(f.npcs.get(peer).roleKey.empty());

    // The replacement lands after REPLACEMENT_DELAY (A-48: 1 game-day), inside its
    // scheduled step (Npcs) on the first tick whose worldTime reaches the due time.
    f.ctx.now += REPLACEMENT_DELAY;
    f.scheduler.runTick(f.ctx);
    CHECK(f.npcs.get(peer).roleKey == "guard");
    CHECK_FALSE(f.roles.hasOpenSeat(1, "guard"));
    CHECK(f.npcs.get(peer).integrity >= 5000); // drawn from the role's range
}

TEST_CASE(
    "TestNpc_PopulationBoundedTwoGameYears"
)
{
    Fixture f;
    // One village, template band [3, 6]: starts under minimum, one farmer feeds it.
    f.addTerritory(1, 0.0, 0.0, 1, false, 3, 6);
    spawnNpc(f, 1, 1, 1, 20, "farmer");
    spawnNpc(f, 1, 1, 1, 22);

    for (int day = 0; day < 2 * 365; ++day)
    { // 2 game-years, no player
        f.ctx.now += kDay;
        lifecycleDailySweep(f.ctx);
    }

    // Population bounded by the template band (soak precursor): births refill to the
    // minimum and stop; nobody starves (the farmer produces into the regional stock).
    std::uint32_t pop = 0;
    for (const auto &[id, npc] : f.npcs.all())
    {
        (void)id;
        if (npc.homeTerritory == 1)
            ++pop;
    }
    CHECK(pop >= 3);
    CHECK(pop <= 6);
    CHECK(f.justice.crimes.empty());
    CHECK(f.justice.assaults.empty());
    // The food loop actually ran: the regional grain stock is positive.
    const goodId grain = f.economy.goodIdForKey("grain");
    REQUIRE(grain != 0);
    const RegionStock *stock = f.economy.findStock(1, grain);
    REQUIRE(stock != nullptr);
    CHECK(stock->supply > 0);
}
