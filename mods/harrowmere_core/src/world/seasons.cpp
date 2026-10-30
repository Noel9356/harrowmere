// seasons.cpp — task 3.8 implementation (see seasons.hpp for the contract).
//
// The two machines keep no hidden state: the season clock is a pure
// function of worldTime and the weather machine's cadence is the
// worldTime pair of the slice (see updateWeather). Everything the machines
// write lands in Region's SeasonState/WeatherState fields through
// WorldStore::region() — the storage task 1.4 declared for exactly this
// owner. Regions are visited in ascending id order everywhere (std::map,
// R7); the weather roll draws exactly one SeedRng::pct from the pinned
// "weather" stream per region per roll (R6/R8 — never the wall clock).

#include "world/seasons.hpp"

#include <array>
#include <cassert>
#include <cstddef>

#include "core/constants.hpp" // A-51 / A-52 / A-53, D-SHELTER-1, MIGRATION_WINTER_PULL
#include "core/math.hpp"      // mulDivU64 — the R3 wide-intermediate product

namespace harrowmere::world
{

namespace
{

// The one RNG stream of the weather machine (task 0.3's pinned list; the
// other pinned streams belong to their owning systems).
constexpr const char *kWeatherStream = "weather";

constexpr std::size_t kSeasonCount  = static_cast<std::size_t>(Season::COUNT);
constexpr std::size_t kWeatherKinds = static_cast<std::size_t>(Weather::COUNT);

// ---- the pinned markov table (card 3.8) -------------------------------------
//
// Card 3.8 pins the temperate row exactly — "clear 60% / rain 25% /
// storm 10% / fog 5%" — and the climate/season modifiers in prose:
// "winter swaps rain→snow; swamp +fog; mountain +snow". The modifier
// magnitudes are 3.8-pinned here (DEVIATIONS.md 2026-10-30; the same
// file-local pattern worldgen used for its card-unpinned climate weights):
//
//   * swamp  +fog   2500 (clear pays: 6000 -> 3500, fog 500 -> 3000 — fog
//     as frequent as rain in a swamp);
//   * mountain +snow 1500 (clear pays: 6000 -> 4500 — snow takes half of
//     clear's share); the winter rain→snow swap stacks onto it (4000);
//   * winter: the RAIN weight moves to SNOW wholesale ("swaps rain→snow")
//     in every climate.
//
// Every table row sums to SCALE (static_asserts below) so the cumulative
// walk over a pct() draw always resolves and no roll can escape the table.

constexpr std::uint32_t kClearBase = 6'000; // card 3.8: clear 60%
constexpr std::uint32_t kRainBase  = 2'500; // card 3.8: rain 25%
constexpr std::uint32_t kStormBase = 1'000; // card 3.8: storm 10%
constexpr std::uint32_t kFogBase   = 500;   // card 3.8: fog 5%

constexpr std::uint32_t kSwampFogBonus     = 2'500; // 3.8-pinned delta (see above)
constexpr std::uint32_t kMountainSnowBonus = 1'500; // 3.8-pinned delta (see above)

// The transition weights of one (climate, season) cell, ordered by Weather.
constexpr std::array<std::uint32_t, kWeatherKinds> weatherWeights(
    Climate climate, Season season
) noexcept
{
    std::array<std::uint32_t, kWeatherKinds> weights{kClearBase, kRainBase, kStormBase, kFogBase, 0};
    switch (climate)
    {
    case Climate::SWAMP:
        weights[static_cast<std::size_t>(Weather::CLEAR)] -= kSwampFogBonus;
        weights[static_cast<std::size_t>(Weather::FOG)] += kSwampFogBonus;
        break;
    case Climate::MOUNTAIN:
        weights[static_cast<std::size_t>(Weather::CLEAR)] -= kMountainSnowBonus;
        weights[static_cast<std::size_t>(Weather::SNOW)] += kMountainSnowBonus;
        break;
    case Climate::TEMPERATE:
    case Climate::FOREST:
    case Climate::COASTAL:
        break; // the base row, verbatim
    case Climate::COUNT:
    default:
        assert(false && "weather weights: climate outside the data-declared set");
        break; // deterministic recovery: the base row (common.md §3.4)
    }
    if (season == Season::WINTER)
    {
        // "winter swaps rain→snow" — the rain weight moves wholesale.
        weights[static_cast<std::size_t>(Weather::SNOW)] += weights[static_cast<std::size_t>(Weather::RAIN)];
        weights[static_cast<std::size_t>(Weather::RAIN)] = 0;
    }
    return weights;
}

constexpr std::uint32_t weightSum(
    const std::array<std::uint32_t, kWeatherKinds> &weights
) noexcept
{
    std::uint32_t sum = 0;
    for (const std::uint32_t weight : weights)
        sum += weight;
    return sum;
}

// Every (climate, season) cell sums to SCALE — the markov roll's
// completeness proof (R5: no floats; SCALE = 10'000, A-1).
static_assert(weightSum(weatherWeights(Climate::TEMPERATE, Season::SPRING)) == kScale);
static_assert(weightSum(weatherWeights(Climate::TEMPERATE, Season::WINTER)) == kScale);
static_assert(weightSum(weatherWeights(Climate::FOREST, Season::SPRING)) == kScale);
static_assert(weightSum(weatherWeights(Climate::FOREST, Season::WINTER)) == kScale);
static_assert(weightSum(weatherWeights(Climate::MOUNTAIN, Season::SPRING)) == kScale);
static_assert(weightSum(weatherWeights(Climate::MOUNTAIN, Season::WINTER)) == kScale);
static_assert(weightSum(weatherWeights(Climate::SWAMP, Season::SPRING)) == kScale);
static_assert(weightSum(weatherWeights(Climate::SWAMP, Season::WINTER)) == kScale);
static_assert(weightSum(weatherWeights(Climate::COASTAL, Season::SPRING)) == kScale);
static_assert(weightSum(weatherWeights(Climate::COASTAL, Season::WINTER)) == kScale);

// The card's pinned temperate row, spelled out where a reviewer reads first.
static_assert(weatherWeights(Climate::TEMPERATE, Season::SPRING)[static_cast<std::size_t>(Weather::CLEAR)] == 6'000);
static_assert(weatherWeights(Climate::TEMPERATE, Season::SPRING)[static_cast<std::size_t>(Weather::RAIN)] == 2'500);
static_assert(weatherWeights(Climate::TEMPERATE, Season::SPRING)[static_cast<std::size_t>(Weather::STORM)] == 1'000);
static_assert(weatherWeights(Climate::TEMPERATE, Season::SPRING)[static_cast<std::size_t>(Weather::FOG)] == 500);
static_assert(weatherWeights(Climate::TEMPERATE, Season::WINTER)[static_cast<std::size_t>(Weather::SNOW)] == 2'500);
static_assert(weatherWeights(Climate::TEMPERATE, Season::WINTER)[static_cast<std::size_t>(Weather::RAIN)] == 0);
static_assert(weatherWeights(Climate::MOUNTAIN, Season::WINTER)[static_cast<std::size_t>(Weather::SNOW)] == 4'000);
static_assert(weatherWeights(Climate::SWAMP, Season::SPRING)[static_cast<std::size_t>(Weather::FOG)] == 3'000);

// One markov roll for one region cell: a single pct() draw walked through
// the cumulative weights (R6: the only draw of a roll; R5: integer walk).
Weather rollWeather(
    Climate climate, Season season, core::SeedRng &rng
)
{
    const std::array<std::uint32_t, kWeatherKinds> weights = weatherWeights(climate, season);
    const std::uint32_t roll                               = rng.pct(kWeatherStream); // [0, SCALE)
    std::uint32_t cumulative                               = 0;
    for (std::size_t kind = 0; kind < kWeatherKinds; ++kind)
    {
        cumulative += weights[kind];
        if (roll < cumulative)
            return static_cast<Weather>(kind);
    }
    return Weather::CLEAR; // unreachable: every cell sums to SCALE (static_asserts above)
}

} // namespace

// ---- the state vocabularies ---------------------------------------------------

const char *weatherKey(
    Weather weather
) noexcept
{
    switch (weather)
    {
    case Weather::CLEAR:
        return "clear";
    case Weather::RAIN:
        return "rain";
    case Weather::STORM:
        return "storm";
    case Weather::FOG:
        return "fog";
    case Weather::SNOW:
        return "snow";
    case Weather::COUNT:
    default:
        assert(false && "weatherKey: weather outside the pinned table");
        return "clear"; // deterministic recovery (common.md §3.4)
    }
}

Season seasonFromIndex(
    std::uint32_t index
) noexcept
{
    if (index >= kSeasonCount)
    {
        assert(false && "seasonFromIndex: SeasonState.index outside the pinned cycle");
        return Season::SPRING; // deterministic recovery: the zero value
    }
    return static_cast<Season>(index);
}

Weather weatherFromKind(
    std::uint32_t kind
) noexcept
{
    if (kind >= kWeatherKinds)
    {
        assert(false && "weatherFromKind: WeatherState.kind outside the pinned table");
        return Weather::CLEAR; // deterministic recovery: the zero value
    }
    return static_cast<Weather>(kind);
}

// ---- the season clock -----------------------------------------------------------

Season seasonAt(
    std::uint64_t worldTime
) noexcept
{
    const std::uint64_t seasonIndex = worldTime / seasonLengthGameSeconds() % static_cast<std::uint64_t>(kSeasonCount);
    return seasonFromIndex(static_cast<std::uint32_t>(seasonIndex));
}

// ---- the two fixed-update slices --------------------------------------------

void updateSeasons(
    WorldStore &store, std::uint64_t worldTime
)
{
    const std::uint32_t index     = static_cast<std::uint32_t>(seasonAt(worldTime));
    const std::uint64_t startedAt = seasonStartWorldTime(worldTime);
    for (const auto &entry : store.regions())
    {
        Region *region = store.region(entry.first); // ascending regionId (std::map, R7)
        if (region == nullptr)
            continue; // unreachable: the map's own entry; guarded for the recovery pattern
        if (region->season.index == index && region->season.startedAtWorldTime == startedAt)
            continue; // no transition: the tick dirties nothing
        region->season.index              = index;
        region->season.startedAtWorldTime = startedAt;
    }
}

void updateWeather(
    WorldStore &store, std::uint64_t prevWorldTime, std::uint64_t worldTime, core::SeedRng &rng
)
{
    assert(prevWorldTime <= worldTime && "updateWeather: time moved backwards");
    // The roll cadence IS the game-hour boundary (card 3.8: "weather per
    // region per game-hour"): a slice rolls exactly when its two timestamps
    // sit in different game hours. No roll-cadence state anywhere — the
    // boundary test is pure worldTime math (R5/R8).
    if (prevWorldTime / GAME_HOUR_SECONDS == worldTime / GAME_HOUR_SECONDS)
        return;
    const std::uint64_t sinceWorldTime = worldTime / GAME_HOUR_SECONDS * GAME_HOUR_SECONDS;
    for (const auto &entry : store.regions())
    {
        Region *region = store.region(entry.first); // ascending regionId (std::map, R7)
        if (region == nullptr)
            continue;
        const Weather rolled = rollWeather(region->climate, seasonAt(worldTime), rng);
        if (static_cast<std::uint32_t>(rolled) == region->weather.kind)
            continue; // re-rolled where it already was: the weather did not begin again
        region->weather.kind           = static_cast<std::uint32_t>(rolled);
        region->weather.sinceWorldTime = sinceWorldTime;
    }
}

void tickSeasonsAndWeather(
    core::WorldClock &clock, WorldStore &store, core::SeedRng &rng
)
{
    const std::uint64_t prevWorldTime = clock.worldTime;
    clock.advanceTick();                                       // §4.4 step 0: the clocks (task 0.3)
    updateSeasons(store, clock.worldTime);                     // step 0: the season roll rides the clocks
    updateWeather(store, prevWorldTime, clock.worldTime, rng); // step 2: the weather machine
}

// ---- hook 1: crop yields ------------------------------------------------------

std::uint32_t seasonYieldMultiplier(
    Season season
) noexcept
{
    switch (season)
    {
    case Season::SPRING:
        return SEASON_YIELD_SPRING; // A-52
    case Season::SUMMER:
        return SEASON_YIELD_SUMMER; // A-52
    case Season::AUTUMN:
        return SEASON_YIELD_AUTUMN; // A-52
    case Season::WINTER:
        return SEASON_YIELD_WINTER; // A-52
    case Season::COUNT:
    default:
        assert(false && "seasonYieldMultiplier: season outside the pinned cycle");
        return SEASON_YIELD_SPRING; // deterministic recovery: the zero value's row
    }
}

std::uint32_t weatherYieldMultiplier(
    Weather weather
) noexcept
{
    switch (weather)
    {
    case Weather::STORM:
        return WEATHER_YIELD_STORM; // A-52
    case Weather::RAIN:
        return WEATHER_YIELD_RAIN; // A-52
    case Weather::CLEAR:
    case Weather::FOG:
    case Weather::SNOW:
        return kScale; // A-52 names only storm and rain; the rest are unmodified
    case Weather::COUNT:
    default:
        assert(false && "weatherYieldMultiplier: weather outside the pinned table");
        return kScale; // deterministic recovery: no modifier
    }
}

std::uint32_t cropYieldMultiplier(
    Season season, Weather weather
) noexcept
{
    // A-52's full product on the SCALE grid (R3: the product of two SCALE
    // values rides the wide-intermediate helper; 10^4 x 10^4 fits u64
    // trivially, the helper keeps the pattern uniform).
    return static_cast<std::uint32_t>(
        mulDivU64(seasonYieldMultiplier(season), weatherYieldMultiplier(weather), kScale)
    );
}

// ---- hook 2: NPC migration ----------------------------------------------------

std::uint32_t migrationClimatePull(
    Season season, Climate climate
) noexcept
{
    // "Winter adds pull toward warmer-climate regions" (card 3.8). The
    // design reference names no warmth ordering over the five data-declared
    // climates, so 3.8 pins the reading (DEVIATIONS.md 2026-10-30): the
    // maritime-mild pair — coastal and temperate — is "warm"; forest,
    // swamp and mountain are not. 3.4 (the consumer) may re-pin the set
    // through a deviation row once migration lands.
    const bool winter = season == Season::WINTER;
    const bool warm   = climate == Climate::COASTAL || climate == Climate::TEMPERATE;
    return winter && warm ? MIGRATION_WINTER_PULL : 0;
}

// ---- hook 3: event probabilities ----------------------------------------------

std::uint32_t weatherModifier(
    Weather weather, const std::map<std::string, std::uint32_t> &weatherAffinity
) noexcept
{
    const auto it = weatherAffinity.find(weatherKey(weather));
    return it == weatherAffinity.end() ? 0 : it->second;
}

// ---- hook 4: exposure -----------------------------------------------------------

bool exposureActive(
    Weather weather
) noexcept
{
    return weather == Weather::STORM || weather == Weather::SNOW;
}

std::uint32_t exposureDamagePerTick() noexcept
{
    return EXPOSURE_DPS; // A-53
}

bool isSheltered(
    const Position &p, const Position &settlementCenter
) noexcept
{
    // D-SHELTER-1, boundary inclusive. The threshold is squared instead of
    // paying a sqrt (R5: the comparison is f64, the sim math stays integer
    // elsewhere).
    const double shelterRadiusSquared = SHELTER_RADIUS_M * SHELTER_RADIUS_M;
    return distanceSquaredM(p, settlementCenter) <= shelterRadiusSquared;
}

} // namespace harrowmere::world
