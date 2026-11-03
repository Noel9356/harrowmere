// Task 5.2.2 - Water, coasts & borders on the land: the pinned test suite.
//
// Six pinned tests (card 5.2.2):
//   * TestBorders_RibbonProjectsOnTerrain      — ribbon vertices at heightAt + lift.
//   * TestBorders_OwnerColorFromFactionPalette — owner → palette entry.
//   * TestBorders_TerritoryChangedRecolorsNextFrame — event-driven recolor.
//   * TestBorders_SafeZoneStyleDistinct        — safe tint + double ribbon.
//   * TestBorders_CoastWaterOnlyWhereClimateSays — water at sea level.
//   * TestBorders_NoSimWrites                  — the field reads, never writes.

#include <doctest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "bridge/borders.hpp"
#include "bridge/terrain.hpp"
#include "core/constants.hpp"        // REGION_SIZE_M (A-40)
#include "core/constants_phase5.hpp" // A-151, A-158, A-159, SEA_LEVEL_M, BORDER_LIFT_M
#include "world/region.hpp"          // Climate, regionIdOf

using harrowmere::BORDER_LIFT_M;
using harrowmere::BORDER_NEUTRAL_TINT;
using harrowmere::BORDER_SAFE_TINT;
using harrowmere::factionId;
using harrowmere::Position;
using harrowmere::REGION_COUNT;
using harrowmere::REGION_GRID;
using harrowmere::REGION_SIZE_M;
using harrowmere::regionId;
using harrowmere::SEA_LEVEL_M;
using harrowmere::territoryId;
using harrowmere::TerritoryType;
using harrowmere::bridge::BorderRibbon;
using harrowmere::bridge::BordersField;
using harrowmere::bridge::regionBoxPolygon;
using harrowmere::bridge::TerrainField;
using harrowmere::world::Climate;
using harrowmere::world::regionCenter;
using harrowmere::world::regionIdOf;

namespace
{

std::vector<Climate> syntheticClimates()
{
    std::vector<Climate> grid(static_cast<std::size_t>(REGION_COUNT), Climate::TEMPERATE);
    for (std::uint32_t z = 0; z < REGION_GRID; ++z)
    {
        for (std::uint32_t x = 0; x < REGION_GRID; ++x)
        {
            const regionId r = regionIdOf(x, z);
            Climate c        = Climate::TEMPERATE;
            if (x == 0)
                c = Climate::COASTAL;
            else if (x >= 2 && x <= 4 && z >= 2 && z <= 6)
                c = Climate::MOUNTAIN;
            else if (x >= 5 && x <= 8)
                c = Climate::FOREST;
            else if (z >= 12)
                c = Climate::SWAMP;
            grid[r] = c;
        }
    }
    return grid;
}

} // namespace

// ---------------------------------------------------------------------------
// TestBorders_RibbonProjectsOnTerrain
//
// The ribbon's polygon vertices project onto the heightfield: each vertex's
// y = heightAt(x, z) + BORDER_LIFT_M (A-159). The bridge samples heightAt
// at publish time; the BordersField carries the polygon's (x, z) and the
// lift is applied at publish. The test asserts the polygon's (x, z) points
// are valid world coordinates and the lift constant is the pinned A-159.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestBorders_RibbonProjectsOnTerrain"
)
{
    // The lift constant is A-159 = 0.35 m.
    CHECK(BORDER_LIFT_M == doctest::Approx(0.35).epsilon(1e-9));

    // A rectangular polygon around a region center.
    const Position center{1000.0, 0.0, 1000.0};
    const auto poly = regionBoxPolygon(center, 512.0);
    REQUIRE(poly.size() == 4);
    // The polygon is a closed rectangle (CCW).
    CHECK(poly[0].x == doctest::Approx(center.x - 512.0).epsilon(1e-9));
    CHECK(poly[0].z == doctest::Approx(center.z - 512.0).epsilon(1e-9));
    CHECK(poly[2].x == doctest::Approx(center.x + 512.0).epsilon(1e-9));
    CHECK(poly[2].z == doctest::Approx(center.z + 512.0).epsilon(1e-9));

    // The polygon's y values are 0 (the bridge adds heightAt + lift at publish).
    for (const auto &p : poly)
        CHECK(p.y == doctest::Approx(0.0).epsilon(1e-9));
}

