// test_registry.cpp — Task 1.3's pinned test suite.
//
// Implements the two tests pinned in phase-1-walk.md card 1.3's Tests block
// (names verbatim — common.md §3.5: "Test names are pinned; keep them"):
//
//   * TestRegistry_LoadOrderAndCrossRefErrors — the pinned load order is
//     observable in the report (a species-stage failure is reported before a
//     roles-stage failure), and the cross-reference pass reports dangling
//     references with file + pointer + reason.
//   * TestRegistry_SpeciesAccessIsO1Array — species rows live in an
//     id-indexed array: index == speciesId, contiguous, no scan.
//
// Plus the card's Done-when behaviors: a clean load commits the whole
// snapshot (all accessors live), and a broken file commits NOTHING ("fails
// with file+pointer+reason and no partial state").
//
// Broken data files are built the same way test_enums_data_loaders.cpp
// builds its broken rows: raw-string JSON written into a temp copy of the
// shipped package — never nlohmann outside src/data/ (D-DATA-1).
//
// Test harness: doctest v2.4.12 (vendored per task 0.6); no main here —
// tests/main.cpp owns DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/enums.hpp"
#include "data/registry.hpp"

namespace fs = std::filesystem;

using harrowmere::CrimeType;
using harrowmere::TerritoryType;
using harrowmere::data::DataError;
using harrowmere::data::DataErrors;
using harrowmere::data::DataRegistry;

namespace
{

// Resolves the shipped data directory: HARROWMERE_TEST_DATA env first (CI),
// then the CMake-baked absolute path, then a source-tree walk.
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

// A writable copy of the shipped package for the broken-file subcases.
fs::path makeTempPackage(
    const fs::path &from, const std::string &tag
)
{
    const fs::path dir = fs::temp_directory_path() / ("harrowmere_registry_" + tag);
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::copy(from, dir, fs::copy_options::recursive);
    return dir;
}

// First index in `errors` whose file path contains `fragment`; the vector
// size when the report carries no such error (a sentinel "not found" that
// still orders past every real entry).
std::size_t firstErrorIndexForFile(
    const DataErrors &errors, const std::string &fragment
)
{
    for (std::size_t i = 0; i < errors.size(); ++i)
        if (errors[i].file.find(fragment) != std::string::npos)
            return i;
    return errors.size();
}

} // namespace

