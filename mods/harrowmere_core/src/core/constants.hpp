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

// A-39: CELL_SIZE_M = 32. The spatial index's uniform cell edge (task 1.6):
// ~10 NPCs/cell average at the full planned population — a radius query
// scans a handful of cells, never a tree. src/world/spatial_grid.hpp derives
// its cell projection from this one constant (A-39 is the only home).
// Integer: the world plane must tile evenly into cells (compile-checked).
inline constexpr std::uint32_t CELL_SIZE_M = 32;

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

// ---- task 3.10: player control & interaction intents (Appendix A.4) -----------

// A-57: MOVE_SPEED_MPS = 4 (sprint x2). The kinematic walk speed in metres
// per real second (card 3.10: "kinematic walk (MOVE_SPEED_MPS = 4, A-57)").
// Consumed ONLY by the player controller's fixed-update integration
// (Position math, R5's carve-out) — a sprinting player moves at 8 m/s. The
// sprint cost (rest need) arrives with 2.1's player mirror; Phase 1 sprints
// free, per the card.
inline constexpr double MOVE_SPEED_MPS     = 4.0;
inline constexpr double MOVE_SPRINT_FACTOR = 2.0; // A-57's "(sprint ×2)"

// A-58: AFK_TIMEOUT_REAL_S = 120 real seconds. D-AFK-1: no UI intents for
// 120 real seconds -> afk = true until the next intent; 4.5 exempts AFK
// states from suspicion acts and 6.3 stops abandonment credit. REAL seconds
// (D-TIME-1: AFK detection is one of the three real-time doc rules) — the
// controller compares against WorldClock.realTime, never worldTime (R8).
inline constexpr std::uint64_t AFK_TIMEOUT_REAL_S = 120;

// Card 3.10: "Interactions by proximity + target type". The interaction
// range — the gate every Interact{target, action} intent must pass — is not
// pinned by any Appendix A row (no number exists in the card, the design
// reference, or Appendix A for it), so 3.10 pins it here pending Appendix A
// promotion (the same pattern as MIGRATION_WINTER_PULL above; DEVIATIONS.md
// 2026-10-30 carries the row). 3 m is a touch/talk reach: strictly inside
// every pinned social radius (A-55 IDENTIFY_RANGE_M 15, A-60 WITNESS_RADIUS
// 30, A-26 CONSENT_BROADCAST_M 50), so everything an interaction can touch
// also saw it happen. Boundary inclusive, compared on the x/z plane (R5).
inline constexpr double INTERACT_RANGE_M = 3.0;

// ---- task 2.0: character records & the store (Appendix A.2) -------------------

// A-41: STARTER_CURRENCY = 100. The new player's purse (task 2.0's pinned
// starting state): lets the player buy starter goods; below any claim or
// faction cost. i64 storage on Progression - debt (conviction fines, 4.3)
// is the only path below zero.
inline constexpr std::int64_t STARTER_CURRENCY = 100;

// A-42: BASE_HP = 100. The level-1 HP floor (task 2.0): a round number
// sized so one villager punch (~5-15 dmg) never one-shots a fresh player.
inline constexpr std::uint64_t BASE_HP = 100;

// A-43: HP_PER_LEVEL = 20. Linear HP growth per level (task 2.0): level 75
// is ~1600 hp - peak mortals are ~16x a farmer, strong but killable. The
// formula lives exactly once, in character/character.hpp's maxHpForLevel();
// species do NOT modify HP (the doc species table lists no HP column).
inline constexpr std::uint64_t HP_PER_LEVEL = 20;

// Appendix A.2, "species ids" row: 0 = player, the one hardcoded species id
// (design rule: "players are always the `player` species"; every other
// species id is data-assigned and referenced by key, task 1.1's pitfall).
// u16 literal so it converts to the speciesId alias without a cast.
inline constexpr std::uint16_t PLAYER_SPECIES_ID = 0;

// ---- task 2.1: statuses & cooldowns (Appendix A.1) -----------------------------

// A-4: COMBAT_TIMEOUT_S = 30 game-seconds. IN_COMBAT is DERIVED (D-CHAR-1):
// character/character.hpp stamps lastCombatTime on every damage application
// (3.2) and character/status.hpp derives the flag while
// now - lastCombatTime < COMBAT_TIMEOUT_S. Long enough to chain fights,
// short enough that "peaceful" respec (2.3) is meaningful.
inline constexpr std::uint64_t COMBAT_TIMEOUT_S = 30;

// ---- task 2.3: progression engine (Appendix A.3) -------------------------------

// A-6: MAX_LEVEL = 100 / PLAYER_MAX_LEVEL = 75. Players cap at late-stage
// mortal (design rule: "players can only be at most ... late-stage mortals");
// NPCs cap at peak-stage. The stage bands (early 1-25, middle 26-50, late
// 51-75, peak 76-100) are cosmetic UI text, pinned here as comment only.
inline constexpr std::uint16_t MAX_LEVEL        = 100;
inline constexpr std::uint16_t PLAYER_MAX_LEVEL = 75;

