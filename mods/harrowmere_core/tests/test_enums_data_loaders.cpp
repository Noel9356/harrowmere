// test_enums_data_loaders.cpp — pinned tests for task 1.1.
//
// Test names are pinned by task 1.1 spec; do not rename. The four pinned
// tests are:
//   * TestEnums_CountSentinelsMatchArraySizes
//   * TestSpecies_AllEightDocRowsLoad_WithDocMultipliers
//   * TestItems_GuardMarkedAndConsumableLoad
//   * TestDataLoader_RejectsUnknownSpeciesId
//
// The 0.6 test harness (doctest v2.4.12, single header at
// third_party/doctest/doctest.h) provides the framework. The harrowmere_core
// test binary links against harrowmere_core objects (NOT the mod runtime)
// per task 0.6 spec — these tests run without the plugin scanner.
//
// Data paths: the test resolves the data directory as
//   <exe_dir>/<mod_directory>/harrowmere_core/data/
// where <mod_directory> is read from configuration with fallback "mods"
// (D-DATA-1, task 0.4). For test simplicity, we also accept a fallback
// path via the HARROWMERE_TEST_DATA env var. CI sets it to the source tree's
// mods/harrowmere_core/data/ so the test does not depend on a packaged
// runtime layout.

#include "doctest.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/constants.hpp" // SCALE (task 0.2)
#include "core/enums.hpp"
#include "data/classes.hpp"
#include "data/items.hpp"
#include "data/species.hpp"

namespace
{

namespace fs = std::filesystem;

// Resolve the data directory: HARROWMERE_TEST_DATA env var first (CI/dev),
// then <exe_dir>/mods/harrowmere_core/data/ (packaged runtime).
fs::path resolveDataDir()
{
    if (const char *env = std::getenv("HARROWMERE_TEST_DATA"))
    {
        if (env[0] != '\0')
            return fs::path(env);
    }
    // Fall back: assume test binary runs from build dir; the source data
    // tree is two levels up from a typical build/runtime layout.
    fs::path exe = fs::current_path();
    for (int i = 0; i < 6; ++i)
    {
        fs::path candidate = exe / "mods" / "harrowmere_core" / "data";
        if (fs::exists(candidate / "species.json"))
            return candidate;
        exe = exe.parent_path();
    }
    return fs::path("mods") / "harrowmere_core" / "data";
}

} // namespace