// ---------------------------------------------------------------------------
// TestRegistry_LoadOrderAndCrossRefErrors
// The pinned load order is observable in the aggregated report (stage N's
// errors precede stage N+1's), and the cross-reference pass — which runs
// AFTER all load stages — reports dangling references with file + pointer +
// reason. A clean package commits; a broken one commits nothing.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestRegistry_LoadOrderAndCrossRefErrors"
)
{
    const fs::path dataDir = resolveDataDir();
    REQUIRE(fs::exists(dataDir / "species.json"));

    // ---- clean load: every stage green, no cross-ref errors, full commit ----
    bool cleanCommitted = false;
    {
        DataRegistry registry;
        DataErrors errors;
        cleanCommitted = registry.load(dataDir.string(), errors);
        CHECK(cleanCommitted);
        CHECK(errors.empty());
        CHECK(registry.ok());

        // The shipped set, pinned by tasks 1.1/1.2's files.
        CHECK(registry.speciesCount() == 8);
        CHECK(registry.itemCount() == 10);
        CHECK(registry.classCount() == 6);
        CHECK(registry.crimeData().complete());
        CHECK(registry.lawSetCount() == 4);
        CHECK(registry.roleCount() == 9);
        CHECK(registry.suspicionCount() == 6);

        // The accessors later tasks consume.
        CHECK(registry.defaultLaws().key == "default");
        CHECK(registry.lawSet("xenophobic_village") != nullptr);
        CHECK(registry.lawSet("missing_set") == nullptr);
        REQUIRE(registry.role("guard") != nullptr);
        CHECK(registry.role("guard")->critical);
        CHECK(registry.role("guard")->baseReportWeight == 9000);
        CHECK(registry.crime(CrimeType::MURDER).lethalForce);
        REQUIRE(registry.itemById(2) != nullptr); // guard_halberd
        CHECK(registry.itemById(2)->guardMarked);
        CHECK(registry.territoryTemplate(TerritoryType::CITY).lawSet == "tolerant_city");
        CHECK(registry.territoryTemplate(TerritoryType::CITY).safe == false);
    }

    // ---- load order: species (stage 1) reports before roles (stage 6) ----
    {
        const fs::path pkg = makeTempPackage(dataDir, "order");
        {
            std::ofstream out(pkg / "species.json");
            out << "{ broken"; // syntax error -> a stage-1 load error
        }
        {
            std::ofstream out(pkg / "roles.json");
            // Structurally a JSON array, but missing every required field ->
            // a stage-6 load error.
            out << R"([ { "key": "guard" } ])";
        }

        DataRegistry registry;
        DataErrors errors;
        CHECK_FALSE(registry.load(pkg.string(), errors));
        REQUIRE_FALSE(errors.empty());

        const std::size_t speciesAt = firstErrorIndexForFile(errors, "species.json");
        const std::size_t rolesAt   = firstErrorIndexForFile(errors, "roles.json");
        CHECK_MESSAGE(speciesAt < rolesAt, "the pinned load order: species stage errors precede roles stage errors");
        CHECK(speciesAt < errors.size()); // both stages reported
        CHECK(rolesAt < errors.size());

        // Order-test data was broken, everything else clean: no cross-ref
        // noise (the pass reads only stages that loaded).
        CHECK(firstErrorIndexForFile(errors, "items.json") == errors.size());
        CHECK(firstErrorIndexForFile(errors, "laws.json") == errors.size());
        fs::remove_all(pkg);
    }

    // ---- cross-ref: a dangling exemption species id (card 1.3 verbatim) ----
    {
        const fs::path pkg = makeTempPackage(dataDir, "exemption");
        {
            // Minimal valid laws.json (loader requirements met: the two set
            // keys the shipped territory_templates reference, each with all
            // eight conviction thresholds) whose only defect is an exemption
            // naming speciesId 42 — species.json stops at 7. tolerant_city
            // must exist so the template cross-ref has nothing to say; the
            // exemption is then the single dangling reference in the run.
            std::ofstream out(pkg / "laws.json");
            out << R"({ "sets": [
                { "key": "default",
                  "convictionThreshold": { "MURDER": 7000, "ASSAULT": 6000, "THEFT": 5000,
                      "ARSON": 6000, "TRESPASS": 4000, "POACHING": 5000,
                      "SMUGGLING": 5000, "OBSTRUCTION": 5000 },
                  "suppression": {},
                  "exemptions": { "42": { "MURDER": true } },
                  "suspicion": [] },
                { "key": "tolerant_city",
                  "convictionThreshold": { "MURDER": 8000, "ASSAULT": 7000, "THEFT": 6000,
                      "ARSON": 7000, "TRESPASS": 4000, "POACHING": 5000,
                      "SMUGGLING": 5000, "OBSTRUCTION": 5000 },
                  "suppression": {},
                  "exemptions": {},
                  "suspicion": [] } ] })";
        }

        DataRegistry registry;
        DataErrors errors;
        CHECK_FALSE(registry.load(pkg.string(), errors));
        REQUIRE_FALSE(errors.empty());
        CHECK(errors.size() == 1); // one dangling reference, one precise report

        CHECK(errors[0].file.find("laws.json") != std::string::npos);
        CHECK(errors[0].jsonPointer == "/sets/default/exemptions/42");
        CHECK(errors[0].message.find("42") != std::string::npos);
        CHECK(errors[0].message.find("species.json") != std::string::npos);
        fs::remove_all(pkg);
    }

    // ---- cross-ref: a territory template lawSet key that resolves nowhere ----
    {
        const fs::path pkg = makeTempPackage(dataDir, "lawset");
        {
            std::ofstream out(pkg / "territory_templates.json");
            out << R"({ "types": {
                "VILLAGE": { "population": [30, 60], "lawSet": "no_such_set", "safe": false },
                "CITY":    { "population": [150, 300], "lawSet": "default", "safe": false },
                "KINGDOM": { "population": [0, 0], "lawSet": "default", "safe": false },
                "EMPIRE":  { "population": [0, 0], "lawSet": "default", "safe": false } },
                "villages_per_city": 4, "cities_per_kingdom": 3, "kingdoms_per_empire": 4,
                "regionFlags": {} })";
        }

        DataRegistry registry;
        DataErrors errors;
        CHECK_FALSE(registry.load(pkg.string(), errors));
        REQUIRE_FALSE(errors.empty());
        CHECK(errors.size() == 1);

        CHECK(errors[0].file.find("territory_templates.json") != std::string::npos);
        CHECK(errors[0].jsonPointer == "/types/VILLAGE/lawSet");
        CHECK(errors[0].message.find("no_such_set") != std::string::npos);
        fs::remove_all(pkg);
    }

    // ---- no partial state: the failed load committed nothing ----
    {
        const fs::path pkg = makeTempPackage(dataDir, "partial");
        {
            std::ofstream out(pkg / "species.json");
            out << "{ broken";
        }

        DataRegistry registry;
        DataErrors errors;
        CHECK_FALSE(registry.load(pkg.string(), errors));
        CHECK_FALSE(registry.ok());
        CHECK(registry.speciesCount() == 0);
        CHECK(registry.itemCount() == 0);
        CHECK(registry.lawSetCount() == 0);
        CHECK(registry.roleCount() == 0);
        CHECK(registry.suspicionCount() == 0);
        CHECK(registry.speciesTable().empty());
        fs::remove_all(pkg);
    }

    // ---- reload contract: a failed reload keeps serving the previous commit ----
    {
        DataRegistry registry;
        DataErrors errors;
        REQUIRE(registry.load(dataDir.string(), errors));
        const auto *humanBefore = registry.speciesByKey("human");
        REQUIRE(humanBefore != nullptr);

        const fs::path pkg = makeTempPackage(dataDir, "reload");
        {
            std::ofstream out(pkg / "items.json");
            out << "{ broken";
        }
        CHECK_FALSE(registry.load(pkg.string(), errors)); // this load failed...
        CHECK(registry.ok());                             // ...but the snapshot still serves
        CHECK(registry.speciesCount() == 8);
        CHECK(registry.speciesByKey("human") == humanBefore);
        fs::remove_all(pkg);
    }

    (void)cleanCommitted;
}

