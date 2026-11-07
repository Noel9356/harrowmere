// Task 5.2.1 - Scatter: vegetation, rock & settlement props.
//
// The land is inhabited from the outside: trees, rocks, shrubs and settlement
// buildings stand where the world says they should — dense near villages,
// sparse on the frontier — all of it deterministic, all of it data.
//
// D-IMG-2 (common.md Appendix A.14): SCATTER IS DATA + SEED. A new data file
// `scatter_tables.json` (Appendix B.9 shape) maps climate class → prop table
// (prop kind, density per hectare, scale range, allowed slope). Placement
// draws from a dedicated seeded hash of (worldSeed, cellX, cellZ) — NO RNG
// stream is touched (the same discipline as 5.1.2's terrain noise: the
// worldgen stream is a sim resource; borrowing it changes every downstream
// draw).
//
// What ships here:
//   * ScatterProp / ScatterPlacement — pure-data records describing one prop
//     kind and one placed prop instance.
//   * ScatterTables — the loaded data (props + per-climate tables + per-tier
//     settlement footprints). Loaded from scatter_tables.json through the
//     game's strict JSON door (D-DATA-1).
//   * ScatterField — build-once, read-many: given (seed, climate grid,
//     territory templates, terrain field), produces the deterministic
//     placement list. Aligned to the heightfield (5.1.2's heightAt), never
//     on water (A-151's sea level) or on steep slopes (maxSlope per prop).
//   * ScatterView — the per-view cap (A-152 = 12,000 instances): the field
//     produces placements deterministically; the view caps the count so a
//     frame never draws more than A-152.
//
// What does NOT ship here:
//   * The renderer's instanced batch path (the renderer's existing batch
//     path carries it — D-IMG-3: no new engine module). The bridge reads
//     the placements and drives the scene sink.
//   * Sim existence for props (no store rows, no save entries, no tick cost —
//     the card: "props have no sim existence: no store rows, no save entries,
//     no tick cost — they are derived exactly like terrain (rebuilt from the
//     seed; loading a save reproduces them bit-stable)").
//
// Determinism: every placement is a pure function of (seed, climate grid,
// territory templates, terrain field). Same seed → identical placements
// (TestScatter_SameSeedSamePlacements). Loading a save reproduces them
// (TestScatter_SaveLoadReproducesPlacements — the placements derive from the
// seed, which the save persists; no separate scatter state to serialize).

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "core/types.hpp"            // Position, regionId, territoryId (task 0.2)
#include "core/constants_phase5.hpp" // A-150, A-151, A-152

namespace harrowmere::world
{
enum class Climate : std::uint32_t;
class WorldStore;
} // namespace harrowmere::world

namespace harrowmere::data
{
class DataRegistry;
} // namespace harrowmere::data

namespace harrowmere::bridge
{

class TerrainField; // forward (5.1.2)

// ---- data types -----------------------------------------------------------

// The primitive palette (the card: "Don't invent prop kinds beyond the pinned
// primitive palette (box, plate, cone, ring); variety comes from
// scale/rotation/clustering, not new geometry"). Four primitives.
enum class ScatterPrimitive : std::uint8_t
{
    BOX    = 0,
    PLATE  = 1,
    CONE   = 2,
    SPHERE = 3, // the card's "ring" is a sphere shell (same as the faction ring)
    COUNT
};

// One prop kind (a row in scatter_tables.json's "props" array).
struct ScatterProp
{
    std::string key; // "pine", "boulder", "house", ...
    ScatterPrimitive primitive = ScatterPrimitive::BOX;
    double scaleMinM           = 1.0; // [min, max] scale range
    double scaleMaxM           = 1.0;
};

// One entry in a climate's prop table: which prop, how dense, max slope.
struct ScatterClimateEntry
{
    std::string propKey; // references ScatterProp::key
    std::uint32_t perHectare = 0;
    double maxSlope          = 1.0; // [0, 1] — 1.0 = any slope, 0.0 = flat only
};

// One territory tier's settlement footprint (A-150).
struct ScatterSettlementFootprint
{
    std::string propKey; // the building prop key (usually "house")
    std::uint32_t minBuildings = 0;
    std::uint32_t maxBuildings = 0;
};

// The loaded scatter tables (scatter_tables.json). Built once at startup
// through DataRegistry; immutable thereafter.
struct ScatterTables
{
    std::vector<ScatterProp> props;                      // keyed by index
    std::map<std::string, std::uint32_t> propIndexByKey; // key → props[] index

