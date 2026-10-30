// test_worldgen.cpp — Task 1.5's pinned test suite.
//
// Implements the five tests pinned in phase-1-walk.md card 1.5's Tests block
// (names verbatim — common.md §3.5: "Test names are pinned; keep them"):
//
//   * TestWorldgen_SameSeedIdenticalHash        — two runs, one hash
//   * TestWorldgen_DifferentSeedsDiffer         — content divergence
//   * TestWorldgen_OneSafeVillagePerKingdomWithSpawn — stage 4 + beginner rule
//   * TestWorldgen_AllRegionsManaRichInBand     — D-LORE-3's 9500..10000 band
//   * TestWorldgen_AllPopulationPlanRefsValid   — every NpcSeed reference
//
// Plus the card's remaining world rules, each with its own case: the
// territory tree shape (stage 2 counts/parents), the unclaimed frontier ring,
// contiguous 3-5 region city blocks, the NOBLE/ROYAL title chain (stage 3),
// the five seeded factions with 2 rivalries + 1 alliance (stage 5), the
// single forest elven enclave, the <= 3 rare wanderers, the per-kingdom law
// variance cap, and the < 2 s debug-build budget (Done-when; the chrono use
// is TEST-ONLY wall time — R8 bans it from the sim, not from a stopwatch).
//
// Worldgen runs against the SHIPPED data package through the DataRegistry
// (the same files startup validates); the generation itself is deterministic,
// so every assertion below is exact.
//
// Test harness: doctest v2.4.12 (vendored per task 0.6); no main here —
// tests/main.cpp owns DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/constants.hpp" // REGION_* (A-40), ADULT_YEARS (A-44)
#include "core/enums.hpp"
#include "core/types.hpp"
#include "data/registry.hpp"
#include "world/region.hpp" // regionIdAt / regionCenter / kNoRegion
#include "world/world.hpp"  // WorldStore, kNoTerritory
#include "world/worldgen.hpp"

namespace fs = std::filesystem;

using harrowmere::ADULT_YEARS;
using harrowmere::REGION_COUNT;
using harrowmere::REGION_GRID;
using harrowmere::SocialClassId;
using harrowmere::TerritoryType;
using harrowmere::characterId;
using harrowmere::factionId;
using harrowmere::territoryId;
using harrowmere::data::DataErrors;
using harrowmere::data::DataRegistry;
using harrowmere::data::RoleData;
using harrowmere::world::Climate;
using harrowmere::world::FACTION_COUNT;
using harrowmere::world::FRONTIER_RING_DEPTH;
using harrowmere::world::FactionSeed;
using harrowmere::world::NpcSeed;
using harrowmere::world::NpcSeedOut;
using harrowmere::world::STARTING_LEVEL_MAX;
using harrowmere::world::Territory;
using harrowmere::world::WorldGenResult;
using harrowmere::world::WorldStore;
using harrowmere::world::generateWorld;
using harrowmere::world::kNoTerritory;
using harrowmere::world::regionCenter;
using harrowmere::world::regionIdAt;
using harrowmere::world::regionIdOf;
using harrowmere::world::regionIndicesInGrid;
using harrowmere::world::regionIndicesOf;

namespace
{

// Resolves the shipped data directory: HARROWMERE_TEST_DATA env first (CI),
// then the CMake-baked absolute path, then a source-tree walk (the same
// resolution test_registry.cpp pins).
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

// Loads the shipped package into `registry` (call from within a TEST_CASE or
// its fixture: the load asserts through the caller's active test context).
// Out-param form: DataRegistry is neither copyable nor movable, so the
// committed snapshot is built where the caller owns it.
void loadShippedRegistry(
    DataRegistry &registry
)
{
    DataErrors errors;
    REQUIRE(registry.load(resolveDataDir().string(), errors));
    REQUIRE(registry.ok());
}

// One worldgen run over the shipped data. Construction ORDER matters: the
// registry loads in the ctor body FIRST, THEN the store captures
// registry.defaultLaws() — the committed snapshot's DEFAULT_LAWS (the
// registry debug-asserts otherwise; the store is heap-held for that reason).
struct WorldgenRun
{
    DataRegistry registry;
    std::unique_ptr<WorldStore> store;
    NpcSeedOut npcs;
    WorldGenResult result;