// A-7 + A-8: level costs. levelCosts[i] is the XP to advance from level i to
// i+1 (design reference §Levels); the table grows 1.15x per entry from the
// 100 base (progression/progression.hpp precomputes it once, constexpr).
// DEVIATIONS.md 2026-10-31: the design source's own indexing makes the FIRST
// purchase (1->2) cost levelCosts[1] = 115, not 100 - levelCosts[0] is the
// recurrence seed (the advance 0->1, unreachable: characters start at
// level 1). Appendix A.7's "why" paraphrase said 100; the doc + card spec
// (availableXp -= levelCosts[level]) win.
inline constexpr std::uint64_t BASE_LEVEL_COST     = 100;
inline constexpr std::uint64_t LEVEL_COST_RATE_NUM = 115;
inline constexpr std::uint64_t LEVEL_COST_RATE_DEN = 100;

// A-10: stat costs. statCosts[i] is the XP to raise a stat from i to i+1;
// 1.12x growth per point. getRefund (D-REFUND-1, literal) hands back exactly
// statCosts[point-1] - the last point's cost.
inline constexpr std::uint64_t BASE_STAT_COST     = 50;
inline constexpr std::uint64_t STAT_COST_RATE_NUM = 112;
inline constexpr std::uint64_t STAT_COST_RATE_DEN = 100;

// A-11: skill costs - skills grow a bit steeper than stats (1.15x from 80).
inline constexpr std::uint64_t BASE_SKILL_COST     = 80;
inline constexpr std::uint64_t SKILL_COST_RATE_NUM = 115;
inline constexpr std::uint64_t SKILL_COST_RATE_DEN = 100;

// A-12: the soft-cap knee (STAT_THRESHOLD / SKILL_THRESHOLD = 50). Points at
// or below the knee count at face value; beyond it getEffectiveness adds
// isqrt(point - threshold) only. DEVIATIONS.md 2026-10-31: the A-13/A-14
// caps (max 30 at level 100) bind below this knee, so the sqrt branch and
// D-LORE-1's mana rule are unreachable through normal v1 progression - both
// are implemented verbatim and re-entry is data (raise the caps).
inline constexpr std::uint64_t STAT_THRESHOLD  = 50;
inline constexpr std::uint64_t SKILL_THRESHOLD = 50;

// A-13 + A-14: stat caps. caps[i] = BASE_STAT_CAP + i / STAT_CAP_DIVISOR
// (i = level): 10 at level 1, +1 per 5 levels, 30 at cap-level 100.
inline constexpr std::uint64_t BASE_STAT_CAP    = 10;
inline constexpr std::uint64_t STAT_CAP_DIVISOR = 5;

// A-15 + A-16: skill caps - as stats.
inline constexpr std::uint64_t BASE_SKILL_CAP    = 10;
inline constexpr std::uint64_t SKILL_CAP_DIVISOR = 5;

// D-LORE-1 (card 2.3) mana-enhancement constants: buying a stat point above
// STAT_THRESHOLD draws on ARCANA knowledge; a shortfall risks a painful
// failure. No Appendix A rows pin these three yet - the values are card text
// ("pct = min(SCALE, shortfall * 2000)", "maxHp * (5 + shortfall/500) / 100"),
// so they live here named, the same pattern as A-121/A-122 before promotion
// (DEVIATIONS.md 2026-10-31 carries the row).
inline constexpr std::uint64_t MANA_FAILURE_CHANCE_PER_POINT = 2'000; // failure pct per point of shortfall (SCALE grid)
inline constexpr std::uint64_t MANA_PAIN_BASE_PCT            = 5;     // min pain, % of maxHp
inline constexpr std::uint64_t MANA_PAIN_SHORTFALL_DIVISOR   = 500;   // +1 pain-% per this many shortfall points
inline constexpr std::uint64_t MANA_PAIN_PCT_DENOMINATOR     = 100;   // the formula's fixed /100

// Respec cooldown duration (card 2.3: "respec with cooldowns"). Neither the
// design reference nor Appendix A pins a length - canRespec only reads the
// cooldown slot - so v1 pins ONE game-day (A-3's 86'400 game seconds, kept
// literal like SEASON_LENGTH_GAME_SECONDS above; constants.hpp stays a leaf
// header), the same cadence as A-24's appearance cooldown. Awaiting Appendix
// A promotion (DEVIATIONS.md 2026-10-31).
inline constexpr std::uint64_t RESPEC_COOLDOWN_GAME_S = 86'400;

// ---- task 2.4: items, inventory, gear, crafting (Appendix A.3) -----------------

// A-17: BASE_ITEM_SCORE = 10. The naked character's gear floor: getItemScore
// adds this base for every equipped item (items/gear.hpp, design reference
// verbatim: BASE + W_RARITY*rarity/SCALE + W_LEVEL*level/SCALE).
inline constexpr std::uint64_t BASE_ITEM_SCORE = 10;

// A-18: W_RARITY = 40. The rarity term's weight on the SCALE grid; gear is a
// minority of power (the doc's design: skills/stats dominate).
inline constexpr std::uint64_t W_RARITY = 40;

// A-19: W_LEVEL = 60. The item-level term's weight on the SCALE grid — as
// A-18, kept small so gear never dominates the power formula.
inline constexpr std::uint64_t W_LEVEL = 60;

