// constants.hpp — the Constants Registry home (task 0.2; plan §1.3, Appendix A).
//
// CONTRACT (task 0.2): every game constant lives here with an `// A-nn` Appendix A
// reference, or is data (JSON). This file is APPEND-ONLY — later tasks append their
// rows as their cards land. No other file may define its own SCALE (task 0.2
// Done-when); math.hpp includes this header rather than redefining anything.

#pragma once

#include <cstdint>

// A-1 — SCALE: the fixed-point grid all game math runs on (§3.2, rules R1–R3).
// A probability of 0.37 is 3700; a multiplier of 1.5x is 15000.
inline constexpr std::uint64_t SCALE = 10'000;

// A-2 — MAX_XP: 10^15. availableXp (i64) may go negative down to -MAX_XP exactly
// (R4); totalXp (u64) never decreases. applyXpDelta (math.hpp) enforces the floor.
inline constexpr std::int64_t MAX_XP = 1'000'000'000'000'000;

static_assert(MAX_XP > 0, "MAX_XP must be a positive magnitude (R4)");