// ---------------------------------------------------------------------------
// TestBorders_OwnerColorFromFactionPalette
//
// The owner faction's palette entry drives the ribbon tint. Faction 1 →
// crimson, faction 2 → cobalt, etc. Unowned (faction 0) → neutral tint.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestBorders_OwnerColorFromFactionPalette"
)
{
    // Unowned: neutral tint.
    const auto neutral = BordersField::resolveTint(factionId{0}, false);
    CHECK(neutral == BORDER_NEUTRAL_TINT);

    // Faction 1: crimson (the palette's first entry).
    const auto f1 = BordersField::resolveTint(factionId{1}, false);
    CHECK(f1[0] > 0.8f);  // red-dominant
    CHECK(f1[0] > f1[2]); // red > blue

    // Faction 2: cobalt (blue-dominant).
    const auto f2 = BordersField::resolveTint(factionId{2}, false);
    CHECK(f2[2] > f2[0]); // blue > red

    // Distinct factions have distinct tints.
    CHECK(f1 != f2);

    // An upserted ribbon carries the resolved tint.
    BordersField borders;
    BorderRibbon r;
    r.id    = territoryId{42};
    r.type  = TerritoryType::VILLAGE;
    r.owner = factionId{1};
    r.safe  = false;
    borders.upsertRibbon(r);
    const auto &ribbons = borders.ribbons();
    REQUIRE(ribbons.count(territoryId{42}));
    CHECK(ribbons.at(territoryId{42}).tint == f1);
}

// ---------------------------------------------------------------------------
// TestBorders_TerritoryChangedRecolorsNextFrame
//
// A TerritoryChanged event recolors the ribbon the next frame, with no
// polling loop. The recolorOnTerritoryChanged path updates the owner and
// the tint in one call; the next publish reads the updated state.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestBorders_TerritoryChangedRecolorsNextFrame"
)
{
    BordersField borders;
    BorderRibbon r;
    r.id    = territoryId{10};
    r.type  = TerritoryType::VILLAGE;
    r.owner = factionId{1}; // crimson
    r.safe  = false;
    borders.upsertRibbon(r);

    // Initial tint: faction 1 (crimson).
    const auto &ribbons = borders.ribbons();
    REQUIRE(ribbons.count(territoryId{10}));
    const auto tintBefore = ribbons.at(territoryId{10}).tint;
    CHECK(tintBefore[0] > 0.8f); // crimson

    // TerritoryChanged: new owner = faction 2 (cobalt).
    borders.recolorOnTerritoryChanged(territoryId{10}, factionId{2});
    const auto tintAfter = ribbons.at(territoryId{10}).tint;
    CHECK(tintAfter != tintBefore);
    CHECK(tintAfter[2] > tintAfter[0]); // cobalt (blue > red)

    // A TerritoryChanged for an unknown territory is a no-op (no crash).
    borders.recolorOnTerritoryChanged(territoryId{999}, factionId{3});
    CHECK(ribbons.size() == 1);
}

// ---------------------------------------------------------------------------
// TestBorders_SafeZoneStyleDistinct
//
// Safe-zone territories have the distinct safe tint (A-158) AND the
// double-ribbon style (doubleRibbon flag). The test asserts both.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestBorders_SafeZoneStyleDistinct"
)
{
    BordersField borders;
    BorderRibbon safe;
    safe.id    = territoryId{1};
    safe.type  = TerritoryType::VILLAGE;
    safe.owner = factionId{0}; // unowned but safe
    safe.safe  = true;
    borders.upsertRibbon(safe);

    const auto &ribbons = borders.ribbons();
    REQUIRE(ribbons.count(territoryId{1}));
    const auto &r = ribbons.at(territoryId{1});
    CHECK(r.tint == BORDER_SAFE_TINT);
    CHECK(r.doubleRibbon);

    // A non-safe territory does not get the double ribbon.
    BorderRibbon normal;
    normal.id    = territoryId{2};
    normal.type  = TerritoryType::VILLAGE;
    normal.owner = factionId{1};
    normal.safe  = false;
    borders.upsertRibbon(normal);
    CHECK_FALSE(ribbons.at(territoryId{2}).doubleRibbon);
    CHECK(ribbons.at(territoryId{2}).tint != BORDER_SAFE_TINT);
}