// ---- task 2.5: power scores & matchmaking (Appendix A.3) -----------------------

// A-20: statWeight, in StatType declaration order (STR/DEF/AGI/INT/END/PER).
// Offense & intellect outweigh mobility; defense mid. Named constants (not an
// array) so constants.hpp stays a leaf header; progression/power.cpp builds
// the indexed array from them, with a static_assert tying the two together.
inline constexpr std::uint64_t STAT_WEIGHT_STRENGTH     = 12'000;
inline constexpr std::uint64_t STAT_WEIGHT_DEFENCE      = 10'000;
inline constexpr std::uint64_t STAT_WEIGHT_AGILITY      = 8'000;
inline constexpr std::uint64_t STAT_WEIGHT_INTELLIGENCE = 10'000;
inline constexpr std::uint64_t STAT_WEIGHT_ENDURANCE    = 6'000;
inline constexpr std::uint64_t STAT_WEIGHT_PERCEPTION   = 6'000;

// A-21: skillWeight — 6000 for ALL eight skills (skills equal until content
// differentiates; adjust per-A-row only).
inline constexpr std::uint64_t SKILL_WEIGHT_ALL = 6'000;

// ---- task 2.6: appearance (Appendix A.4) ---------------------------------------

// A-22: BASE_APPEARANCE_COST = 50. One early level's worth of XP —
// noticeable, not painful. Paid from availableXp (the doc's own comparison;
// never currency).
inline constexpr std::uint64_t BASE_APPEARANCE_COST = 50;

// A-23: APPEARANCE_COST_RATE = 10 per level. Linear growth (level 75 change
// costs 800) — anti-griefing by escalation.
inline constexpr std::uint64_t APPEARANCE_COST_RATE = 10;

// A-24: appearance cooldown = 1 game-day (86'400 game seconds, A-3). Armed
// through CooldownType::APPEARANCE on every successful change.
inline constexpr std::uint64_t APPEARANCE_COOLDOWN_GAME_S = 86'400;

// A-25: "frequent changes" = >= 3 changes within 10 game-days. The countable
// form of the doc's suspicion trigger; character/appearance.hpp fires the
// suspicion seam when the window's count reaches the threshold.
inline constexpr std::uint32_t APPEARANCE_FREQUENT_COUNT         = 3;
inline constexpr std::uint64_t APPEARANCE_FREQUENT_WINDOW_GAME_S = 10ull * 86'400ull;

// ---- task 2.7: duels & arenas (Appendix A.4) -----------------------------------

// A-26: CONSENT_BROADCAST_M = 50. The duel/arena consent broadcast radius —
// the same radius as WITNESS_RADIUS's world (A-60 30 m identification, A-55
// 15 m faces): everyone who could see the fight was warned, so a witnessed
// duel generates zero assault evidence (combat/consent.hpp).
inline constexpr double CONSENT_BROADCAST_M = 50.0;

// A-27: DUEL_TIMEOUT_S = 600 (10 game-minutes). Idle duels cannot hold
// consent open indefinitely; the sweep (combat/consent.hpp sweepDuels) ends a
// duel whose last mutual damage sits this many game-seconds in the past.
inline constexpr std::uint64_t DUEL_TIMEOUT_S = 600;

// Card 2.7: "range separation > 100 m" ends a duel. No Appendix A row pins
// the value (the card text is the only home), so it lives here named pending
// Appendix A promotion — the same pattern as INTERACT_RANGE_M above
// (DEVIATIONS.md carries the row).
inline constexpr double DUEL_MAX_SEPARATION_M = 100.0;

// ---- task 3.9: relationships, perception & witness qualification ---------------

// A-54: ACQUAINTANCE_AFFINITY = 2000. The acquaintance threshold on the
// affinity map (task 3.9): affinity >= this value means the observer KNOWS
// the other by id — identifications match regardless of the culprit's
// appearance version (design rule: "disguises defeat strangers, never
// acquaintances"). Note the deliberate asymmetry: a NEGATIVE affinity of the
// same magnitude is an enemy, not an acquaintance — and enemies testify
// harder (the witness weight's (SCALE - affinity) term).
inline constexpr std::int64_t ACQUAINTANCE_AFFINITY = 2'000;

// A-55: IDENTIFY_RANGE_M = 15. The face-identification radius (task 3.9): a
// sighting only becomes a stranger-memory record when the observer is within
// this range of the culprit at sighting time (and the culprit is not MASKED).
// Strictly inside WITNESS_RADIUS (A-60, 30 m): you may see a crime from twice
// the distance you can identify a face.
inline constexpr double IDENTIFY_RANGE_M = 15.0;

// A-56: STRANGER_MEMORY_S = 10 game-minutes (600 game seconds). The stranger
// sighting's lapse window (task 3.9): a stranger's memory persists while the
// observer remains within sight of the culprit (NEAR-tier co-presence keeps
// refreshing the sighting stamp) and lapses once they have lost sight for
// this long. Acquaintance memories never lapse.
inline constexpr std::uint64_t STRANGER_MEMORY_S = 600;

