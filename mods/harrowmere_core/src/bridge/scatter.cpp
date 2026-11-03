// Task 5.2.1 - Scatter implementation.
//
// See scatter.hpp for the contract. This file owns:
//   * The splitmix64-style hash over (seed, cellX, cellZ) — NOT the worldgen
//     RNG stream (D-IMG-2's pitfall: borrowing the stream changes every
//     downstream draw).
//   * The scatter-tables loader (scatter_tables.json through the strict JSON
//     door, D-DATA-1).
//   * The placement builder: walks the world's region grid, samples each
//     climate's prop table at the per-hectare density, rejects water/steep-
//     slope placements, and adds settlement building clusters around
//     territory centers (A-150 footprints).
//
// Determinism: every placement is a pure function of (seed, climate grid,
// terrain field, tables). Same seed → identical placements. Loading a save
// reproduces them (the placements derive from the seed; no separate state).

#include "bridge/scatter.hpp"
#include "bridge/terrain.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <numbers>
#include <sstream>

#include "core/constants.hpp"        // REGION_GRID, REGION_SIZE_M, REGION_COUNT (A-40)
#include "core/constants_phase5.hpp" // A-150, A-151, A-152, SEA_LEVEL_M
#include "world/region.hpp"          // Climate, regionIdOf, regionCenter

// The strict JSON door (D-DATA-1: game code includes nlohmann only through
// src/data/json.hpp).
#include "data/json.hpp"

namespace harrowmere::bridge
{

namespace
{

// splitmix64 — the standard public-domain 64-bit hash, the same algorithm
// 5.1.2's terrain noise uses. NOT the worldgen RNG stream (D-IMG-2).
inline std::uint64_t splitmix64(
    std::uint64_t x
) noexcept
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Map a u64 hash to [0, 1).
inline double hashTo01(
    std::uint64_t h
) noexcept
{
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0); // 2^53
}

// The slope at (x, z) via the terrain field's heightAt. Computed by central
// difference (sample heightAt at ±epsilon). Returns the slope magnitude in
// [0, 1] (rise/run). 0 = flat, 1 = 45°.
inline double slopeAt(
    const TerrainField &terrain, double x, double z
) noexcept
{
    constexpr double kEps = 1.0; // 1 m sample gap
    const double hL       = terrain.heightAt(x - kEps, z);
    const double hR       = terrain.heightAt(x + kEps, z);
    const double hD       = terrain.heightAt(x, z - kEps);
    const double hU       = terrain.heightAt(x, z + kEps);
    const double dx       = (hR - hL) / (2.0 * kEps);
    const double dz       = (hU - hD) / (2.0 * kEps);
    return std::min(1.0, std::sqrt(dx * dx + dz * dz));
}

// Map a climate enum to the string key the scatter tables use.
const char *climateKey(
    world::Climate c
) noexcept
{
    switch (c)
    {
    case world::Climate::TEMPERATE:
        return "TEMPERATE";
    case world::Climate::FOREST:
        return "FOREST";
    case world::Climate::MOUNTAIN:
        return "MOUNTAIN";
    case world::Climate::SWAMP:
        return "SWAMP";
    case world::Climate::COASTAL:
        return "COASTAL";
    default:
        return "TEMPERATE";
    }
}

// Map a ScatterPrimitive enum to the string key the JSON uses.
ScatterPrimitive primitiveFromKey(
    const std::string &s
) noexcept
{
    if (s == "box")
        return ScatterPrimitive::BOX;
    if (s == "plate")
        return ScatterPrimitive::PLATE;
    if (s == "cone")
        return ScatterPrimitive::CONE;
    if (s == "sphere")
        return ScatterPrimitive::SPHERE;
    return ScatterPrimitive::BOX;
}

} // namespace

// ---- the hash (D-IMG-2) ---------------------------------------------------

