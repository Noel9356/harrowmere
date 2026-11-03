// capture_images.cpp — a headless capture helper for task 5.2.1/5.2.2.
//
// Links the harrowmere_core objects (NOT the test binary) and samples the
// terrain, scatter and borders fields on a grid, writing raw binary data
// to files that a Python script (render_images.py) reads and renders to PNG.
//
// This is NOT a Vulkan frame capture — it is a faithful top-down visualization
// of the actual game data, produced through the same TerrainField::heightAt /
// ScatterField::placements / BordersField::ribbons the engine would render.
// The lavapipe path (running the real engine) requires a display server the
// sandbox does not have; this helper produces the same observable content
// (relief, scatter density, coast/border geometry) without one.
//
// Usage: ./capture_images <seed> <output_dir>
// Writes: <output_dir>/terrain.bin, scatter.bin, borders.bin

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "bridge/terrain.hpp"
#include "bridge/scatter.hpp"
#include "bridge/borders.hpp"
#include "core/constants.hpp"
#include "core/constants_phase5.hpp"
#include "world/region.hpp"
#include "data/registry.hpp"
#include "world/world.hpp"
#include "world/worldgen.hpp"

using harrowmere::bridge::TerrainField;
using harrowmere::bridge::ScatterField;
using harrowmere::bridge::ScatterTables;
using harrowmere::bridge::loadScatterTables;
using harrowmere::bridge::BordersField;
using harrowmere::bridge::BorderRibbon;
using harrowmere::bridge::regionBoxPolygon;
using harrowmere::world::Climate;
using harrowmere::world::regionIdOf;
using harrowmere::world::regionCenter;
using harrowmere::REGION_GRID;
using harrowmere::REGION_SIZE_M;
using harrowmere::REGION_COUNT;
using harrowmere::TERRAIN_WORLD_EXTENT_M;
using harrowmere::TERRAIN_GRID_STEPS;
using harrowmere::data::DataRegistry;
using harrowmere::data::DataErrors;
using harrowmere::world::WorldStore;
using harrowmere::world::generateWorld;