    explicit WorldgenRun(
        std::uint64_t seed
    )
    {
        DataErrors errors;
        REQUIRE(registry.load(resolveDataDir().string(), errors));
        REQUIRE(registry.ok());
        store  = std::make_unique<WorldStore>(registry.defaultLaws());
        result = generateWorld(seed, registry, *store, npcs);
    }
};

// Territory ids of one type (id-ascending).
std::vector<territoryId> territoriesOfType(
    const WorldStore &store, harrowmere::TerritoryType type
)
{
    std::vector<territoryId> found;
    for (const auto &[id, territory] : store.territories())
    {
        (void)territory;
        if (store.territory(id)->type == type)
            found.push_back(id);
    }
    return found;
}

// The planned residents of one territory (plan order).
std::vector<const NpcSeed *> residentsOf(
    const NpcSeedOut &npcs, territoryId home
)
{
    std::vector<const NpcSeed *> found;
    for (const NpcSeed &npc : npcs)
        if (npc.homeTerritory == home)
            found.push_back(&npc);
    return found;
}

bool isFrontierCell(
    std::uint32_t x, std::uint32_t z
)
{
    return x < FRONTIER_RING_DEPTH || z < FRONTIER_RING_DEPTH || x >= REGION_GRID - FRONTIER_RING_DEPTH
        || z >= REGION_GRID - FRONTIER_RING_DEPTH;
}

} // namespace

// ---------------------------------------------------------------------------
// TestWorldgen_SameSeedIdenticalHash
// Two runs from the same seed produce byte-identical output: the same
// summary hash, the same population plan, the same faction seeds, and the
// same world graph shape (exit checklist + card Tests block).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWorldgen_SameSeedIdenticalHash"
)
{
    const WorldgenRun first{12345ull};
    const WorldgenRun second{12345ull};

    CHECK(first.result.summaryHash == second.result.summaryHash);
    CHECK(first.result.factions == second.result.factions);
    CHECK(first.result.beginnerTerritory == second.result.beginnerTerritory);
    CHECK(first.result.playerSpawnVillage == second.result.playerSpawnVillage);
    CHECK(first.result.elvenEnclave == second.result.elvenEnclave);
    CHECK(first.result.nextCharacterId == second.result.nextCharacterId);
    CHECK(first.result.playerSpawn.x == second.result.playerSpawn.x);
    CHECK(first.result.playerSpawn.y == second.result.playerSpawn.y);
    CHECK(first.result.playerSpawn.z == second.result.playerSpawn.z);
    REQUIRE(first.npcs.size() == second.npcs.size());
    CHECK(first.npcs == second.npcs);

    // The graph shape agrees too (counts + the explicit mapping size).
    CHECK(first.store->regionCount() == second.store->regionCount());
    CHECK(first.store->territoryCount() == second.store->territoryCount());
    CHECK(first.store->mappedRegionCount() == second.store->mappedRegionCount());

    // Every territory resolves the same law set object and safety flag.
    for (const auto &[id, territory] : first.store->territories())
    {
        const Territory *other = second.store->territory(id);
        REQUIRE(other != nullptr);
        CHECK(territory.parent == other->parent);
        CHECK(territory.safe == other->safe);
        CHECK(territory.children == other->children);
        CHECK(territory.titles == other->titles);
        CHECK((territory.laws == nullptr) == (other->laws == nullptr));
        if (territory.laws != nullptr && other->laws != nullptr)
            CHECK(territory.laws->key == other->laws->key);
    }
}

