#ifndef HARROWMERE_CORE_WORLD_SEASONS_HPP
#define HARROWMERE_CORE_WORLD_SEASONS_HPP

// seasons.hpp — the regional season clock and the weather state machine
// (task 3.8; design reference "Seasons and Weather: seasons and weather are
// regionally dynamic. They affect crop yields, NPC migration, event
// probabilities, and exposure risk.").
//
// Storage vs. machinery: Region (world/region.hpp) has carried the
// SeasonState/WeatherState fields since task 1.4 ("owned by task 3.8,
// declared here"); THIS header is the owner those fields were waiting for.
// It defines the two state vocabularies the ordinal fields encode, the two
// fixed-update slices that advance them, and the four effect hooks the
// design reference names.
//
// ---- Phase-1 wiring (card 3.8) ----------------------------------------------
//
// There is no Scheduler in this phase. The two updates are standalone
// functions of (world state, worldTime[, RNG]) — no hidden clocks, no
// singletons — so any subscriber shape can drive them:
//
//   * tickSeasonsAndWeather() composes one fixed-update slice in the exact
//     order §4.4 pins for the Phase-1 subscription: the clocks advance
//     (0.3's WorldClock::advanceTick — §4.4 step 0), the season rolls with
//     the clocks (step 0's "season roll"), then the weather machine runs
//     (step 2).
//
//   * When 3.1's Scheduler lands (Phase 2) it registers updateSeasons into
//     step 0 and updateWeather into step 2 and owns the clock advance — a
//     RE-REGISTRATION of these functions, not a rewrite (the card's own
//     migration rule; the Phase-1 tests drive the functions directly and
//     stay green unchanged).
//
// ---- The four effect hooks (frozen signatures; card 3.8) --------------------
//
// Each hook is the one-line interface its owner task already expects. Their
// consumers arrive with owner tasks in later phases — nothing inside this
// phase calls them (the exposure sweep's Phase-1 proof runs on fakes):
//
//   1. Crop yields      -> owner: 3.4 (farmer duty output)
//                          cropYieldMultiplier(season, weather)
//   2. NPC migration    -> owner: 3.4 (migration pull)
//                          migrationClimatePull(season, climate)
//   3. Event probabilit -> owner: 5.1 (event spawn rolls)
//                          weatherModifier(weather, weatherAffinity)
//   4. Exposure risk    -> owner: 3.3/3.4 (Phase 2 — needs characters and
//                          applyDamage; deaths route to onEnvironmentDeath)
//                          exposureActive / exposureDamagePerTick /
//                          isSheltered / applyExposure
//
// Determinism (R5–R8): the season roll is a pure function of worldTime — no
// RNG. The weather machine draws ONLY from SeedRng's pinned "weather"
// stream (R6/R8), exactly one draw per region per game-hour roll, in
// ascending regionId order (std::map iteration, R7). No floating point
// enters the state machines (R5); the one f64 consumer is the shelter
// radius comparison, routed through region.hpp's D-WORLD-1 distance helper.

#include <cstdint>
#include <map>
#include <string>

#include "core/clock.hpp"   // WorldClock — the one heartbeat (task 0.3, D-TIME-1)
#include "core/rng.hpp"     // SeedRng — the pinned named streams (R6)
#include "core/types.hpp"   // characterId, Position (R10)
#include "world/region.hpp" // Region, SeasonState/WeatherState, Climate
#include "world/world.hpp"  // WorldStore — the regions the machines advance