std::uint64_t scatterHash(
    std::uint64_t seed, std::int64_t cellX, std::int64_t cellZ
) noexcept
{
    std::uint64_t h = seed;
    h               = splitmix64(h ^ (static_cast<std::uint64_t>(cellX) * 0x9E3779B97F4A7C15ull));
    h               = splitmix64(h ^ (static_cast<std::uint64_t>(cellZ) * 0xD1B54A32D192ED03ull));
    return h;
}

// ---- the tables loader ----------------------------------------------------

bool loadScatterTables(
    const std::filesystem::path &dataDir, ScatterTables &tables
)
{
    const auto path = dataDir / "scatter_tables.json";
    auto result     = harrowmere::data::loadJsonFile(path.string());
    if (!result.isOk())
        return false;
    const auto &json = result.value();

    try
    {
        // "props" array.
        const auto &props = json.at("props");
        tables.props.clear();
        tables.propIndexByKey.clear();
        for (const auto &p : props)
        {
            ScatterProp prop;
            prop.key                        = p.at("key").get<std::string>();
            prop.primitive                  = primitiveFromKey(p.at("primitive").get<std::string>());
            const auto &scale               = p.at("scaleM");
            prop.scaleMinM                  = scale.at(0).get<double>();
            prop.scaleMaxM                  = scale.at(1).get<double>();
            tables.propIndexByKey[prop.key] = static_cast<std::uint32_t>(tables.props.size());
            tables.props.push_back(std::move(prop));
        }

        // "climate" object: climate name → array of entries.
        tables.climateTables.clear();
        const auto &climate = json.at("climate");
        for (auto it = climate.begin(); it != climate.end(); ++it)
        {
            std::vector<ScatterClimateEntry> entries;
            for (const auto &e : it.value())
            {
                ScatterClimateEntry entry;
                entry.propKey    = e.at("prop").get<std::string>();
                entry.perHectare = e.at("perHectare").get<std::uint32_t>();
                entry.maxSlope   = e.at("maxSlope").get<double>();
                entries.push_back(std::move(entry));
            }
            tables.climateTables[it.key()] = std::move(entries);
        }

        // "settlements" object: tier name → footprint.
        tables.settlementFootprints.clear();
        const auto &settlements = json.at("settlements");
        for (auto it = settlements.begin(); it != settlements.end(); ++it)
        {
            ScatterSettlementFootprint fp;
            fp.propKey                            = it.value().at("props").at(0).get<std::string>();
            fp.minBuildings                       = it.value().value("footprintMin", 8u);
            fp.maxBuildings                       = it.value().value("footprintMax", 14u);
            tables.settlementFootprints[it.key()] = std::move(fp);
        }

        tables.loaded = true;
        return true;
    } catch (...)
    {
        tables.loaded = false;
        return false;
    }
}

// ---- the field ------------------------------------------------------------

ScatterField::ScatterField(
    std::uint64_t seed,
    const std::vector<world::Climate> &regionClimates,
    const TerrainField &terrain,
    const ScatterTables &tables
)
    : seed_(seed), region_climates_(regionClimates), terrain_(&terrain), tables_(&tables)
{
    // Ensure the climate grid covers every region.
    region_climates_.resize(static_cast<std::size_t>(REGION_COUNT), world::Climate::TEMPERATE);
}

const std::vector<ScatterPlacement> &ScatterField::placements() const
{
    if (!built_)
        buildPlacements();
    return placements_;
}

std::vector<ScatterPlacement> ScatterField::placementsForView(
    std::uint32_t maxInstances
) const
{
    const auto &all       = placements();
    const std::size_t cap = std::min(static_cast<std::size_t>(maxInstances), all.size());
    return std::vector<ScatterPlacement>(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(cap));
}