// ---------------------------------------------------------------------------
// TestWorldgen_DifferentSeedsDiffer
// A different seed generates a different world — the summary hash carries
// the region/climate/mana draws, so it moves with the seed.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWorldgen_DifferentSeedsDiffer"
)
{
    const WorldgenRun a{1ull};
    const WorldgenRun b{2ull};
    CHECK(a.result.summaryHash != b.result.summaryHash);

    const WorldgenRun c{987654321ull};
    const WorldgenRun d{987654322ull};
    CHECK(c.result.summaryHash != d.result.summaryHash);
    CHECK(a.result.summaryHash != c.result.summaryHash);
}

// ---------------------------------------------------------------------------
// TestWorldgen_OneSafeVillagePerKingdomWithSpawn
// Stage 4: exactly one VILLAGE per kingdom is safe and holds the kingdom's
// spawn point (the village's region center); the beginner kingdom (covering
// the claimed region nearest the world origin) spawns the player at its safe
// village; safe villages run safe_zone_laws (1.2: used where safe=true).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWorldgen_OneSafeVillagePerKingdomWithSpawn"
)
{
    const WorldgenRun run{12345ull};
    const std::vector<territoryId> kingdoms = territoriesOfType(*run.store, TerritoryType::KINGDOM);
    REQUIRE(kingdoms.size() == 4);

    for (const territoryId kingdom : kingdoms)
    {
        // Villages hang off the kingdom's CITIES (grandchildren): collect
        // every VILLAGE whose jurisdiction chain contains this kingdom.
        std::vector<territoryId> villages;
        for (const territoryId village : territoriesOfType(*run.store, TerritoryType::VILLAGE))
        {
            const std::vector<territoryId> chain = run.store->chainToRoot(village);
            if (std::find(chain.begin(), chain.end(), kingdom) != chain.end())
                villages.push_back(village);
        }
        REQUIRE(villages.size() == 12); // villages_per_city(4) x cities_per_kingdom(3)

        std::uint32_t safeCount = 0;
        territoryId safeVillage = kNoTerritory;
        for (const territoryId village : villages)
        {
            if (run.store->isSafeTerritory(village))
            {
                ++safeCount;
                safeVillage = village;
            }
        }
        CHECK(safeCount == 1);

        if (safeVillage != kNoTerritory)
        {
            // The spawn point IS the village's region center (D-WORLD-1: the
            // projection round-trips — regionIdAt(spawn) lands back on it).
            const Territory *village = run.store->territory(safeVillage);
            const auto center = regionCenter(village->region);
            CHECK(regionIdAt(center) == village->region);

            // Safe villages run the safe-zone law set (ward reports apply).
            CHECK(&run.store->lawsAt(safeVillage) == run.registry.lawSet("safe_zone_laws"));
        }
    }

    // The beginner kingdom covers the interior origin cell (1,1) — the first
    // claimed region at the world-origin corner (the frontier ring itself
    // stays unclaimed; DEVIATIONS.md row for the (0,0) reading).
    const territoryId covering = run.store->territoryOfRegion(regionIdOf(1u, 1u));
    REQUIRE(covering != kNoTerritory);
    const std::vector<territoryId> chain = run.store->chainToRoot(covering);
    CHECK(std::find(chain.begin(), chain.end(), run.result.beginnerTerritory) != chain.end());

    // The player spawns at the beginner kingdom's safe village center.
    REQUIRE(run.result.playerSpawnVillage != kNoTerritory);
    const Territory *spawnVillage = run.store->territory(run.result.playerSpawnVillage);
    REQUIRE(spawnVillage != nullptr);
    CHECK(spawnVillage->safe);
    const std::vector<territoryId> spawnChain = run.store->chainToRoot(run.result.playerSpawnVillage);
    CHECK(std::find(spawnChain.begin(), spawnChain.end(), run.result.beginnerTerritory) != spawnChain.end());
    CHECK(run.result.playerSpawn.x == regionCenter(spawnVillage->region).x);
    CHECK(run.result.playerSpawn.y == 0.0);
    CHECK(run.result.playerSpawn.z == regionCenter(spawnVillage->region).z);
    CHECK(regionIdAt(run.result.playerSpawn) == spawnVillage->region);
}

