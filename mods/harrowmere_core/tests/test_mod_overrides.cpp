#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "data/mod_overrides.hpp"
#include "data/registry.hpp"
#include "items/crafting.hpp"
#include "quests/quests.hpp"

namespace
{

using namespace harrowmere::data;

void writeText(
    const std::filesystem::path &path, const std::string &value
)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << value;
}

} // namespace

TEST_CASE(
    "TestMod_OverrideMergesAfterBase"
)
{
    const Json base = parseJsonText(
        R"([{"key":"human","id":1,"appearance":"base"},{"key":"elf","id":2,"appearance":"elf"}])", "base"
    )
                          .value();
    const Json over = parseJsonText(
        R"([{"key":"human","appearance":"modded"},{"key":"mossling","id":9,"appearance":"moss"}])", "over"
    )
                          .value();

    const Json merged = mergeModData("species.json", base, over);
    REQUIRE(merged.size() == 3);
    CHECK(merged[0]["key"] == "elf");
    CHECK(merged[1]["key"] == "human");
    CHECK(merged[1]["id"] == 1);
    CHECK(merged[1]["appearance"] == "modded");
    CHECK(merged[2]["key"] == "mossling");
}

TEST_CASE(
    "TestMod_IdOrderDeterministic"
)
{
    const auto root    = std::filesystem::temp_directory_path() / "harrowmere_mod_order_test";
    const auto base    = root / "base";
    const auto outputA = root / "out_a";
    const auto outputB = root / "out_b";
    std::filesystem::remove_all(root);
    writeText(base / "species.json", R"([{"id":1,"key":"human","sapient":true}])");
    writeText(root / "mods" / "zeta" / "data" / "species.json", R"([{"id":3,"key":"zeta","sapient":true}])");
    writeText(root / "mods" / "alpha" / "data" / "species.json", R"([{"id":2,"key":"alpha","sapient":true}])");

    std::vector<DataError> errors;
    REQUIRE(materializeMergedData(
        base, {{"zeta", root / "mods" / "zeta"}, {"alpha", root / "mods" / "alpha"}}, outputA, errors
    ));
    REQUIRE(errors.empty());
    const auto first = loadJsonFile((outputA / "species.json").string());
    REQUIRE(first.isOk());
    CHECK(first.value().size() == 3);
    CHECK(first.value()[0]["key"] == "alpha");
    CHECK(first.value()[1]["key"] == "human");
    CHECK(first.value()[2]["key"] == "zeta");

    REQUIRE(materializeMergedData(
        base, {{"alpha", root / "mods" / "alpha"}, {"zeta", root / "mods" / "zeta"}}, outputB, errors
    ));
    const auto second = loadJsonFile((outputB / "species.json").string());
    REQUIRE(second.isOk());
    CHECK(first.value() == second.value());
    std::filesystem::remove_all(root);
}

TEST_CASE(
    "TestMod_InvalidOverrideFailsStartup"
)
{
    const auto root   = std::filesystem::temp_directory_path() / "harrowmere_mod_invalid_test";
    const auto base   = root / "base";
    const auto output = root / "out";
    std::filesystem::remove_all(root);
    writeText(base / "species.json", R"([{"id":1,"key":"human"}])");
    writeText(root / "mods" / "broken" / "data" / "species.json", R"([{"id":2,"appearance":"no-key"}])");

    std::vector<DataError> errors;
    CHECK_FALSE(materializeMergedData(base, {{"broken", root / "mods" / "broken"}}, output, errors));
    REQUIRE_FALSE(errors.empty());
    CHECK(errors.front().file.find("species.json") != std::string::npos);
    CHECK(errors.front().message.find("key") != std::string::npos);
    std::filesystem::remove_all(root);
}

TEST_CASE(
    "TestMod_EntityArrayIdentityOverridePreservesBaseRows"
)
{
    const Json base
        = parseJsonText(R"([{"type":"MURDER","severity":9000},{"type":"THEFT","severity":2000}])", "base").value();
    const Json over   = parseJsonText(R"([{"type":"MURDER","severity":7500}])", "over").value();
    const Json merged = mergeModData("crime_data.json", base, over);
    REQUIRE(merged.size() == 2);
    CHECK(merged[0]["severity"] == 7500);
    CHECK(merged[1]["type"] == "THEFT");
}

TEST_CASE(
    "TestMod_ExampleSpeciesPackageLoadsThroughMergedValidators"
)
{
#ifdef HARROWMERE_CORE_DATA_DIR
    const std::filesystem::path coreData = HARROWMERE_CORE_DATA_DIR;
#else
    const std::filesystem::path coreData = std::filesystem::path("mods") / "harrowmere_core" / "data";
#endif
    const std::filesystem::path samplePackage = coreData.parent_path().parent_path() / "example_species_mod";
    const std::filesystem::path output
        = std::filesystem::temp_directory_path() / "harrowmere_example_species_merge_test";
    std::error_code ignored;
    std::filesystem::remove_all(output, ignored);

    std::vector<DataError> errors;
    REQUIRE(materializeMergedData(coreData, {{"example_species_mod", samplePackage}}, output, errors));
    REQUIRE(errors.empty());

    DataRegistry registry;
    REQUIRE(registry.load(output.string(), errors));
    REQUIRE(errors.empty());
    CHECK(registry.speciesByKey("mossling") != nullptr);

    harrowmere::RecipeTable recipes;
    REQUIRE(recipes.load((output / "recipes.json").string(), registry, errors));
    REQUIRE(errors.empty());
    CHECK(recipes.recipe("craft_mossling_wrap") != nullptr);

    harrowmere::quests::QuestTable quests;
    REQUIRE(harrowmere::quests::loadQuests((output / "quests.json").string(), quests, errors));
    CHECK(quests.size() > 0);
    CHECK(errors.empty());
    std::filesystem::remove_all(output, ignored);
}