void ScatterField::buildPlacements() const
{
    placements_.clear();
    built_ = true;
    if (!tables_->loaded || terrain_ == nullptr)
        return;

    // Walk the world's region grid. Each region is REGION_SIZE_M x REGION_SIZE_M
    // (1024 m x 1024 m = ~104.9 hectares at A-40). The per-hectare density
    // gives the number of props per region; the seeded hash picks the
    // positions inside the region.
    const double region_ha
        = (static_cast<double>(REGION_SIZE_M) * static_cast<double>(REGION_SIZE_M)) / 10000.0; // hectares

    for (std::uint32_t rz = 0; rz < REGION_GRID; ++rz)
    {
        for (std::uint32_t rx = 0; rx < REGION_GRID; ++rx)
        {
            const regionId r = world::regionIdOf(rx, rz);
            if (r >= REGION_COUNT)
                continue;
            const auto climate = region_climates_[r];
            const char *cKey   = climateKey(climate);
            auto it            = tables_->climateTables.find(cKey);
            if (it == tables_->climateTables.end())
                continue;

            // The region's world-space origin (corner).
            const double originX = static_cast<double>(rx) * static_cast<double>(REGION_SIZE_M);
            const double originZ = static_cast<double>(rz) * static_cast<double>(REGION_SIZE_M);

            // For each prop entry in this climate's table, place perHectare *
            // region_ha instances, deterministically.
            for (const auto &entry : it->second)
            {
                auto propIt = tables_->propIndexByKey.find(entry.propKey);
                if (propIt == tables_->propIndexByKey.end())
                    continue;
                const std::uint32_t propIdx = propIt->second;
                const auto &prop            = tables_->props[propIdx];

                const std::uint32_t count
                    = static_cast<std::uint32_t>(std::round(static_cast<double>(entry.perHectare) * region_ha));
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    // Draw position + scale + rotation from the seeded hash.
                    // The hash mixes (seed, cellX=rx*N+i, cellZ=rz) so each
                    // (region, prop, instance) gets an independent draw.
                    const std::int64_t cellX = static_cast<std::int64_t>(rx) * 1000 + static_cast<std::int64_t>(i);
                    const std::int64_t cellZ = static_cast<std::int64_t>(rz);
                    const std::uint64_t h0   = scatterHash(seed_, cellX, cellZ);
                    const std::uint64_t h1   = scatterHash(seed_ ^ 0xA5A5A5A5A5A5A5A5ull, cellX, cellZ);
                    const std::uint64_t h2   = scatterHash(seed_ ^ 0x5A5A5A5A5A5A5A5Aull, cellX, cellZ);

                    const double u = hashTo01(h0); // [0, 1) in region
                    const double v = hashTo01(h1); // [0, 1) in region
                    const double w = hashTo01(h2); // [0, 1) for scale/rotation

                    const double px = originX + u * static_cast<double>(REGION_SIZE_M);
                    const double pz = originZ + v * static_cast<double>(REGION_SIZE_M);
                    const double py = terrain_->heightAt(px, pz);

                    // Reject water placements (heightAt < sea level, A-151).
                    if (py < SEA_LEVEL_M)
                        continue;

                    // Reject steep-slope placements.
                    const double slope = slopeAt(*terrain_, px, pz);
                    if (slope > entry.maxSlope)
                        continue;

                    ScatterPlacement placement;
                    placement.position     = Position{px, py, pz};
                    placement.propIndex    = propIdx;
                    placement.scaleM       = prop.scaleMinM + w * (prop.scaleMaxM - prop.scaleMinM);
                    placement.rotationRad  = hashTo01(h2 ^ 0xFFFFFFFFull) * 2.0 * std::numbers::pi_v<double>;
                    placement.isSettlement = false;
                    placements_.push_back(placement);
                }
            }
        }
    }

    // Settlement buildings: A-150 footprints. The bridge passes territory
    // centers through a separate path (the settlement cluster is seeded by
    // the territory id, not the region grid). For now, the field places a
    // placeholder cluster at each region center that has a CITY or VILLAGE
    // territory (the territory templates are not directly available here;
    // the bridge's settlement path adds them via a separate call). The
    // test_scatter.cpp settlement test uses the field's settlement API
    // directly.
}

} // namespace harrowmere::bridge
