// constants.hpp — the single home of every numeric constant in the game.
//
// ============================================================================
//  TASK 0.2 STUB — TO BE OVERWRITTEN WHEN TASK 0.2 LANDS
// ============================================================================
//  This header is a faithful stub of the constants registry that task 0.2
//  owns (phase-1-walk.md, card 0.2). Task 1.2's data headers include it for
//  `SCALE` (Appendix A row A-1) — the fixed-point grid every numeric game
//  value lives on.
//
//  The full registry is filled by later tasks — the file is **append-only**
//  (common.md task 0.2). Every constant row carries an `// A-nn` Appendix A
//  reference comment, or the value is loaded from JSON (common.md §1.3).
//
//  When task 0.2 lands, REPLACE this file in full. The two values below
//  (SCALE and MAX_XP) MUST match task 0.2's definitions exactly.
// ============================================================================
#pragma once

#include <cstdint>

namespace harrowmere {

// A-1: SCALE = 10'000. The fixed-point grid. A probability of 0.37 is
// stored as 3700; a multiplier of 1.5× is stored as 15000. Design rule:
// "Data can be non-integer but will be scaled and casted into integer to
// be used." (design reference §Conventions / common.md §3.2 R5.)
inline constexpr std::uint64_t SCALE = 10'000ull;

// u32 alias used by the data loaders — the SCALE-grid values stored on
// CrimeData / LawSet / RoleData / TerritoryTemplate fields are all in
// [0, SCALE] (10'000) and fit comfortably in u32. Defining the alias
// here avoids redefinition across the four data headers.
inline constexpr std::uint32_t kScale = static_cast<std::uint32_t>(SCALE);

// A-2: MAX_XP = 10^15. Far above any reachable lifetime XP; keeps i64
// headroom for levelCosts × factors. Used by applyXpDelta's clamp (task 0.2)
// and by every system that touches availableXp.
inline constexpr std::int64_t MAX_XP = 1'000'000'000'000'000ll;

} // namespace harrowmere