// Card 3.9's affinity sources (the card text is the pin; the values await
// Appendix A promotion like every card-pinned constant — DEVIATIONS.md row):
//   * +AFFINITY_CO_PRESENCE_PER_HOUR (20) per full game-hour of NEAR
//     co-presence, saturating at AFFINITY_CO_PRESENCE_CAP (+4000) from this
//     source — proximity familiarity, not friendship;
//   * +AFFINITY_PER_TRADE_EVENT (100) per trade/gift event (uncapped at the
//     source; the map clamps to [-SCALE, +SCALE]);
//   * +AFFINITY_SAME_FACTION_BASE (2000) seeded as the base when the pair
//     shares a faction (6.1's runtime reads it from there).
inline constexpr std::int64_t AFFINITY_CO_PRESENCE_PER_HOUR = 20;
inline constexpr std::int64_t AFFINITY_CO_PRESENCE_CAP      = 4'000;
inline constexpr std::int64_t AFFINITY_PER_TRADE_EVENT      = 100;
inline constexpr std::int64_t AFFINITY_SAME_FACTION_BASE    = 2'000;

// ---- task 4.x: the justice layer (Appendix A.5 — A-59..A-88) --------------------

// A-60's WITNESS_RADIUS lives in core/constants_phase2.hpp (WITNESS_RADIUS_M,
// 30 m) — the Phase-2 registry already carried it; this block does not
// redefine it. All weights/scores below are 0..SCALE on the fixed-point grid
// unless the row says otherwise (day counts, currency, metres).

// A-59: WARD_WEIGHT = SCALE. A safe-zone ward report is maximal testimony
// (the doc's `Report{0, WARD_WEIGHT, true}` — source 0 = the ward, an
// identity source in safe zones by design; do not "fix" it to false).
inline constexpr std::uint32_t WARD_WEIGHT = static_cast<std::uint32_t>(SCALE);

// A-61: REPORT_CAP = 8. capReports keeps the heaviest eight reports; the
// excess merges into aggregateReportWeight (stable sort: equal weights keep
// insertion order — R7 determinism).
inline constexpr std::size_t REPORT_CAP = 8;

// A-62: PHYS_BASE = 500 (D-PHYS-2). The bare-scene score; scene markers add
// 500-4000 on top. Bounds [0, SCALE].
inline constexpr std::uint32_t PHYS_BASE = 500;

// The D-PHYS-2 marker weights (each adds to PHYS_BASE when its scene context
// holds; the markers without a v1 state source stay inert — see
// justice/evidence.cpp's marker table and the DEVIATIONS.md row):
inline constexpr std::uint32_t PHYS_MARKER_BLOODSTAINED = 2'000; // the suspicion act active on the culprit
inline constexpr std::uint32_t PHYS_MARKER_FORCED_ENTRY = 1'500; // forced entry / private-property trespass context
inline constexpr std::uint32_t PHYS_MARKER_BURNING      = 4'000; // arson: a burning structure at the scene
inline constexpr std::uint32_t PHYS_MARKER_GUARD_MARKED = 3'000; // theft of a guardMarked item (2.4's flag)
inline constexpr std::uint32_t PHYS_MARKER_NIGHT        = 500;   // harder to see the culprit flee (v1: additive)

// A-63: DELAYED_BODY_WEIGHT = 6000. A discovered body is strong evidence,
// weaker than a live witness; appended to the ORIGINAL murder evidence.
inline constexpr std::uint32_t DELAYED_BODY_WEIGHT = 6'000;

// A-64/A-65: MISSING_DAYS = 7 game-days; MISSING_WEIGHT = 2000. The
// missing-persons flag's window and the context weight it adds to the
// matching evidence when the person is found dead.
inline constexpr std::uint64_t MISSING_DAYS   = 7;
inline constexpr std::uint32_t MISSING_WEIGHT = 2'000;

// A-66: VENDOR_MARK_WEIGHT = 4000. Selling a guardMarked/reportedStolen item
// flags the sale: an anonymous report (source 0, never identity) on the
// matching THEFT evidence.
inline constexpr std::uint32_t VENDOR_MARK_WEIGHT = 4'000;

// A-67: OBSTRUCTION_WEIGHT = 5000. A failed bribery attempt appends this as
// a report identifying the briber, on a fresh OBSTRUCTION evidence.
inline constexpr std::uint32_t OBSTRUCTION_WEIGHT = 5'000;

// A-68/A-69: SELF_REPORT_WINDOW_DAYS = 2 game-days; SELF_REPORT_WEIGHT =
// 8000. A player victim's post-respawn self-report: adds score, never
// satisfies identity (hasIdentity counts NPC reports only — 4.2 enforces).
inline constexpr std::uint64_t SELF_REPORT_WINDOW_DAYS = 2;
inline constexpr std::uint32_t SELF_REPORT_WEIGHT      = 8'000;

// A-70: PHYSICAL_IDENTITY_MIN = 6000. Overwhelming physical evidence alone
// satisfies hasIdentity (4.2).
inline constexpr std::uint32_t PHYSICAL_IDENTITY_MIN = 6'000;

// A-71: LIMIT_DAYS = 30 game-days. The case window: older un-wanted cases
// archive (the sweep is the backstop; updateEvidence checks per tick).
inline constexpr std::uint64_t LIMIT_DAYS = 30;

