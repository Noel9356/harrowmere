// worldgen.cpp — task 1.5: world generation from seed (see worldgen.hpp for
// the contract). The file is organized strictly by the card's pinned stage
// order; every stage draws only from SeedRng's "worldgen" stream (R6), and
// every container the output can observe is ordered (R7).
//
// Geometry: all region/position math goes through world/region.hpp's
// D-WORLD-1 helpers (regionIdOf / regionIndicesInGrid / regionCenter) —
// never raw coordinate math (card 1.5 pinned decision D-WORLD-1).

#include "world/worldgen.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/constants.hpp" // SCALE (A-1), REGION_* (A-40), ADULT_YEARS (A-44)
#include "core/log.hpp"
#include "core/rng.hpp"  // SeedRng — the pinned named streams (R6)
#include "data/json.hpp" // the sanctioned JSON door for game code (D-DATA-1)
#include "data/registry.hpp"
#include "world/region.hpp"

namespace harrowmere::world
{

namespace
{

// The one RNG stream of world generation (card stage 1 pins it; the other
// pinned streams belong to their owning systems, task 0.3).
constexpr const char *kStream = "worldgen";

// ==== stage 1: climate weights (SCALE grid; DEVIATIONS.md row: the card
// points at territory_templates.json, whose shipped file carries no climate
// table — the weights are worldgen-pinned until the templates loader grows
// the field; order = Climate enum order) ====
constexpr std::array<std::uint32_t, static_cast<std::size_t>(Climate::COUNT)> kClimateWeights{
    3000, // TEMPERATE
    2500, // FOREST
    2000, // MOUNTAIN
    1000, // SWAMP
    1500, // COASTAL
};
static_assert(
    [] {
        std::uint32_t sum = 0;
        for (const std::uint32_t weight : kClimateWeights)
            sum += weight;
        return sum == kScale;
    }(),
    "climate weights must sum to SCALE"
);

// ==== stage 6: species mixes (SCALE grid; card stage 6 pins the SHAPE —
// "humans dominate kingdoms; beastkins/taurs in frontier & tolerant
// settlements; elves one forest enclave" — the exact weights are worldgen
// pins, DEVIATIONS.md row). Order is pinned: human, beastkin, taur, elf,
// vampire. Dragons and goo beasts never settle (frontier wanderers only). ====
struct SpeciesWeight
{
    const char *key;
    std::uint32_t weight;
};

constexpr std::array<SpeciesWeight, 5> kDefaultMix{
    SpeciesWeight{"human", 9000}, SpeciesWeight{"beastkin", 200}, SpeciesWeight{"taur", 100},
    SpeciesWeight{"elf", 400},    SpeciesWeight{"vampire", 300},
};
constexpr std::array<SpeciesWeight, 5> kXenophobicMix{
    SpeciesWeight{"human", 9400}, SpeciesWeight{"beastkin", 0}, SpeciesWeight{"taur", 0},
    SpeciesWeight{"elf", 300},    SpeciesWeight{"vampire", 300},
};
constexpr std::array<SpeciesWeight, 5> kTolerantMix{
    SpeciesWeight{"human", 6000}, SpeciesWeight{"beastkin", 2000}, SpeciesWeight{"taur", 1200},
    SpeciesWeight{"elf", 500},    SpeciesWeight{"vampire", 300},
};
constexpr std::array<SpeciesWeight, 5> kEnclaveMix{
    SpeciesWeight{"human", 1000}, SpeciesWeight{"beastkin", 200}, SpeciesWeight{"taur", 0},
    SpeciesWeight{"elf", 8800},   SpeciesWeight{"vampire", 0},
};
constexpr std::array<SpeciesWeight, 5> kFrontierMix{
    SpeciesWeight{"human", 1500}, SpeciesWeight{"beastkin", 4000}, SpeciesWeight{"taur", 3500},
    SpeciesWeight{"elf", 1000},   SpeciesWeight{"vampire", 0},
};
static_assert(
    [] {
        std::uint32_t sum = 0;
        for (const auto &row : kDefaultMix)
            sum += row.weight;
        return sum == kScale;
    }() &&
    [] {
        std::uint32_t sum = 0;
        for (const auto &row : kXenophobicMix)
            sum += row.weight;
        return sum == kScale;
    }() &&
    [] {
        std::uint32_t sum = 0;
        for (const auto &row : kTolerantMix)
            sum += row.weight;
        return sum == kScale;
    }() &&
    [] {
        std::uint32_t sum = 0;
        for (const auto &row : kEnclaveMix)
            sum += row.weight;
        return sum == kScale;
    }() &&
    [] {
        std::uint32_t sum = 0;
        for (const auto &row : kFrontierMix)
            sum += row.weight;
        return sum == kScale;
    }(),
    "every species mix must sum to SCALE"
);

// Stage 6 residual role fill (SCALE grid). The card pins "farmer/laborer 50%,
// vendor 15%, child 10%" and leaves 25% unstated — the residual goes to
// militia_veteran, the one pinned v1 role the quotas do not place
// (DEVIATIONS.md row). Captains are deliberately not seeded: the card's
// weight list does not name them (3.6's replacement path can promote).
struct RoleWeight
{
    const char *key;
    std::uint32_t weight;
};
constexpr std::array<RoleWeight, 5> kResidualRoles{
    RoleWeight{"farmer", 2500}, RoleWeight{"laborer", 2500}, RoleWeight{"vendor", 1500},
    RoleWeight{"child", 1000},  RoleWeight{"militia_veteran", 2500},
};
static_assert(
    [] {
        std::uint32_t sum = 0;
        for (const auto &row : kResidualRoles)
            sum += row.weight;
        return sum == kScale;
    }(),
    "residual role weights must sum to SCALE"
);

// Stage 6 wanderer roles (frontier pool + rare wanderers): the free company
// shape — no settlement jobs on the frontier.
constexpr std::array<RoleWeight, 2> kWandererRoles{
    RoleWeight{"militia_veteran", 5000},
    RoleWeight{"laborer", 5000},
};

// Stage 3 variance variants (the swap targets; 50/50 draw picks one).
constexpr const char *kVariantLawSets[2] = {"xenophobic_village", "tolerant_city"};

// ==== stage 7: FNV-1a 64 (standard algorithm constants — hashing plumbing,
// not a game rule; no Appendix A row) ====
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime       = 1099511628211ull;

void hashByte(
    std::uint64_t &hash, std::uint8_t byte
)
{
    hash ^= byte;
    hash *= kFnvPrime;
}

void hashU32(
    std::uint64_t &hash, std::uint32_t value
)
{
    for (int i = 0; i < 4; ++i)
        hashByte(hash, static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
}

void hashU64(
    std::uint64_t &hash, std::uint64_t value
)
{
    for (int i = 0; i < 8; ++i)
        hashByte(hash, static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
}

void hashString(
    std::uint64_t &hash, const std::string &text
)
{
    hashU64(hash, static_cast<std::uint64_t>(text.size()));
    for (const char c : text)
        hashByte(hash, static_cast<std::uint8_t>(c));
}

// ==== species keys -> ids (resolved once; referenced by KEY per the 1.1
// pitfall — never literal ids) ====
struct SpeciesIds
{
    speciesId human = 0;
    speciesId beastkin    = 0;
    speciesId taur        = 0;
    speciesId elf         = 0;
    speciesId vampire     = 0;
    speciesId dragon      = 0;
    speciesId gooBeast    = 0;
    bool complete         = true; // every key resolved
};

SpeciesIds resolveSpecies(
    const data::DataRegistry &registry
)
{
    SpeciesIds ids;
    const auto resolve = [&registry, &ids](speciesId &slot, const char *key) {
        const data::SpeciesData *row = registry.speciesByKey(key);
        if (row == nullptr)
        {
            HARROWMERE_LOG_ERROR(sim, "worldgen: species key \"%s\" missing from species.json", key);
            ids.complete = false;
            return;
        }
        slot = row->id;
    };
    resolve(ids.human, "human");
    resolve(ids.beastkin, "beastkin");
    resolve(ids.taur, "taur");
    resolve(ids.elf, "elf");
    resolve(ids.vampire, "vampire");
    resolve(ids.dragon, "dragon");
    resolve(ids.gooBeast, "goo_beast");
    return ids;
}

// Map a mix row's key to its resolved id (the mix tables have distinct
// first letters by construction — a direct, allocation-free mapping).
speciesId speciesFor(
    const SpeciesIds &ids, const char *key
)
{
    switch (key[0])
    {
    case 'h':
        return ids.human;
    case 'b':
        return ids.beastkin;
    case 't':
        return ids.taur;
    case 'e':
        return ids.elf;
    default:
        return ids.vampire;
    }
}

// One mix row resolved to a species id (zero-weight rows keep their slot so
// the draw loop stays table-driven and the stream order is data-stable).
struct MixEntry
{
    speciesId species;
    std::uint32_t weight;
};

template <std::size_t N>
void resolveMix(
    const SpeciesIds &ids, const std::array<SpeciesWeight, N> &mix, std::vector<MixEntry> &out
)
{
    out.clear();
    for (const auto &row : mix)
        out.push_back(MixEntry{speciesFor(ids, row.key), row.weight});
}

// Draw a species id from a resolved mix (cumulative weights on the SCALE
// grid; pct() covers [0, SCALE) and every mix sums to SCALE).
speciesId drawSpecies(
    core::SeedRng &rng, const std::vector<MixEntry> &mix
)
{
    const std::uint32_t roll = rng.pct(kStream);
    std::uint32_t accumulated = 0;
    for (const MixEntry &entry : mix)
    {
        accumulated += entry.weight;
        if (roll < accumulated)
            return entry.species;
    }
    return mix.back().species; // unreachable while a mix sums to SCALE (R2 guard)
}

// Draw a role key from a weight table.
const char *drawRole(
    core::SeedRng &rng, const RoleWeight *roles, std::size_t count
)
{
    const std::uint32_t roll = rng.pct(kStream);
    std::uint32_t accumulated = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        accumulated += roles[i].weight;
        if (roll < accumulated)
            return roles[i].key;
    }
    return roles[count - 1].key; // unreachable while the table sums to SCALE (R2 guard)
}

// ==== name tables (stage 3: the title chain is seeded "using classes.json +
// name tables"). The registry has no names stage yet (its pinned load order
// ends at suspicion until 5.1/6.4 land), so worldgen reads the package's
// data/names/<species>_{given,family}.json directly through the game's JSON
// door; a missing or malformed table degrades to empty names with a WARN —
// DEVIATIONS.md carries the row. ====
struct NameTables
{
    std::map<std::string, std::vector<std::string>> givenNames;
    std::map<std::string, std::vector<std::string>> familyNames;
};

std::vector<std::string> loadNameList(
    const std::string &path
)
{
    const data::Result<data::Json, data::DataError> loaded = data::loadJsonFile(path);
    if (!loaded)
    {
        HARROWMERE_LOG_WARN(sim, "worldgen: name table unavailable: %s", loaded.error().toString().c_str());
        return {};
    }
    const data::Json &root = loaded.value();
    if (!root.is_array())
    {
        HARROWMERE_LOG_WARN(sim, "worldgen: %s: expected a root array of name strings", path.c_str());
        return {};
    }
    std::vector<std::string> names;
    for (std::size_t i = 0; i < root.size(); ++i)
    {
        const data::Json &entry = root[i];
        if (!entry.is_string() || entry.get<std::string>().empty())
        {
            HARROWMERE_LOG_WARN(sim, "worldgen: %s: /%zu: expected a non-empty name string", path.c_str(), i);
            continue;
        }
        names.push_back(entry.get<std::string>());
    }
    return names;
}

// Loads (and caches) one species' given/family tables. An empty table caches
// too — one WARN per missing table, ever, and the draw order stays a
// function of the data shape.
void ensureNameTables(
    NameTables &tables, const std::string &packageDir, const std::string &speciesKey
)
{
    if (tables.givenNames.count(speciesKey) != 0)
        return;
    tables.givenNames[speciesKey]  = loadNameList(packageDir + "/names/" + speciesKey + "_given.json");
    tables.familyNames[speciesKey] = loadNameList(packageDir + "/names/" + speciesKey + "_family.json");
}

// Draws one name from a cached table (no draw consumed when the table is
// empty — the stream order is a function of the data shape).
std::string drawName(
    core::SeedRng &rng, const std::vector<std::string> &table
)
{
    if (table.empty())
        return {};
    return table[rng.range(kStream, 0, static_cast<std::uint32_t>(table.size()))];
}

// ==== age & level (stage 6 pinned derivations) ====

// Adult age from the species lifespan band: [ADULT_YEARS, lifespan] (A-44
// floors adulthood; the docs' lifespan band is the only age source). The
// `child` role re-folds the ALREADY-DRAWN age under ADULT_YEARS afterwards —
// one age draw per NPC, in the pinned order (species -> age -> role ->
// integrity); the fold adds no draw.
std::uint32_t drawAgeYears(
    core::SeedRng &rng, const data::SpeciesData &species
)
{
    if (species.lifespan_years <= ADULT_YEARS)
        return ADULT_YEARS; // R2 fallback: no shipped species is this short-lived
    return rng.range(kStream, ADULT_YEARS, species.lifespan_years + 1u);
}

// The child clamp: [1, ADULT_YEARS) — children never testify and never hold
// non-child roles (A-44); a pure re-fold of the drawn age, no draw.
std::uint32_t foldChildAge(
    std::uint32_t drawnAge
)
{
    return 1u + (drawnAge % (ADULT_YEARS - 1u));
}

// Starting level band by age: 1 at adulthood's floor to STARTING_LEVEL_MAX at
// the species' full lifespan (worldgen pin, DEVIATIONS.md row).
std::uint32_t levelForAge(
    std::uint32_t ageYears, std::uint32_t lifespanYears
)
{
    if (lifespanYears == 0)
        return 1; // R2 guard: no shipped species has a zero lifespan
    const std::uint32_t level = 1u + (ageYears * (STARTING_LEVEL_MAX - 1u)) / lifespanYears;
    return level > STARTING_LEVEL_MAX ? STARTING_LEVEL_MAX : level;
}

// Draws an NPC's integrity from its role's range (roles.json integrityRange,
// both bounds inclusive).
std::uint32_t drawIntegrity(
    core::SeedRng &rng, const data::RoleData &role
)
{
    if (role.integrityMax <= role.integrityMin)
        return role.integrityMin; // R2 guard: single-point range
    return rng.range(kStream, role.integrityMin, role.integrityMax + 1u);
}

// ==== stage 2 layout types ====

struct SettlementLayout
{
    territoryId id = 0;
    TerritoryType type = TerritoryType::VILLAGE;
    territoryId parent = 0;
    regionId anchor = kNoRegion;
    std::vector<regionId> blockRegions; // villages: exactly 1; cities: 3-5 contiguous
};

struct KingdomLayout
{
    territoryId id = 0;
    std::uint32_t qx = 0; // quadrant origin (interior cells)
    std::uint32_t qz = 0;
    std::uint32_t side = 0;
    std::vector<territoryId> cities;   // id order
    std::vector<territoryId> villages; // id order
};

// The boustrophedon (snake) linearization of a quadrant: consecutive indices
// are always 4-adjacent, so consecutive city-block cells are contiguous.
regionId snakeCell(
    const KingdomLayout &kingdom, std::uint32_t index
)
{
    const std::uint32_t row = index / kingdom.side;
    const std::uint32_t col = index % kingdom.side;
    const std::uint32_t x = (row % 2u == 0u) ? kingdom.qx + col : kingdom.qx + (kingdom.side - 1u - col);
    const std::uint32_t z = kingdom.qz + row;
    return regionIdOf(x, z);
}

} // namespace

// =============================================================================
// generateWorld — the pinned stage order (card 1.5). DO NOT reorder stages or
// per-NPC draws: the stream consumption order IS the generation contract.
// =============================================================================
WorldGenResult generateWorld(
    std::uint64_t worldSeed, const data::DataRegistry &registry, WorldStore &world, NpcSeedOut &npcsOut
)
{
    assert(registry.ok() && "worldgen requires a committed DataRegistry snapshot");

    core::SeedRng rng;
    rng.seedAll(worldSeed);

    IdGenerator ids; // the character id space (types.hpp); 0 is reserved (R10)
    WorldGenResult result;
    npcsOut.clear();
    world.clear();

    const SpeciesIds speciesIds = resolveSpecies(registry);
    const data::TerritoryTemplateSet &templates = registry.territoryTemplates();

    const std::uint32_t kingdomsPerEmpire  = templates.kingdomsPerEmpire();
    const std::uint32_t citiesPerKingdom   = templates.citiesPerKingdom();
    const std::uint32_t villagesPerCity    = templates.villagesPerCity();
    const std::uint32_t villagesPerKingdom = citiesPerKingdom * villagesPerCity;

    // ---- stage 1: regions (stream "worldgen") --------------------------------
    // Per-region draw order is pinned: climate first, then mana richness.
    for (std::uint32_t id = 0; id < REGION_COUNT; ++id)
    {
        Region region;
        region.id = static_cast<regionId>(id);
        region.flags = 0; // consequence flags are event/war state (5.5, 6.2); none at genesis

        const std::uint32_t roll = rng.pct(kStream);
        std::uint32_t accumulated = 0;
        Climate climate = Climate::TEMPERATE;
        for (std::size_t c = 0; c < kClimateWeights.size(); ++c)
        {
            accumulated += kClimateWeights[c];
            if (roll < accumulated)
            {
                climate = static_cast<Climate>(c);
                break;
            }
        }
        region.climate = climate;
        region.manaRichness = rng.range(kStream, MANA_RICHNESS_MIN, MANA_RICHNESS_MAX + 1u);
        world.addRegion(region);
    }

    // ---- stage 2: the territory tree ------------------------------------------
    // Interior = the plane minus the frontier ring. Kingdoms lay out as a
    // square-ish grid of quadrants; each kingdom's quadrant is linearized in
    // snake order, the cities take consecutive (=> contiguous) blocks of 3-5
    // regions, the villages take evenly spread single cells, and every
    // remaining cell is kingdom (rural) land mapped to the kingdom.
    const std::uint32_t interiorSide = REGION_GRID - 2u * FRONTIER_RING_DEPTH;
    std::uint32_t quadrantRows = 1;
    while (quadrantRows * quadrantRows < kingdomsPerEmpire)
        ++quadrantRows;
    const std::uint32_t quadrantCols = (kingdomsPerEmpire + quadrantRows - 1u) / quadrantRows;
    const std::uint32_t quadrantSide =
        std::max<std::uint32_t>(1u, quadrantRows > 0u ? interiorSide / quadrantRows : 1u);

    std::vector<KingdomLayout> kingdoms(kingdomsPerEmpire);
    std::vector<SettlementLayout> settlements; // id order: all cities, then all villages
    settlements.reserve(kingdomsPerEmpire * (citiesPerKingdom + villagesPerKingdom));

    territoryId nextTerritoryId = IdGenerator::FIRST_ID;
    const territoryId empireId = nextTerritoryId++;

    for (std::uint32_t k = 0; k < kingdomsPerEmpire; ++k)
    {
        KingdomLayout &kingdom = kingdoms[k];
        kingdom.id = nextTerritoryId++;
        kingdom.qx = 1u + (k % quadrantCols) * quadrantSide;
        kingdom.qz = 1u + (k / quadrantCols) * quadrantSide;
        kingdom.side = quadrantSide;

        const std::uint32_t cellCount = kingdom.side * kingdom.side;
        const std::uint32_t villagesNeeded = villagesPerKingdom;

        // City blocks: consecutive snake cells (contiguous by construction).
        std::uint32_t cursor = 0;
        std::vector<bool> used(cellCount, false);
        for (std::uint32_t c = 0; c < citiesPerKingdom; ++c)
        {
            std::uint32_t size = rng.range(kStream, CITY_REGIONS_MIN, CITY_REGIONS_MAX + 1u);
            if (cursor + size + villagesNeeded > cellCount)
            {
                // R2 guard for degenerate template counts: keep every village
                // placeable; the shipped templates never enter this branch.
                const std::uint32_t affordable =
                    cellCount > cursor + villagesNeeded ? cellCount - cursor - villagesNeeded : 0u;
                size = std::min(size, affordable);
            }

            SettlementLayout city;
            city.type = TerritoryType::CITY;
            city.parent = kingdom.id;
            city.id = nextTerritoryId++;
            for (std::uint32_t cell = 0; cell < size; ++cell)
            {
                const regionId cellId = snakeCell(kingdom, cursor + cell);
                city.blockRegions.push_back(cellId);
                used[cursor + cell] = true;
            }
            cursor += size;
            city.anchor = !city.blockRegions.empty() ? city.blockRegions.front() : snakeCell(kingdom, cursor);
            kingdom.cities.push_back(city.id);
            settlements.push_back(city);
        }

        // Villages: evenly spread single cells among the remaining cells
        // ((2v+1)/2V fractions — deterministic, no draw). With the pinned
        // templates the remaining pool is large enough that every village
        // lands on a distinct cell.
        std::vector<regionId> remaining;
        for (std::uint32_t cell = cursor; cell < cellCount; ++cell)
            if (!used[cell])
                remaining.push_back(snakeCell(kingdom, cell));
        const std::uint32_t remainingCount = static_cast<std::uint32_t>(remaining.size());
        for (std::uint32_t v = 0; v < villagesPerKingdom; ++v)
        {
            SettlementLayout village;
            village.type = TerritoryType::VILLAGE;
            // villages_per_city: villages parent to the kingdom's cities in order.
            village.parent = kingdom.cities[v / (villagesPerCity > 0u ? villagesPerCity : 1u)];
            village.id = nextTerritoryId++;
            const std::uint32_t pick =
                remainingCount > 0u
                    ? std::min((2u * v + 1u) * remainingCount / (2u * villagesPerKingdom), remainingCount - 1u)
                    : 0u;
            village.anchor = remainingCount > 0u ? remaining[pick] : snakeCell(kingdom, 0u);
            village.blockRegions.push_back(village.anchor);
            kingdom.villages.push_back(village.id);
            settlements.push_back(village);
        }
    }

    // The explicit generation mapping (card 1.4): every claimed region maps
    // to the FINEST territory covering it — villages and city blocks map to
    // themselves; every other interior cell maps to its kingdom; the
    // frontier ring (and any degenerate-count leftover) stays unmapped.
    std::map<regionId, territoryId> regionOwner;
    for (const SettlementLayout &settlement : settlements)
        for (const regionId cell : settlement.blockRegions)
            regionOwner[cell] = settlement.id;
    for (const KingdomLayout &kingdom : kingdoms)
    {
        for (std::uint32_t cell = 0; cell < kingdom.side * kingdom.side; ++cell)
        {
            const regionId cellId = snakeCell(kingdom, cell);
            if (regionOwner.count(cellId) == 0)
                regionOwner[cellId] = kingdom.id;
        }
    }
    for (const auto &[cellId, owner] : regionOwner)
        world.setRegionTerritory(cellId, owner);

    // ---- stage 3: laws & titles ------------------------------------------------
    // 3a: every territory's law set starts at its template's `lawSet`.
    std::map<territoryId, std::string> lawKeys;
    lawKeys[empireId] = registry.territoryTemplate(TerritoryType::EMPIRE).lawSet;
    for (const KingdomLayout &kingdom : kingdoms)
    {
        lawKeys[kingdom.id] = registry.territoryTemplate(TerritoryType::KINGDOM).lawSet;
        for (const SettlementLayout &settlement : settlements)
            if (settlement.parent == kingdom.id)
                lawKeys[settlement.id] = registry.territoryTemplate(settlement.type).lawSet;
    }

    // 3b: the pinned per-kingdom variance roll — 10% to swap ONE city/village
    // of that kingdom to xenophobic_village or tolerant_city. (Stage 4 may
    // still override the safe village to safe_zone_laws: safe=true wins.)
    for (const KingdomLayout &kingdom : kingdoms)
    {
        if (rng.pct(kStream) >= LAW_VARIANCE_CHANCE)
            continue;
        std::vector<territoryId> kingdomSettlements; // cities then villages, id order
        for (const SettlementLayout &settlement : settlements)
            if (settlement.parent == kingdom.id)
                kingdomSettlements.push_back(settlement.id);
        if (kingdomSettlements.empty())
            continue;
        const std::uint32_t pick = rng.range(kStream, 0, static_cast<std::uint32_t>(kingdomSettlements.size()));
        const std::uint32_t variant = rng.range(kStream, 0, 2u);
        lawKeys[kingdomSettlements[pick]] = kVariantLawSets[variant];
    }

    // 3c: the NOBLE/ROYAL title chain — a royal per kingdom, a noble per
    // city, seeded with classes.json + the species name tables. These are
    // planned NPCs like any other; a titled NPC's draw order is pinned as
    // class -> given name -> family name -> age -> integrity (the role is
    // fixed "noble"). Created BEFORE stage 6 so the title maps can reference
    // their ids; the per-city noble IS stage 6's "noble 1/city" quota.
    // Titled NPCs are human (the doc's human-kingdom courts).
    NameTables nameTables;
    std::map<territoryId, std::map<characterId, SocialClassId>> titles; // stage-3 titles
    std::map<territoryId, std::string> royalFamilyNames;                // stage 5 court flavor
    const std::string packageDir = registry.packageDir();
    const data::RoleData *nobleRole = registry.role("noble");

    const auto seedTitledNpc = [&](territoryId homeTerritory, regionId homeRegion, SocialClassId cls) {
        NpcSeed npc;
        npc.id = ids.nextCharacter();
        npc.species = speciesIds.human;
        npc.roleKey = "noble";
        npc.homeTerritory = homeTerritory;
        npc.homeRegion = homeRegion;
        npc.socialClass = cls;

        if (!registry.classes().empty())
            npc.classKey =
                registry.classes()[rng.range(kStream, 0, static_cast<std::uint32_t>(registry.classes().size()))].key;
        ensureNameTables(nameTables, packageDir, "human");
        npc.givenName  = drawName(rng, nameTables.givenNames["human"]);
        npc.familyName = drawName(rng, nameTables.familyNames["human"]);
        const data::SpeciesData &humanSpecies = registry.species(speciesIds.human);
        npc.ageYears = drawAgeYears(rng, humanSpecies);
        npc.level    = levelForAge(npc.ageYears, humanSpecies.lifespan_years);
        npc.integrity = nobleRole != nullptr ? drawIntegrity(rng, *nobleRole) : 0u;

        npcsOut.push_back(npc);
        return npc;
    };

    for (const KingdomLayout &kingdom : kingdoms)
    {
        // The kingdom's royal (anchored at the kingdom's center cell).
        const regionId kingdomAnchor =
            regionIdOf(kingdom.qx + kingdom.side / 2u, kingdom.qz + kingdom.side / 2u);
        const NpcSeed royal = seedTitledNpc(kingdom.id, kingdomAnchor, SocialClassId::ROYAL);
        titles[kingdom.id][royal.id] = SocialClassId::ROYAL;
        royalFamilyNames[kingdom.id] = royal.familyName;

        // One noble per city (id order).
        for (const SettlementLayout &settlement : settlements)
        {
            if (settlement.parent != kingdom.id || settlement.type != TerritoryType::CITY)
                continue;
            const NpcSeed noble = seedTitledNpc(settlement.id, settlement.anchor, SocialClassId::NOBLE);
            titles[settlement.id][noble.id] = SocialClassId::NOBLE;
        }
    }

    // 3d: materialize the tree (ascending ids: parents exist before children,
    // so WorldStore::addTerritory wires every children vector on the way).
    {
        Territory empire;
        empire.type   = TerritoryType::EMPIRE;
        empire.id     = empireId;
        empire.parent = kNoTerritory; // root: sovereign (design rule)
        empire.laws   = registry.lawSet(lawKeys[empireId]);
        empire.region = regionIdOf(REGION_GRID / 2u, REGION_GRID / 2u);
        world.addTerritory(empire);

        for (const KingdomLayout &kingdom : kingdoms)
        {
            Territory node;
            node.type   = TerritoryType::KINGDOM;
            node.id     = kingdom.id;
            node.parent = empireId;
            node.laws   = registry.lawSet(lawKeys[kingdom.id]);
            node.titles = titles[kingdom.id];
            node.region = regionIdOf(kingdom.qx + kingdom.side / 2u, kingdom.qz + kingdom.side / 2u);
            world.addTerritory(node);
        }
        for (const SettlementLayout &settlement : settlements)
        {
            Territory node;
            node.type   = settlement.type;
            node.id     = settlement.id;
            node.parent = settlement.parent;
            node.laws   = registry.lawSet(lawKeys[settlement.id]);
            node.titles = titles[settlement.id];
            node.region = settlement.anchor;
            world.addTerritory(node);
        }
    }

    // ---- stage 4: safe zones -----------------------------------------------------
    // Exactly one VILLAGE per kingdom is safe and holds the kingdom's spawn
    // point (the village's region center). The kingdom covering the claimed
    // region nearest the world origin — the interior origin cell (1,1), just
    // inside the frontier ring — is the beginner territory (the card's
    // "kingdom containing region (0,0)": the outermost ring itself stays
    // unclaimed per stage 2, so the beginner kingdom is the one covering the
    // first claimed cell at the origin corner — DEVIATIONS.md row).
    for (const KingdomLayout &kingdom : kingdoms)
    {
        const std::uint32_t villageCount = static_cast<std::uint32_t>(kingdom.villages.size());
        if (villageCount == 0u)
            continue;
        const std::uint32_t pick = rng.range(kStream, 0, villageCount);
        const territoryId safeVillage = kingdom.villages[pick];

        Territory *village = world.territory(safeVillage);
        if (village == nullptr)
            continue; // unreachable: stage 3d added every settlement
        village->safe = true;
        lawKeys[safeVillage] = "safe_zone_laws"; // 1.2: safe_zone_laws is used where safe=true
        village->laws        = registry.lawSet(lawKeys[safeVillage]);

        // The beginner kingdom contains the interior origin cell (1,1).
        if (kingdom.qx <= 1u && 1u < kingdom.qx + kingdom.side && kingdom.qz <= 1u && 1u < kingdom.qz + kingdom.side)
        {
            result.beginnerTerritory  = kingdom.id;
            result.playerSpawnVillage = safeVillage;
            result.playerSpawn        = regionCenter(village->region);
        }
    }

    // ---- stage 5: factions ---------------------------------------------------------
    // FACTION_COUNT factions: one court per kingdom + the frontier free
    // company. Rivalries: one drawn perfect matching over the four courts
    // (2 pairs); the alliance pairs the first rivals of each rivalry pair.
    if (kingdoms.size() == 4u && FACTION_COUNT == 5u)
    {
        constexpr std::uint32_t kMatchings[3][2][2]{
            {{0, 1}, {2, 3}},
            {{0, 2}, {1, 3}},
            {{0, 3}, {1, 2}}
        };
        const std::uint32_t matching = rng.range(kStream, 0, 3u);

        for (std::size_t k = 0; k < kingdoms.size(); ++k)
        {
            FactionSeed court;
            court.id = static_cast<factionId>(k + 1u);
            court.key = "court_kingdom_" + std::to_string(kingdoms[k].id);
            const std::string &family = royalFamilyNames[kingdoms[k].id];
            court.name = "Court of " + (family.empty() ? std::string("the Kingdom") : family);
            court.courtTerritory = kingdoms[k].id;
            result.factions.push_back(std::move(court));
        }
        for (std::uint32_t pair = 0; pair < 2u; ++pair)
        {
            const factionId a = static_cast<factionId>(kMatchings[matching][pair][0] + 1u);
            const factionId b = static_cast<factionId>(kMatchings[matching][pair][1] + 1u);
            result.factions[a - 1u].rivals = {b};
            result.factions[b - 1u].rivals = {a};
        }
        const factionId a = static_cast<factionId>(kMatchings[matching][0][0] + 1u);
        const factionId b = static_cast<factionId>(kMatchings[matching][1][0] + 1u);
        result.factions[a - 1u].allies = {b};
        result.factions[b - 1u].allies = {a};

        FactionSeed freeCompany;
        freeCompany.id             = static_cast<factionId>(FACTION_COUNT);
        freeCompany.key            = "frontier_free_company";
        freeCompany.name           = "Free Company of the Frontier";
        freeCompany.courtTerritory = kNoTerritory;
        result.factions.push_back(std::move(freeCompany));
    }
    else
    {
        // Degenerate template counts: seed the courts, skip the pinned
        // matching (it presumes exactly four courts) — deterministic and
        // logged; the shipped templates always take the branch above.
        HARROWMERE_LOG_WARN(
            sim,
            "worldgen: kingdom count %u does not support the pinned rivalry matching; seeding courts only",
            static_cast<unsigned>(kingdoms.size())
        );
        for (std::size_t k = 0; k < kingdoms.size() && k + 1u < FACTION_COUNT; ++k)
        {
            FactionSeed court;
            court.id             = static_cast<factionId>(k + 1u);
            court.key            = "court_kingdom_" + std::to_string(kingdoms[k].id);
            court.name           = "Court of the Kingdom";
            court.courtTerritory = kingdoms[k].id;
            result.factions.push_back(std::move(court));
        }
    }

    // ---- stage 6: the population plan ------------------------------------------------
    // Draw order per planned NPC is PINNED: species -> age -> role (only when
    // drawn; quota roles are fixed) -> integrity. Level derives from age.
    std::vector<MixEntry> mix;
    const auto fillMix = [&speciesIds, &mix](const std::string &lawKey, territoryId settlementId, territoryId enclave) {
        if (settlementId == enclave)
            resolveMix(speciesIds, kEnclaveMix, mix);
        else if (lawKey == "tolerant_city")
            resolveMix(speciesIds, kTolerantMix, mix);
        else if (lawKey == "xenophobic_village")
            resolveMix(speciesIds, kXenophobicMix, mix);
        else
            resolveMix(speciesIds, kDefaultMix, mix);
    };

    // The elven enclave: the FIRST village (id order) whose region is forest;
    // fallback: the first village ("elves one forest enclave" — worldgen pin).
    {
        territoryId firstVillage = kNoTerritory;
        for (const SettlementLayout &settlement : settlements)
        {
            if (settlement.type != TerritoryType::VILLAGE)
                continue;
            if (firstVillage == kNoTerritory)
                firstVillage = settlement.id;
            const Region *region = world.region(settlement.anchor);
            if (region != nullptr && region->climate == Climate::FOREST)
            {
                result.elvenEnclave = settlement.id;
                break;
            }
        }
        if (result.elvenEnclave == kNoTerritory)
            result.elvenEnclave = firstVillage;
    }

    // Settlements in id order (cities first, then villages — id assignment).
    for (const SettlementLayout &settlement : settlements)
    {
        const data::TerritoryTypeTemplate &tmpl = registry.territoryTemplate(settlement.type);
        if (tmpl.populationMax == 0u)
            continue; // no residents templated (kingdoms/empires carry only their court)
        const std::uint32_t population = rng.range(kStream, tmpl.populationMin, tmpl.populationMax + 1u);

        // Stage-3 titled residents count toward the population (the city noble).
        std::uint32_t prePlanned = 0;
        for (const NpcSeed &npc : npcsOut)
            if (npc.homeTerritory == settlement.id)
                ++prePlanned;
        const std::uint32_t fill = population > prePlanned ? population - prePlanned : 0u;

        fillMix(lawKeys[settlement.id], settlement.id, result.elvenEnclave);

        // Quotas first: guards (8/city, 2/village), then the city healer.
        std::uint32_t assigned = 0;
        const std::uint32_t guardQuota =
            settlement.type == TerritoryType::CITY ? GUARDS_PER_CITY : GUARDS_PER_VILLAGE;
        const std::uint32_t healerQuota = settlement.type == TerritoryType::CITY ? HEALERS_PER_CITY : 0u;
        for (std::uint32_t quota = 0; quota < guardQuota + healerQuota && assigned < fill; ++quota)
        {
            const bool guardSlot = quota < guardQuota;
            NpcSeed npc;
            npc.id      = ids.nextCharacter();
            npc.species = drawSpecies(rng, mix); // pinned draw 1: species
            const data::SpeciesData &speciesRow = registry.species(npc.species);
            npc.ageYears = drawAgeYears(rng, speciesRow); // pinned draw 2: age
            npc.roleKey  = guardSlot ? "guard" : "healer"; // quota: no role draw
            npc.level    = levelForAge(npc.ageYears, speciesRow.lifespan_years);
            npc.homeTerritory = settlement.id;
            npc.homeRegion    = settlement.anchor;
            const data::RoleData *role = registry.role(npc.roleKey);
            npc.integrity = role != nullptr ? drawIntegrity(rng, *role) : 0u; // pinned draw 3
            npcsOut.push_back(npc);
            ++assigned;
        }

        // The residual fill: the pinned weight table (farmer/laborer 50%,
        // vendor 15%, child 10%, militia_veteran 25% residual).
        while (assigned < fill)
        {
            NpcSeed npc;
            npc.id      = ids.nextCharacter();
            npc.species = drawSpecies(rng, mix);              // pinned draw 1: species
            const data::SpeciesData &speciesRow = registry.species(npc.species);
            npc.ageYears = drawAgeYears(rng, speciesRow);     // pinned draw 2: age
            npc.roleKey  = drawRole(rng, kResidualRoles.data(), kResidualRoles.size()); // draw 3: role
            if (npc.roleKey == "child")
                npc.ageYears = foldChildAge(npc.ageYears);    // re-fold, NO new draw
            npc.level         = levelForAge(npc.ageYears, speciesRow.lifespan_years);
            npc.homeTerritory = settlement.id;
            npc.homeRegion    = settlement.anchor;
            const data::RoleData *role = registry.role(npc.roleKey);
            npc.integrity = role != nullptr ? drawIntegrity(rng, *role) : 0u; // draw 4
            npcsOut.push_back(npc);
            ++assigned;
        }
    }

    // The frontier wanderer pool (the free company's founding membership by
    // convention) + the rare FAR-tier wanderers (dragons / goo beasts,
    // <= RARE_WANDERER_MAX world-wide, card rule).
    std::vector<regionId> frontierRegions;
    for (std::uint32_t id = 0; id < REGION_COUNT; ++id)
    {
        std::uint32_t x = 0, z = 0;
        regionIndicesOf(static_cast<regionId>(id), x, z);
        if (x < FRONTIER_RING_DEPTH || z < FRONTIER_RING_DEPTH || x >= REGION_GRID - FRONTIER_RING_DEPTH
            || z >= REGION_GRID - FRONTIER_RING_DEPTH)
            frontierRegions.push_back(static_cast<regionId>(id));
    }

    std::vector<MixEntry> frontierMix;
    resolveMix(speciesIds, kFrontierMix, frontierMix);

    const auto planWanderer = [&](speciesId rare, regionId homeRegion) {
        NpcSeed npc;
        npc.id      = ids.nextCharacter();
        npc.species = rare;
        const data::SpeciesData &speciesRow = registry.species(npc.species);
        npc.ageYears = drawAgeYears(rng, speciesRow); // pinned draw 2 (species drawn by caller)
        npc.roleKey  = drawRole(rng, kWandererRoles.data(), kWandererRoles.size());
        npc.level    = levelForAge(npc.ageYears, speciesRow.lifespan_years);
        npc.homeTerritory = kNoTerritory;
        npc.homeRegion    = homeRegion;
        const data::RoleData *role = registry.role(npc.roleKey);
        npc.integrity = role != nullptr ? drawIntegrity(rng, *role) : 0u;
        npcsOut.push_back(npc);
    };

    std::size_t wandererSlot = 0;
    for (std::uint32_t i = 0; i < FRONTIER_WANDERER_COUNT && !frontierRegions.empty(); ++i)
    {
        const regionId home = frontierRegions[wandererSlot % frontierRegions.size()];
        planWanderer(drawSpecies(rng, frontierMix), home); // pinned draw 1 inside the call
        ++wandererSlot;
    }
    {
        const std::uint32_t rareCount = rng.range(kStream, 0, RARE_WANDERER_MAX + 1u);
        for (std::uint32_t i = 0; i < rareCount && !frontierRegions.empty(); ++i)
        {
            const std::uint32_t roll = rng.pct(kStream);
            const speciesId rare =
                roll < static_cast<std::uint32_t>(kScale) / 2u ? speciesIds.dragon : speciesIds.gooBeast;
            const regionId home = frontierRegions[wandererSlot % frontierRegions.size()];
            planWanderer(rare, home);
            ++wandererSlot;
        }
    }

    result.nextCharacterId = ids.peek("character");

    // ---- stage 7: the determinism hash ------------------------------------------------
    // Canonical summary over region flags/climate/mana, the tree shape, and
    // the plan's counts (card: "region flags, tree shape, counts"). FNV-1a
    // over fixed-width little-endian fields; NO doubles and NO seed enter
    // the hash (same seed -> identical world -> identical hash; different
    // seeds differ through the content itself).
    std::uint64_t hash = kFnvOffsetBasis;
    hashU32(hash, REGION_COUNT);
    for (const auto &[id, region] : world.regions())
    {
        hashU32(hash, static_cast<std::uint32_t>(id));
        hashU32(hash, region.flags);
        hashU32(hash, region.manaRichness);
        hashU32(hash, static_cast<std::uint32_t>(region.climate));
    }

    hashU64(hash, world.territoryCount());
    for (const auto &[id, territory] : world.territories())
    {
        hashU64(hash, static_cast<std::uint64_t>(id));
        hashU32(hash, static_cast<std::uint32_t>(territory.type));
        hashU64(hash, territory.parent);
        hashU64(hash, static_cast<std::uint64_t>(territory.children.size()));
        for (const territoryId child : territory.children)
            hashU64(hash, child);
        hashU32(hash, territory.safe ? 1u : 0u);
        hashU32(hash, territory.lawless ? 1u : 0u);
        if (territory.laws != nullptr)
        {
            hashU32(hash, 1u);
            hashString(hash, territory.laws->key);
        }
        else
        {
            hashU32(hash, 0u);
        }
        hashU64(hash, static_cast<std::uint64_t>(territory.titles.size()));
        for (const auto &[holder, cls] : territory.titles)
        {
            hashU64(hash, holder);
            hashU32(hash, static_cast<std::uint32_t>(cls));
        }
    }

    // Plan counts per territory + the frontier pool + species/role histograms.
    std::map<territoryId, std::uint64_t> plannedByTerritory;
    std::map<std::uint32_t, std::uint64_t> plannedBySpecies;
    std::map<std::string, std::uint64_t> plannedByRole;
    std::uint64_t frontierPlanned = 0;
    for (const NpcSeed &npc : npcsOut)
    {
        if (npc.homeTerritory == kNoTerritory)
            ++frontierPlanned;
        else
            ++plannedByTerritory[npc.homeTerritory];
        ++plannedBySpecies[npc.species];
        ++plannedByRole[npc.roleKey];
    }
    hashU64(hash, static_cast<std::uint64_t>(npcsOut.size()));
    hashU64(hash, frontierPlanned);
    for (const auto &[owner, count] : plannedByTerritory)
    {
        hashU64(hash, owner);
        hashU64(hash, count);
    }
    for (const data::SpeciesData &row : registry.speciesTable())
        hashU64(hash, plannedBySpecies[row.id]);
    for (const auto &[roleKey, roleRow] : registry.roles())
    {
        (void)roleRow;
        hashU64(hash, plannedByRole[roleKey]);
    }
    hashU64(hash, static_cast<std::uint64_t>(result.factions.size()));
    for (const FactionSeed &faction : result.factions)
    {
        hashU32(hash, faction.id);
        hashString(hash, faction.key);
        hashU64(hash, faction.courtTerritory);
        hashU64(hash, static_cast<std::uint64_t>(faction.rivals.size()));
        for (const factionId rival : faction.rivals)
            hashU32(hash, rival);
        hashU64(hash, static_cast<std::uint64_t>(faction.allies.size()));
        for (const factionId ally : faction.allies)
            hashU32(hash, ally);
    }
    hashU64(hash, result.beginnerTerritory);
    hashU64(hash, result.playerSpawnVillage);
    hashU64(hash, result.elvenEnclave);
    result.summaryHash = hash;

    // System-entry event (task 0.5 rule 3): the decisive numbers inline.
    HARROWMERE_LOG_INFO(
        sim,
        "world generated: seed=%llu regions=%u territories=%u npcs=%llu factions=%u spawn_village=%llu hash=%016llx",
        (unsigned long long)worldSeed,
        (unsigned)world.regionCount(),
        (unsigned)world.territoryCount(),
        (unsigned long long)npcsOut.size(),
        (unsigned)result.factions.size(),
        (unsigned long long)result.playerSpawnVillage,
        (unsigned long long)result.summaryHash
    );
    return result;
}

} // namespace harrowmere::world
