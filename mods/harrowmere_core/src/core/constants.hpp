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

} // namespace harrowmere
