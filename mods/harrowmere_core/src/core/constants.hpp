// constants.hpp - the single home of every numeric constant in the game
// (task 0.2; common.md Appendix A header).
//
// Contract (section 1.3, quoted): no number may appear in game code unless
// it is either loaded from JSON data or named here. Every constant below
// carries its Appendix A row reference; the file is APPEND-ONLY - later
// tasks add rows, none remove or renumber one.
#pragma once

#include <cstdint>

namespace harrowmere
{

// A-1: SCALE = 10'000. The fixed-point grid (section 3.2). A probability of
// 0.37 is stored as 3700; a multiplier of 1.5x is stored as 15000. Design
// rule: "Data can be non-integer but will be scaled and casted into integer
// to be used." (design reference section Conventions.)
inline constexpr std::uint64_t SCALE = 10'000ull;

// The u32 alias the data loaders use: every SCALE-grid value stored on the
// loader structs (CrimeData, LawSet, RoleData, TerritoryTemplate fields) is
// in [0, SCALE] and fits u32. One alias so the four data headers never
// redefine it.
inline constexpr std::uint32_t kScale = static_cast<std::uint32_t>(SCALE);

// A-2: MAX_XP = 10^15. Far above any reachable lifetime XP; keeps i64
// headroom for levelCosts x factors. applyXpDelta (math.hpp) clamps
// availableXp at exactly -MAX_XP (R4); totalXp never decreases.
inline constexpr std::int64_t MAX_XP = 1'000'000'000'000'000ll;

// A-40: REGION_GRID x REGION_GRID regions of REGION_SIZE_M metres on edge —
// the 16x16 region grid (256 regions, ~4 km^2 world; task 1.4's territoryAt
// projection and task 1.5's worldgen region pass). The world plane spans
// [0, REGION_GRID * REGION_SIZE_M) on both axes; positions outside it have
// no region (src/world/region.hpp's kNoRegion) and no territory.
inline constexpr std::uint32_t REGION_GRID   = 16;
inline constexpr std::uint32_t REGION_SIZE_M = 1024;
inline constexpr std::uint32_t REGION_COUNT  = REGION_GRID * REGION_GRID;

// A-44: ADULT_YEARS = 16. The species-agnostic adult threshold (tasks 3.4
// and 3.9): children never testify and never hold non-child roles. Task
// 1.5's population plan draws adult ages from [ADULT_YEARS, species
// lifespan] and clamps the `child` role under it.
inline constexpr std::uint32_t ADULT_YEARS = 16;

} // namespace harrowmere