// A-72: INVESTIGATE_DIVISOR = 2. Half the conviction threshold starts the
// investigation (status INVESTIGATING).
inline constexpr std::uint64_t INVESTIGATE_DIVISOR = 2;

// A-73: BOUNTY_CAP = 50'000 currency. D-BOUNTY-1: bounty = min(cap,
// severity * 5 currency per severity point) — keeps "bounty total stays
// below the conviction penalty" true at every severity.
inline constexpr std::uint64_t BOUNTY_CAP                = 50'000;
inline constexpr std::uint64_t BOUNTY_CURRENCY_PER_POINT = 5; // D-BOUNTY-1's severity rate
// The conviction fine (4.3's fineFor; the docs name the function, no A-row
// pins the rate): severity * 10 currency per point — exactly twice the
// bounty rate, so the D-BOUNTY-1 invariant (bounty < fine at every
// severity, cap included) holds structurally. Awaits Appendix A promotion
// (DEVIATIONS.md carries the row).
inline constexpr std::uint64_t FINE_CURRENCY_PER_POINT = 10;

// A-74: WARRANT_DURATION = 7 game-days. issueWarrant stamps
// expiresTime = now + duration; expiry is a pause (WANTED -> INVESTIGATING),
// not an eraser — the case resumes building.
inline constexpr std::uint64_t WARRANT_DURATION_DAYS   = 7;
inline constexpr std::uint64_t WARRANT_DURATION_GAME_S = 7ull * 86'400ull;

// A-75: CASE_TICK_RADIUS_M = 500. Case ticks run only for territories with
// the player within this radius; sweepExpiredEvidence is the backstop
// elsewhere.
inline constexpr double CASE_TICK_RADIUS_M = 500.0;

// A-77: CONVICTION_PENALTY_FACTOR = 20000 (2.0x). The XP debt's base factor
// against the culprit's next-level cost, before the zone/reputation
// multipliers (the mulDiv128 wide path — R3).
inline constexpr std::int64_t CONVICTION_PENALTY_FACTOR = 20'000;

// A-78: CONVICTION_REPUTATION_LOSS = 3000. changeReputation(-3000) at the
// evidence territory (doubles in safe zones, §Safe Zones) — propagates up
// the chain (4.4).
inline constexpr std::int32_t CONVICTION_REPUTATION_LOSS = 3'000;

// A-79/A-80: the sentence. SENTENCE_BASE_DAYS = 1 game-day plus
// SENTENCE_SEVERITY_DAYS (1) per SENTENCE_SEVERITY_DIVISOR (5) severity
// points: sentence_days = 1 + severity/5 (the A-80 row's own form: "1 per
// severity point ×5"). Murder (sev 9000) lands at 1801 game-days.
inline constexpr std::uint64_t SENTENCE_BASE_DAYS        = 1;
inline constexpr std::uint64_t SENTENCE_SEVERITY_DAYS    = 1;
inline constexpr std::uint64_t SENTENCE_SEVERITY_DIVISOR = 5;

// A-81: ESCAPE_DIFFICULTY = 6000. The escape check: pct = clamp(eff * 2 -
// DIFFICULTY + 2000, 200, 9000) over the ESCAPE skill effectiveness.
inline constexpr std::uint64_t ESCAPE_DIFFICULTY = 6'000;
inline constexpr std::uint64_t ESCAPE_PCT_MIN    = 200;
inline constexpr std::uint64_t ESCAPE_PCT_MAX    = 9'000;
inline constexpr std::uint64_t ESCAPE_PCT_BONUS  = 2'000;

// A-82/A-83: ESCAPE_EVIDENCE_WEIGHT = 7000 (the fresh OBSTRUCTION evidence
// an escape attempt leaves behind — "escape near-convicts on its own");
// ESCAPE_FAIL_WEIGHT = 3000 (a failed attempt stacks onto it and doubles the
// remaining sentence).
inline constexpr std::uint32_t ESCAPE_EVIDENCE_WEIGHT = 7'000;
inline constexpr std::uint32_t ESCAPE_FAIL_WEIGHT     = 3'000;

// A-84: REPUTATION_DECAY_INTERVAL = 1 game-day. The lazy-decay quantum —
// decayIfDue applies (now - lastDecayTime) / interval whole steps at once;
// there is NO global periodic tick (the doc's "lazy decay replaces global
// ticks").
inline constexpr std::uint64_t REPUTATION_DECAY_INTERVAL = 86'400;

// A-85: REPUTATION_DECAY = 500/day toward zero, sign-aware (negative values
// decay UP; full ±range heals in ~20 days of good behavior).
inline constexpr std::int64_t REPUTATION_DECAY = 500;

// A-86: NOTORIETY_DECAY = 1000/day, floored at 0 (infamy fades faster than
// hate).
inline constexpr std::int64_t NOTORIETY_DECAY = 1'000;

// A-87: MAX_REPUTATION = 10'000 (±SCALE — symmetric sainthood/pariah).
inline constexpr std::int64_t MAX_REPUTATION = 10'000;

// A-88: REPUTATION_PROPAGATION = 5000 (0.5x per tree level — "crime tourism
// works locally, dies kingdom-wide").
inline constexpr std::uint64_t REPUTATION_PROPAGATION = 5'000;

