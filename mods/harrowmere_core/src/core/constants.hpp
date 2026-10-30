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

// ---- task 3.8: seasons & weather (Appendix A.6) -------------------------------

// A-51: SEASON_LENGTH_GAME_DAYS = 30. One season per game-month; the four
// seasons cycle spring -> summer -> autumn -> winter (card 3.8). The
// seconds form below is the single conversion the season clock uses
// (SEASON_LENGTH_GAME_DAYS x A-3's 86'400 game seconds per game day,
// core/clock.hpp's DAY_LENGTH_SECONDS — the derivation is kept literal so
// constants.hpp stays a leaf header; test_seasons.cpp static_asserts the
// two forms agree).
inline constexpr std::uint32_t SEASON_LENGTH_GAME_DAYS    = 30;
inline constexpr std::uint64_t SEASON_LENGTH_GAME_SECONDS = 30ull * 86'400ull;

// A-3 derived: the game hour (86'400 / 24 = 3'600 game seconds). The
// weather machine rolls on the game-hour cadence (card 3.8: "weather per
// region per game-hour"), never on the wall clock (R8).
inline constexpr std::uint64_t GAME_HOUR_SECONDS = 3'600ull;

// A-52 (season half): farmer-duty yield multipliers on the SCALE grid —
// spring 1.0, summer 1.2, autumn 0.8, winter 0.3. Winter famines drive
// migration (the design reference's doc hook).
inline constexpr std::uint32_t SEASON_YIELD_SPRING = 10'000;
inline constexpr std::uint32_t SEASON_YIELD_SUMMER = 12'000;
inline constexpr std::uint32_t SEASON_YIELD_AUTUMN = 8'000;
inline constexpr std::uint32_t SEASON_YIELD_WINTER = 3'000;

// A-52 (weather half): weather yield multipliers on the SCALE grid — storm
// 0.7, rain 1.1; every other weather state carries no modifier (SCALE).
inline constexpr std::uint32_t WEATHER_YIELD_STORM = 7'000;
inline constexpr std::uint32_t WEATHER_YIELD_RAIN  = 11'000;

// A-53: EXPOSURE_DPS = 2 hp/tick. Exposure damage taken per tick by every
// NEAR character unsheltered in a storm/snow region (card 3.8); ~3 min
// unsheltered in a storm is ~10% of a fresh player's HP. Applied ONLY
// through the damage path (applyDamage, source = 0 environment) so deaths
// route through 3.3's onEnvironmentDeath single path.
inline constexpr std::uint32_t EXPOSURE_DPS = 2;

// D-SHELTER-1: shelter = within 50 m of a building marker; "settlement
// center = sheltered in v1" (card 3.8). Distance compares as f64 (R5).
inline constexpr double SHELTER_RADIUS_M = 50.0;

// Card 3.8 Spec: "winter adds pull toward warmer-climate regions (+1000)".
// The SCALE-grid pull bonus the migration system (3.4) adds for a candidate
// region in winter. Not yet an Appendix A row (the value is card-pinned,
// the warmth set is 3.8's reading — see DEVIATIONS.md 2026-10-30); awaits
// Appendix A promotion like worldgen's pinned climate weights.
inline constexpr std::uint32_t MIGRATION_WINTER_PULL = 1'000;

} // namespace harrowmere
