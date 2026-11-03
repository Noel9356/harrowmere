// Task 5.2.2 - Water, coasts & borders on the land.
//
// The map's political and geographic truth is visible ON the terrain: water
// where the climate data says coast, territory borders drawn on the land,
// safe zones styled, and every border redrawn when ownership changes.
//
// What ships here:
//   * WaterPlane — the single sea-level plane (A-151 = 0 m) clipped against
//     terrain. Coasts read as coasts where the terrain dips below sea level.
//   * BorderRibbon — one territory's border as a ribbon mesh projected onto
//     the heightfield, lifted by A-159 (0.35 m) to avoid z-fighting. Color
//     from the owner faction's palette (or the neutral tint if unowned; the
//     safe-zone tint + double-ribbon style if safe).
//   * BordersField — the per-frame border state: builds ribbons from the
//     territory tree + faction ownership, consumes TerritoryChanged events
//     to recolor ribbons the next frame (no polling loop), publishes the
//     water plane + ribbons through the scene sink.
//
// What does NOT ship here:
//   * The territory polygon data (1.5.1's WorldStore owns it; this module
//     reads the territory tree's region anchors + safe flag + owner).
//   * The faction palette (3.1.5's FactionStore owns it; this module reads
//     the owner's palette entry).
//   * Movement/navigation effects (borders never alter movement — the card:
//     "Borders never alter movement or navigation (presentation only,
//     D-IMG-1's rule extended)").
//
// Determinism: the border state is a pure function of (territory tree,
// ownership map, faction palette). No wall clock, no RNG stream. Redraws
// are event-driven (TerritoryChanged), not per-frame polling (the card's
// pitfall: "War recolors must come from EVENTS, not from re-reading the
// faction store per frame per territory (that is the polling loop the
// publish step forbids)").

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include "core/types.hpp"            // Position, territoryId, factionId (task 0.2)
#include "core/enums.hpp"            // TerritoryType (task 1.1)
#include "core/constants_phase5.hpp" // A-151, A-158, A-159, SEA_LEVEL_M, BORDER_LIFT_M

namespace harrowmere::bridge
{

class TerrainField; // forward (5.1.2)

// ---- water ----------------------------------------------------------------

// The water plane: a single flat plane at y = SEA_LEVEL_M (A-151 = 0 m).
// The plane spans the world extent; the renderer clips it against the
// terrain so water shows only where the terrain dips below sea level
// (coasts read as coasts). The water plane is published once per frame;
// its tint shifts with the weather (5.1.4's atmosphere can raise fog over
// water, but the water itself is a constant tint at 1.0).
struct WaterPlane
{
    double seaLevelM          = SEA_LEVEL_M;           // A-151
    double extentM            = 0.0;                   // world extent (TERRAIN_WORLD_EXTENT_M)
    std::array<float, 3> tint = {0.15f, 0.25f, 0.40f}; // deep water blue
};

// ---- borders --------------------------------------------------------------

// One territory's border ribbon. Pure data — the bridge publishes this as a
// ribbon mesh through the scene sink.
struct BorderRibbon
{
    territoryId id     = 0;
    TerritoryType type = TerritoryType::VILLAGE;
    // The ribbon's polygon: a closed loop of world-space (x, z) points
    // around the territory's region footprint. The y is sampled from
    // heightAt at publish time + BORDER_LIFT_M (A-159).
    std::vector<Position> polygon{};
    // The owner faction (0 = unowned → neutral tint).
    factionId owner = 0;
    bool safe       = false;
    // The tint (resolved from the owner's palette or the neutral/safe tint).
    std::array<float, 3> tint = {0.55f, 0.50f, 0.45f};
    // True iff this is a safe-zone territory (double-ribbon style, A-158).
    bool doubleRibbon = false;
};

// The per-frame border state. Built from the territory tree + ownership map;
// recolored on TerritoryChanged events (no polling).
class BordersField
{
public:
    BordersField();

    // Set the terrain field (for heightAt on ribbon vertices). Required
    // before publish — ribbons project onto the heightfield.
    void setTerrain(
        const TerrainField *terrain
    ) noexcept
    {
        terrain_ = terrain;
    }

    // Add or update a border ribbon from a territory descriptor. The ribbon's
    // polygon is built from the territory's region footprint (the territory's
    // region anchor center + a box around it at REGION_SIZE_M). The tint is
    // resolved from the owner faction's palette (or the neutral/safe tint).
    void upsertRibbon(const BorderRibbon &ribbon);

    // Recolor a ribbon on a TerritoryChanged event (the card: "a changed
    // owner recolors the ribbon the next frame, with no polling loop").
    // Called from the bridge's TerritoryChanged subscriber.
    void recolorOnTerritoryChanged(territoryId id, factionId newOwner);

    // The water plane + all ribbons. Pure reads — the bridge publishes these.
    const WaterPlane &water() const noexcept
    {
        return water_;
    }
    const std::map<territoryId, BorderRibbon> &ribbons() const noexcept
    {
        return ribbons_;
    }

    // The water plane's extent (set once the terrain field is known).
    void setWaterExtent(
        double extentM
    ) noexcept
    {
        water_.extentM = extentM;
    }

    // Resolve a border tint: owner faction's palette entry, or the neutral
    // tint if unowned, or the safe tint if safe. Exposed for the test.
    static std::array<float, 3> resolveTint(factionId owner, bool safe) noexcept;

private:
    WaterPlane water_;
    std::map<territoryId, BorderRibbon> ribbons_;
    const TerrainField *terrain_ = nullptr;
};

// Build a rectangular polygon around a region center (the territory's
// footprint at REGION_SIZE_M). Used by the upsertRibbon path when the
// territory has a single region; multi-region territories extend the
// polygon in a later card.
std::vector<Position> regionBoxPolygon(Position center, double halfExtentM) noexcept;

} // namespace harrowmere::bridge
