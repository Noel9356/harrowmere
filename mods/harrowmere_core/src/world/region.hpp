#ifndef HARROWMERE_CORE_WORLD_REGION_HPP
#define HARROWMERE_CORE_WORLD_REGION_HPP

// Region — the spatial map cell of the world model (task 1.4; design
// reference §World). A region holds the land's long-term state: consequence
// flags, ambient mana richness, and the season/weather overlay.
//
// Region vs territory (Appendix C glossary): a region is a spatial cell — a
// territory is the LEGAL hierarchy node (village ⊂ city ⊂ kingdom ⊂ empire)
// that laws, titles and safety attach to. A region has no laws; a territory
// has no weather. The region -> territory mapping is stored explicitly at
// generation (WorldStore::setRegionTerritory), so runtime lookups are one
// map hit — no geometric recompute (card 1.4).
//
// D-WORLD-1 seam (flat v1 plane): the position -> region projection below is
// the ONE place world coordinates become region indices. All geometry routes
// through these helpers — never raw coordinate math — so a spherical
// projection can replace the flat grid later by touching exactly this file
// (card 1.5's pinned decision; registered in Appendix E with re-entry
// points: worldgen region topology + bridge sky rendering).

#include <cstdint>

#include "core/constants.hpp" // REGION_GRID / REGION_SIZE_M / REGION_COUNT (A-40)
#include "core/types.hpp"     // regionId, Position (task 0.2)

namespace harrowmere::world
{

// Sentinel regionId: "no region" (a position outside the world plane). 0 is
// NOT usable as the sentinel — worldgen assigns regions 0..REGION_COUNT-1.
// Id-space sentinel next to its type, like core/types.hpp's FIRST_ID; not a
// game-rule value, so it claims no Appendix A row.
inline constexpr regionId kNoRegion = 0xFFFF;

// ---- SeasonState / WeatherState ---------------------------------------------
//
// Declared HERE (card 1.4: "owned by task 3.8, declared here"): the world
// model owns the storage from day one so Region is complete, while the state
// machines that advance them are task 3.8's. Both stamp the worldTime
// (game seconds, task 0.3's WorldClock) their current state began, so 3.8's
// machines and the UI can render "how long has this been going on" without
// re-deriving it.
struct SeasonState
{
    std::uint32_t index              = 0; // season ordinal (3.8's data-driven season list)
    std::uint64_t startedAtWorldTime = 0; // worldTime when this season began
};

struct WeatherState
{
    std::uint32_t kind           = 0; // weather kind ordinal (3.8's state machine)
    std::uint64_t sinceWorldTime = 0; // worldTime when this weather began
};

// Region — design reference §World, fields verbatim (card 1.4).
struct Region
{
    regionId id = 0;

    // Bit flags with meanings declared in data (design rule: "Flags ... are
    // stored by the core system and defined in data files") — the shipped
    // table is territory_templates.json's regionFlags (raided/blessed/
    // blighted/war_torn). Events and war mutate them (tasks 5.5, 6.2); they
    // persist long-term consequences.
    std::uint32_t flags = 0;

    // Ambient mana of the land, 0..SCALE. D-LORE-3: the whole planet lies
    // inside the mana-bearing region, so every region draws 9500..10000 at
    // worldgen (task 1.5); the field is static after generation in v1.
    std::uint32_t manaRichness = 0;

    // The regional weather/season overlay (task 3.8 advances them).
    SeasonState season{};
    WeatherState weather{};
};

// ---- the D-WORLD-1 projection helpers ---------------------------------------

// The region-id layout (pinned here with the projection, so worldgen, the
// spatial index (1.6) and the save schema (7.1) all derive it from one
// place): regionId = zIndex * REGION_GRID + xIndex — x fastest.
inline constexpr regionId regionIdOf(
    std::uint32_t xIndex, std::uint32_t zIndex
) noexcept
{
    return static_cast<regionId>(zIndex * REGION_GRID + xIndex);
}

// Is (xIndex, zIndex) inside the world plane?
inline constexpr bool regionIndicesInGrid(
    std::int64_t xIndex, std::int64_t zIndex
) noexcept
{
    return xIndex >= 0 && xIndex < static_cast<std::int64_t>(REGION_GRID) && zIndex >= 0
        && zIndex < static_cast<std::int64_t>(REGION_GRID);
}

// The region containing world position `p`, or kNoRegion when p lies outside
// the world plane ([0, REGION_GRID * REGION_SIZE_M) on x and z — the grid is
// 2D: y is cosmetic (card 1.6). The projection is pure integer math on the
// position's integral part: for in-grid (non-negative) coordinates,
// truncation equals floor, so there is no float edge case at region borders
// (R5: f64 stays in Position; no float arithmetic enters the result).
inline regionId regionIdAt(
    const Position &p
) noexcept
{
    if (p.x < 0.0 || p.z < 0.0)
        return kNoRegion;
    const auto x      = static_cast<long long>(p.x);
    const auto z      = static_cast<long long>(p.z);
    const auto xIndex = x / static_cast<long long>(REGION_SIZE_M);
    const auto zIndex = z / static_cast<long long>(REGION_SIZE_M);
    if (!regionIndicesInGrid(xIndex, zIndex))
        return kNoRegion;
    return regionIdOf(static_cast<std::uint32_t>(xIndex), static_cast<std::uint32_t>(zIndex));
}

// True when `id` names one of the grid's regions.
inline constexpr bool isGridRegion(
    regionId id
) noexcept
{
    return id < static_cast<regionId>(REGION_COUNT);
}

} // namespace harrowmere::world

#endif // HARROWMERE_CORE_WORLD_REGION_HPP