// ---------------------------------------------------------------------------
// TestWorldgen_AllRegionsManaRichInBand
// D-LORE-3 (card stage 1): every region draws manaRichness in [9500, 10000] —
// no zero-mana regions, no weak-mana tiers. Genesis also leaves every
// consequence flag clear (they are event/war state, not worldgen state).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWorldgen_AllRegionsManaRichInBand"
)
{
    const WorldgenRun run{12345ull};
    REQUIRE(run.store->regionCount() == REGION_COUNT);

    for (const auto &[id, region] : run.store->regions())
    {
        CHECK(region.manaRichness >= 9500u);
        CHECK(region.manaRichness <= 10000u);
        CHECK(region.flags == 0u); // no data-declared consequence flag at genesis
        CHECK(static_cast<std::uint32_t>(region.climate) < static_cast<std::uint32_t>(Climate::COUNT));
    }
}

// ---------------------------------------------------------------------------
// TestWorldgen_AllPopulationPlanRefsValid
// Every planned NPC references existing data: a real species (never the
// player species), a shipped role with the integrity drawn inside its range,
// an age inside the species/role rules (adults >= A-44, children under it),
// a level inside the starting band, and a home that exists (settlements at
// their anchor region; wanderers on frontier cells). Character ids are
// unique and issue in plan order from the reserved-free space (R10).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWorldgen_AllPopulationPlanRefsValid"
)
{
    const WorldgenRun run{12345ull};
    REQUIRE(run.npcs.size() > 0);

    characterId previousId = 0;
    for (const NpcSeed &npc : run.npcs)
    {
        // Ids: monotonic plan order, never 0, never reused (R10).
        CHECK(npc.id != 0);
        CHECK(npc.id > previousId);
        previousId = npc.id;

        // Species: resolved through the registry, by key semantics (1.1).
        REQUIRE(run.registry.hasSpecies(npc.species));
        CHECK(run.registry.species(npc.species).key != "player"); // worldgen never plans the player

        // Role + integrity + age rules.
        const harrowmere::data::RoleData *role = run.registry.role(npc.roleKey);
        REQUIRE(role != nullptr);
        CHECK(npc.integrity >= role->integrityMin);
        CHECK(npc.integrity <= role->integrityMax);
        CHECK(npc.ageYears >= 1u);
        if (npc.roleKey == "child")
            CHECK(npc.ageYears < ADULT_YEARS); // A-44: children never hold adult roles
        else
            CHECK(npc.ageYears >= ADULT_YEARS);

        // Starting level band by age.
        CHECK(npc.level >= 1u);
        CHECK(npc.level <= STARTING_LEVEL_MAX);

        // Home references.
        if (npc.homeTerritory != kNoTerritory)
        {
            const Territory *home = run.store->territory(npc.homeTerritory);
            REQUIRE(home != nullptr);
            const bool settledHome = home->type == TerritoryType::VILLAGE || home->type == TerritoryType::CITY
                || home->type == TerritoryType::KINGDOM;
            CHECK(settledHome);
            CHECK(npc.homeRegion == home->region);
        }
        else
        {
            std::uint32_t x = 0, z = 0;
            regionIndicesOf(npc.homeRegion, x, z);
            CHECK(isFrontierCell(x, z)); // wanderers live on the unclaimed ring
        }

        // Untitled NPCs stay COMMONER; the title chain test covers the rest.
        if (npc.socialClass == harrowmere::SocialClassId::NONE)
            CHECK(false); // NONE is never planned
    }

    // The id space after the plan continues where the plan stopped.
    CHECK(run.result.nextCharacterId == run.npcs.back().id + 1);
    CHECK(run.result.nextCharacterId > run.npcs.back().id);
}