namespace harrowmere::world
{

// ---- the two state vocabularies (the ordinals Region carries) ---------------
//
// SeasonState.index / WeatherState.kind (world/region.hpp) store these
// ordinals. The zero values are deliberate: a fresh world (worldTime 0,
// worldgen's defaults) reads as spring, clear — the SeasonState/WeatherState
// zero-initializers need no worldgen writes to be correct.

// The pinned season cycle (card 3.8: "seasons cycle spring→summer→autumn→
// winter"). COUNT sentinels per R9 — the cycle length is the sentinel.
enum class Season : std::uint32_t
{
    SPRING = 0,
    SUMMER,
    AUTUMN,
    WINTER,
    COUNT
};

// The weather kinds of the markov table (card 3.8's pinned transition
// table: clear / rain / storm / fog, with snow entering via the winter
// rain→snow swap and the mountain climate). CLEAR = 0 so a default
// WeatherState is "clear since the world epoch".
enum class Weather : std::uint32_t
{
    CLEAR = 0,
    RAIN,
    STORM,
    FOG,
    SNOW,
    COUNT
};

// The data key a weather kind answers to ("clear"/"rain"/"storm"/"fog"/
// "snow") — the design reference's event-template field
// "weatherAffinity": { "storm": 2000 } keys its map by these names, and
// 5.1's loader stores that map verbatim.
const char *weatherKey(Weather weather) noexcept;

// Ordinal -> enum, the read-side guard for Region's ordinal fields: an
// out-of-range ordinal is a programmer error (debug assert; the
// deterministic-recovery pattern returns the zero value in release —
// common.md §3.4, never undefined behavior).
Season seasonFromIndex(std::uint32_t index) noexcept;
Weather weatherFromKind(std::uint32_t kind) noexcept;

// ---- the season clock (pure worldTime math — no RNG, no state) --------------

// A-51 × A-3: one season of game seconds (2'592'000 = 30 game-days).
inline std::uint64_t seasonLengthGameSeconds() noexcept
{
    return SEASON_LENGTH_GAME_SECONDS;
}

// The season `worldTime` sits in. Cycle length = Season::COUNT (R9); the
// cycle order is the enum order (spring -> summer -> autumn -> winter).
Season seasonAt(std::uint64_t worldTime) noexcept;

// The worldTime the current season began at (the exact A-51 boundary at or
// before `worldTime`) — what SeasonState::startedAtWorldTime stores, so the
// UI reads "how long has this season been going on" without re-deriving it.
inline std::uint64_t seasonStartWorldTime(
    std::uint64_t worldTime
) noexcept
{
    return worldTime / seasonLengthGameSeconds() * seasonLengthGameSeconds();
}

// ---- the two fixed-update slices (§4.4 steps 0 and 2) -----------------------

// §4.4 step 0's season roll ("advance worldTime, day/night phase, season
// roll"). Writes every region's SeasonState: the season index derived from
// worldTime, and startedAtWorldTime at the A-51 boundary — only where
// either actually changed (a no-transition tick dirties nothing). Regions
// are visited in ascending id order (R7).
void updateSeasons(WorldStore &store, std::uint64_t worldTime);

// §4.4 step 2's weather machine ("weather state machine"). Card 3.8: a
// markov roll per region per GAME HOUR on the pinned "weather" stream —
// the roll lands on the tick that crosses a game-hour boundary, so the
// machine keeps NO roll-cadence state of its own (the worldTime pair is
// the cadence; nothing new to persist for 7.1). Exactly one draw per
// region per call (a tick that jumps several hour boundaries rolls once —
// the table is a per-hour markov chain, not a per-boundary one), in
// ascending regionId order (R7). prevWorldTime <= worldTime is the caller
// contract (debug-asserted).
//
// On a kind change WeatherState::sinceWorldTime stamps the crossed hour
// boundary — "when this weather began"; a re-rolled kind that lands where
// it already was leaves the stamp alone (the weather did not begin again).
void updateWeather(WorldStore &store, std::uint64_t prevWorldTime, std::uint64_t worldTime, core::SeedRng &rng);

// The Phase-1 fixed-update slice (card 3.8's Phase-1 wiring): the clocks
// advance, the season rolls with the clocks (§4.4 step 0's order), then
// the weather machine (step 2). Phase 2's Scheduler (3.1) replaces this
// composition by registering the two updates above into the step table —
// re-registration, not a rewrite; the Phase-1 tests keep calling both.
void tickSeasonsAndWeather(core::WorldClock &clock, WorldStore &store, core::SeedRng &rng);

// ---- hook 1: crop yields (owner: 3.4, the farmer-duty consumer) -------------

// A-52 season half on the SCALE grid: spring 10000, summer 12000,
// autumn 8000, winter 3000. Winter famines drive migration (doc hook).
std::uint32_t seasonYieldMultiplier(Season season) noexcept;

// A-52 weather half on the SCALE grid: storm 7000, rain 11000; every other
// state carries no modifier (SCALE). The winter rain→snow swap is a
// TRANSITION-TABLE substitution (the markov roll routes winter's rain
// weight into snow) — snow itself is not rain for the yield table.
std::uint32_t weatherYieldMultiplier(Weather weather) noexcept;

// The farmer duty's one-line multiplier (A-52 full product, R3 wide
// intermediate): farmer duty output × seasonYieldMultiplier ×
// weatherYieldMultiplier, collapsed to × cropYieldMultiplier(season,
// weather).
std::uint32_t cropYieldMultiplier(Season season, Weather weather) noexcept;

// ---- hook 2: NPC migration (owner: 3.4, the migration pull) -----------------

// Card 3.8: "winter adds pull toward warmer-climate regions (+1000; reads
// Region.climate)". The SCALE-grid pull bonus a candidate region earns in
// winter when its climate counts as warm; 0 otherwise. 3.4 adds this to
// the region's pull score when it ranks candidate regions.
std::uint32_t migrationClimatePull(Season season, Climate climate) noexcept;

// ---- hook 3: event probabilities (owner: 5.1, the event engine) -------------

// Card 3.8: "5.1 reads weatherModifier(eventTemplate): storm +2000 for
// disaster-flavored templates (template field weatherAffinity)". The
// design reference's event template carries
// "weatherAffinity": { "<weatherKey>": bonus-on-SCALE }; this returns the
// bonus the given weather contributes to that template's spawn roll (0
// when the template names none). 5.1 stores the field as
// std::map<std::string, std::uint32_t> keyed by weatherKey() — the
// frozen shape this signature pins.
std::uint32_t weatherModifier(Weather weather, const std::map<std::string, std::uint32_t> &weatherAffinity) noexcept;

// ---- hook 4: exposure risk (owner: 3.3/3.4, Phase 2) -------------------------
//
// The card pins the behavior: "during storm/snow: every NEAR character
// unsheltered (not inside settlement radius, D-SHELTER-1 = 50 m of a
// building marker) takes EXPOSURE_DPS (A-53 = 2 hp/tick) through
// applyDamage with source = 0 (environment) — deaths route to
// onEnvironmentDeath (3.3)". 3.8 owns the per-region gate, the per-tick
// damage and the shelter math below; the NEAR iteration and the real
// applyDamage binding arrive with the character stores (Phase 2) — the
// sweep template below is the frozen seam they instantiate.

// Storm and snow expose; the other states do not.
bool exposureActive(Weather weather) noexcept;

// A-53: the hp a single exposed tick costs (2). Meaningful only while
// exposureActive() holds for the region's weather.
std::uint32_t exposureDamagePerTick() noexcept;

// D-SHELTER-1: sheltered = within SHELTER_RADIUS_M (50 m) of a building
// marker; "settlement center = sheltered in v1", so the caller passes the
// settlement center (v1's building marker — e.g. the anchor region center
// of the character's territory). Distance is x/z plane f64 through
// region.hpp's D-WORLD-1 helper; the boundary is inclusive (50.0 m is
// sheltered).
bool isSheltered(const Position &p, const Position &settlementCenter) noexcept;

// One NEAR-tier member (3.5's tier, as planned) of the region being
// swept: who they are and where they stand. Phase 2's stores produce
// these; Phase 1's tests feed fakes.
struct ExposureCandidate
{
    characterId id{};    // 0 never appears (R10: id 0 is reserved)
    Position position{}; // x/z shelter-relevant; y cosmetic
};

// The one-line exposure sweep (the card's frozen interface; NOTHING calls
// it in Phase 1 — the owner tests TestExposure_StormDamagesUnshelteredOnly
// / TestExposure_EnvironmentDeathRoute land with 3.3, Phase 2):
//
//   for every candidate in `nearCharacters` (the NEAR-tier members of
//   `region`) that is NOT sheltered by `shelterAnchor`, apply
//   exposureDamagePerTick() THROUGH `applyDamage` with source = 0
//   (environment). Exposure is DAMAGE, not HP subtraction — the pitfall
//   the card pins: it must ride the one damage path so the death handler
//   chain stays single-path (deaths route to 3.3's onEnvironmentDeath).
//
// `applyDamage` is the consumer's damage entry, called as
//     applyDamage(characterId who, std::uint32_t hp, characterId source)
// with source = kEnvironmentSource (0 — the docs' convention shared with
// CharacterDied's environment killer). `shelterAnchor` is the settlement
// center of the swept bubble (a 50 m NEAR bubble can never span two
// settlements, so one anchor per sweep is exact in v1).
//
// Template on the range/sink types because the character stores do not
// exist yet; the Phase-2 owner instantiates it against the real stores
// without rework — this signature is the freeze.
inline constexpr characterId kEnvironmentSource = 0; // docs convention: source 0 = environment

template <typename CharacterRange, typename ApplyDamageFn>
void applyExposure(
    const Region &region,
    const CharacterRange &nearCharacters,
    const Position &shelterAnchor,
    ApplyDamageFn &&applyDamage
)
{
    if (!exposureActive(weatherFromKind(region.weather.kind)))
        return;
    const std::uint32_t damagePerTick = exposureDamagePerTick();
    for (const ExposureCandidate &candidate : nearCharacters)
    {
        if (isSheltered(candidate.position, shelterAnchor))
            continue;
        applyDamage(candidate.id, damagePerTick, kEnvironmentSource);
    }
}

} // namespace harrowmere::world

#endif // HARROWMERE_CORE_WORLD_SEASONS_HPP
