// test_power.cpp — Task 2.5's pinned test suite.
//
// Implements the five tests pinned in phase-2-alive.md card 2.5's Tests
// block (names verbatim — common.md §3.5):
//
//   * TestPower_CacheReturnsUntilDirty     — the cache serves until the flag
//   * TestPower_PeakNeverDecreases         — unequip everything: current
//                                            drops, the watermark holds (the
//                                            doc's own sandbagging scenario)
//   * TestPower_DirtyCallSites_Complete    — the call-site audit: every
//                                            mutation that enters the formula
//                                            fires markPowerDirty
//   * TestPower_LevelUpDoesNotDirtyCache   — level changes do not enter the
//                                            formula (the audit's negative)
//   * TestMatchmaking_WeightsEightToTwo    — the 8:2 peak/potential blend
//
// Audit scope note (kept with the test on purpose): the pinned call-site
// list is stat purchase, skill purchase, respec, equip, unequip, and 3.3's
// onNpcKill XP gain. 3.3 has not landed yet (2.4-2.7 are the current cards),
// so the audit below covers every call site that EXISTS today — the seven
// functions it exercises — and TestDeath_NpcKillTransfersXpAndDirtiesPower
// (3.3's own pinned test) extends the audit when that card lands. consumeItem
// is 2.4's XP grant and IS audited here (the card pins the dirty on it).
//
// Test harness: doctest v2.4.12; no main here — tests/main.cpp owns the
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.
#include <doctest/doctest.h>

#include <cstdint>

#include "character/character.hpp"
#include "core/enums.hpp"
#include "core/types.hpp"
#include "data/registry.hpp"
#include "items/gear.hpp"
#include "items/inventory.hpp"
#include "progression/power.hpp"
#include "progression/progression.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace
{

std::string powerDataDir()
{
#ifdef HARROWMERE_CORE_DATA_DIR
    return std::string(HARROWMERE_CORE_DATA_DIR);
#else
    return std::string("mods/harrowmere_core/data");
#endif
}

struct PowerFixture
{
    harrowmere::data::DataRegistry registry;
    harrowmere::items::ItemInstanceRegistry instances;
    harrowmere::items::InventoryStore bags;
    harrowmere::WorldContext ctx;

    PowerFixture()
    {
        std::vector<harrowmere::data::DataError> errors;
        REQUIRE(registry.load(powerDataDir(), errors));
        REQUIRE(errors.empty());
        ctx.now  = 1'000;
        ctx.data = &registry;
    }

    static harrowmere::Character makeCharacter(
        harrowmere::characterId id
    )
    {
        harrowmere::Character c{};
        c.id             = id;
        c.body.maxHp     = harrowmere::maxHpForLevel(1);
        c.body.currentHp = c.body.maxHp;
        return c;
    }
};

} // namespace

// ---- pinned: TestPower_CacheReturnsUntilDirty --------------------------------------

TEST_CASE(
    "TestPower_CacheReturnsUntilDirty"
)
{
    PowerFixture fx;
    harrowmere::Character c = PowerFixture::makeCharacter(1);

    // The formula by hand for a naked level-1 player (species 0, all stats/
    // skills 0, no gear):
    //   gear 0
    //   + Σ statWeight * eff / SCALE  = 0 (all stats 0)
    //   + Σ skillWeight * eff / SCALE = 0
    // and the first getCurrentPower computes that 0, stores it, clears the
    // flag.
    CHECK(c.powerDirty);
    const std::uint64_t first = harrowmere::getCurrentPower(c, fx.registry, fx.instances);
    CHECK(first == 0);
    CHECK_FALSE(c.powerDirty);
    CHECK(c.cachedPower == 0);
    CHECK(c.peakPower == 0);

    // Mutate the UNDERLYING stat without the flag: the cache still serves —
    // this is exactly why every mutation must fire markPowerDirty.
    c.progression.stats[static_cast<std::size_t>(harrowmere::StatType::STRENGTH)] = 10;
    CHECK(harrowmere::getCurrentPower(c, fx.registry, fx.instances) == 0);
    CHECK_FALSE(c.powerDirty); // reading never dirties

    // Flag it: the recompute picks the change up. STR weight 12000
    // (A-20) x effectiveness 10 (below the A-12 knee) / SCALE = 12.
    harrowmere::markPowerDirty(c);
    const std::uint64_t second = harrowmere::getCurrentPower(c, fx.registry, fx.instances);
    CHECK(second == 12);
    CHECK(c.cachedPower == 12);
    CHECK_FALSE(c.powerDirty);
    CHECK(c.peakPower == 12);

    // The watermark: a recompute that DROPS the total still stores the
    // smaller cache but never the smaller peak (the sandbagging guarantee).
    c.progression.stats[static_cast<std::size_t>(harrowmere::StatType::STRENGTH)] = 0;
    harrowmere::markPowerDirty(c);
    CHECK(harrowmere::getCurrentPower(c, fx.registry, fx.instances) == 0);
    CHECK(c.cachedPower == 0);
    CHECK(c.peakPower == 12);
}