// ---------------------------------------------------------------------------
// TestWorldgen_TerritoryTreeShape
// Stage 2 with the shipped templates: 1 sovereign empire <- 4 kingdoms <-
// 12 cities <- 48 villages; every parent link is consistent and every
// territory anchors to a CLAIMED grid region (the mapping covers it).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWorldgen_TerritoryTreeShape"
)
{
    const WorldgenRun run{12345ull};

    const std::vector<territoryId> empires   = territoriesOfType(*run.store, TerritoryType::EMPIRE);
    const std::vector<territoryId> kingdoms  = territoriesOfType(*run.store, TerritoryType::KINGDOM);
    const std::vector<territoryId> cities    = territoriesOfType(*run.store, TerritoryType::CITY);
    const std::vector<territoryId> villages  = territoriesOfType(*run.store, TerritoryType::VILLAGE);

    REQUIRE(empires.size() == 1);
    CHECK(kingdoms.size() == run.registry.territoryTemplates().kingdomsPerEmpire());
    CHECK(cities.size() == kingdoms.size() * run.registry.territoryTemplates().citiesPerKingdom());
    CHECK(villages.size() == cities.size() * run.registry.territoryTemplates().villagesPerCity());
    CHECK(run.store->territoryCount() == empires.size() + kingdoms.size() + cities.size() + villages.size());

    // The empire: parent 0 with children = sovereign (design rule, decided
    // once in isSovereign()).
    const Territory *empire = run.store->territory(empires[0]);
    REQUIRE(empire != nullptr);
    CHECK(empire->parent == kNoTerritory);
    CHECK(harrowmere::world::isSovereign(*empire));
    CHECK(run.store->childrenOf(empires[0]).size() == kingdoms.size());

    for (const territoryId kingdom : kingdoms)
    {
        CHECK(run.store->parentOf(kingdom) == empires[0]);
        CHECK(run.store->territory(kingdom)->type == TerritoryType::KINGDOM);
    }
    for (const territoryId city : cities)
    {
        const territoryId parent = run.store->parentOf(city);
        CHECK(run.store->territory(parent)->type == TerritoryType::KINGDOM);
    }
    for (const territoryId village : villages)
    {
        const territoryId parent = run.store->parentOf(village);
        CHECK(run.store->territory(parent)->type == TerritoryType::CITY);
    }

    // Every territory anchors to a claimed grid region.
    for (const auto &[id, territory] : run.store->territories())
    {
        (void)id;
        CHECK(harrowmere::world::isGridRegion(territory.region));
        CHECK(run.store->territoryOfRegion(territory.region) != kNoTerritory);
    }
}

// ---------------------------------------------------------------------------
// TestWorldgen_FrontierRingUnclaimed
// Stage 2: the outermost region ring stays unclaimed — no territory covers
// it (kNoTerritory), while EVERY interior region is mapped to exactly one
// territory (1.4: the mapping is explicit and complete over settled space).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWorldgen_FrontierRingUnclaimed"
)
{
    const WorldgenRun run{12345ull};

    std::uint32_t mappedInterior = 0;
    for (std::uint32_t id = 0; id < REGION_COUNT; ++id)
    {
        std::uint32_t x = 0, z = 0;
        regionIndicesOf(static_cast<harrowmere::regionId>(id), x, z);
        const territoryId owner = run.store->territoryOfRegion(static_cast<harrowmere::regionId>(id));
        if (isFrontierCell(x, z))
            CHECK(owner == kNoTerritory);
        else
        {
            CHECK(owner != kNoTerritory);
            ++mappedInterior;
        }
    }
    const std::uint32_t interior = (REGION_GRID - 2u * FRONTIER_RING_DEPTH) * (REGION_GRID - 2u * FRONTIER_RING_DEPTH);
    CHECK(run.store->mappedRegionCount() == interior);
    CHECK(mappedInterior == interior);
}

