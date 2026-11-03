// Task 5.2.2 - Borders implementation.
//
// See borders.hpp for the contract. This file owns:
//   * The water plane (a single flat plane at A-151's sea level).
//   * The border ribbons (rectangular polygons around territory region
//     centers, lifted by A-159, tinted by owner/safe/neutral).
//   * The TerritoryChanged recolor path (no polling — event-driven).
//
// Determinism: the border state is a pure function of (territory tree,
// ownership map). No wall clock, no RNG stream.

#include "bridge/borders.hpp"

#include <algorithm>
#include <cmath>

#include "core/constants.hpp"        // REGION_SIZE_M (A-40)
#include "core/constants_phase5.hpp" // A-151, A-158, A-159

namespace harrowmere::bridge
{

namespace
{

// The faction palette (8 slots by id, mirroring scene_bridge.hpp's
// FACTION_RING_COLORS). Borders use the same palette so owner colors are
// consistent across the faction ring and the border ribbon.
const std::array<std::array<float, 3>, 8> kFactionPalette{{
    {0.90f, 0.30f, 0.30f}, // 1: crimson
    {0.30f, 0.45f, 0.90f}, // 2: cobalt
    {0.25f, 0.70f, 0.35f}, // 3: verdant
    {0.90f, 0.75f, 0.25f}, // 4: gold
    {0.70f, 0.35f, 0.85f}, // 5: violet
    {0.30f, 0.80f, 0.80f}, // 6: cyan
    {0.95f, 0.55f, 0.20f}, // 7: ember
    {0.75f, 0.75f, 0.78f}, // 8: iron
}};

} // namespace

// ---- water ----------------------------------------------------------------

// (WaterPlane is a pure-data struct; no implementation needed beyond the
// defaults in the header.)

// ---- borders --------------------------------------------------------------

BordersField::BordersField()
{
    water_.seaLevelM = SEA_LEVEL_M; // A-151
}

std::array<float, 3> BordersField::resolveTint(
    factionId owner, bool safe
) noexcept
{
    if (safe)
        return BORDER_SAFE_TINT; // A-158 safe zone
    if (owner == 0)
        return BORDER_NEUTRAL_TINT; // A-158 unowned
    // The faction palette wraps by id (ids 1..8 → indices 0..7; ids > 8 wrap).
    const std::size_t idx = (static_cast<std::size_t>(owner) - 1) % kFactionPalette.size();
    return kFactionPalette[idx];
}

void BordersField::upsertRibbon(
    const BorderRibbon &ribbon
)
{
    BorderRibbon r = ribbon;
    r.tint         = resolveTint(r.owner, r.safe);
    r.doubleRibbon = r.safe; // A-158: safe zones get the double-ribbon style
    ribbons_[r.id] = std::move(r);
}

void BordersField::recolorOnTerritoryChanged(
    territoryId id, factionId newOwner
)
{
    auto it = ribbons_.find(id);
    if (it == ribbons_.end())
        return;
    it->second.owner = newOwner;
    it->second.tint  = resolveTint(newOwner, it->second.safe);
    // The double-ribbon flag depends only on the safe flag, which does not
    // change on a TerritoryChanged event (safe is a territory-template
    // property, not an ownership property).
}

std::vector<Position> regionBoxPolygon(
    Position center, double halfExtentM
) noexcept
{
    // A rectangular polygon (4 corners) around the center, CCW.
    return {
        Position{center.x - halfExtentM, 0.0, center.z - halfExtentM},
        Position{center.x + halfExtentM, 0.0, center.z - halfExtentM},
        Position{center.x + halfExtentM, 0.0, center.z + halfExtentM},
        Position{center.x - halfExtentM, 0.0, center.z + halfExtentM},
    };
}

} // namespace harrowmere::bridge