// ---- pinned: TestPower_PeakNeverDecreases -------------------------------------------

TEST_CASE(
    "TestPower_PeakNeverDecreases"
)
{
    PowerFixture fx;
    harrowmere::Character c           = PowerFixture::makeCharacter(1);
    harrowmere::items::Inventory &bag = fx.bags.bag(c.id);

    const harrowmere::data::ItemData *sword = fx.registry.itemByKey("iron_sword");
    REQUIRE(sword != nullptr);
    const std::uint64_t swordA = fx.instances.spawn(sword->id);
    const std::uint64_t swordB = fx.instances.spawn(sword->id);
    harrowmere::items::addLooseInstance(bag, swordA);
    harrowmere::items::addLooseInstance(bag, swordB);
    REQUIRE(harrowmere::equip(c, bag, fx.instances, fx.registry, swordA).isOk());
    REQUIRE(harrowmere::equip(c, bag, fx.instances, fx.registry, swordB).isOk());

    // Two swords: gear = 2 x 18 (the doc formula, pinned in test_items).
    const std::uint64_t geared = harrowmere::getCurrentPower(c, fx.registry, fx.instances);
    CHECK(geared == 36);
    CHECK(c.peakPower == 36);

    // THE doc's own sandbagging scenario: strip EVERYTHING. Current drops;
    // the watermark holds — "unequipping gear cannot drop a player into
    // easier brackets" (design reference §Matchmaking).
    REQUIRE(harrowmere::unequip(c, bag, fx.instances, fx.registry, swordA).isOk());
    REQUIRE(harrowmere::unequip(c, bag, fx.instances, fx.registry, swordB).isOk());
    CHECK(harrowmere::getCurrentPower(c, fx.registry, fx.instances) == 0);
    CHECK(c.cachedPower == 0);
    CHECK(c.peakPower == 36);

    // And the matchmaking score reads the WATERMARK, not the strip job.
    CHECK(harrowmere::getMatchmakingScore(c, fx.registry) == (8 * 36 + 2 * 0) / 10);
}

// ---- pinned: TestPower_DirtyCallSites_Complete ---------------------------------------

TEST_CASE(
    "TestPower_DirtyCallSites_Complete"
)
{
    PowerFixture fx;

    // The audit's static list — every mutation that enters the formula, each
    // proven to fire markPowerDirty by observing the flag flip. 3.3's
    // onNpcKill joins this list when that card lands (see the file header).
    enum class Site
    {
        BuyStat,
        BuySkill,
        RespecStat,
        RespecSkill,
        Equip,
        Unequip,
        ConsumeItem,
    };

    for (const Site site :
        {Site::BuyStat,
            Site::BuySkill,
            Site::RespecStat,
            Site::RespecSkill,
            Site::Equip,
            Site::Unequip,
            Site::ConsumeItem})
    {
        // Fresh character, clean cache (one getCurrentPower clears the flag).
        harrowmere::Character c           = PowerFixture::makeCharacter(1);
        c.progression.totalXp             = 100'000; // headroom for purchases
        c.progression.availableXp         = 100'000;
        harrowmere::items::Inventory &bag = fx.bags.bag(c.id);
        harrowmere::items::addStacked(bag, fx.registry.itemByKey("bandage")->id, 5);
        harrowmere::items::addStacked(bag, fx.registry.itemByKey("mana_tonic")->id, 5); // the ConsumeItem site's stock

        // A fresh sword instance for the equip/unequip sites.
        const std::uint64_t sword = fx.instances.spawn(fx.registry.itemByKey("iron_sword")->id);

        CAPTURE(static_cast<int>(site));
        (void)harrowmere::getCurrentPower(c, fx.registry, fx.instances);
        REQUIRE_FALSE(c.powerDirty); // fixture sanity: the cache is clean

        bool mutationApplied = false;
        switch (site)
        {
        case Site::BuyStat:
            mutationApplied = harrowmere::buyStat(c, harrowmere::StatType::STRENGTH, fx.ctx).isOk();
            break;
        case Site::BuySkill:
            mutationApplied = harrowmere::buySkill(c, harrowmere::SkillType::MELEE, fx.ctx).isOk();
            break;
        case Site::RespecStat:
            // Respec needs a point to give back.
            c.progression.stats[static_cast<std::size_t>(harrowmere::StatType::STRENGTH)] = 3;
            mutationApplied = harrowmere::respecStat(c, harrowmere::StatType::STRENGTH, fx.ctx).isOk();
            break;
        case Site::RespecSkill:
            c.progression.skills[static_cast<std::size_t>(harrowmere::SkillType::MELEE)] = 3;
            mutationApplied = harrowmere::respecSkill(c, harrowmere::SkillType::MELEE, fx.ctx).isOk();
            break;
        case Site::Equip:
            harrowmere::items::addLooseInstance(bag, sword);
            mutationApplied = harrowmere::equip(c, bag, fx.instances, fx.registry, sword).isOk();
            break;
        case Site::Unequip:
            harrowmere::items::addLooseInstance(bag, sword);
            REQUIRE(harrowmere::equip(c, bag, fx.instances, fx.registry, sword).isOk());
            harrowmere::markPowerDirty(c);
            (void)harrowmere::getCurrentPower(c, fx.registry, fx.instances);
            REQUIRE_FALSE(c.powerDirty);
            mutationApplied = harrowmere::unequip(c, bag, fx.instances, fx.registry, sword).isOk();
            break;
        case Site::ConsumeItem:
            mutationApplied = harrowmere::consumeItem(c, bag, fx.registry, "mana_tonic").isOk();
            break;
        }
        REQUIRE(mutationApplied); // the site must actually mutate for the audit to mean anything
        CHECK(c.powerDirty);      // THE audit: the mutation fired markPowerDirty

        // And a recompute really moves the cache (the flag is not decorative).
        (void)harrowmere::getCurrentPower(c, fx.registry, fx.instances);
        CHECK_FALSE(c.powerDirty);
    }
}

