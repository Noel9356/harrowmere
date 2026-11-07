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
#include <array>
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

    // ---- settlement buildings FIRST (the graphics-uplift round) ------------
    //
    // placementsForView caps the list by PREFIX, and the A-150 footprints
    // are the inhabited frame every village view needs - so the clusters
    // prepend ahead of the region-walk scatter. The roads block above lays
    // its slabs first for the same reason. Each center below lays its tier's
    // footprint count around itself on a deterministic golden-angle spiral
    // (the hash domain mixes the territory id, D-IMG-2: no RNG streams), a
    // clear market square stays open in the middle, and underwater/steep
    // draws are re-rolled from the same hash up to an attempts budget.

    // ---- the roads (rev 31, the inhabited-land round) ----------------------
    //
    // The owner's "worldgen is still far from what I want": the frames show
    // villages as islands on an empty billiard table. Roads are the cheapest
    // structure that makes land READ as a world: for every settlement, the
    // two nearest sibling settlements get a deterministic dirt-road walk.
    // 12 m slabs step ~10.5 m along the straight line between the centers;
    // each step re-samples the terrain (the road follows the ground), and a
    // step is rejected over water, over slopes a rigid slab cannot lie on,
    // and inside the plaza/street radius both endpoints already pave. The
    // slabs are MESH instances (one prop_road mesh) - the analytic SDF
    // budget stays with the buildings that occlude.
    const auto roadPropIt = tables_->propIndexByKey.find("road");
    if (roadPropIt != tables_->propIndexByKey.end() && settlementCenters_.size() > 1)
    {
        const std::size_t centerCount = settlementCenters_.size();
        // 2-nearest siblings per center: O(n^2) over ~60 centers once, with
        // the pair index (seedMix) as the deterministic tie-break.
        std::vector<std::array<std::size_t, 2>> neighbors(centerCount, {centerCount, centerCount});
        for (std::size_t a = 0; a < centerCount; ++a)
        {
            double bestD = 1e30;
            double secondD = 1e30;
            for (std::size_t b = 0; b < centerCount; ++b)
            {
                if (b == a)
                    continue;
                const double dx = settlementCenters_[b].position.x - settlementCenters_[a].position.x;
                const double dz = settlementCenters_[b].position.z - settlementCenters_[a].position.z;
                const double d2 = dx * dx + dz * dz;
                const bool closer = d2 < bestD
                                        || (d2 == bestD && settlementCenters_[b].seedMix < settlementCenters_[neighbors[a][0]].seedMix);
                if (closer)
                {
                    secondD      = bestD;
                    neighbors[a][1] = neighbors[a][0];
                    bestD        = d2;
                    neighbors[a][0] = b;
                } else if (d2 < secondD
                           || (d2 == secondD && neighbors[a][1] < centerCount
                                   && settlementCenters_[b].seedMix < settlementCenters_[neighbors[a][1]].seedMix))
                {
                    secondD       = d2;
                    neighbors[a][1]  = b;
                }
            }
        }
        // Dedupe: a pair walks ONCE (the smaller center index owns it).
        std::vector<std::pair<std::uint64_t, std::uint64_t>> connected;
        connected.reserve(centerCount * 2);
        const auto isConnected = [&connected](std::uint64_t idA, std::uint64_t idB) {
            for (const auto &pair : connected)
            {
                if ((pair.first == idA && pair.second == idB) || (pair.first == idB && pair.second == idA))
                {
                    return true;
                }
            }
            return false;
        };
        for (std::size_t a = 0; a < centerCount; ++a)
        {
            const ScatterSettlementCenter &from = settlementCenters_[a];
            for (std::size_t lane = 0; lane < 2; ++lane)
            {
                const std::size_t b = neighbors[a][lane];
                if (b >= centerCount)
                    continue;
                const ScatterSettlementCenter &to = settlementCenters_[b];
                const std::uint64_t idA = std::min(from.seedMix, to.seedMix);
                const std::uint64_t idB = std::max(from.seedMix, to.seedMix);
                if (isConnected(idA, idB))
                    continue;
                connected.emplace_back(idA, idB);

                const double dx = to.position.x - from.position.x;
                const double dz = to.position.z - from.position.z;
                const double lengthM = std::sqrt(dx * dx + dz * dz);
                if (lengthM < 40.0)
                    continue;
                const double dirX = dx / lengthM;
                const double dirZ = dz / lengthM;
                const double yaw = std::atan2(dirX, dirZ); // rotY: +Z forward
                const double clearStart = 16.0;            // the plaza + street stubs pave this radius
                const double clearEnd   = lengthM - 16.0;
                const std::uint64_t roadStream = from.seedMix ^ to.seedMix;
                for (double along = clearStart; along <= clearEnd; along += 10.5)
                {
                    const double rx = from.position.x + dirX * along;
                    const double rz = from.position.z + dirZ * along;
                    const double ry = terrain_->heightAt(rx, rz);
                    if (ry < SEA_LEVEL_M + 0.4)
                        continue; // the road breaks at ponds - a bridge is a later card
                    if (slopeAt(*terrain_, rx, rz) > 0.38)
                        continue;
                    // A 12 m rigid slab on a drop floats an end: the ground
                    // under the slab's half-length must stay within a step.
                    const double dropMax = std::max({
                        std::fabs(terrain_->heightAt(rx + dirX * 5.0, rz + dirZ * 5.0) - ry),
                        std::fabs(terrain_->heightAt(rx - dirX * 5.0, rz - dirZ * 5.0) - ry)});
                    if (dropMax > 0.55)
                        continue;
                    ScatterPlacement rp;
                    rp.position     = Position{rx, ry, rz};
                    rp.propIndex    = roadPropIt->second;
                    rp.scaleM       = 1.0;
                    rp.rotationRad  = yaw;
                    rp.isSettlement = true;
                    // The hash mixes the pair + the step (D-IMG-2), so a
                    // neighboring road never reuses the same jitter domain.
                    const std::uint64_t stepHash = scatterHash(roadStream, static_cast<std::int64_t>(along * 10.0), 61);
                    rp.rotationRad += (hashTo01(stepHash) - 0.5) * 0.06; // the hand-laid wiggle
                    placements_.push_back(rp);
                }
            }
        }
    }

    // ---- the settlement loops: fields, detail rings, docks (rev 31) --------
    const auto cropsPropIt = tables_->propIndexByKey.find("crops");
    const auto fencePropIt = tables_->propIndexByKey.find("fence");
    const auto shrubPropIt = tables_->propIndexByKey.find("shrub");
    const auto boulderPropIt = tables_->propIndexByKey.find("boulder");
    const auto dockPropIt = tables_->propIndexByKey.find("dock");
    const auto boatPropIt = tables_->propIndexByKey.find("boat");

    for (const ScatterSettlementCenter &center : settlementCenters_)
    {
        const auto footIt = tables_->settlementFootprints.find(center.tierKey);
        if (footIt == tables_->settlementFootprints.end())
            continue;
        const ScatterSettlementFootprint &footprint = footIt->second;
        const auto propIt = tables_->propIndexByKey.find(footprint.propKey);
        if (propIt == tables_->propIndexByKey.end())
            continue;
        const std::uint32_t propIdx = propIt->second;
        const auto &prop            = tables_->props[propIdx];

        const std::uint64_t countHash = scatterHash(seed_ ^ center.seedMix, 7919, 104729);
        const std::uint32_t span      = footprint.maxBuildings - footprint.minBuildings + 1;
        const std::uint32_t count     = footprint.minBuildings + static_cast<std::uint32_t>(countHash % span);

        const std::uint32_t maxAttempts = count * 4 + 8;
        std::uint32_t placed            = 0;
        for (std::uint32_t attempt = 0; attempt < maxAttempts && placed < count; ++attempt)
        {
            const std::int64_t cell = static_cast<std::int64_t>(attempt) + 1;
            const std::uint64_t h0  = scatterHash(seed_ ^ center.seedMix, cell, 17);
            const std::uint64_t h1  = scatterHash(seed_ ^ center.seedMix, cell, 39);
            const std::uint64_t h2  = scatterHash(seed_ ^ center.seedMix, cell, 73);
            const double jitterA    = hashTo01(h0);
            const double jitterR    = hashTo01(h1);
            const double w          = hashTo01(h2);

            // Golden-angle spiral: ring spacing keeps the cottages off each
            // other's walls; the jitter breaks the compass regularity a raw
            // spiral would print. The center's first 6 m stay clear (the
            // market square / well ground).
            const double angle  = static_cast<double>(placed) * 2.399963229728653 + jitterA * 0.8;
            const double radius = 9.0 + 2.15 * std::sqrt(static_cast<double>(placed)) + jitterR * 3.5;
            const double px     = center.position.x + std::cos(angle) * radius;
            const double pz     = center.position.z + std::sin(angle) * radius;
            const double py     = terrain_->heightAt(px, pz);
            if (py < SEA_LEVEL_M + 0.5)
                continue; // a cottage never stands in the water
            const double slope = slopeAt(*terrain_, px, pz);
            if (slope > 0.45)
                continue; // foundations need gentler ground than a pine does
            // The center-point slope misses ledges: a cottage whose footprint
            // straddles a drop composes floating parts (the coast headland's
            // hovering roof - the uplift round's debt note). The ground under
            // the ~3 m half-footprint must stay within a step of the center.
            const double dropMax = std::max({
                std::fabs(terrain_->heightAt(px - 3.0, pz) - py),
                std::fabs(terrain_->heightAt(px + 3.0, pz) - py),
                std::fabs(terrain_->heightAt(px, pz - 3.0) - py),
                std::fabs(terrain_->heightAt(px, pz + 3.0) - py)});
            if (dropMax > 1.2)
                continue; // the footprint straddles a ledge - parts would float

            ScatterPlacement placement;
            placement.position     = Position{px, py, pz};
            placement.propIndex    = propIdx;
            placement.scaleM       = prop.scaleMinM + w * (prop.scaleMaxM - prop.scaleMinM);
            placement.rotationRad  = angle; // the roof-tier pick reads it; parts stay axis-aligned
            placement.isSettlement = true;
            placements_.push_back(placement);
            ++placed;
        }

        // ---- the settlement yard (rev 23, the inhabited-village round) ----
        //
        // Houses alone read as a depot; the yard furniture is what makes a
        // village read inhabited: a well on the square, crates, fence
        // posts, a handcart, lamp posts (the warm night views need their
        // light points). Deterministic from the same center seed (D-IMG-2);
        // every draw passes the same water/slope rejections the houses use.
        static constexpr const char *yardKeys[] = {"well", "crate", "fence", "lamp", "barrel",
            "fence", "cart", "lamp", "stall", "campfire", "hay", "barrel"};
        // Rev 25 (the drama round): 12-16 yard props (was 8-12) - the empty
        // plain read as boring partly because the village read as three
        // sheds; the yard furniture is the inhabited texture.
        const std::uint32_t yardTarget = 12 + static_cast<std::uint32_t>(countHash % 5); // 12-16 props
        std::uint32_t yardPlaced       = 0;
        for (std::uint32_t attempt = 0; attempt < yardTarget * 3 && yardPlaced < yardTarget; ++attempt)
        {
            const auto yardIt = tables_->propIndexByKey.find(yardKeys[yardPlaced % 12]);
            if (yardIt == tables_->propIndexByKey.end())
                break; // the tables lack the yard props - skip silently
            const std::int64_t ycell  = 5000 + static_cast<std::int64_t>(attempt);
            const std::uint64_t y0    = scatterHash(seed_ ^ center.seedMix, ycell, 23);
            const std::uint64_t y1    = scatterHash(seed_ ^ center.seedMix, ycell, 29);
            const std::uint64_t y2    = scatterHash(seed_ ^ center.seedMix, ycell, 31);
            const double ya           = hashTo01(y0);
            const double yr           = hashTo01(y1);
            const double yw           = hashTo01(y2);
            const double yangle       = ya * 6.283185307179586;
            const double yradius      = (yardPlaced == 0 ? 6.5 : 7.5) + yr * 12.0; // the well hugs the square
            const double ypx          = center.position.x + std::cos(yangle) * yradius;
            const double ypz          = center.position.z + std::sin(yangle) * yradius;
            const double ypy          = terrain_->heightAt(ypx, ypz);
            if (ypy < SEA_LEVEL_M + 0.5)
                continue;
            if (slopeAt(*terrain_, ypx, ypz) > 0.35)
                continue;
            const ScatterProp &yprop = tables_->props[yardIt->second];
            ScatterPlacement yp;
            yp.position     = Position{ypx, ypy, ypz};
            yp.propIndex    = yardIt->second;
            yp.scaleM       = yprop.scaleMinM + yw * (yprop.scaleMaxM - yprop.scaleMinM);
            yp.rotationRad  = yangle;
            yp.isSettlement = true;
            placements_.push_back(yp);
            ++yardPlaced;
        }

        // ---- the market square (rev 26, the crowd round) -------------------
        //
        // The square itself was bare ground: the street-level reads it as
        // grass with furniture dropped on it. A packed-dirt plaza (the
        // "plaza" prop composes flat dirt ground boxes in the bridge) plus
        // two market stalls flanking the well and a handcart give the
        // center the lived-in market look the crowd shot stands in.
        const auto plazaIt = tables_->propIndexByKey.find("plaza");
        if (plazaIt != tables_->propIndexByKey.end())
        {
            const double py = terrain_->heightAt(center.position.x, center.position.z);
            if (py >= SEA_LEVEL_M + 0.5)
            {
                ScatterPlacement pp;
                pp.position     = Position{center.position.x, py, center.position.z};
                pp.propIndex    = plazaIt->second;
                pp.scaleM       = 1.0;
                pp.rotationRad  = 0.0;
                pp.isSettlement = true;
                placements_.push_back(pp);
            }
        }
        const auto stallIt = tables_->propIndexByKey.find("stall");
        if (stallIt != tables_->propIndexByKey.end())
        {
            // Two stalls east/west of the well (4.5 m out), facing the
            // square's center; the hash picks the red/green canopy variant
            // in the bridge. One handcart backs the north edge.
            const ScatterProp &stallProp = tables_->props[stallIt->second];
            for (int side = 0; side < 2; ++side)
            {
                const double sa  = (side == 0 ? 0.0 : 3.14159265358979323846);
                const double sx  = center.position.x + std::cos(sa) * 4.5;
                const double sz  = center.position.z + std::sin(sa) * 4.5;
                const double sy  = terrain_->heightAt(sx, sz);
                if (sy < SEA_LEVEL_M + 0.5 || slopeAt(*terrain_, sx, sz) > 0.30)
                    continue;
                ScatterPlacement sp;
                sp.position     = Position{sx, sy, sz};
                sp.propIndex    = stallIt->second;
                sp.scaleM       = stallProp.scaleMinM;
                sp.rotationRad  = sa + 1.5707963267948966; // face the center
                sp.isSettlement = true;
                placements_.push_back(sp);
            }
            const auto cartIt = tables_->propIndexByKey.find("cart");
            if (cartIt != tables_->propIndexByKey.end())
            {
                const double cx2 = center.position.x;
                const double cz2 = center.position.z - 8.5;
                const double cy2 = terrain_->heightAt(cx2, cz2);
                if (cy2 >= SEA_LEVEL_M + 0.5)
                {
                    const ScatterProp &cartProp = tables_->props[cartIt->second];
                    ScatterPlacement cp;
                    cp.position     = Position{cx2, cy2, cz2};
                    cp.propIndex    = cartIt->second;
                    cp.scaleM       = cartProp.scaleMinM + 0.3;
                    cp.rotationRad  = 0.4;
                    cp.isSettlement = true;
                    placements_.push_back(cp);
                }
            }
        }

        // ---- the watchtower landmark (rev 25, the drama round) ------------
        //
        // Every settlement gets ONE stone watchtower on its square's rim
        // (17 m out, inside the level pad): the vertical anchor the village
        // silhouette was missing - three cottage sheds on a plain read as a
        // depot, a tower reads as a place.
        const auto towerIt = tables_->propIndexByKey.find("tower");
        if (towerIt != tables_->propIndexByKey.end())
        {
            const std::uint64_t t0  = scatterHash(seed_ ^ center.seedMix, 7777, 53);
            const double ta         = hashTo01(t0) * 6.283185307179586;
            const double tx         = center.position.x + std::cos(ta) * 17.0;
            const double tz         = center.position.z + std::sin(ta) * 17.0;
            const double ty         = terrain_->heightAt(tx, tz);
            if (ty >= SEA_LEVEL_M + 0.5)
            {
                ScatterPlacement tp;
                tp.position     = Position{tx, ty, tz};
                tp.propIndex    = towerIt->second;
                tp.scaleM       = 1.0;
                tp.rotationRad  = ta;
                tp.isSettlement = true;
                placements_.push_back(tp);
            }
        }

        // ---- the village tree ring (rev 25, the drama round) --------------
        //
        // A green frame around the settlement: 6-9 trees on a hash-drawn
        // ring at 34-64 m (outside the level pad, inside the region walk's
        // sparse field). The ring gives every village view a layered
        // mid-ground - trees BETWEEN the camera and the houses - and, with
        // the rev-25 occluder proxies, real cast shadows across the pad.
        const auto treeIt = tables_->propIndexByKey.find("pine");
        if (treeIt != tables_->propIndexByKey.end())
        {
            const std::uint32_t ringTarget = 6 + static_cast<std::uint32_t>((countHash >> 5) % 4);
            std::uint32_t ringPlaced       = 0;
            for (std::uint32_t attempt = 0; attempt < ringTarget * 3 && ringPlaced < ringTarget; ++attempt)
            {
                const std::int64_t rcell  = 9000 + static_cast<std::int64_t>(attempt);
                const std::uint64_t r0    = scatterHash(seed_ ^ center.seedMix, rcell, 37);
                const std::uint64_t r1    = scatterHash(seed_ ^ center.seedMix, rcell, 41);
                const std::uint64_t r2    = scatterHash(seed_ ^ center.seedMix, rcell, 43);
                const double ra           = hashTo01(r0) * 6.283185307179586;
                const double rr           = 34.0 + hashTo01(r1) * 30.0;
                const double rx           = center.position.x + std::cos(ra) * rr;
                const double rz           = center.position.z + std::sin(ra) * rr;
                const double ry           = terrain_->heightAt(rx, rz);
                if (ry < SEA_LEVEL_M + 0.5)
                    continue;
                if (slopeAt(*terrain_, rx, rz) > 0.45)
                    continue;
                const ScatterProp &rprop = tables_->props[treeIt->second];
                ScatterPlacement rp;
                rp.position     = Position{rx, ry, rz};
                rp.propIndex    = treeIt->second;
                rp.scaleM       = rprop.scaleMinM + hashTo01(r2) * (rprop.scaleMaxM - rprop.scaleMinM);
                rp.rotationRad  = ra;
                rp.isSettlement = true;
                placements_.push_back(rp);
                ++ringPlaced;
            }
        }

        // ---- the farm fields (rev 31, the inhabited-land round) ------------
        //
        // A village feeds itself: 1-2 fenced crop fields 26-58 m out on
        // gentle ground. Each field is 6-9 crop strips (the prop_crops mesh
        // instances in rows) between 6 fence posts on the two long sides -
        // the row structure the aerial frames read as farmland, the thing
        // the empty plain never had. The strips run along the field's long
        // axis; the hash picks the axis and the rows' wheat/green split
        // happens in the bridge.
        if (cropsPropIt != tables_->propIndexByKey.end() && fencePropIt != tables_->propIndexByKey.end())
        {
            const std::uint32_t fieldCount = 1 + static_cast<std::uint32_t>((countHash >> 11) % 2u); // 1-2
            for (std::uint32_t field = 0; field < fieldCount; ++field)
            {
                const std::int64_t fcell = 7000 + static_cast<std::int64_t>(field) * 97;
                const std::uint64_t f0 = scatterHash(seed_ ^ center.seedMix, fcell, 67);
                const std::uint64_t f1 = scatterHash(seed_ ^ center.seedMix, fcell, 71);
                const std::uint64_t f2 = scatterHash(seed_ ^ center.seedMix, fcell, 79);
                const double fAngle = hashTo01(f0) * 6.283185307179586;
                const double fRadius = 26.0 + hashTo01(f1) * 32.0;
                const double fpx = center.position.x + std::cos(fAngle) * fRadius;
                const double fpz = center.position.z + std::sin(fAngle) * fRadius;
                if (terrain_->heightAt(fpx, fpz) < SEA_LEVEL_M + 0.5)
                    continue;
                if (slopeAt(*terrain_, fpx, fpz) > 0.28)
                    continue;
                // The field's long axis: the hash rotation (aligned fields
                // read as factory plots; a per-field axis reads as handwork).
                const double rowYaw = hashTo01(f2) * 3.14159265358979323846;
                const double alongX = std::sin(rowYaw);
                const double alongZ = std::cos(rowYaw);
                const double acrossX = std::cos(rowYaw);
                const double acrossZ = -std::sin(rowYaw);
                // 6-9 crop strips stepping 1.6 m across the 1.4 m-wide rows.
                const std::uint64_t rowCountHash = scatterHash(seed_ ^ center.seedMix, fcell, 83);
                const std::uint32_t rowCount = 6 + static_cast<std::uint32_t>(rowCountHash % 4u);
                for (std::uint32_t row = 0; row < rowCount; ++row)
                {
                    const double off = (static_cast<double>(row) - (rowCount - 1) * 0.5) * 1.6;
                    const double cxp = fpx + acrossX * off;
                    const double czp = fpz + acrossZ * off;
                    const double cyp = terrain_->heightAt(cxp, czp);
                    if (cyp < SEA_LEVEL_M + 0.5 || slopeAt(*terrain_, cxp, czp) > 0.30)
                        continue;
                    ScatterPlacement cp;
                    cp.position     = Position{cxp, cyp, czp};
                    cp.propIndex    = cropsPropIt->second;
                    cp.scaleM       = 1.0;
                    cp.rotationRad  = rowYaw;
                    cp.isSettlement = true;
                    placements_.push_back(cp);
                }
                // The fence: 3 posts per long side (the corners + the mid).
                for (int side = 0; side < 2; ++side)
                {
                    for (int post = 0; post < 3; ++post)
                    {
                        const double alongOff = (static_cast<double>(post) - 1.0) * 5.5;
                        const double sideOff = (side == 0 ? 2.6 : -2.6);
                        const double px = fpx + alongX * alongOff + acrossX * sideOff;
                        const double pz = fpz + alongZ * alongOff + acrossZ * sideOff;
                        const double py = terrain_->heightAt(px, pz);
                        if (py < SEA_LEVEL_M + 0.5)
                            continue;
                        const ScatterProp &fprop = tables_->props[fencePropIt->second];
                        ScatterPlacement fp;
                        fp.position     = Position{px, py, pz};
                        fp.propIndex    = fencePropIt->second;
                        fp.scaleM       = fprop.scaleMinM + hashTo01(scatterHash(seed_ ^ center.seedMix, fcell + post, side)) * (fprop.scaleMaxM - fprop.scaleMinM);
                        fp.rotationRad  = rowYaw + 1.5707963267948966; // rails run along the field
                        fp.isSettlement = true;
                        placements_.push_back(fp);
                    }
                }
            }
        }

        // ---- the settlement detail ring (rev 31) ----------------------------
        //
        // The 16-70 m band around the pad carries only the tree ring; the
        // street-level frames read the ground as bare. A dozen mixed shrubs
        // and boulders (the existing vocabulary, no new meshes) give the
        // band the ground cover every real landscape has.
        if (shrubPropIt != tables_->propIndexByKey.end() && boulderPropIt != tables_->propIndexByKey.end())
        {
            const std::uint32_t detailTarget = 10 + static_cast<std::uint32_t>((countHash >> 17) % 5u); // 10-14
            std::uint32_t detailPlaced = 0;
            for (std::uint32_t attempt = 0; attempt < detailTarget * 3 && detailPlaced < detailTarget; ++attempt)
            {
                const std::int64_t dcell = 11000 + static_cast<std::int64_t>(attempt);
                const std::uint64_t d0 = scatterHash(seed_ ^ center.seedMix, dcell, 89);
                const std::uint64_t d1 = scatterHash(seed_ ^ center.seedMix, dcell, 97);
                const std::uint64_t d2 = scatterHash(seed_ ^ center.seedMix, dcell, 101);
                const bool pickBoulder = (hashTo01(d2) < 0.3) && boulderPropIt != tables_->propIndexByKey.end();
                const auto propIt2 = pickBoulder ? boulderPropIt : shrubPropIt;
                const double dAngle = hashTo01(d0) * 6.283185307179586;
                const double dRadius = 16.0 + hashTo01(d1) * 54.0;
                const double dpx = center.position.x + std::cos(dAngle) * dRadius;
                const double dpz = center.position.z + std::sin(dAngle) * dRadius;
                const double dpy = terrain_->heightAt(dpx, dpz);
                if (dpy < SEA_LEVEL_M + 0.5)
                    continue;
                if (slopeAt(*terrain_, dpx, dpz) > 0.45)
                    continue;
                const ScatterProp &dprop = tables_->props[propIt2->second];
                ScatterPlacement dp;
                dp.position     = Position{dpx, dpy, dpz};
                dp.propIndex    = propIt2->second;
                dp.scaleM       = dprop.scaleMinM + hashTo01(scatterHash(seed_ ^ center.seedMix, dcell, 103)) * (dprop.scaleMaxM - dprop.scaleMinM);
                dp.rotationRad  = dAngle;
                dp.isSettlement = true;
                placements_.push_back(dp);
                ++detailPlaced;
            }
        }

        // ---- the coastal dock + boat (rev 31) -------------------------------
        //
        // A COASTAL settlement gets a small pier: 4-5 plank instances
        // stepping down towards the water plus one boat beside the pier end,
        // its keel at the waterline (the transparency layer shows the
        // submerged hull). The shore direction: 8 rays probe for the nearest
        // water within 40 m; no water, no dock (a hill village stays dry).
        if (dockPropIt != tables_->propIndexByKey.end() && boatPropIt != tables_->propIndexByKey.end())
        {
            // The center's climate through the region grid.
            const std::uint32_t crx = static_cast<std::uint32_t>(std::max(0, static_cast<int>(center.position.x / REGION_SIZE_M)));
            const std::uint32_t crz = static_cast<std::uint32_t>(std::max(0, static_cast<int>(center.position.z / REGION_SIZE_M)));
            const regionId cr = world::regionIdOf(crx, crz);
            if (cr < REGION_COUNT && region_climates_[cr] == world::Climate::COASTAL)
            {
                double shoreAngle = 0.0;
                double shoreDist = 0.0;
                bool shoreFound = false;
                for (int ray = 0; ray < 8 && !shoreFound; ++ray)
                {
                    const double a = static_cast<double>(ray) * 0.7853981633974483;
                    for (double d = 12.0; d <= 40.0; d += 4.0)
                    {
                        const double sxp = center.position.x + std::cos(a) * d;
                        const double szp = center.position.z + std::sin(a) * d;
                        if (terrain_->heightAt(sxp, szp) < SEA_LEVEL_M - 0.3)
                        {
                            shoreAngle = a;
                            shoreDist = d;
                            shoreFound = true;
                            break;
                        }
                    }
                }
                if (shoreFound)
                {
                    const double dirX = std::cos(shoreAngle);
                    const double dirZ = std::sin(shoreAngle);
                    // 4 planks from the shore line out over the water, each
                    // stepping 2.4 m along the shore ray, deck dropping 6 cm
                    // per plank (the weathered pier ramp read).
                    for (int plank = 0; plank < 4; ++plank)
                    {
                        const double px = center.position.x + dirX * (shoreDist - 2.0 + plank * 2.4);
                        const double pz = center.position.z + dirZ * (shoreDist - 2.0 + plank * 2.4);
                        const double groundHere = terrain_->heightAt(px, pz);
                        const double deckY = std::max(groundHere, SEA_LEVEL_M - 0.05) + 0.25 - plank * 0.06;
                        ScatterPlacement dp;
                        dp.position     = Position{px, deckY, pz};
                        dp.propIndex    = dockPropIt->second;
                        dp.scaleM       = 1.0;
                        dp.rotationRad  = shoreAngle + 1.5707963267948966; // planks lie across the walk line
                        dp.isSettlement = true;
                        placements_.push_back(dp);
                    }
                    // The boat: beside the pier's far end, keel at the water
                    // plane (SEA_LEVEL - 0.3 + a hair up; the transparency
                    // blend shows the submerged hull below the waterline).
                    const double bx = center.position.x + dirX * (shoreDist + 4.0) + dirZ * 3.0;
                    const double bz = center.position.z + dirZ * (shoreDist + 4.0) - dirX * 3.0;
                    ScatterPlacement bp;
                    bp.position     = Position{bx, SEA_LEVEL_M - 0.24, bz};
                    bp.propIndex    = boatPropIt->second;
                    bp.scaleM       = 1.0;
                    bp.rotationRad  = shoreAngle + 0.9;
                    bp.isSettlement = true;
                    placements_.push_back(bp);
                }
            }
        }
    }

    // ---- the region-walk scatter (vegetation, rocks per climate) ----------

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
                // rev 24: the (prop, instance) pair keys the hash - the first
                // draft hashed (rx*1000+i, rz) for EVERY prop, so shrub i and
                // pine i drew the SAME (u, v) and every prop of one index
                // stacked at one spot (the coast capture's grass sphere under
                // every bare trunk). The prop index mixes the stream; the
                // instance index still spreads within it.
                const std::int64_t propStream = static_cast<std::int64_t>(propIdx) * 2654435761ull;
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    // Draw position + scale + rotation from the seeded hash.
                    // The hash mixes (seed, cellX=rx*N+i, cellZ=rz) so each
                    // (region, prop, instance) gets an independent draw.
                    const std::int64_t cellX = static_cast<std::int64_t>(rx) * 1000 + static_cast<std::int64_t>(i);
                    const std::int64_t cellZ = rz ^ propStream;
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
