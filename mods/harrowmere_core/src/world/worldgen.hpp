#ifndef HARROWMERE_CORE_WORLD_WORLDGEN_HPP
#define HARROWMERE_CORE_WORLD_WORLDGEN_HPP

// Worldgen — task 1.5: world generation from seed.
//
// generateWorld(SEED, DataRegistry, WorldStore&, NpcSeedOut&) builds the
// entire static world deterministically: the region grid (stage 1), the
// territory tree (stage 2), laws and the NOBLE/ROYAL title chain (stage 3),
// the safe-zone spawn points (stage 4), the NPC faction seeds (stage 5), the
// initial population PLAN (stage 6 — actual NPC instantiation is task 3.4's
// spawn path), and the canonical summary hash (stage 7 — task 8.1 reuses
// it). The pinned stage order IS the generation contract: each stage draws
// only from the "worldgen" RNG stream (task 0.3's pinned streams), in the
// order the stages run, so two runs with the same seed produce byte-identical
// output and the summary hashes match (exit checklist).
//
// Determinism rules this file lives by (common.md §3.2):
//   * R6 — every roll goes through SeedRng's pinned "worldgen" stream; no
//     other stream, no wall clock, no std::random_device.
//   * R7 — every container whose iteration order the output can observe is
//     std::map or a sorted/indexed vector; unordered containers are banned
//     here (the card's pitfall: "worldgen is the place it hurts most").
//   * R10 — species are referenced by KEY through the registry, never by
//     literal id (the player species' id 0 is the one hardcoded constant,
//     and worldgen never plans a player). Character ids come from a fresh
//     IdGenerator in strict plan order; WorldGenResult::nextCharacterId
//     hands the runtime the counter value to adopt (7.1 persists it).
//
// Per-NPC draw order (card pitfall — fixed FOREVER, do not reorder):
//     species -> age -> role (only when the role is drawn, not quoted) ->
//     integrity. Level is derived from age afterwards (no draw).
//
// Data-driven inputs: territory templates carry the tree multipliers
// (kingdoms_per_empire / cities_per_kingdom / villages_per_city) and the
// per-type population ranges and law sets; species, roles and classes come
// from the registry; the per-species name tables load from the package's
// data/names/ directory (the title chain is seeded "using classes.json +
// name tables", card stage 3).

#include <cstdint>
#include <string>
#include <vector>

#include "core/enums.hpp"   // SocialClassId (NpcSeed)
#include "core/types.hpp"   // id aliases, Position, IdGenerator (task 0.2)
#include "world/world.hpp"  // WorldStore, kNoTerritory

namespace harrowmere::data
{
class DataRegistry; // the committed content snapshot (data/registry.hpp); reference-only here
} // namespace harrowmere::data

namespace harrowmere::world
{

// ---- worldgen-owned generation constants ------------------------------------
//
// Values the plan pins directly live here next to their card citation; every
// number the plan leaves open carries a "worldgen pin" note and a
// DEVIATIONS.md row (the lead promotes them into Appendix A when accepted —
// common.md §5, "new constants have one door").

// D-LORE-3 (card stage 1): the planet lies entirely inside the mana-bearing
// region — every region draws manaRichness uniformly in [MIN, MAX] (both
// inclusive; MAX is reachable).
inline constexpr std::uint32_t MANA_RICHNESS_MIN = 9500;
inline constexpr std::uint32_t MANA_RICHNESS_MAX = 10000;

// Card stage 2: EMPIRE_COUNT = 1 (the root empire, sovereign). KINGDOMS =
// 4, CITIES = 3/kingdom, VILLAGES = 4/city come from territory_templates.json
// (kingdoms_per_empire / cities_per_kingdom / villages_per_city, card:
// "overridable in territory_templates.json") — the literals below are only
// the degenerate-data fallbacks and are asserted against the templates.
inline constexpr std::uint32_t EMPIRE_COUNT = 1;

// Card stage 2: contiguous region blocks — cities take 3-5 regions (drawn,
// inclusive bounds), villages exactly 1. The frontier ring (the outermost
// region row/column) stays unclaimed.
inline constexpr std::uint32_t CITY_REGIONS_MIN   = 3;
inline constexpr std::uint32_t CITY_REGIONS_MAX   = 5;
inline constexpr std::uint32_t FRONTIER_RING_DEPTH = 1;

// Card stage 3: the pinned per-kingdom law variance roll — 10% on the SCALE
// grid to swap one city/village of that kingdom to xenophobic_village or
// tolerant_city (the doc's "some human kingdoms order beastkins killed on
// sight, other kingdoms accept many species").
inline constexpr std::uint32_t LAW_VARIANCE_CHANCE = 1000;

// Card stage 5: FACTION_COUNT = 5 — one faction per kingdom court plus one
// frontier free company; 2 rivalry pairs and 1 alliance among the courts.
inline constexpr std::uint32_t FACTION_COUNT = 5;

// Card stage 6 role quotas (fixed counts, not draws): guards 8 per city and
// 2 per village, one healer per city. The "noble 1/city" quota IS the
// stage-3 title chain (the titled noble counts toward the city population).
inline constexpr std::uint32_t GUARDS_PER_CITY    = 8;
inline constexpr std::uint32_t GUARDS_PER_VILLAGE = 2;
inline constexpr std::uint32_t HEALERS_PER_CITY   = 1;

// Card stage 6, species mix rule "dragons/goo beasts rare: <= 3 total
// world-wide, FAR-tier wanderers": the draw is [0, RARE_WANDERER_MAX]
// inclusive across BOTH species combined.
inline constexpr std::uint32_t RARE_WANDERER_MAX = 3;

// Worldgen pins (plan leaves them open; DEVIATIONS.md carries the rows):
//   * FRONTIER_WANDERER_COUNT — the frontier population plan size (the
//     beastkin/taur-weighted wanderer pool; also the free company's founding
//     membership by convention).
//   * STARTING_LEVEL_MAX — the top of the starting level band by age:
//     level = 1 + age * (STARTING_LEVEL_MAX - 1) / lifespan, so a
//     full-lifespan adult starts at level 10 and the young at 1.
inline constexpr std::uint32_t FRONTIER_WANDERER_COUNT = 24;
inline constexpr std::uint32_t STARTING_LEVEL_MAX      = 10;

// ---- the population plan (consumed verbatim by task 3.4's spawn path) -------

// One planned NPC. The card pins the field set for stage-6 NPCs (species,
// role, age, integrity, level); the stage-3 title chain adds the identity
// fields (class + names, "seeded using classes.json + name tables").
// NpcSeed is PLANNING data — no Character exists until 3.4 spawns it.
struct NpcSeed
{
    characterId id = 0;                    // pre-assigned in plan order (0 is reserved, R10)
    speciesId species = 0;                 // resolved from the species mix by KEY (1.1 pitfall)
    std::string roleKey;                   // roles.json key ("farmer", "guard", ...)
    std::uint32_t ageYears = 0;            // adult band [ADULT_YEARS, lifespan]; `child` clamps below A-44
    std::uint32_t integrity = 0;           // 0..SCALE, drawn from the role's integrityRange
    std::uint32_t level = 1;               // starting level band by age (STARTING_LEVEL_MAX)
    territoryId homeTerritory = kNoTerritory; // kNoTerritory = frontier wanderer
    regionId homeRegion = kNoRegion;       // the spawn anchor for 3.4 (settlement anchor / frontier cell)
    SocialClassId socialClass = SocialClassId::COMMONER; // NOBLE/ROYAL on the title chain only
    std::string classKey;                  // classes.json key — title chain only; empty otherwise
    std::string givenName;                 // species name table — title chain only; empty otherwise
    std::string familyName;