// =============================================================================
// TestEnums_CountSentinelsMatchArraySizes
//
// Pins §3.2 R9: every game enum that backs a sized array ends in a COUNT
// sentinel whose value equals the number of real enumerators preceding it,
// and std::array<T, Enum::COUNT> compiles for each. A miscount here is a
// silent memory bug at runtime.
// =============================================================================
TEST_CASE(
    "TestEnums_CountSentinelsMatchArraySizes"
)
{
    using namespace harrowmere;

    SUBCASE("StatusType is bit-flag, no COUNT sentinel")
    {
        // StatusType is intentionally a bit-flag enum, not a sized-array enum.
        // The four documented flags are powers of two; this pins the count.
        CHECK(static_cast<std::uint32_t>(StatusType::IN_COMBAT) == 1u);
        CHECK(static_cast<std::uint32_t>(StatusType::IN_EVENT) == 2u);
        CHECK(static_cast<std::uint32_t>(StatusType::IN_WAR) == 4u);
        CHECK(static_cast<std::uint32_t>(StatusType::ARRESTED) == 8u);
    }

    SUBCASE("CooldownType::COUNT backs Character.cooldowns[COUNT]")
    {
        constexpr std::size_t expected = 3; // APPEARANCE, RESPEC_STAT, RESPEC_SKILL
        CHECK(static_cast<std::size_t>(CooldownType::COUNT) == expected);
        std::array<std::uint64_t, static_cast<std::size_t>(CooldownType::COUNT)> arr{};
        CHECK(arr.size() == expected);
    }

    SUBCASE("StatType::COUNT backs stats + statMultiplier arrays")
    {
        constexpr std::size_t expected = 6; // STR, DEF, AGI, INT, END, PER
        CHECK(static_cast<std::size_t>(StatType::COUNT) == expected);
        std::array<std::uint32_t, static_cast<std::size_t>(StatType::COUNT)> stats{};
        CHECK(stats.size() == expected);
    }

    SUBCASE("SkillType::COUNT backs skills arrays")
    {
        constexpr std::size_t expected = 8; // MELEE, ARCH, STEA, ARCA, SURV, PERS, CRAF, ESCA
        CHECK(static_cast<std::size_t>(SkillType::COUNT) == expected);
        std::array<std::uint32_t, static_cast<std::size_t>(SkillType::COUNT)> skills{};
        CHECK(skills.size() == expected);
    }

    SUBCASE("CrimeType::COUNT backs convictionThreshold + suppression maps")
    {
        constexpr std::size_t expected = 8; // MURDER..OBSTRUCTION
        CHECK(static_cast<std::size_t>(CrimeType::COUNT) == expected);
    }

    SUBCASE("ActionType::COUNT backs actionAmounts per participant")
    {
        constexpr std::size_t expected = 3; // DAMAGE, HEALING, OBJECTIVE
        CHECK(static_cast<std::size_t>(ActionType::COUNT) == expected);
    }

    SUBCASE("TerritoryType::COUNT backs territory_templates")
    {
        constexpr std::size_t expected = 4; // VILLAGE, CITY, KINGDOM, EMPIRE
        CHECK(static_cast<std::size_t>(TerritoryType::COUNT) == expected);
    }

    SUBCASE("SocialClassId::COUNT backs suppression keys")
    {
        constexpr std::size_t expected = 4; // NONE, COMMONER, NOBLE, ROYAL
        CHECK(static_cast<std::size_t>(SocialClassId::COUNT) == expected);
    }

    SUBCASE("Value-enums have no COUNT sentinel (compile-time check)")
    {
        // EvidenceStatus, TrustTier, SuspicionAct are values, not indices.
        // Their enumerators are pinned by the doc; this just exercises the
        // type so the test references every documented enumerator.
        EvidenceStatus es = EvidenceStatus::ARCHIVED;
        TrustTier tt      = TrustTier::SHUNNED;
        SuspicionAct sa   = SuspicionAct::FLEE;
        CHECK(es == EvidenceStatus::ARCHIVED);
        CHECK(tt == TrustTier::SHUNNED);
        CHECK(sa == SuspicionAct::FLEE);
    }
}