// D-REP-2's recovery term: the conviction multiplier reads
// SCALE + max(0, -rep) * 2000 / SCALE (a pariah's debt grows up to +20%).
inline constexpr std::int64_t REPUTATION_DEBT_MULTIPLIER_TERM = 2'000;

// The assault threshold (the doc's own name; no Appendix A row pins it).
// noteAssault records the ASSAULT evidence when a (culprit, victim) pair's
// accumulated unconsented damage crosses this. 1 = MIN_DAMAGE: a fresh
// player's punch deals exactly MIN_DAMAGE (3.2's floor), and the §4.6 worked
// trace ("punching a villager in a village at night") must record the
// assault at the first landed blow — any higher value would make the doc's
// own trace unreachable at level 1. Awaits Appendix A promotion
// (DEVIATIONS.md carries the row).
inline constexpr std::uint64_t ASSAULT_THRESHOLD = 1;

// Night band for scorePhysical's +500 marker, derived from clock.hpp's
// documented dayPhase01 anchors (0.25 = 06:00, 0.75 = 18:00) — pure integer
// game-second math (R5): night is [18:00, 06:00).
inline constexpr std::uint64_t NIGHT_START_GAME_S = 86'400ull * 3 / 4; // 18:00
inline constexpr std::uint64_t NIGHT_END_GAME_S   = 86'400ull / 4;     // 06:00

// ---- task 4.5: suspicion & trust (Appendix A.7 — A-89..A-94) --------------------

// A-89: MEMORY_CAP = 16. The per-NPC suspicion memory's entry cap; eviction
// drops the lowest trust tier first and acquaintances outrank strangers (the
// doc rule).
inline constexpr std::size_t MEMORY_CAP = 16;

// A-90: the trust-tier ladder thresholds — WATCHED 2000, DISTRUSTED 5000,
// SHUNNED 8000 (tierFor's verbatim ladder; below WATCHED the observed is a
// STRANGER).
inline constexpr std::uint32_t TRUST_TIER_WATCHED    = 2'000;
inline constexpr std::uint32_t TRUST_TIER_DISTRUSTED = 5'000;
inline constexpr std::uint32_t TRUST_TIER_SHUNNED    = 8'000;

// A-91: SUSPICION_DECAY = 100 per game-day — a WATCHED passer-by decays to
// STRANGER in ~20 days (the lazy per-entry decay quantum).
inline constexpr std::uint32_t SUSPICION_DECAY = 100;

// A-92: donation/service relief = -1000 score — one act of service is worth
// about ten days of decay.
inline constexpr std::uint32_t SUSPICION_DONATION_RELIEF = 1'000;

// A-93: the tier price modifiers on sell receipts — WATCHED -5%, DISTRUSTED
// -10%, SHUNNED -20% (SCALE-grid multipliers the 3.7 sell path reads through
// the IPriceModifierSink); the buy-side surcharge band is +0..20% (dormant
// until the buy-path consumer lands — the pinned upper bound rides here).
inline constexpr std::uint64_t TIER_RECEIPT_MODIFIER_WATCHED    = 9'500;
inline constexpr std::uint64_t TIER_RECEIPT_MODIFIER_DISTRUSTED = 9'000;
inline constexpr std::uint64_t TIER_RECEIPT_MODIFIER_SHUNNED    = 8'000;
inline constexpr std::uint64_t TIER_SURCHARGE_MAX               = 2'000;

// A-94: APPEARANCE_SUSPICION_WEIGHT = 3000 — the 2.6 frequent-change signal
// (ISuspicionSink::onAppearanceFrequent) lands just under WATCHED on its own.
inline constexpr std::uint32_t APPEARANCE_SUSPICION_WEIGHT = 3'000;

// ---- task 4.6: law enforcement AI (Appendix A.9 — A-95/A-96) --------------------

// A-95: ENGAGE_RATIO = 15000 (1.5x) — guards engage only at a power
// advantage: localGuardPower * ENGAGE_RATIO >= culpritPower * SCALE. The
// ratio multiplies the GUARD side (the card pitfall: inverting makes
// suicide-guards; both directions are pinned by tests).
inline constexpr std::uint64_t ENGAGE_RATIO = 15'000;

// A-96: LAWLESS_AFTER_DAYS = 3 game-days — a territory with zero guard-role
// NPCs and no safe flag sits lawless after this long; lawsAt falls back to
// DEFAULT_LAWS and enforcement/case ticks stop running there.
inline constexpr std::uint64_t LAWLESS_AFTER_DAYS = 3;

// ---- task 4.7: citizen justice (Appendix A.9 — A-97..A-102) ---------------------

// A-97: MILITIA_ODDS_RATIO = 20000 (2.0x) — a posse engages only when its
// summed power reaches twice the target's ("veteran militias engage only at
// favorable odds"); below it the posse shadows and reports.
inline constexpr std::uint64_t MILITIA_ODDS_RATIO = 20'000;

// A-98: DEAD_BOUNTY_RATIO = 6000 (0.6x) — "alive capture pays full; dead
// pays partial".
inline constexpr std::uint64_t DEAD_BOUNTY_RATIO = 6'000;