// ---- pinned: TestPower_LevelUpDoesNotDirtyCache ---------------------------------------

TEST_CASE(
    "TestPower_LevelUpDoesNotDirtyCache"
)
{
    PowerFixture fx;
    harrowmere::Character c   = PowerFixture::makeCharacter(1);
    c.progression.availableXp = 1'000'000; // level 1 -> 2 costs levelCosts[1] = 115

    (void)harrowmere::getCurrentPower(c, fx.registry, fx.instances);
    REQUIRE_FALSE(c.powerDirty);

    // The level itself advances...
    REQUIRE(harrowmere::buyLevel(c, /*automatic=*/false).isOk());
    CHECK(c.progression.level == 2);

    // ...and the cache does NOT care: "level changes do not enter the
    // formula" (design reference; the audit's pinned negative). maxHp grew,
    // the power number did not move.
    CHECK_FALSE(c.powerDirty);
    CHECK(harrowmere::getCurrentPower(c, fx.registry, fx.instances) == 0);

    // The automatic path (3.4's NPC loop) is held to the same rule.
    REQUIRE(harrowmere::buyLevel(c, /*automatic=*/true).isOk());
    CHECK(c.progression.level > 2);
    CHECK_FALSE(c.powerDirty);
}

// ---- pinned: TestMatchmaking_WeightsEightToTwo -----------------------------------------

TEST_CASE(
    "TestMatchmaking_WeightsEightToTwo"
)
{
    PowerFixture fx;
    harrowmere::Character c = PowerFixture::makeCharacter(1);

    // peak 1000, potential = isqrt(400) = 20:
    //   (8 * 1000 + 2 * 20) / 10 = (8000 + 40) / 10 = 804
    c.peakPower           = 1'000;
    c.progression.totalXp = 400;
    CHECK(harrowmere::getMatchmakingScore(c, fx.registry) == 804);

    // The blend is exactly 8:2 — symmetric check with a different pair.
    c.peakPower           = 50;
    c.progression.totalXp = 10'000; // isqrt = 100
    CHECK(harrowmere::getMatchmakingScore(c, fx.registry) == (8 * 50 + 2 * 100) / 10);
    CHECK(harrowmere::getMatchmakingScore(c, fx.registry) == 60);

    // Zeros stay zeros (no division hazard — the /10 is the formula's own).
    harrowmere::Character fresh = PowerFixture::makeCharacter(2);
    CHECK(harrowmere::getMatchmakingScore(fresh, fx.registry) == 0);

    // getPotentialPower is the doc's tamper-proof potential: isqrt(totalXp),
    // and totalXp never decreases — so the potential never does either.
    // (c's totalXp sits at 10'000 here: isqrt = 100.)
    CHECK(harrowmere::getPotentialPower(c) == 100);
    c.progression.totalXp += 201;                   // a kill lands: 10'000 -> 10'201
    CHECK(harrowmere::getPotentialPower(c) == 101); // isqrt(10'201) — and it never moved DOWN
}
