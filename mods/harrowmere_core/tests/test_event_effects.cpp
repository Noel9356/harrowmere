// test_event_effects.cpp — task 5.5's pinned tests: the D-FLAGS-1 split
// (transient targets despawn at endEvent; price biases expire; region flags
// persist past the end; nested spawns depth-check) and the reconquest path
// (the shipped GARRISON_RETURN template re-garrisons and clears lawless).
#include <doctest.h>

#include <cstdint>
#include <vector>

#include "core/constants.hpp"
#include "events/engine.hpp"
#include "events/template.hpp"
#include "events/world_effects.hpp"
#include "justice/enforcement.hpp"
#include "world/seasons.hpp" // the closed Weather list (the weather-bias pin)

#include "test_justice_fixture.hpp"

namespace
{

using namespace harrowmere;
using namespace justice_test;
namespace ev = harrowmere::events;

// A template with a SpawnTargets entry action and NO DespawnTargets — the
// pure transient shape (5.5's finishEvent despawn owns the cleanup).
ev::EventTemplate spawnOnlyTemplate()
{
    ev::EventTemplate tpl;
    tpl.id                 = 1;
    tpl.key                = "spawn_only";
    tpl.global             = false;
    tpl.initialStateId     = 1;
    tpl.initialDifficulty  = 0;
    tpl.initialSuccessRate = 5000;
    tpl.maxDurationDays    = 1;

    ev::TemplateState a;
    a.stateId = 1;
    ev::Action spawn;
    spawn.kind       = ev::ActionKind::SpawnTargets;
    spawn.speciesKey = "human";
    spawn.roleKey    = "bandit";
    spawn.count      = 3;
    a.entryActions.push_back(spawn);
    ev::Transition toB;
    toB.to = 2;
    ev::Condition timer;
    timer.kind = ev::ConditionKind::Timer;
    timer.days = 1;
    toB.conditions.push_back(timer);
    a.transitions.push_back(toB);

    ev::TemplateState b;
    b.stateId = 2;
    ev::Action end;
    end.kind    = ev::ActionKind::EndEvent;
    end.success = true;
    b.entryActions.push_back(end);

    tpl.states.push_back(a);
    tpl.states.push_back(b);
    return tpl;
}

// A template whose state exits set a region flag (the persistent shape) and
// hold every remaining interpreter branch the Done-when names.
ev::EventTemplate branchesTemplate()
{
    ev::EventTemplate tpl;
    tpl.id                 = 2;
    tpl.key                = "branches";
    tpl.global             = false;
    tpl.initialStateId     = 1;
    tpl.initialDifficulty  = 0;
    tpl.initialSuccessRate = 5000;
    tpl.maxDurationDays    = 30;

    ev::TemplateState a;
    a.stateId = 1;
    ev::Action biasPrice;
    biasPrice.kind = ev::ActionKind::PriceBias;
    biasPrice.good = "iron";
    biasPrice.pct  = 12500; // 1.25x, on the SCALE grid
    biasPrice.days = 1;
    a.entryActions.push_back(biasPrice);
    ev::Action biasWeather;
    biasWeather.kind  = ev::ActionKind::WeatherBias;
    biasWeather.state = "storm";
    biasWeather.days  = 1;
    a.entryActions.push_back(biasWeather);
    ev::Action tier;
    tier.kind = ev::ActionKind::GrantRewardTier;
    tier.tier = 1;
    a.entryActions.push_back(tier);
    ev::Transition toB;
    toB.to = 2;
    ev::Condition timer;
    timer.kind = ev::ConditionKind::Timer;
    timer.days = 1;
    toB.conditions.push_back(timer);
    a.transitions.push_back(toB);

    ev::Action flag;
    flag.kind = ev::ActionKind::SetRegionFlag;
    flag.flag = "blessed";
    flag.set  = true;
    a.exitActions.push_back(flag); // the state-1 exit (the "ending is not exiting" rule keeps state 2's exits unrun)

    ev::TemplateState b;
    b.stateId = 2;
    ev::Action end;
    end.kind    = ev::ActionKind::EndEvent;
    end.success = true;
    b.entryActions.push_back(end);

    tpl.states.push_back(a);
    tpl.states.push_back(b);
    return tpl;
}

struct EffectsRig
{
    JusticeFixture f;
    ev::EventTemplateTable table;
    ev::EventEngine engine;

    EffectsRig()
    {
        f.base.ctx.now = 1000;
        engine.bind(&table);
        engine.bindContext(&f.base.ctx);
    }

    std::uint64_t start(
        const char *key, regionId region
    )
    {
        const std::uint64_t id = engine.startEventByKey(key, region, 0, nullptr, f.base.ctx);
        REQUIRE(id != 0);
        return id;
    }
};

} // namespace