    bool operator==(const NpcSeed &) const = default;
};

// One seeded faction (card stage 5). Task 6.1 owns the runtime Faction
// records; these seeds are its new-game input. Rivalries: exactly 2 pairs
// among the four kingdom courts; alliance: exactly 1 cross pair. The
// frontier free company (id FACTION_COUNT) stays unaligned in v1.
struct FactionSeed
{
    factionId id = 0;                          // 1..FACTION_COUNT, creation order
    std::string key;                           // "court_kingdom_<id>" / "frontier_free_company"
    std::string name;                          // deterministic display flavor
    territoryId courtTerritory = kNoTerritory; // the kingdom whose court this is; 0 = free company
    std::vector<factionId> rivals;             // sorted, unique
    std::vector<factionId> allies;             // sorted, unique

    bool operator==(const FactionSeed &) const = default;
};

// Everything generateWorld produces beyond the WorldStore graph itself.
// The outputs are fully persisted at new-game (D-WGEN-1; task 7.1) — loading
// a save never re-generates.
struct WorldGenResult
{
    std::uint64_t summaryHash = 0; // stage 7: FNV-1a over the canonical summary
    std::vector<FactionSeed> factions;

    // Stage 4: the beginner kingdom (the kingdom covering the claimed region
    // nearest the world origin — see the frontier-ring note in worldgen.cpp),
    // its safe village (the design rule: safe zones are spawn points and the
    // beginner territory), and the spawn Position at that village's center.
    territoryId beginnerTerritory = kNoTerritory;
    territoryId playerSpawnVillage = kNoTerritory;
    Position playerSpawn{};

    // Stage 6: the single elf-majority forest enclave ("elves one forest
    // enclave") — a VILLAGE id, kNoTerritory when no forest village exists.
    territoryId elvenEnclave = kNoTerritory;

    // The character id space after the plan (worldgen drew
    // nextCharacterId - IdGenerator::FIRST_ID ids); the runtime adopts this
    // so spawn-path and player ids continue the same sequence (7.1).
    std::uint64_t nextCharacterId = IdGenerator::FIRST_ID;

    bool operator==(const WorldGenResult &) const = default;
};

// The card's output parameter type ("Output: std::vector<NpcSeed>
// consumed by 3.4").
using NpcSeedOut = std::vector<NpcSeed>;

// Generate the world. Precondition: `registry.ok()` (a committed, validated
// snapshot — worldgen dereferences registry-owned LawSets for the whole
// session). `world` is cleared and repopulated; `npcsOut` is overwritten.
//
// D-WGEN-2: synchronous, runs in well under the 2 s debug-build budget for
// the v1 world (256 regions, ~10^3-10^4 planned NPCs). Never throws; the
// same (seed, registry content) always yields byte-identical outputs.
WorldGenResult generateWorld(
    std::uint64_t worldSeed, const data::DataRegistry &registry, WorldStore &world, NpcSeedOut &npcsOut
);

} // namespace harrowmere::world

#endif // HARROWMERE_CORE_WORLD_WORLDGEN_HPP