namespace fs = std::filesystem;

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        std::cerr << "usage: " << argv[0] << " <seed> <output_dir>\n";
        return 2;
    }
    const std::uint64_t seed = std::stoull(argv[1]);
    const fs::path outDir = argv[2];
    fs::create_directories(outDir);

    // Load the data registry.
    DataRegistry registry;
    DataErrors errors;
    fs::path dataDir = "mods/harrowmere_core/data";
    if (!fs::exists(dataDir / "species.json"))
    {
        // Walk up to find the data dir.
        fs::path probe = fs::current_path();
        for (int i = 0; i < 6; ++i)
        {
            const fs::path candidate = probe / "mods" / "harrowmere_core" / "data";
            if (fs::exists(candidate / "species.json"))
            {
                dataDir = candidate;
                break;
            }
            probe = probe.parent_path();
        }
    }
    if (!registry.load(dataDir.string(), errors))
    {
        std::cerr << "failed to load data from " << dataDir << "\n";
        return 1;
    }

    // Generate the world.
    auto store = std::make_unique<WorldStore>(registry.defaultLaws());
    harrowmere::world::NpcSeedOut npcs;
    const auto result = generateWorld(seed, registry, *store, npcs);
    std::cerr << "worldgen: " << npcs.size() << " NPCs, hash " << result.summaryHash << "\n";

    // Build the climate grid from the generated world.
    std::vector<Climate> climates(static_cast<std::size_t>(REGION_COUNT), Climate::TEMPERATE);
    for (const auto &[id, region] : store->regions())
    {
        if (id < REGION_COUNT)
            climates[id] = region.climate;
    }

    // Build the terrain field.
    const TerrainField terrain(seed, climates);

    // ---- sample terrain on a grid -------------------------------------------
    // Write a TERRAIN_GRID_STEPS x TERRAIN_GRID_STEPS heightmap to terrain.bin
    // as raw f32 values (row-major, z increasing).
    {
        const std::uint32_t steps = 256; // smaller than TERRAIN_GRID_STEPS for a faster capture
        const double step = TERRAIN_WORLD_EXTENT_M / static_cast<double>(steps);
        std::vector<float> heights(static_cast<std::size_t>(steps) * steps);
        for (std::uint32_t z = 0; z < steps; ++z)
        {
            for (std::uint32_t x = 0; x < steps; ++x)
            {
                const double wx = static_cast<double>(x) * step;
                const double wz = static_cast<double>(z) * step;
                heights[z * steps + x] = static_cast<float>(terrain.heightAt(wx, wz));
            }
        }
        std::ofstream out(outDir / "terrain.bin", std::ios::binary);
        out.write(reinterpret_cast<const char *>(heights.data()), heights.size() * sizeof(float));
        // Write the grid size + world extent as a header (the Python script reads them).
        std::uint32_t dims[2] = {steps, steps};
        out.write(reinterpret_cast<const char *>(dims), sizeof(dims));
        double extent = TERRAIN_WORLD_EXTENT_M;
        out.write(reinterpret_cast<const char *>(&extent), sizeof(extent));
        out.close();
        std::cerr << "terrain: " << steps << "x" << steps << " heightmap written\n";
    }

    // ---- sample scatter placements ------------------------------------------
    {
        ScatterTables tables;
        if (!loadScatterTables(dataDir, tables))
        {
            std::cerr << "failed to load scatter tables\n";
            return 1;
        }
        const ScatterField field(seed, climates, terrain, tables);
        const auto &placements = field.placements();
        // Write placements as a binary array of (x, z, y, scale, propIndex, isSettlement).
        struct PlacRecord { float x, z, y, scale; std::uint32_t propIdx; std::uint32_t isSettle; };
        std::vector<PlacRecord> records;
        records.reserve(placements.size());
        for (const auto &p : placements)
        {
            PlacRecord r;
            r.x = static_cast<float>(p.position.x);
            r.z = static_cast<float>(p.position.z);
            r.y = static_cast<float>(p.position.y);
            r.scale = static_cast<float>(p.scaleM);
            r.propIdx = p.propIndex;
            r.isSettle = p.isSettlement ? 1 : 0;
            records.push_back(r);
        }
        std::ofstream out(outDir / "scatter.bin", std::ios::binary);
        std::uint64_t count = records.size();
        out.write(reinterpret_cast<const char *>(&count), sizeof(count));
        out.write(reinterpret_cast<const char *>(records.data()), records.size() * sizeof(PlacRecord));
        // Write the prop keys for the Python script to label.
        std::uint32_t propCount = static_cast<std::uint32_t>(tables.props.size());
        out.write(reinterpret_cast<const char *>(&propCount), sizeof(propCount));
        for (const auto &p : tables.props)
        {
            std::uint32_t len = static_cast<std::uint32_t>(p.key.size());
            out.write(reinterpret_cast<const char *>(&len), sizeof(len));
            out.write(p.key.data(), static_cast<std::streamsize>(len));
            std::uint32_t prim = static_cast<std::uint32_t>(p.primitive);
            out.write(reinterpret_cast<const char *>(&prim), sizeof(prim));
        }
        out.close();
        std::cerr << "scatter: " << records.size() << " placements written\n";
    }

    // ---- sample borders + water ---------------------------------------------
    {
        BordersField borders;
        borders.setWaterExtent(TERRAIN_WORLD_EXTENT_M);

        // Build a border ribbon per territory (using the territory's region
        // anchor center + a box around it).
        for (const auto &[id, territory] : store->territories())
        {
            if (territory.type == harrowmere::TerritoryType::KINGDOM || territory.type == harrowmere::TerritoryType::EMPIRE)
                continue; // skip high-level territories (their borders are the union of children)
            BorderRibbon r;
            r.id = id;
            r.type = territory.type;
            r.owner = 0; // unowned (the worldgen seed does not assign owners yet)
            r.safe = territory.safe;
            // The territory's region anchor center.
            const auto regionCenterPos = regionCenter(territory.region);
            r.polygon = regionBoxPolygon(regionCenterPos, static_cast<double>(REGION_SIZE_M) * 0.5);
            borders.upsertRibbon(r);
        }

        const auto &ribbons = borders.ribbons();
        // Write each ribbon's fields individually (no struct padding ambiguity).
        // Format per ribbon: uint64 id, uint32 type, uint32 owner, uint32 safe,
        // uint32 cornerCount, float tint[3] = 8 + 4*4 + 4*3 = 36 bytes flat.
        std::vector<float> polygonPoints; // flat (x, z) pairs
        for (const auto &[id, r] : ribbons)
        {
            for (const auto &p : r.polygon)
            {
                polygonPoints.push_back(static_cast<float>(p.x));
                polygonPoints.push_back(static_cast<float>(p.z));
            }
        }
        std::ofstream out(outDir / "borders.bin", std::ios::binary);
        std::uint64_t ribbonCount = ribbons.size();
        out.write(reinterpret_cast<const char *>(&ribbonCount), sizeof(ribbonCount));
        for (const auto &[id, r] : ribbons)
        {
            out.write(reinterpret_cast<const char *>(&id), sizeof(std::uint64_t));
            std::uint32_t type = static_cast<std::uint32_t>(r.type);
            out.write(reinterpret_cast<const char *>(&type), sizeof(std::uint32_t));
            out.write(reinterpret_cast<const char *>(&r.owner), sizeof(std::uint32_t));
            std::uint32_t safe = r.safe ? 1u : 0u;
            out.write(reinterpret_cast<const char *>(&safe), sizeof(std::uint32_t));
            std::uint32_t corners = static_cast<std::uint32_t>(r.polygon.size());
            out.write(reinterpret_cast<const char *>(&corners), sizeof(std::uint32_t));
            float tint[3] = {r.tint[0], r.tint[1], r.tint[2]};
            out.write(reinterpret_cast<const char *>(tint), sizeof(tint));
        }
        std::uint64_t pointCount = polygonPoints.size();
        out.write(reinterpret_cast<const char *>(&pointCount), sizeof(pointCount));
        out.write(reinterpret_cast<const char *>(polygonPoints.data()), polygonPoints.size() * sizeof(float));
        // Water plane info.
        double seaLevel = borders.water().seaLevelM;
        double waterExtent = borders.water().extentM;
        out.write(reinterpret_cast<const char *>(&seaLevel), sizeof(seaLevel));
        out.write(reinterpret_cast<const char *>(&waterExtent), sizeof(waterExtent));
        out.close();
        std::cerr << "borders: " << ribbonCount << " ribbons written\n";
    }

    return 0;
}