// ============================================================================
// Pinned test — the transient split: an event's spawned targets despawn at
// endEvent even with no explicit DespawnTargets action (the D-FLAGS-1
// reading); the world population is whole again and the books hold.
// ============================================================================
TEST_CASE(
    "TestEffects_TransientTargetsRemovedAtEnd"
)
{
    EffectsRig rig;
    rig.table.insert(spawnOnlyTemplate());

    const std::size_t populationBefore = rig.f.base.characters.population();
    const std::uint64_t id             = rig.start("spawn_only", JusticeFixture::kRegionA);
    const ev::Event *ev                = rig.engine.find(id);
    REQUIRE(ev != nullptr);
    REQUIRE(ev->targetIds.size() == 3); // SpawnTargets{count: 3}
    CHECK(rig.f.base.characters.population() == populationBefore + 3);

    // The event ends from state 2's entry: the despawn is the lifecycle
    // tail (no template action asked for it).
    rig.f.base.ctx.now += core::DAY_LENGTH_SECONDS;
    rig.engine.tick(rig.f.base.ctx);
    CHECK(rig.engine.activeCount() == 0);
    CHECK(rig.f.base.characters.population() == populationBefore);
    const ev::Event *ended = rig.engine.find(id);
    REQUIRE(ended != nullptr);
    CHECK(ended->targetIds.empty());
}

// ============================================================================
// Pinned test — price biases are transient by TIME: an active bias reads
// its multiplier; past the until-timestamp the read answers SCALE again.
// The economy's recompute consumes the same read (the 3.7 hook wiring).
// ============================================================================
TEST_CASE(
    "TestEffects_PriceBiasExpires"
)
{
    EffectsRig rig;
    rig.table.insert(branchesTemplate());
    rig.start("branches", JusticeFixture::kRegionA);

    const goodId iron = rig.f.base.economy.goodIdForKey("iron");
    REQUIRE(iron != 0);
    CHECK(rig.engine.priceBiasFor(JusticeFixture::kRegionA, iron, rig.f.base.ctx.now) == 12500);
    // A different region/good reads no bias.
    CHECK(rig.engine.priceBiasFor(JusticeFixture::kRegionB, iron, rig.f.base.ctx.now) == SCALE);

    // The bias expires by itself (1 day).
    rig.f.base.ctx.now += core::DAY_LENGTH_SECONDS + 1;
    CHECK(rig.engine.priceBiasFor(JusticeFixture::kRegionA, iron, rig.f.base.ctx.now) == SCALE);

    // The consumer wiring: recomputePrices applies the bias inside the
    // D-ECON-1 band while it holds. The stock sits mid-band so the pin is
    // not vacuous (a stock-heavy price clamps at base/4 and would read 0).
    rig.f.base.ctx.now -= core::DAY_LENGTH_SECONDS + 1; // back inside the window
    rig.f.base.economy.setPopulation(JusticeFixture::kRegionA, 100000);
    rig.f.base.economy.addSupply(JusticeFixture::kRegionA, iron, 1000);
    const std::uint64_t unbiased = rig.f.base.economy.priceFor(JusticeFixture::kRegionA, iron);
    REQUIRE(unbiased > 0);
    rig.f.base.ctx.eventEffects = &rig.engine; // the plugin's binding (the test swaps it in)
    rig.f.base.economy.recomputePrices(rig.f.base.ctx);
    const std::uint64_t biased = rig.f.base.economy.publishedPrice(JusticeFixture::kRegionA, iron);
    CHECK(biased == unbiased * 12500 / SCALE);
    rig.f.base.ctx.eventEffects = nullptr;
}

// ============================================================================
// Pinned test — the persistent split: SetRegionFlag writes Region.flags and
// endEvent NEVER reverts it ("while flags may remain changed").
// ============================================================================
TEST_CASE(
    "TestEffects_RegionFlagPersistsAfterEnd"
)
{
    EffectsRig rig;
    rig.table.insert(branchesTemplate());
    const std::uint64_t id = rig.start("branches", JusticeFixture::kRegionA);
    CHECK(rig.engine.find(id) != nullptr);
    world::Region *regionA = rig.f.world.region(JusticeFixture::kRegionA);
    REQUIRE(regionA != nullptr);
    CHECK((regionA->flags & 2) == 0); // territory_templates.json: "blessed": 2

    // The event resolves (state 2's exits set the flag; the entry ends it).
    rig.f.base.ctx.now += core::DAY_LENGTH_SECONDS;
    rig.engine.tick(rig.f.base.ctx);
    CHECK(rig.engine.activeCount() == 0);
    CHECK((regionA->flags & 2) != 0); // the flag REMAINS after the end
}