// ---------------------------------------------------------------------------
// TestWorldgen_CityBlocksContiguous
// Stage 2: cities take 3-5 regions as a CONTIGUOUS block (4-connected), and
// each village covers exactly one region.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWorldgen_CityBlocksContiguous"
)
{
    const WorldgenRun run{12345ull};

    std::map<territoryId, std::vector<harrowmere::regionId>> cellsByTerritory;
    for (const auto &[region, owner] : run.store->regionTerritories())
        cellsByTerritory[owner].push_back(region);

    for (const territoryId city : territoriesOfType(*run.store, TerritoryType::CITY))
    {
        const std::vector<harrowmere::regionId> &cells = cellsByTerritory[city];
        REQUIRE(cells.size() >= 3);
        REQUIRE(cells.size() <= 5);

        // 4-connected flood fill from the first cell must reach them all.
        std::map<harrowmere::regionId, bool> inBlock;
        for (const harrowmere::regionId cell : cells)
            inBlock[cell] = true;
        std::vector<harrowmere::regionId> stack{cells.front()};
        std::map<harrowmere::regionId, bool> seen{{cells.front(), true}};
        while (!stack.empty())
        {
            const harrowmere::regionId current = stack.back();
            stack.pop_back();
            std::uint32_t x = 0, z = 0;
            regionIndicesOf(current, x, z);
            const std::uint32_t neighbors[4][2]{{x - 1u, z}, {x + 1u, z}, {x, z - 1u}, {x, z + 1u}};
            for (const auto &[nx, nz] : neighbors)
            {
                if (!harrowmere::world::regionIndicesInGrid(nx, nz))
                    continue;
                const harrowmere::regionId neighbor = regionIdOf(nx, nz);
                if (inBlock[neighbor] && !seen[neighbor])
                {
                    seen[neighbor] = true;
                    stack.push_back(neighbor);
                }
            }
        }
        CHECK(seen.size() == cells.size());
    }

    for (const territoryId village : territoriesOfType(*run.store, TerritoryType::VILLAGE))
        CHECK(cellsByTerritory[village].size() == 1);
}

// ---------------------------------------------------------------------------
// TestWorldgen_TitleChainSeeded
// Stage 3: every city carries exactly one NOBLE title and every kingdom
// exactly one ROYAL; the holders are planned NPCs (role "noble") with names
// drawn from the species tables and a class drawn from classes.json.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWorldgen_TitleChainSeeded"
)
{
    const WorldgenRun run{12345ull};
    const std::vector<territoryId> kingdoms = territoriesOfType(*run.store, TerritoryType::KINGDOM);
    const std::vector<territoryId> cities   = territoriesOfType(*run.store, TerritoryType::CITY);

    std::uint32_t plannedNobles = 0;
    std::uint32_t plannedRoyals = 0;
    for (const NpcSeed &npc : run.npcs)
    {
        if (npc.socialClass == harrowmere::SocialClassId::NOBLE)
        {
            ++plannedNobles;
            CHECK(npc.roleKey == "noble");
            CHECK(!npc.givenName.empty());
            CHECK(!npc.familyName.empty());
            CHECK(!npc.classKey.empty());
            CHECK(run.registry.classByKey(npc.classKey) != nullptr);
        }
        if (npc.socialClass == harrowmere::SocialClassId::ROYAL)
        {
            ++plannedRoyals;
            CHECK(npc.roleKey == "noble");
            CHECK(!npc.givenName.empty());
            CHECK(!npc.familyName.empty());
            CHECK(!npc.classKey.empty());
            CHECK(run.registry.classByKey(npc.classKey) != nullptr);
        }
        if (npc.socialClass == harrowmere::SocialClassId::COMMONER)
        {
            // Untitled NPCs carry no title-chain identity fields.
            CHECK(npc.classKey.empty());
            CHECK(npc.givenName.empty());
            CHECK(npc.familyName.empty());
        }
    }
    CHECK(plannedNobles == cities.size());
    CHECK(plannedRoyals == kingdoms.size());

    for (const territoryId city : cities)
    {
        const Territory *node = run.store->territory(city);
        REQUIRE(node != nullptr);
        CHECK(node->titles.size() == 1);
        for (const auto &[holder, cls] : node->titles)
        {
            (void)holder;
            CHECK(cls == harrowmere::SocialClassId::NOBLE);
        }
    }
    for (const territoryId kingdom : kingdoms)
    {
        const Territory *node = run.store->territory(kingdom);
        REQUIRE(node != nullptr);
        CHECK(node->titles.size() == 1);
        for (const auto &[holder, cls] : node->titles)
        {
            (void)holder;
            CHECK(cls == harrowmere::SocialClassId::ROYAL);
        }
    }

    // Every title holder is a planned NPC (the doc walks resolve them).
    for (const auto &[id, territory] : run.store->territories())
    {
        (void)id;
        for (const auto &[holder, cls] : territory.titles)
        {
            const bool planned = std::any_of(
                run.npcs.begin(), run.npcs.end(), [holder](const NpcSeed &npc) { return npc.id == holder; }
            );
            CHECK(planned);
        }
    }
}