// ---------------------------------------------------------------------------
// TestRegistry_SpeciesAccessIsO1Array
// Species access is a direct index into an id-keyed array (card 1.3): the
// row for id N lives at offset N in a contiguous table — no scan, no map —
// and the by-key lookup returns the SAME row the by-id index returns.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestRegistry_SpeciesAccessIsO1Array"
)
{
    const fs::path dataDir = resolveDataDir();
    REQUIRE(fs::exists(dataDir / "species.json"));

    DataRegistry registry;
    DataErrors errors;
    REQUIRE(registry.load(dataDir.string(), errors));
    REQUIRE(errors.empty());

    // The shipped table is dense over ids 0..7 (task 1.1's eight rows).
    REQUIRE(registry.speciesCount() == 8);
    for (std::uint16_t id = 0; id < 8; ++id)
        CHECK(registry.hasSpecies(id));
    CHECK_FALSE(registry.hasSpecies(8));
    CHECK_FALSE(registry.hasSpecies(9999));

    // Array addressing: id N lives at index N of a contiguous table.
    CHECK(&registry.species(2) == &registry.speciesTable()[2]);
    CHECK(&registry.species(2) + 1 == &registry.species(3));
    CHECK(&registry.species(6) + 1 == &registry.species(7));

    // The by-id index and the by-key scan land on the SAME row.
    CHECK(registry.speciesByKey("beastkin") == &registry.species(2));
    CHECK(registry.speciesByKey("player") == &registry.species(0));

    // The pinned rows (task 1.1's id assignments).
    CHECK(registry.species(0).key == "player");
    CHECK(registry.species(2).key == "beastkin");
    CHECK(registry.species(7).key == "goo_beast");

    // Unknown keys find nothing (an empty key matches no hole row either).
    CHECK(registry.speciesByKey("dwarf") == nullptr);
    CHECK(registry.speciesByKey("") == nullptr);
}