    // Per-climate prop tables. Keyed by climate name ("MOUNTAIN", "FOREST", ...).
    std::map<std::string, std::vector<ScatterClimateEntry>> climateTables;

    // Per-territory-tier settlement footprints. Keyed by TerritoryType name
    // ("VILLAGE", "CITY").
    std::map<std::string, ScatterSettlementFootprint> settlementFootprints;

    bool loaded = false;
};

// One placed prop instance. Pure data — the bridge reads this and spawns
// the scene primitive.
struct ScatterPlacement
{
    Position position{};             // world space, y = heightAt(x, z)
    std::uint32_t propIndex = 0;     // index into ScatterTables::props
    double scaleM           = 1.0;   // drawn from the seeded hash, in [scaleMin, scaleMax]
    double rotationRad      = 0.0;   // drawn from the seeded hash, [0, 2*pi)
    bool isSettlement       = false; // true iff this is a settlement building (vs scatter)
};

// One settlement's seed center (the graphics-uplift round): the bridge feeds
// the world's village/city territory centers before the first placements()
// call, and the field lays each center's A-150 footprint cluster around it.
struct ScatterSettlementCenter
{
    Position position{};          // the territory's region center (world metres)
    const char *tierKey = "VILLAGE"; // the settlementFootprints key ("VILLAGE"/"CITY")
    std::uint64_t seedMix = 0;    // the territory id — the cluster layout's hash domain
};

// ---- the field ------------------------------------------------------------

// The scatter field for one world. Constructed from (seed, climate grid,
// territory centers, terrain field, scatter tables); immutable thereafter.
// The field produces the full placement list lazily ( placementsFor() ),
// capped by A-152 per view.
//
// Construction does NOT touch the RNG streams (D-IMG-2): the placements
// derive from a splitmix64-style hash of (seed, cellX, cellZ), the same
// discipline as 5.1.2's terrain noise.
class ScatterField
{
public:
    ScatterField(
        std::uint64_t seed,
        const std::vector<world::Climate> &regionClimates,
        const TerrainField &terrain,
        const ScatterTables &tables
    );

    // The full placement list (scatter + settlement), deterministic from
    // (seed, climates, terrain, tables). The list is built once on first
    // call and cached. The view caps the count at A-152; the cap is the
    // LAST N placements (settlement buildings first, then the densest
    // scatter cells — a teleport's view never exceeds the cap).
    const std::vector<ScatterPlacement> &placements() const;

    // The per-view cap: at most A-152 placements. The field's full list
    // may exceed this; the view returns the first A-152 (settlement
    // buildings always included; scatter fills the rest by density).
    std::vector<ScatterPlacement> placementsForView(std::uint32_t maxInstances) const;

    std::uint64_t seed() const noexcept
    {
        return seed_;
    }
    std::size_t totalPlacementCount() const noexcept
    {
        return placements_.size();
    }

    // The loaded tables (the bridge's prop-composition publisher reads the
    // prop keys by index; the placement list carries indices only).
    const ScatterTables &tables() const noexcept
    {
        return *tables_;
    }

    // The settlement centers (the graphics-uplift round): MUST precede the
    // first placements() call (buildPlacements runs once and reads them).
    // Empty by default, so the synthetic-field tests keep their exact
    // vegetation-only lists.
    void setSettlementCenters(std::vector<ScatterSettlementCenter> centers) const
    {
        settlementCenters_ = std::move(centers);
    }

private:
    void buildPlacements() const;

    std::uint64_t seed_ = 0;
    std::vector<world::Climate> region_climates_;
    const TerrainField *terrain_ = nullptr;
    const ScatterTables *tables_ = nullptr;
    mutable std::vector<ScatterPlacement> placements_;
    mutable std::vector<ScatterSettlementCenter> settlementCenters_;
    mutable bool built_ = false;
};

// ---- the hash (D-IMG-2: NOT the worldgen RNG stream) ----------------------
//
// A splitmix64-style hash over (seed, cellX, cellZ) → u64. The same algorithm
// 5.1.2's terrain noise uses (so the two fields are independent draws from
// the same seed value, never from the SeedRng stream table).
std::uint64_t scatterHash(std::uint64_t seed, std::int64_t cellX, std::int64_t cellZ) noexcept;

// Load the scatter tables from scatter_tables.json through the DataRegistry's
// strict JSON door. Returns true on success; fills `tables` on success and
// leaves it unchanged on failure.
bool loadScatterTables(const std::filesystem::path &dataDir, ScatterTables &tables);

} // namespace harrowmere::bridge
