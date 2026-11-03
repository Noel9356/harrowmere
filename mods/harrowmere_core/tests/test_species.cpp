// test_species.cpp — Task 2.2's pinned test suite.
//
// Implements the four tests pinned in phase-2-alive.md card 2.2's Tests
// block (names verbatim — common.md §3.5: "Test names are pinned; keep
// them"):
//
//   * TestSpecies_MultipliersAppliedToEffectiveness — the doc formula over
//     the shipped species.json rows
//   * TestSpecies_ElfIntelligence4x                — the doc table's 4x elf
//     INTELLIGENCE row, read from data (never code)
//   * TestSpecies_IsSapientFalseForBeasts          — beastkin true; the
//     non-sapient beast rows (wolf, goo_beast) false
//   * TestSpecies_LifespanGrowsWithLevel           — A-5's linear growth
//     (2x at level 100)
//
// Plus the bias-core support (the data-driven xenophobiaBias + territory
// culture term 3.9 builds on) the card's Spec block pins. All data comes
// from the shipped species.json through the DataRegistry — no species id,
// key or name is hardcoded in the code under test (the card's grep
// contract; the tests may name keys, they are the data's customers).
//
// Test harness: doctest v2.4.12 (vendored per task 0.6); no main here.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>

#include "character/character.hpp"
#include "character/species.hpp"
#include "core/constants.hpp" // PLAYER_SPECIES_ID, STAT_THRESHOLD
#include "core/enums.hpp"
#include "core/types.hpp"
#include "data/registry.hpp"

namespace fs = std::filesystem;

using harrowmere::Character;
using harrowmere::StatType;
using harrowmere::data::DataRegistry;
using harrowmere::data::SpeciesData;

namespace
{

// The shipped data directory (test_registry.cpp's resolution order: env,
// baked define, source-tree walk).
fs::path resolveDataDir()
{
    if (const char *env = std::getenv("HARROWMERE_TEST_DATA"))
    {
        if (env[0] != '\0')
            return fs::path(env);
    }
#ifdef HARROWMERE_CORE_DATA_DIR
    return fs::path(HARROWMERE_CORE_DATA_DIR);
#endif
    fs::path probe = fs::current_path();
    for (int i = 0; i < 6; ++i)
    {
        const fs::path candidate = probe / "mods" / "harrowmere_core" / "data";
        if (fs::exists(candidate / "species.json"))
            return candidate;
        probe = probe.parent_path();
    }
    return fs::path("mods") / "harrowmere_core" / "data";
}

// One committed registry for the whole binary run (the snapshot is
// immutable once committed; DataRegistry is non-copyable, so the static
// owns it by pointer).
DataRegistry &shippedRegistry()
{
    static DataRegistry *registry = [] {
        auto r = std::make_unique<DataRegistry>();
        std::vector<harrowmere::data::DataError> errors;
        REQUIRE_MESSAGE(r->load(resolveDataDir().string(), errors), "shipped data registry load failed");
        return r.release();
    }();
    return *registry;
}

Character makeCharacterOfSpecies(
    harrowmere::speciesId species
)
{
    Character c{};
    c.id                = 1;
    c.species           = species;
    c.progression.level = 1;
    return c;
}

harrowmere::speciesId speciesIdByKey(
    const char *key
)
{
    const SpeciesData *row = shippedRegistry().speciesByKey(key);
    REQUIRE_MESSAGE(row != nullptr, "shipped species.json is missing key");
    return row->id;
}

} // namespace