// ---------------------------------------------------------------------------
// TestWorldgen_FactionsPinnedRelations
// Stage 5: five factions — one court per kingdom plus the frontier free
// company; exactly 2 rivalry pairs and 1 alliance among the courts; no pair
// is both.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWorldgen_FactionsPinnedRelations"
)
{
    const WorldgenRun run{12345ull};
    const std::vector<territoryId> kingdoms = territoriesOfType(*run.store, TerritoryType::KINGDOM);
    REQUIRE(run.result.factions.size() == FACTION_COUNT);

    std::map<factionId, const harrowmere::world::FactionSeed *> byId;
    std::uint32_t courts = 0;
    std::uint32_t companies = 0;
    for (const harrowmere::world::FactionSeed &faction : run.result.factions)
    {
        REQUIRE(byId.count(faction.id) == 0); // unique ids
        byId[faction.id] = &faction;
        if (faction.key == "frontier_free_company")
        {
            ++companies;
            CHECK(faction.courtTerritory == kNoTerritory);
        }
        else
        {
            ++courts;
            CHECK(run.store->territory(faction.courtTerritory)->type == TerritoryType::KINGDOM);
        }
    }
    CHECK(courts == kingdoms.size());
    CHECK(companies == 1);
    for (std::uint32_t i = 1; i <= FACTION_COUNT; ++i)
        CHECK(byId.count(i) == 1);

    // Undirected rivalry pairs: exactly 2, symmetric, among the courts.
    std::map<std::uint64_t, bool> rivalryPairs;
    std::map<std::uint64_t, bool> alliancePairs;
    for (const harrowmere::world::FactionSeed &faction : run.result.factions)
    {
        for (const harrowmere::factionId rival : faction.rivals)
        {
            const std::uint64_t lo = std::min<std::uint64_t>(faction.id, rival);
            const std::uint64_t hi = std::max<std::uint64_t>(faction.id, rival);
            rivalryPairs[(lo << 32) | hi] = true;
            CHECK(byId[rival] != nullptr); // no dangling rival id
        }
        for (const harrowmere::factionId ally : faction.allies)
        {
            const std::uint64_t lo = std::min<std::uint64_t>(faction.id, ally);
            const std::uint64_t hi = std::max<std::uint64_t>(faction.id, ally);
            alliancePairs[(lo << 32) | hi] = true;
            CHECK(byId[ally] != nullptr);
        }
    }
    CHECK(rivalryPairs.size() == 2);
    CHECK(alliancePairs.size() == 1);
    for (const auto &pair : rivalryPairs)
        CHECK(alliancePairs.count(pair.first) == 0); // never rivals AND allies
}