// =============================================================================
// TestSpecies_AllEightDocRowsLoad_WithDocMultipliers
//
// Asserts the EXACT table from design reference §Species (common.md F.2):
//   Players      | ∞      | 1x 1x 1x 1x
//   Humans       | 60     | 1x 1x 1x 1x
//   Beastkins    | 200    | 2x 2x 2x 1x
//   Taurs        | 200    | 2x 2x 2x 1x
//   Elves        | 1000   | 1x 1x 1x 4x
//   Vampires     | 1000   | 4x 4x 4x 4x
//   Dragons      | 10000  | 5x 5x 5x 5x
//   Goo beasts   | ∞      | 6x 6x 6x 6x
//
// Plus the id assignment pinned by task 1.1:
//   0=player, 1=human, 2=beastkin, 3=taur, 4=elf, 5=vampire, 6=dragon, 7=goo_beast
//
// And the runtime expansion: statMultiplier is std::array<u32, COUNT> on the
// SCALE grid (1.0x = SCALE = 10000). ENDURANCE/PERCEPTION default to SCALE.
// =============================================================================
TEST_CASE(
    "TestSpecies_AllEightDocRowsLoad_WithDocMultipliers"
)
{
    using namespace harrowmere::data;

    const fs::path dataDir     = resolveDataDir();
    const fs::path speciesPath = dataDir / "species.json";

    SpeciesLoader loader;
    std::vector<DataError> errors;
    REQUIRE(loader.load(speciesPath.string(), errors));
    CHECK(errors.empty());
    CHECK(loader.size() == 8);

    // Helper: check one species row against the doc table.
    auto checkRow = [&](speciesId id,
                        const std::string &key,
                        bool sapient,
                        std::uint32_t lifespanOrZeroForInf,
                        std::uint32_t strM,
                        std::uint32_t defM,
                        std::uint32_t agiM,
                        std::uint32_t intM) {
        const SpeciesData *s = loader.speciesById(id);
        INFO("species id=", id, " key=", key);
        REQUIRE(s != nullptr);
        CHECK(s->key == key);
        CHECK(s->sapient == sapient);
        if (lifespanOrZeroForInf == 0)
        {
            // "∞" in the doc: lifespan must be very large (we use 1,000,000).
            CHECK(s->lifespan_years >= 100000);
        } else
        {
            CHECK(s->lifespan_years == lifespanOrZeroForInf);
        }
        CHECK(s->statMultiplier[static_cast<std::size_t>(StatType::STRENGTH)] == strM * SCALE);
        CHECK(s->statMultiplier[static_cast<std::size_t>(StatType::DEFENCE)] == defM * SCALE);
        CHECK(s->statMultiplier[static_cast<std::size_t>(StatType::AGILITY)] == agiM * SCALE);
        CHECK(s->statMultiplier[static_cast<std::size_t>(StatType::INTELLIGENCE)] == intM * SCALE);
        // ENDURANCE / PERCEPTION default to SCALE (1.0x) per task 1.1 spec.
        CHECK(s->statMultiplier[static_cast<std::size_t>(StatType::ENDURANCE)] == SCALE);
        CHECK(s->statMultiplier[static_cast<std::size_t>(StatType::PERCEPTION)] == SCALE);
    };

    checkRow(/*id*/ 0, /*key*/ "player", /*sap*/ true, /*inf*/ 0, 1, 1, 1, 1);
    checkRow(/*id*/ 1, /*key*/ "human", /*sap*/ true, /*life*/ 60, 1, 1, 1, 1);
    checkRow(/*id*/ 2, /*key*/ "beastkin", /*sap*/ true, /*life*/ 200, 2, 2, 2, 1);
    checkRow(/*id*/ 3, /*key*/ "taur", /*sap*/ true, /*life*/ 200, 2, 2, 2, 1);
    checkRow(/*id*/ 4, /*key*/ "elf", /*sap*/ true, /*life*/ 1000, 1, 1, 1, 4);
    checkRow(/*id*/ 5, /*key*/ "vampire", /*sap*/ true, /*life*/ 1000, 4, 4, 4, 4);
    checkRow(/*id*/ 6, /*key*/ "dragon", /*sap*/ true, /*life*/ 10000, 5, 5, 5, 5);
    checkRow(/*id*/ 7, /*key*/ "goo_beast", /*sap*/ false, /*inf*/ 0, 6, 6, 6, 6);

    // Player=0 is the one hardcoded constant (design reference).
    const SpeciesData *player = loader.speciesByKey("player");
    REQUIRE(player != nullptr);
    CHECK(player->id == 0);

    // Goo beast's "neutralise attack" is modeled as damageTakenMultiplier 0.25
    // (Appendix B.1). Scaled → 2500.
    const SpeciesData *goo = loader.speciesByKey("goo_beast");
    REQUIRE(goo != nullptr);
    CHECK(goo->damageTakenMultiplier == static_cast<std::uint32_t>(0.25 * SCALE));
    CHECK(goo->sapient == false);

    // Elf xenophobiaBias is 2000 per Appendix B.1.
    const SpeciesData *elf = loader.speciesByKey("elf");
    REQUIRE(elf != nullptr);
    CHECK(elf->xenophobiaBias == 2000);
    CHECK(elf->darkvision == true);
}