// ============================================================================
// Pinned test — the nested spawn depth-check through the interpreter: the
// child lands at depth 1 in the parent's region; the grandchild refuses
// (A-103's cap).
// ============================================================================
TEST_CASE(
    "TestEffects_NestedDepthChecked"
)
{
    EffectsRig rig;
    ev::EventTemplate parent = spawnOnlyTemplate();
    parent.id                = 1;
    parent.key               = "parent";
    ev::TemplateState &a     = parent.states[0];
    a.entryActions.clear();
    ev::Action nested;
    nested.kind        = ev::ActionKind::SpawnNestedEvent;
    nested.templateKey = "child";
    a.entryActions.push_back(nested);
    rig.table.insert(parent);

    ev::EventTemplate child = spawnOnlyTemplate();
    child.id                = 2;
    child.key               = "child";
    ev::TemplateState &ca   = child.states[0];
    ca.entryActions.clear();
    ev::Action grandchild;
    grandchild.kind        = ev::ActionKind::SpawnNestedEvent;
    grandchild.templateKey = "child"; // the depth-1 event spawns again
    ca.entryActions.push_back(grandchild);
    rig.table.insert(child);

    const std::uint64_t id = rig.start("parent", JusticeFixture::kRegionA);
    // The parent's entry spawned the child (depth 1, same region — the
    // nested bypass of the same-region refusal); the child's entry tried a
    // grandchild and the cap refused it.
    CHECK(rig.engine.activeCount() == 2);
    const ev::Event *parentEv = rig.engine.find(id);
    REQUIRE(parentEv != nullptr);
    CHECK(parentEv->depth == 0);
    std::size_t children = 0;
    for (const auto &[eid, e] : rig.engine.active())
    {
        if (eid != id)
        {
            CHECK(e.depth == 1);
            ++children;
        }
    }
    CHECK(children == 1); // no grandchild: the depth cap refused
}

// ============================================================================
// Pinned test — the reconquest: the shipped GARRISON_RETURN template's exit
// re-garrisons the territory (the participants take the guard role) and
// clears the lawless flag — the NPC-reconquest path out of lawlessness.
// ============================================================================
TEST_CASE(
    "TestEffects_GarrisonReturnClearsLawless"
)
{
    ev::EventTemplateTable shipped;
    std::vector<data::DataError> errors;
    REQUIRE(ev::loadEventTemplates("mods/harrowmere_core/data/event_templates.json", shipped, errors));
    REQUIRE(shipped.byKey("GARRISON_RETURN") != nullptr);
    EffectsRig rig;
    rig.table = std::move(shipped);

    // The villageB garrison is gone: the lawlessness watch starts, and the
    // territory promotes to lawless after A-96 (3 game-days).
    rig.f.base.ctx.now = 10 * core::DAY_LENGTH_SECONDS; // a non-zero epoch
    justice::updateLawlessness(rig.f.base.ctx);         // stamps the watch on the garrison-less ground
    rig.f.base.ctx.now += 3 * core::DAY_LENGTH_SECONDS;
    justice::updateLawlessness(rig.f.base.ctx);
    CHECK(rig.f.world.isLawless(JusticeFixture::kVillageB));

    // The reconquest: the event starts in villageB's region, the village's
    // militia veterans join (the npcJoinPolicy's roles), the timer resolves
    // it, and the exit re-garrisons + clears the flag.
    const characterId volunteer
        = rig.f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "militia_veteran", 7000);
    const std::uint64_t id = rig.start("GARRISON_RETURN", JusticeFixture::kRegionB);
    rig.engine.join(volunteer, id, rig.f.base.ctx);
    REQUIRE(rig.f.world.isLawless(JusticeFixture::kVillageB)); // until the exit runs

    rig.f.base.ctx.now += 2 * core::DAY_LENGTH_SECONDS + 1;
    rig.engine.tick(rig.f.base.ctx);
    CHECK(rig.engine.activeCount() == 0);                          // the timer resolved the reconquest
    CHECK_FALSE(rig.f.world.isLawless(JusticeFixture::kVillageB)); // the flag cleared
    CHECK(rig.f.base.npcs.get(volunteer).roleKey == "guard");      // the garrison took its post
}

// ============================================================================
// Extra pin — the interpreter's GrantRewardTier branch records the tier for
// 5.4's payout read, and the weather-bias branch's window holds the region
// weather (the step-2 slice's pin).
// ============================================================================
TEST_CASE(
    "TestEffects_WeatherBiasAndTierBranches"
)
{
    EffectsRig rig;
    rig.table.insert(branchesTemplate());
    const std::uint64_t id = rig.start("branches", JusticeFixture::kRegionA);

    // The tier branch: 5.4's read.
    CHECK(rig.engine.grantedTiers(id).size() == 1);
    CHECK(rig.engine.grantedTiers(id).front() == 1);

    // The weather branch: the record is live; the step-2 slice pins the
    // region's weather for the window.
    CHECK(rig.engine.weatherBiasFor(JusticeFixture::kRegionA, rig.f.base.ctx.now) != nullptr);
    world::Region *regionA = rig.f.world.region(JusticeFixture::kRegionA);
    REQUIRE(regionA != nullptr);
    regionA->weather.kind = static_cast<std::uint32_t>(world::Weather::CLEAR);
    ev::WorldEffectsTickSystem effects;
    effects.engine = &rig.engine;
    effects.run(rig.f.base.ctx);
    CHECK(regionA->weather.kind == static_cast<std::uint32_t>(world::Weather::STORM));

    // Expiry hands back: past the window the slice writes nothing.
    rig.f.base.ctx.now += core::DAY_LENGTH_SECONDS + 1;
    regionA->weather.kind = static_cast<std::uint32_t>(world::Weather::CLEAR);
    effects.run(rig.f.base.ctx);
    CHECK(regionA->weather.kind == static_cast<std::uint32_t>(world::Weather::CLEAR));
}