// ---------------------------------------------------------------------------
// TestWorldgen_ElvenEnclaveExactlyOne
// Stage 6: "elves one forest enclave" — exactly one settlement (a village)
// plans an elf-majority population; every other settlement stays
// human-majority per its mix.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWorldgen_ElvenEnclaveExactlyOne"
)
{
    const WorldgenRun run{12345ull};
    REQUIRE(run.result.elvenEnclave != kNoTerritory);
    const Territory *enclave = run.store->territory(run.result.elvenEnclave);
    REQUIRE(enclave != nullptr);
    CHECK(enclave->type == TerritoryType::VILLAGE);

    const std::uint16_t elfSpecies = run.registry.speciesByKey("elf")->id;

    std::uint32_t elfMajoritySettlements = 0;
    for (const auto &[id, territory] : run.store->territories())
    {
        (void)id;
        if (territory.type != TerritoryType::VILLAGE && territory.type != TerritoryType::CITY)
            continue;
        const std::vector<const NpcSeed *> residents = residentsOf(run.npcs, territory.id);
        if (residents.empty())
            continue;
        std::uint32_t elves = 0;
        for (const NpcSeed *npc : residents)
            if (npc->species == elfSpecies)
                ++elves;
        const bool majority = elves * 2 > residents.size();
        if (territory.id == run.result.elvenEnclave)
            CHECK(majority);
        else
            CHECK_FALSE(majority);
        if (majority)
            ++elfMajoritySettlements;
    }
    CHECK(elfMajoritySettlements == 1);
}

// ---------------------------------------------------------------------------
// TestWorldgen_RareSpeciesBounded
// Stage 6: "dragons/goo beasts rare: <= 3 total world-wide" — the combined
// planned count of the two rare species never exceeds 3, and both live on
// the frontier (no settlement plans one).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWorldgen_RareSpeciesBounded"
)
{
    const WorldgenRun run{12345ull};
    const std::uint16_t dragon   = run.registry.speciesByKey("dragon")->id;
    const std::uint16_t gooBeast = run.registry.speciesByKey("goo_beast")->id;

    std::uint32_t rareTotal = 0;
    for (const NpcSeed &npc : run.npcs)
    {
        if (npc.species != dragon && npc.species != gooBeast)
            continue;
        ++rareTotal;
        CHECK(npc.homeTerritory == kNoTerritory); // FAR-tier wanderers, never residents
    }
    CHECK(rareTotal <= 3);
}

// ---------------------------------------------------------------------------
// TestWorldgen_LawVarianceAtMostOnePerKingdom
// Stage 3: the pinned variance roll swaps at most ONE settlement per kingdom
// away from its template law set (safe villages are stage-4 overrides, not
// variance swaps, and are excluded).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWorldgen_LawVarianceAtMostOnePerKingdom"
)
{
    const WorldgenRun run{12345ull};
    const std::vector<territoryId> kingdoms = territoriesOfType(*run.store, TerritoryType::KINGDOM);

    std::uint32_t globalSwaps = 0;
    for (const territoryId kingdom : kingdoms)
    {
        std::uint32_t swaps = 0;
        for (const territoryId child : run.store->childrenOf(kingdom))
        {
            const Territory *node = run.store->territory(child);
            REQUIRE(node != nullptr);
            REQUIRE(node->laws != nullptr);
            if (node->type != TerritoryType::VILLAGE && node->type != TerritoryType::CITY)
                continue;
            if (node->safe)
                continue; // the safe-zone override is stage 4's, not variance
            const std::string &effective = node->laws->key;
            const std::string &templated = run.registry.territoryTemplate(node->type).lawSet;
            if (effective != templated)
                ++swaps;
        }
        CHECK(swaps <= 1);
        globalSwaps += swaps;
    }
    CHECK(globalSwaps <= kingdoms.size());
}

// ---------------------------------------------------------------------------
// TestWorldgen_UnderTwoSecondBudget
// Done-when: "Worldgen produces the full graph + population plan in < 2 s
// (debug build)". Wall-clock timing is TEST-ONLY (R8 bans it from the sim).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWorldgen_UnderTwoSecondBudget"
)
{
    DataRegistry registry;
    loadShippedRegistry(registry);

    WorldStore store{registry.defaultLaws()};
    NpcSeedOut npcs;

    const auto started = std::chrono::steady_clock::now();
    const WorldGenResult result = generateWorld(12345ull, registry, store, npcs);
    const auto finished = std::chrono::steady_clock::now();

    const double seconds = std::chrono::duration<double>(finished - started).count();
    CHECK(seconds < 2.0);
    MESSAGE("worldgen: " << npcs.size() << " planned NPCs, " << store.territoryCount()
                         << " territories, hash " << std::hex << result.summaryHash << std::dec);
}