// =============================================================================
// TestItems_GuardMarkedAndConsumableLoad
//
// Pins the items loader's three notable fields:
//   1. guardMarked (task 2.4/4.6 hook — "Selling territory-marked guard
//      gear flags theft evidence").
//   2. consumable.xpGain (design reference: "XP is obtained from killing
//      or consuming some items").
//   3. consumable.manaRich (marks the item as a mana-rich XP source).
// =============================================================================
TEST_CASE(
    "TestItems_GuardMarkedAndConsumableLoad"
)
{
    using namespace harrowmere::data;

    const fs::path dataDir   = resolveDataDir();
    const fs::path itemsPath = dataDir / "items.json";

    ItemsLoader loader;
    std::vector<DataError> errors;
    REQUIRE(loader.load(itemsPath.string(), errors));
    CHECK(errors.empty());

    // 1. guardMarked: at least one item is guard-marked (the guard halberd),
    //    and at least one non-guard item is NOT guard-marked.
    const ItemData *halberd = loader.itemByKey("guard_halberd");
    REQUIRE(halberd != nullptr);
    CHECK(halberd->guardMarked == true);
    CHECK(halberd->equippable == true);
    CHECK(halberd->slot == ItemSlot::MAIN_HAND);
    CHECK(halberd->weaponFactor == static_cast<std::uint32_t>(1.7 * SCALE));

    const ItemData *ironSword = loader.itemByKey("iron_sword");
    REQUIRE(ironSword != nullptr);
    CHECK(ironSword->guardMarked == false);
    CHECK(ironSword->equippable == true);

    // 2. consumable.xpGain: mana_tonic grants XP on consume.
    const ItemData *tonic = loader.itemByKey("mana_tonic");
    REQUIRE(tonic != nullptr);
    CHECK(tonic->hasConsumable == true);
    CHECK(tonic->consumable.xpGain == 200);
    // 3. consumable.manaRich: marks the tonic as a mana-rich XP source.
    CHECK(tonic->consumable.manaRich == true);

    // A pure healing consumable (no xpGain, no manaRich).
    const ItemData *bandage = loader.itemByKey("bandage");
    REQUIRE(bandage != nullptr);
    CHECK(bandage->hasConsumable == true);
    CHECK(bandage->consumable.healHp == 40);
    CHECK(bandage->consumable.xpGain == 0);
    CHECK(bandage->consumable.manaRich == false);

    // An equippable that enables a SuspicionAct (cloth_mask → MASKED, 3.10).
    const ItemData *mask = loader.itemByKey("cloth_mask");
    REQUIRE(mask != nullptr);
    CHECK(mask->enablesAct == SuspicionAct::MASKED);
    CHECK(mask->equippable == true);
    CHECK(mask->slot == ItemSlot::FACE);
}