TEST_CASE(
    "TestSpecies_MultipliersAppliedToEffectiveness"
)
{
    auto &registry = shippedRegistry();

    // The doc formula, verbatim (character.md §Species):
    //   effectiveness = getEffectiveness(point, STAT_THRESHOLD)
    //   return effectiveness * speciesData[c.species].statMultiplier[st] / SCALE
    // 1.0x rows return the raw effectiveness; 2.0x rows double it.
    Character human = makeCharacterOfSpecies(speciesIdByKey("human"));
    human.progression.stats[static_cast<std::size_t>(StatType::STRENGTH)] = 10;
    CHECK(harrowmere::getStatEffectiveness(registry, human, StatType::STRENGTH) == 10);

    Character beastkin = makeCharacterOfSpecies(speciesIdByKey("beastkin"));
    beastkin.progression.stats[static_cast<std::size_t>(StatType::STRENGTH)] = 10;
    CHECK(harrowmere::getStatEffectiveness(registry, beastkin, StatType::STRENGTH) == 20);
    // INTELLIGENCE stays 1x for beastkins (the doc row: STR/DEF/AGI 2x, INT 1x).
    beastkin.progression.stats[static_cast<std::size_t>(StatType::INTELLIGENCE)] = 10;
    CHECK(harrowmere::getStatEffectiveness(registry, beastkin, StatType::INTELLIGENCE) == 10);

    // Sub-1.0 multipliers pull DOWN (the shipped wolf row's 0.2 INT).
    Character wolf = makeCharacterOfSpecies(speciesIdByKey("wolf"));
    wolf.progression.stats[static_cast<std::size_t>(StatType::INTELLIGENCE)] = 10;
    CHECK(harrowmere::getStatEffectiveness(registry, wolf, StatType::INTELLIGENCE) == 2);

    // Multiply BEFORE divide: an odd point count keeps the lost fraction in
    // the product, not before it (2x of 15 = 30 exactly, not 15*2.0/SCALE
    // rounded at the wrong stage).
    beastkin.progression.stats[static_cast<std::size_t>(StatType::STRENGTH)] = 15;
    CHECK(harrowmere::getStatEffectiveness(registry, beastkin, StatType::STRENGTH) == 30);
}

TEST_CASE(
    "TestSpecies_ElfIntelligence4x"
)
{
    auto &registry = shippedRegistry();

    Character elf     = makeCharacterOfSpecies(speciesIdByKey("elf"));
    const auto intIdx = static_cast<std::size_t>(StatType::INTELLIGENCE);

    // The doc species table: elves are 1x everywhere but INTELLIGENCE 4x.
    elf.progression.stats[intIdx] = 10;
    CHECK(harrowmere::getStatEffectiveness(registry, elf, StatType::INTELLIGENCE) == 40);

    elf.progression.stats[intIdx] = 25;
    CHECK(harrowmere::getStatEffectiveness(registry, elf, StatType::INTELLIGENCE) == 100);

    // Above the knee the soft cap applies to the RAW point first, then the
    // multiplier: eff(100) = 57 -> 57 * 4 = 228.
    elf.progression.stats[intIdx] = 100;
    CHECK(harrowmere::getStatEffectiveness(registry, elf, StatType::INTELLIGENCE) == 228);

    // ...and the elf's other stats stay 1x.
    elf.progression.stats[static_cast<std::size_t>(StatType::STRENGTH)] = 10;
    CHECK(harrowmere::getStatEffectiveness(registry, elf, StatType::STRENGTH) == 10);

    // The player row (species 0, the hardcoded id) is all 1x.
    Character player                 = makeCharacterOfSpecies(harrowmere::PLAYER_SPECIES_ID);
    player.progression.stats[intIdx] = 25;
    CHECK(harrowmere::getStatEffectiveness(registry, player, StatType::INTELLIGENCE) == 25);
}