// A-99/A-100: capture fatigue — each capture of the same culprit within
// CAPTURE_FATIGUE_WINDOW_DAYS (5 game-days) multiplies the payout by
// CAPTURE_FATIGUE_FACTOR (5000 = 0.5x on the SCALE grid), compounding.
inline constexpr std::uint64_t CAPTURE_FATIGUE_WINDOW_DAYS = 5;
inline constexpr std::uint64_t CAPTURE_FATIGUE_FACTOR      = 5'000;

// A-101/A-102: the village-slaughter marker — MASSACRE_THRESHOLD (5) sapient
// NPC deaths by one culprit in one territory within MASSACRE_WINDOW_DAYS
// (2 game-days) zero the victims' loot currency and register a casus belli.
inline constexpr std::uint32_t MASSACRE_THRESHOLD   = 5;
inline constexpr std::uint64_t MASSACRE_WINDOW_DAYS = 2;

// ---- task 4.7 companions (the card's own numbers, awaiting Appendix A) ----------

// The willing-citizen integrity floor for a posse (the card's "willing
// citizens (affinity to victim >= 2000 — A-54's ACQUAINTANCE_AFFINITY —
// integrity >= 5000)"); the affinity half reuses A-54 verbatim.
inline constexpr std::uint64_t MILITIA_CITIZEN_INTEGRITY = 5'000;

// ---- task 5.1: event engine (Appendix A.10 — A-103) -----------------------------

// A-103: MAX_EVENT_DEPTH = 2 LEVELS — an event may spawn a child (depth 1);
// no grandchildren (a depth-2 spawn is refused; the card's rule list and
// A-103's own why-note pin both halves).
inline constexpr std::uint8_t MAX_EVENT_DEPTH = 2;

// ---- task 5.3: contribution, snapshots & validation (Appendix A.10) -------------

// A-104: MAX_ACTION_AMOUNT = 10000 — the default clamp for uncapped action
// types (5.3's validateActionAmount default arm; the doc's own farm guard).
inline constexpr std::uint32_t MAX_ACTION_AMOUNT = 10'000;

// A-105: MIN_CONTRIBUTION = 100 — 1% of SCALE: token participation pays
// nothing (5.3's getRewardShare floor; 5.4's distributor excludes the same
// set so ineligible participants never shrink the others' shares).
inline constexpr std::uint32_t MIN_CONTRIBUTION = 100;

// A-106: SNAPSHOT_INTERVAL = 60 game-s — the power snapshot lands every
// game-minute of participation (5.3's step-5 slice; the MAX watermark resets
// only at endEvent's clear).
inline constexpr std::uint32_t SNAPSHOT_INTERVAL_GAME_S = 60;

// ---- task 5.4: reward scaling (Appendix A.10 — A-107) ---------------------------
//
// A-107 IS the formula row: "pool x (SCALE + difficulty/2), item rarity
// bonus difficulty/10". The divisors are named so no bare literal rides the
// code (section 1.3); changing one is a one-constant edit + the pinned
// TestRewards_DifficultyScalesPool.

inline constexpr std::uint32_t REWARD_POOL_DIFFICULTY_DIVISOR = 2;
inline constexpr std::uint32_t REWARD_RARITY_BONUS_DIVISOR    = 10;

// ---- task 6.1: factions (Appendix A.11 — A-108) --------------------------------
//
// A-108: FACTION_FOUND_COST = 5000 currency — founding a faction is
// meaningful, not gatekeeping (the row's own why-note). Paid from the
// founder's purse through the i64 currency field; a purse that cannot cover
// it rejects the founding (a rule-level refusal, §3.4).
inline constexpr std::int64_t FACTION_FOUND_COST = 5000;

// The faction-dynamics magnitudes the card names but Appendix A does not pin
// (the FINE_CURRENCY_PER_POINT precedent — DEVIATIONS.md carries the row;
// changing one is a one-constant edit + the pinned test):
//   * the worldgen seeds' starting scores — rivals open at -6000 (rivalry
//     flag set), allies at +6000 (alliance flag set); the doc's relations
//     are "more than acquaintance, less than family" ±6000 pins both.
//   * the monthly drift: neutral pairs relax toward zero by 50/month;
//     a rivalry pair hardens by 100/month toward its floor or softens by
//     100/month toward zero, chosen by the SIGN of the leaders' mean
//     reputation (4.4's read — reviled leaders harden the rivalry).
constexpr std::int32_t FACTION_SEEDED_RIVALRY_SCORE   = -6000;
constexpr std::int32_t FACTION_SEEDED_ALLIANCE_SCORE  = 6000;
constexpr std::int32_t FACTION_DRIFT_PER_MONTH        = 50;
constexpr std::int32_t FACTION_RIVALRY_STEP_PER_MONTH = 100;
// Invite acceptance (6.1: "invite — NPC acceptance by affinity/power"): a
// neutral NPC accepts half the time; the candidate's affinity toward the
// inviter moves the roll 1:2 (±SCALE affinity -> ±5000 pct), and a weaker
// candidate discounts by 2000. Clamped to the [10%, 90%] band the A-28/A-116
// family shares.
constexpr std::int64_t FACTION_INVITE_BASE_PCT         = 5000;
constexpr std::int64_t FACTION_INVITE_AFFINITY_DIVISOR = 2;
constexpr std::int64_t FACTION_INVITE_WEAKER_PENALTY   = 2000;

