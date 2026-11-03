// core/constants_phase2.hpp — every A-row and card-pinned constant FIRST introduced by
// Phase-2 cards 3.1–3.7 (Appendix A is the single home of every number; §1.3).
//
// MERGE NOTE: this block is self-contained and conflict-free with the Phase-1 registry;
// flat-append it into `core/constants.hpp` if you prefer one file, or keep it included
// from there. Every constant cites its Appendix A row (or the pinning task card).
//
// Two values are implementation proposals pending an A-row (flagged in MERGE_NOTES.md
// and destined for DEVIATIONS.md): TAX_SLICE_PER_TRANSACTION and the duty-rate block
// marked "proposed" — the cards demand the paths but pin no magnitudes (§1.3).
#pragma once
#include <array>
#include <cstdint>

#include "core/clock.hpp" // DAY_LENGTH_SECONDS (A-38/A-48 derive from it)
#include "core/constants.hpp"

namespace harrowmere
{

// ---- A-28..A-31 — combat (task 3.2) ----
// hitChance = clamp(SCALE/2 + (agiA - agiD)*100/SCALE, 1000, 9000)              (A-28)
// rawDamage = max(MIN_DAMAGE, attEff(STR)*WEAPON_FACTOR/GRID - defEff(DEF)*DEFENCE_FACTOR/GRID)
// The damage-factor grid is per-mille: A-29's "strength×1.5" with WEAPON_FACTOR=1500 and
// A-42's rationale ("one villager punch ~5-15 dmg" at villager stat caps) both hold only
// on a /1000 grid. Item weaponFactor data (items.json 1.5) scales to the same grid.
constexpr std::uint64_t WEAPON_FACTOR          = 1500; // A-29 (unarmed 800 below)
constexpr std::uint64_t WEAPON_FACTOR_UNARMED  = 800;  // A-29
constexpr std::uint64_t DEFENCE_FACTOR         = 800;  // A-30
constexpr std::uint64_t DAMAGE_FACTOR_GRID     = 1000; // grid the A-29/A-30 factors live on
constexpr std::uint64_t MIN_DAMAGE             = 1;    // task 3.2 spec
constexpr std::uint64_t HIT_CHANCE_MIN         = 1000; // A-28 hard bounds (10%..90%)
constexpr std::uint64_t HIT_CHANCE_MAX         = 9000;
constexpr std::uint64_t ARCANA_COOLDOWN_S      = 3;  // A-31 (D-LORE-2: casting cadence)
constexpr std::uint64_t SPELL_SPARK_MIN_ARCANA = 10; // task 3.2: v1 spells = {"spark": ARCANA 10}

// ---- A-32..A-36 — death & XP transfer (task 3.3) ----
constexpr std::uint64_t ENVIRONMENT_DEATH_PENALTY_FACTOR = 5000;  // 0.5x level cost
constexpr std::uint64_t PVP_VICTIM_PENALTY_FACTOR        = 10000; // 1.0x
constexpr std::uint64_t NPC_VICTIM_PENALTY_FACTOR        = 20000; // 2.0x
constexpr std::uint64_t NPC_KILL_XP_GAIN                 = 8000;  // 0.8x of the loss
constexpr std::uint64_t CORPSE_DECAY_DAYS                = 3;     // A-36

// ---- A-37, A-38, A-44 — NPC lifecycle (task 3.4) ----
constexpr std::uint64_t NEED_DECAY_HUNGER_PER_HOUR = 120; // A-37 (per game-hour)
constexpr std::uint64_t NEED_DECAY_REST_PER_HOUR   = 90;
constexpr std::uint64_t NEED_DECAY_WEALTH_PER_HOUR = 40;
constexpr std::uint64_t NEED_DECAY_SOCIAL_PER_HOUR = 60;
constexpr std::uint64_t YEAR_GAME_SECONDS          = 365ULL * core::DAY_LENGTH_SECONDS; // A-38
// A-44's ADULT_YEARS = 16 lives in core/constants.hpp (the Phase-1 registry
// already carried it; this block does not redefine it).

// Task-3.4 card pins (no A-row of their own — the card text is the pin).
constexpr std::uint64_t NEED_CRISIS_THRESHOLD                      = 2500; // need < 2500 generates its intent
constexpr std::uint64_t MIGRATION_ROLL_PCT_BASE                    = 3000; // pct = 3000 x pull (card 3.4)
constexpr std::uint64_t MIGRATION_ROLL_INTERVAL_DAYS               = 30;   // a monthly roll per NPC
inline constexpr std::array<std::uint16_t, 5> NPC_LEVEL_MILESTONES = {20, 40, 60, 75, 90}; // auto-buy levels
constexpr const char *CHILD_ROLE_KEY                               = "child";              // until ADULT_YEARS (A-44)

// ---- A-45..A-48 — LOD & roles ----
constexpr double NEAR_DISTANCE_M          = 50.0;                     // A-45
constexpr double MID_DISTANCE_M           = 200.0;                    // A-46
constexpr double TIER_MARGIN              = 1.25;                     // A-47
constexpr std::uint64_t REPLACEMENT_DELAY = core::DAY_LENGTH_SECONDS; // A-48 (1 game-day)

// ---- A-49/A-50 + card — economy (task 3.7) ----
// capacity = population * perCapitaNeed (perCapitaNeed is goods.json data, A-49).
constexpr std::uint64_t PRICE_SENSITIVITY      = 4000; // A-50 (0.4x swing at full pressure)
constexpr std::uint64_t PRICE_CHANGED_MIN_MOVE = 1000; // task 3.7: publish on a >=10% move (SCALE grid)

// ---- A-57', A-60 — shared ----
constexpr double NPC_WALK_SPEED_MPS = 2.0;  // A-57' (MID drift)
constexpr double WITNESS_RADIUS_M   = 30.0; // A-60 (corpse discovery reads 4.1's radius)

// ---- proposed (paths demanded by cards, magnitudes unpinned — see MERGE_NOTES.md) ----
constexpr std::uint64_t TAX_SLICE_PER_TRANSACTION  = 500; // 5% of transaction value -> territory wealth (3.7)
constexpr std::uint64_t FARMER_OUTPUT_PER_GAME_DAY = 8;   // grain units per farmer per game-day (3.6 duty)
constexpr std::uint64_t WAGE_PER_DUTY_HOUR         = 1;   // currency per laborer/farmer duty hour (3.7 wage path)
constexpr std::uint64_t HEALER_HEAL_PER_DUTY       = 20;  // hp per healer duty (3.6 duty)

} // namespace harrowmere