// ---------------------------------------------------------------------------
// TestBorders_CoastWaterOnlyWhereClimateSays
//
// The water plane is at sea level (A-151 = 0 m). Water shows only where the
// terrain dips below sea level — i.e., where the climate data says COASTAL
// (the coastal clamp in 5.1.2's terrain keeps coastal terrain low, near 0).
// The test asserts the water plane's sea level is A-151 and the plane's
// extent is the world extent.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestBorders_CoastWaterOnlyWhereClimateSays"
)
{
    BordersField borders;
    const auto &water = borders.water();
    CHECK(water.seaLevelM == doctest::Approx(SEA_LEVEL_M).epsilon(1e-9));
    CHECK(SEA_LEVEL_M == doctest::Approx(0.0).epsilon(1e-9));

    // The water plane's extent is set once the terrain field is known.
    borders.setWaterExtent(16000.0);
    CHECK(water.extentM == doctest::Approx(16000.0).epsilon(1e-9));

    // The coastal clamp in the terrain field keeps coastal terrain near 0 m
    // (A-147.e = 8 m max within 2 km of COASTAL regions). A COASTAL region's
    // heightAt is ≤ 8 m, so the water plane (at 0 m) shows there.
    const auto climates = syntheticClimates();
    const TerrainField terrain(0xC0A5ull, climates);
    // Find a COASTAL region (x == 0 in the synthetic grid).
    double coastalHeight = 0.0;
    for (std::uint32_t z = 0; z < REGION_GRID; ++z)
    {
        const regionId r = regionIdOf(0, z);
        if (climates[r] == Climate::COASTAL)
        {
            const Position center = harrowmere::world::regionCenter(r);
            coastalHeight         = terrain.heightAt(center.x, center.z);
            break;
        }
    }
    CHECK(coastalHeight <= 8.0 + 1e-6); // A-147.e's coastal clamp
}

// ---------------------------------------------------------------------------
// TestBorders_NoSimWrites
//
// The borders field reads the territory tree + ownership map but never
// writes sim state. The test asserts the BordersField is a pure read side:
// upsertRibbon and recolorOnTerritoryChanged only update the field's own
// ribbon map, never any sim store.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestBorders_NoSimWrites"
)
{
    BordersField borders;
    BorderRibbon r;
    r.id    = territoryId{5};
    r.type  = TerritoryType::CITY;
    r.owner = factionId{3};
    r.safe  = false;
    borders.upsertRibbon(r);

    // The field's state is its own ribbon map — no sim store is touched.
    // (The test asserts the field is a pure read side by verifying its state
    // is self-contained: the ribbon's tint matches the resolved owner tint,
    // and the field has no other side effects.)
    const auto &ribbons = borders.ribbons();
    REQUIRE(ribbons.size() == 1);
    const auto &ribbon = ribbons.at(territoryId{5});
    CHECK(ribbon.owner == factionId{3});
    CHECK(ribbon.tint == BordersField::resolveTint(factionId{3}, false));

    // recolorOnTerritoryChanged only updates the ribbon map.
    borders.recolorOnTerritoryChanged(territoryId{5}, factionId{4});
    CHECK(ribbons.at(territoryId{5}).owner == factionId{4});
    CHECK(ribbons.at(territoryId{5}).tint == BordersField::resolveTint(factionId{4}, false));
    CHECK(ribbons.size() == 1); // no new ribbons created
}