// ---- task 6.5: matchmaking & scaling (Appendix A.11 — A-116) -------------------
//
// A-116 IS the formula row: "clamp(SCALE − |Δscore|/10, 1000, 9000)" —
// "reasonable NPC sportsmanship". The falloff divisor, floor and ceiling are
// named so no bare literal rides the code (section 1.3); the volunteer arm
// of the manhunt normalization reuses the same falloff shape, clamped to
// [FLOOR, SCALE] (a volunteer never out-weighs a guard).
constexpr std::uint64_t DUEL_ACCEPTANCE_FALLOFF_DIVISOR = 10;
constexpr std::uint64_t DUEL_ACCEPTANCE_FLOOR           = 1000;
constexpr std::uint64_t DUEL_ACCEPTANCE_CEILING         = 9000;
// The event-seeding divisor (6.5's formula: meanScore / 1000 — a mean score
// of 40000 seeds difficulty 4).
constexpr std::uint64_t EVENT_DIFFICULTY_SEED_DIVISOR = 1000;

// ---- task 6.2: war (Appendix A.12 — A-109..A-112) ------------------------------
//
// A-109: WAR_POWER_RATIO = 15000 (1.5x) — the PowerRatio casus belli: the
// attacker's aggregate member power must reach 1.5x the defender's (the card's
// pinned fallback cb; a faction with no living members never passes — the R2
// zero guard sits at the call site).
inline constexpr std::uint64_t WAR_POWER_RATIO = 15'000;

// A-110: WAR_NOTICE_GAME_S = 24 game-hours — declareWar sets
// noticeEndsAt = now + WAR_NOTICE_GAME_S; IN_WAR and isCriminalAct's war
// exemption share the same flip instant (the card's one-function pitfall,
// warEffectivelyActive).
inline constexpr std::uint64_t WAR_NOTICE_GAME_S = 24ull * GAME_HOUR_SECONDS;

// A-111: WAR_COLLAPSE_RATIO = 3000 (30% of SCALE) — a war ends when one
// side's aggregate member power falls below 30% of the other's; the stronger
// side wins.
inline constexpr std::uint32_t WAR_COLLAPSE_RATIO = 3'000;

// A-112: PEACE_TREATY_DAYS = 7 game-days — endWar stamps
// relations[a][b].treatyUntil = now + PEACE_TREATY_DAYS (both directions);
// declareWar refuses while now < treatyUntil.
inline constexpr std::uint64_t PEACE_TREATY_DAYS = 7;

// The war magnitudes the card names but Appendix A does not pin (the 6.1
// precedent — DEVIATIONS.md carries the row; changing one is a one-constant
// edit + the pinned test):
//   * WAR_MAX_DURATION_DAYS (14) — the timeout arm of resolution: a war
//     running this many game-days ends with the ATTACKER losing.
//   * WAR_REPUTATION_CB_FLOOR (-4000) — the Reputation casus belli: the
//     defender's leader reputation in the attacker's capital must sit below
//     this floor.
constexpr std::uint64_t WAR_MAX_DURATION_DAYS  = 14;
constexpr std::int32_t WAR_REPUTATION_CB_FLOOR = -4000;

//   * WAR_CHECK_INTERVAL_GAME_S (3600 = 1 game-hour) — the war progression
//     sweep's cadence (the collapse/attrition arm and the battle-spawn
//     attempt ride it; the timeout arm stays per-tick). The 8.3 soak's step
//     profile caught the per-tick shape re-walking both factions' full
//     membership (and the territory graph for the contested regions) at
//     ~4.5 ms/tick once a war ran — the doc's resolution rule is a state
//     condition, not a per-tick poll; the sweep lands at most one game-hour
//     late (DEVIATIONS.md, task 8.3 row). Awaiting Appendix A promotion
//     with the owner's next registry pass.
constexpr std::uint64_t WAR_CHECK_INTERVAL_GAME_S = 3'600;

// ---- task 6.3: territory claims (Appendix A.13 — A-113..A-115) -----------------
//
// A-113: CLAIM_COST = 20000 currency — the claim interaction's price (the
// claimant's i64 purse; an abandoned territory claims at half cost — the
// card's own half reading, CLAIM_COST / 2 at the call site).
inline constexpr std::int64_t CLAIM_COST = 20'000;

// A-114: ACTIVITY_MINUTES_REAL = 30 real minutes — one owner presence stint
// of at least this many VALIDATED (non-AFK, D-AFK-1) real minutes resets the
// abandonment timer.
inline constexpr std::uint64_t ACTIVITY_MINUTES_REAL = 30;

// A-115: ABANDON_DAYS_REAL = 14 real days — with no qualifying session for
// this many REAL days (and no active war involvement), the claim flags
// abandoned. Real seconds, not game seconds: D-TIME-1's real-clock rule (the
// two-clocks pitfall — the 14 days are real, the war-active check is game
// time).
inline constexpr std::uint64_t ABANDON_DAYS_REAL = 14;

} // namespace harrowmere