TEST_CASE(
    "TestSpecies_IsSapientFalseForBeasts"
)
{
    auto &registry = shippedRegistry();

    // design reference verbatim: isSapient(speciesId) = speciesData[speciesId].sapient
    CHECK(harrowmere::isSapient(registry, speciesIdByKey("beastkin"))); // sapient despite the beast shape
    CHECK(harrowmere::isSapient(registry, speciesIdByKey("human")));
    CHECK(harrowmere::isSapient(registry, speciesIdByKey("elf")));
    CHECK(harrowmere::isSapient(registry, harrowmere::PLAYER_SPECIES_ID)); // the player row

    // The non-sapient beasts: the shipped wolf row (card: "a non-sapient
    // 'wolf' row in species.json") and the goo beast. Hunting them creates
    // no crime (3.3's isCriminalAct gate reads this).
    CHECK_FALSE(harrowmere::isSapient(registry, speciesIdByKey("wolf")));
    CHECK_FALSE(harrowmere::isSapient(registry, speciesIdByKey("goo_beast")));

    // The goo beast's "neutralise attack" stays data: damageTakenMultiplier
    // 0.25 on the row (3.2 consumes it — nothing here hardcodes the value).
    const SpeciesData *goo = registry.speciesByKey("goo_beast");
    REQUIRE(goo != nullptr);
    CHECK(goo->damageTakenMultiplier == 2'500); // 0.25 on the SCALE grid
}

TEST_CASE(
    "TestSpecies_LifespanGrowsWithLevel"
)
{
    auto &registry = shippedRegistry();

    // A-5: lifespan * (SCALE + level * 100) / SCALE — linear, 2x at level
    // 100 (the doc line "lifespan increases as level increases").
    // The level-1 factor is (SCALE + 100) / SCALE = 1.01 — lifespan * 1.01,
    // truncated. Humans: 60 * 1.01 = 60.6 -> 60.
    Character human = makeCharacterOfSpecies(speciesIdByKey("human")); // 60 years
    CHECK(harrowmere::effectiveLifespan(registry, human) == 60);

    human.progression.level = 50;
    CHECK(harrowmere::effectiveLifespan(registry, human) == 90); // 60 * 1.5 exactly

    human.progression.level = 100;
    CHECK(harrowmere::effectiveLifespan(registry, human) == 120); // exactly 2x the base

    Character elf = makeCharacterOfSpecies(speciesIdByKey("elf")); // 1000 years
    CHECK(harrowmere::effectiveLifespan(registry, elf) == 1010);   // 1000 * 1.01
    elf.progression.level = 50;
    CHECK(harrowmere::effectiveLifespan(registry, elf) == 1500);
    elf.progression.level = 100;
    CHECK(harrowmere::effectiveLifespan(registry, elf) == 2000);

    // The unbounded rows (player / goo beast at 1,000,000 years) grow too
    // but stay far beyond any play horizon.
    Character player = makeCharacterOfSpecies(harrowmere::PLAYER_SPECIES_ID);
    CHECK(harrowmere::effectiveLifespan(registry, player) == 1'010'000); // 1e6 * 1.01
    player.progression.level = 100;
    CHECK(harrowmere::effectiveLifespan(registry, player) == 2'000'000); // exactly 2x the base

    // Monotone in level for every shipped row.
    const auto &table = registry.speciesTable();
    for (const SpeciesData &row : table)
    {
        Character c        = makeCharacterOfSpecies(row.id);
        std::uint64_t prev = 0;
        for (std::uint16_t level :
            {std::uint16_t{1}, std::uint16_t{25}, std::uint16_t{50}, std::uint16_t{75}, std::uint16_t{100}})
        {
            c.progression.level          = level;
            const std::uint64_t lifespan = harrowmere::effectiveLifespan(registry, c);
            CHECK(lifespan >= prev);
            prev = lifespan;
        }
    }
}

TEST_CASE(
    "TestSpecies_BiasCoreSumsDataTerms"
)
{
    // speciesBiasToward is the data-driven core 3.9 wraps: the self row's
    // xenophobiaBias plus the territory's culture-bias term toward the
    // other species. The shipped elf row carries xenophobiaBias 2000 —
    // data, not code (the doc's "Elves are aggressive and xenophobic").
    auto &registry         = shippedRegistry();
    const SpeciesData *elf = registry.speciesByKey("elf");
    REQUIRE(elf != nullptr);
    CHECK(elf->xenophobiaBias == 2'000);

    CHECK(harrowmere::speciesBiasToward(elf->xenophobiaBias, 0) == 2'000);
    CHECK(harrowmere::speciesBiasToward(elf->xenophobiaBias, -500) == 1'500); // a tolerant territory softens it
    CHECK(harrowmere::speciesBiasToward(elf->xenophobiaBias, 1'000) == 3'000);
    CHECK(harrowmere::speciesBiasToward(0, -10'000) == -10'000); // a fully tolerant culture pulls below zero
}