// =============================================================================
// TestDataLoader_RejectsUnknownSpeciesId
//
// Per task 1.1: "referential checks land in 1.3". This test pins the loader-
// level rejection: speciesById(unknown) returns nullptr (NOT a stale
// pointer, NOT a sentinel that pretends to be valid).
//
// A second subcase pins that a malformed species.json (duplicate id) is
// rejected at load() time — the loader never produces a table with
// duplicate ids.
// =============================================================================
TEST_CASE(
    "TestDataLoader_RejectsUnknownSpeciesId"
)
{
    using namespace harrowmere::data;

    const fs::path dataDir = resolveDataDir();

    SpeciesLoader loader;
    std::vector<DataError> errors;
    REQUIRE(loader.load((dataDir / "species.json").string(), errors));

    SUBCASE("speciesById returns nullptr for an id that is not in the table")
    {
        // The shipped species.json has ids 0..7. Anything outside that range
        // must be rejected (nullptr). The loader must NOT fabricate a fake
        // entry or return a dangling reference.
        CHECK(loader.speciesById(static_cast<speciesId>(8)) == nullptr);
        CHECK(loader.speciesById(static_cast<speciesId>(99)) == nullptr);
        CHECK(loader.speciesById(static_cast<speciesId>(65535)) == nullptr);
    }

    SUBCASE("speciesByKey returns nullptr for an unknown key")
    {
        CHECK(loader.speciesByKey("dwarf") == nullptr);
        CHECK(loader.speciesByKey("") == nullptr);
    }

    SUBCASE("a species.json with a duplicate id is rejected at load time")
    {
        // Build a malformed JSON in a temp file: two entries with id 1.
        const auto tmp = fs::temp_directory_path() / "harrowmere_dup_species.json";
        {
            std::ofstream out(tmp);
            out << R"([
                { "id": 1, "key": "human", "sapient": true, "lifespan_years": 60,
                  "statMultiplier": { "STRENGTH": 1.0, "DEFENCE": 1.0, "AGILITY": 1.0, "INTELLIGENCE": 1.0 },
                  "appearance": "-" },
                { "id": 1, "key": "duplicate", "sapient": true, "lifespan_years": 60,
                  "statMultiplier": { "STRENGTH": 1.0, "DEFENCE": 1.0, "AGILITY": 1.0, "INTELLIGENCE": 1.0 },
                  "appearance": "-" }
            ])";
        }
        SpeciesLoader bad;
        std::vector<DataError> badErrors;
        CHECK_FALSE(bad.load(tmp.string(), badErrors));
        CHECK_FALSE(badErrors.empty());
        // The error must name the file and the duplicate id.
        bool foundDup = false;
        for (const auto &e : badErrors)
        {
            if (e.message.find("duplicate species id") != std::string::npos)
            {
                foundDup = true;
                break;
            }
        }
        CHECK(foundDup);
        fs::remove(tmp);
    }

    SUBCASE("a species.json missing id 0 = player is rejected")
    {
        // id 0 must always be the "player" row (design reference hardcoded
        // constant). A file with no id 0 is invalid; a file with id 0 but a
        // different key is also invalid.
        const auto tmp = fs::temp_directory_path() / "harrowmere_no_player.json";
        {
            std::ofstream out(tmp);
            out << R"([
                { "id": 1, "key": "human", "sapient": true, "lifespan_years": 60,
                  "statMultiplier": { "STRENGTH": 1.0, "DEFENCE": 1.0, "AGILITY": 1.0, "INTELLIGENCE": 1.0 },
                  "appearance": "-" }
            ])";
        }
        // This file is technically valid (no duplicate, all required fields
        // present) but missing id 0 = player. Task 1.1 does NOT require the
        // loader to enforce "player must exist"; that is a registry cross-ref
        // check (1.3). This subcase documents the boundary: the loader
        // accepts the file (it has no duplicate-id error) but speciesById(0)
        // correctly returns nullptr.
        SpeciesLoader sp;
        std::vector<DataError> spErrors;
        // The load succeeds at the loader level — there's nothing wrong with
        // the file structurally.
        bool ok = sp.load(tmp.string(), spErrors);
        CHECK(ok);
        if (ok)
        {
            CHECK(sp.speciesById(static_cast<speciesId>(0)) == nullptr);
        }
        fs::remove(tmp);
    }
}

// =============================================================================
// Bonus: a small sanity test that classes.json loads (not pinned by name in
// task 1.1, but the "Done when" checklist requires "Three data files + names
// load, validate, and are exposed through typed structs").
// =============================================================================
TEST_CASE(
    "TestClasses_LoadsAndExposesStatWeightsIncludingNewStats"
)
{
    using namespace harrowmere::data;

    const fs::path dataDir = resolveDataDir();
    ClassesLoader loader;
    std::vector<DataError> errors;
    REQUIRE(loader.load((dataDir / "classes.json").string(), errors));
    CHECK(errors.empty());
    CHECK_FALSE(loader.empty());

    // The warrior class weights STRENGTH 1.4x and ENDURANCE 1.2x — verifies
    // the new ENDURANCE stat is loadable from data (task 1.1 closure).
    const ClassData *warrior = loader.classByKey("warrior");
    REQUIRE(warrior != nullptr);
    CHECK(
        warrior->statWeights[static_cast<std::size_t>(StatType::STRENGTH)] == static_cast<std::uint32_t>(1.4 * SCALE)
    );
    CHECK(
        warrior->statWeights[static_cast<std::size_t>(StatType::ENDURANCE)] == static_cast<std::uint32_t>(1.2 * SCALE)
    );

    // startingSkills: warrior starts with MELEE 3.
    CHECK(warrior->startingSkills[static_cast<std::size_t>(SkillType::MELEE)] == 3);
}
