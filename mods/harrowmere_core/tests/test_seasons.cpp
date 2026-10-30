// test_seasons.cpp — Task 3.8's test suite.
//
// Implements the tests pinned in phase-1-walk.md card 3.8's Tests block
// (names verbatim — common.md §3.5: "Test names are pinned; keep them"):
//
//   TestSeasons_CycleLengthAndOrder — the A-51 season clock and the pinned
//     spring→summer→autumn→winter cycle, plus the SeasonState writes.
//   TestWeather_MarkovStaysInTable — every roll lands in the (climate,
//     season) transition table; winter swaps rain→snow; the climate
//     modifiers fire; no kind outside the table ever appears.
//
// The remaining cases pin the frozen hook signatures and the Phase-1
// wiring against fakes (card 3.8: "Write yours against the frozen hook
// signatures so those tests can be added later without rework"). The
// consumer-side tests the card names — TestExposure_StormDamagesUnshelteredOnly,
// TestExposure_EnvironmentDeathRoute (3.3), TestYields_SeasonMultiplierApplied
// (the farmer duty) — belong to their owner tasks in Phase 2 and are
// deliberately NOT defined here.
//
// Test harness: doctest v2.4.12 (vendored per task 0.6); no main here —
// tests/main.cpp owns DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/clock.hpp" // DAY_LENGTH_SECONDS (A-3) — the derivation check
#include "core/constants.hpp"
#include "core/rng.hpp"
#include "data/laws.hpp" // the LawSet WorldStore resolves root fallbacks against
#include "world/region.hpp"
#include "world/seasons.hpp"
#include "world/world.hpp"

using namespace harrowmere;
using namespace harrowmere::world;

namespace
{

// The store fixture: a WorldStore needs a defaultLaws snapshot to construct
// (production: DataRegistry::defaultLaws()); tests hand it a distinctive
// empty set. Member order keeps the laws alive for the store's lifetime.
struct SeasonFixture
{
    data::LawSet defaultLaws{};
    WorldStore store{defaultLaws};

    SeasonFixture()
    {
        defaultLaws.key = "default";
    }

    Region *addRegion(
        regionId id, Climate climate
    )
    {
        Region region;
        region.id      = id;
        region.climate = climate;
        store.addRegion(region);
        return store.region(id);
    }
};

// The high 64 bits of one named stream's pcg64 state — how a test counts
// draws without knowing pcg64's internals (rng.hpp: state() carries
// "<stream>:hi"/"<stream>:lo" per pinned stream).
std::uint64_t streamHi(
    const core::SeedRng &rng, const char *stream
)
{
    const std::string suffix = std::string(stream) + ":hi";
    for (const auto &entry : rng.state())
    {
        if (entry.first == suffix)
            return entry.second;
    }
    return 0; // unreachable: every pinned stream appears in state()
}

// The card-pinned temperate cumulative thresholds (clear 60 / rain 25 /
// storm 10 / fog 5): which weather a given pct() draw resolves to. The
// cadence test re-derives the machine's roll independently of the
// implementation's own table walk — the table pinned here is card text.
Weather expectedRollFromCardTable(
    std::uint32_t pctDraw
)
{
    if (pctDraw < 6'000)
        return Weather::CLEAR;
    if (pctDraw < 8'500)
        return Weather::RAIN;
    if (pctDraw < 9'500)
        return Weather::STORM;
    return Weather::FOG;
}

} // namespace

// ---------------------------------------------------------------------------
// TestSeasons_CycleLengthAndOrder (pinned)
// A-51: 30 game-days per season; the cycle is spring→summer→autumn→winter
// and wraps; seasonAt reads pure worldTime math.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestSeasons_CycleLengthAndOrder"
)
{
    // The seconds form of A-51 is SEASON_LENGTH_GAME_DAYS x A-3 (the
    // constants header keeps the literal derivation; this is the tie).
    static_assert(
        SEASON_LENGTH_GAME_SECONDS == static_cast<std::uint64_t>(SEASON_LENGTH_GAME_DAYS) * core::DAY_LENGTH_SECONDS
    );
    CHECK(seasonLengthGameSeconds() == 2'592'000ull);
    CHECK(SEASON_LENGTH_GAME_DAYS == 30);

    // The cycle at the exact boundaries: [0, len) spring, [len, 2len)
    // summer, [2len, 3len) autumn, [3len, 4len) winter, then spring again.
    const std::uint64_t len = seasonLengthGameSeconds();
    CHECK(seasonAt(0) == Season::SPRING);
    CHECK(seasonAt(len - 1) == Season::SPRING);
    CHECK(seasonAt(len) == Season::SUMMER);
    CHECK(seasonAt(2 * len - 1) == Season::SUMMER);
    CHECK(seasonAt(2 * len) == Season::AUTUMN);
    CHECK(seasonAt(3 * len - 1) == Season::AUTUMN);
    CHECK(seasonAt(3 * len) == Season::WINTER);
    CHECK(seasonAt(4 * len - 1) == Season::WINTER);
    CHECK(seasonAt(4 * len) == Season::SPRING); // the wrap: one year = one full cycle

    // The started-at boundary: the exact A-51 boundary at or before now.
    CHECK(seasonStartWorldTime(0) == 0);
    CHECK(seasonStartWorldTime(len + 1) == len);
    CHECK(seasonStartWorldTime(4 * len - 1) == 3 * len);

    // The ordinal read-back guard and the R9 sentinel.
    CHECK(seasonFromIndex(0) == Season::SPRING);
    CHECK(seasonFromIndex(3) == Season::WINTER);
    CHECK(static_cast<std::uint32_t>(Season::COUNT) == 4); // the cycle length is the sentinel
}

// ---------------------------------------------------------------------------
// TestSeasons_Update_WritesIndexAndStartedAt
// updateSeasons writes every region's SeasonState (ascending id order, R7)
// and leaves a no-transition tick untouched.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestSeasons_Update_WritesIndexAndStartedAt"
)
{
    SeasonFixture fixture;
    Region *a = fixture.addRegion(0, Climate::TEMPERATE);
    Region *b = fixture.addRegion(7, Climate::SWAMP);
    Region *c = fixture.addRegion(255, Climate::MOUNTAIN);

    // worldTime 0: spring since the world epoch — the defaults already say
    // exactly that, so the write is a no-op.
    updateSeasons(fixture.store, 0);
    CHECK(a->season.index == static_cast<std::uint32_t>(Season::SPRING));
    CHECK(a->season.startedAtWorldTime == 0);

    // 45 game-days: summer since day 30 — every region, whatever its
    // climate, carries the same season clock (card 3.8).
    const std::uint64_t day = core::DAY_LENGTH_SECONDS;
    updateSeasons(fixture.store, 45 * day);
    for (const Region *region : {a, b, c})
    {
        CHECK(region->season.index == static_cast<std::uint32_t>(Season::SUMMER));
        CHECK(region->season.startedAtWorldTime == 30 * day);
    }

    // A later tick inside the same season: no transition, the started-at
    // stamp stays the boundary the season began at.
    updateSeasons(fixture.store, 46 * day);
    CHECK(a->season.startedAtWorldTime == 30 * day);

    // The wrap: day 125 sits in year 2's spring (started at day 120).
    updateSeasons(fixture.store, 125 * day);
    CHECK(a->season.index == static_cast<std::uint32_t>(Season::SPRING));
    CHECK(a->season.startedAtWorldTime == 120 * day);
}

// ---------------------------------------------------------------------------
// TestWeather_MarkovStaysInTable (pinned)
// 30'000 game-hour rolls per climate: every observed kind is in that
// climate/season cell's table; winter never rolls rain ("swaps rain→snow")
// and does roll snow; the swamp fog modifier and the mountain snow modifier
// both fire; non-winter temperate weather never rolls snow.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWeather_MarkovStaysInTable"
)
{
    SeasonFixture fixture;
    constexpr std::size_t kClimates = static_cast<std::size_t>(Climate::COUNT);
    constexpr std::size_t kKinds    = static_cast<std::size_t>(Weather::COUNT);
    for (std::size_t c = 0; c < kClimates; ++c)
        fixture.addRegion(static_cast<regionId>(c), static_cast<Climate>(c));

    core::SeedRng rng;
    rng.seedAll(7u);

    constexpr std::uint32_t kRollsPerClimate = 30'000;
    // seen[climate][kind][winter window?]
    bool seen[kClimates][kKinds][2]{};
    std::size_t kindsOutOfRange = 0;

    const std::uint64_t hour = GAME_HOUR_SECONDS;
    for (std::uint32_t step = 0; step < kRollsPerClimate; ++step)
    {
        const std::uint64_t prev = step * hour;
        const std::uint64_t now  = prev + hour;
        const bool winter        = seasonAt(now) == Season::WINTER;
        updateWeather(fixture.store, prev, now, rng);
        for (std::size_t c = 0; c < kClimates; ++c)
        {
            const Region *region   = fixture.store.region(static_cast<regionId>(c));
            const std::size_t kind = static_cast<std::size_t>(region->weather.kind);
            if (kind >= kKinds)
            {
                ++kindsOutOfRange; // counted, not CHECKed per roll (30k x 5 rolls)
                continue;
            }
            seen[c][kind][winter ? 1 : 0] = true;
        }
    }
    CHECK(kindsOutOfRange == 0);

    // Every observed kind is in the (climate, window) table. Card 3.8's
    // table: the base row (temperate/forest/coastal) is clear/rain/storm/
    // fog with snow only via the winter swap; swamp adds fog; mountain adds
    // snow in every season; winter swaps rain→snow everywhere.
    for (std::size_t c = 0; c < kClimates; ++c)
    {
        const Climate climate = static_cast<Climate>(c);
        for (const bool winter : {false, true})
        {
            for (std::size_t kind = 0; kind < kKinds; ++kind)
            {
                const Weather weather = static_cast<Weather>(kind);
                const bool allowed = winter ? (weather != Weather::RAIN) // the swap is wholesale: rain is out of table
                                            : (weather != Weather::SNOW || climate == Climate::MOUNTAIN);
                const bool unseenOrAllowed = !seen[c][kind][winter ? 1 : 0] || allowed;
                CHECK_MESSAGE(unseenOrAllowed, "weather kind outside the (climate, season) table");
            }
        }
    }

    // The modifiers actually fire (the walk is long enough that every
    // in-table kind of every climate occurs at least once).
    const auto observed = [&](std::size_t c, Weather weather, bool winter) {
        return seen[c][static_cast<std::size_t>(weather)][winter ? 1 : 0];
    };
    CHECK(observed(0, Weather::CLEAR, false)); // temperate
    CHECK(observed(0, Weather::RAIN, false));  // the base row's rain
    CHECK(observed(0, Weather::STORM, false)); // storms happen
    CHECK(observed(0, Weather::FOG, false));   // the base row's fog
    CHECK(observed(0, Weather::SNOW, true));   // winter swapped rain→snow
    CHECK(observed(2, Weather::SNOW, false));  // mountain: snow in every season
    CHECK(observed(2, Weather::RAIN, false));  // mountain keeps rain off-winter
    CHECK(observed(3, Weather::FOG, false));   // swamp: the heavy fog fires
    CHECK(observed(4, Weather::CLEAR, true));  // coastal winter: clear stays possible
    for (std::size_t c = 0; c < kClimates; ++c)
        CHECK_FALSE(observed(c, Weather::RAIN, true)); // winter rain: out of EVERY climate's table
}

// ---------------------------------------------------------------------------
// TestWeather_Cadence_RollsOncePerHourBoundary
// The game-hour cadence: no boundary crossed -> no draw and no write; a
// boundary crossed -> exactly one draw, resolved by the card's temperate
// table; a multi-boundary slice still rolls once (per-hour markov chain).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWeather_Cadence_RollsOncePerHourBoundary"
)
{
    SeasonFixture fixture;
    fixture.addRegion(0, Climate::TEMPERATE);

    // No boundary inside the slice: no draw (the "weather" stream is
    // untouched), no state change.
    core::SeedRng rng;
    rng.seedAll(3u);
    const std::uint64_t hiBefore = streamHi(rng, "weather");
    updateWeather(fixture.store, 0, GAME_HOUR_SECONDS - 1, rng);
    CHECK(streamHi(rng, "weather") == hiBefore);
    const Region *region = fixture.store.region(0);
    CHECK(region->weather.kind == static_cast<std::uint32_t>(Weather::CLEAR));
    CHECK(region->weather.sinceWorldTime == 0);

    // Crossing the first boundary rolls exactly once: a twin rng that makes
    // one pct() draw lands in the same stream state.
    updateWeather(fixture.store, GAME_HOUR_SECONDS - 1, GAME_HOUR_SECONDS, rng);
    core::SeedRng twin;
    twin.seedAll(3u);
    (void)twin.pct("weather");
    CHECK(streamHi(rng, "weather") == streamHi(twin, "weather"));

    // The roll's outcome is the card table walked on that draw — the
    // machine's first roll re-derived independently of the implementation.
    core::SeedRng oracle;
    oracle.seedAll(3u);
    const std::uint32_t firstDraw = oracle.pct("weather"); // the draw the update consumed
    CHECK(region->weather.kind == static_cast<std::uint32_t>(expectedRollFromCardTable(firstDraw)));
    if (region->weather.kind != static_cast<std::uint32_t>(Weather::CLEAR))
        CHECK(region->weather.sinceWorldTime == GAME_HOUR_SECONDS); // stamped at the boundary

    // Staying inside the hour: no further draw. Crossing the next boundary:
    // exactly one more (the twin makes its second draw).
    updateWeather(fixture.store, GAME_HOUR_SECONDS, 2 * GAME_HOUR_SECONDS - 1, rng);
    CHECK(streamHi(rng, "weather") == streamHi(twin, "weather"));
    updateWeather(fixture.store, 2 * GAME_HOUR_SECONDS - 1, 2 * GAME_HOUR_SECONDS, rng);
    (void)twin.pct("weather");
    CHECK(streamHi(rng, "weather") == streamHi(twin, "weather"));

    // A slice spanning several hour boundaries rolls once, not once per
    // boundary (the card pins one roll per region per game-hour slice).
    updateWeather(fixture.store, 2 * GAME_HOUR_SECONDS, 5 * GAME_HOUR_SECONDS, rng);
    (void)twin.pct("weather");
    CHECK(streamHi(rng, "weather") == streamHi(twin, "weather"));
}

// ---------------------------------------------------------------------------
// TestWeather_SeedDeterminism_SameSeedSameSequence
// R6: same seed -> identical weather histories; a different seed diverges.
// Two 16-region worlds, 5'000 hour-rolls each.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestWeather_SeedDeterminism_SameSeedSameSequence"
)
{
    const auto buildWorld = [](SeasonFixture &fixture) {
        for (regionId id = 0; id < 16; ++id)
            fixture.addRegion(id, static_cast<Climate>(id % static_cast<regionId>(Climate::COUNT)));
    };

    SeasonFixture left;
    SeasonFixture right;
    SeasonFixture oddSeed;
    buildWorld(left);
    buildWorld(right);
    buildWorld(oddSeed);

    core::SeedRng leftRng;
    core::SeedRng rightRng;
    core::SeedRng oddRng;
    leftRng.seedAll(1234u);
    rightRng.seedAll(1234u);
    oddRng.seedAll(4321u);

    constexpr std::uint32_t kSteps = 5'000;
    const std::uint64_t hour       = GAME_HOUR_SECONDS;
    std::size_t mismatches         = 0;
    bool diverged                  = false;
    for (std::uint32_t step = 0; step < kSteps; ++step)
    {
        const std::uint64_t prev = step * hour;
        const std::uint64_t now  = prev + hour;
        updateWeather(left.store, prev, now, leftRng);
        updateWeather(right.store, prev, now, rightRng);
        updateWeather(oddSeed.store, prev, now, oddRng);
        for (regionId id = 0; id < 16; ++id)
        {
            const Region *l = left.store.region(id);
            const Region *r = right.store.region(id);
            if (l->weather.kind != r->weather.kind || l->weather.sinceWorldTime != r->weather.sinceWorldTime)
                ++mismatches;
            diverged = diverged || l->weather.kind != oddSeed.store.region(id)->weather.kind;
        }
    }
    CHECK(mismatches == 0);
    CHECK(diverged); // a different seed produced a different world somewhere
}

// ---------------------------------------------------------------------------
// TestSeasonWeather_YearFastForward_AllTransitionsInOrder
// The card's Done-when: "a headless test fast-forwards a full year of ticks
// with visible season/weather transitions". The fast-forward walks 365
// game-days in 6-game-hour slices over the full 256-region grid: the season
// transitions land exactly on the A-51 boundaries in the pinned order, and
// the weather machine rolls visibly all year.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestSeasonWeather_YearFastForward_AllTransitionsInOrder"
)
{
    SeasonFixture fixture;
    for (regionId id = 0; id < REGION_COUNT; ++id)
        fixture.addRegion(id, static_cast<Climate>(id % static_cast<regionId>(Climate::COUNT)));

    core::SeedRng rng;
    rng.seedAll(42u);

    // The fast-forward: 365 game-days in 6-game-hour slices (the updates
    // are worldTime-parameterized, so the walk pays no per-tick cost for
    // the 6-game-second default ticks; the exact per-tick path is
    // TestSeasonWeather_TickPath_OrderAndCadence's job).
    const std::uint64_t day      = core::DAY_LENGTH_SECONDS;
    const std::uint64_t stepSize = 6 * GAME_HOUR_SECONDS;
    const std::uint64_t yearEnd  = 365 * day;

    std::vector<std::pair<std::uint64_t, Season>> transitions;
    std::uint32_t weatherChanges  = 0;
    std::size_t invariantFailures = 0;
    std::uint64_t worldTime       = 0;
    while (worldTime < yearEnd)
    {
        const std::uint64_t prev = worldTime;
        worldTime                = std::min(worldTime + stepSize, yearEnd);
        const Season before      = seasonAt(prev);

        // One updateSeasons + one updateWeather per slice — each sweep owns
        // the whole store (the update functions are store-wide by design).
        std::vector<std::uint32_t> kindsBefore;
        kindsBefore.reserve(REGION_COUNT);
        for (regionId id = 0; id < REGION_COUNT; ++id)
            kindsBefore.push_back(fixture.store.region(id)->weather.kind);

        updateSeasons(fixture.store, worldTime);
        updateWeather(fixture.store, prev, worldTime, rng);

        for (regionId id = 0; id < REGION_COUNT; ++id)
        {
            Region *region = fixture.store.region(id);
            if (id == 0 && region->season.index != static_cast<std::uint32_t>(before))
                transitions.emplace_back(seasonStartWorldTime(worldTime), seasonFromIndex(region->season.index));
            if (region->season.startedAtWorldTime > worldTime || region->weather.sinceWorldTime > worldTime)
                ++invariantFailures; // stamps never come from the future
            if (region->weather.kind != kindsBefore[static_cast<std::size_t>(id)])
                ++weatherChanges;
        }
    }

    // Season transitions: exactly at day 30, 60, ..., 360 (12 of them in a
    // 365-day year), in the pinned order, wrapping spring after winter.
    REQUIRE(transitions.size() == 12);
    for (std::size_t i = 0; i < transitions.size(); ++i)
    {
        CHECK(transitions[i].first == static_cast<std::uint64_t>(i + 1) * seasonLengthGameSeconds());
        CHECK(
            transitions[i].second
            == seasonFromIndex(static_cast<std::uint32_t>((i + 1) % static_cast<std::uint32_t>(Season::COUNT)))
        );
    }

    // Visible weather: the year rolled plenty of changes across the grid.
    CHECK(weatherChanges > 0);
    CHECK(invariantFailures == 0);

    // The final state is well-formed everywhere.
    for (regionId id = 0; id < REGION_COUNT; ++id)
    {
        const Region *region = fixture.store.region(id);
        CHECK(region->weather.kind < static_cast<std::uint32_t>(Weather::COUNT));
        CHECK(region->weather.sinceWorldTime <= yearEnd);
    }
}

// ---------------------------------------------------------------------------
// TestSeasonWeather_TickPath_OrderAndCadence
// The Phase-1 composition (tickSeasonsAndWeather) at the engine's default
// pacing (10 Hz fixed updates, TIME_SCALE 60 -> 6 game-seconds per tick):
// the clock advances, the season roll rides the post-advance worldTime, the
// weather machine rolls exactly once per region per crossed hour boundary.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestSeasonWeather_TickPath_OrderAndCadence"
)
{
    SeasonFixture fixture;
    constexpr regionId kRegions = 8;
    for (regionId id = 0; id < kRegions; ++id)
        fixture.addRegion(id, static_cast<Climate>(id % static_cast<regionId>(Climate::COUNT)));

    core::WorldClock clock; // defaults: timeScale 60, tickRate 10 (D-TIME-1)
    core::SeedRng rng;
    rng.seedAll(11u);

    // 1200 ticks = 7200 game-seconds = exactly two game-hours -> two
    // boundary crossings -> two rolls per region (2 x kRegions draws total).
    for (int tick = 0; tick < 1200; ++tick)
        tickSeasonsAndWeather(clock, fixture.store, rng);
    CHECK(clock.worldTime == 7200);
    CHECK(clock.gameDay() == 0);

    // Seasons did not move (two hours << 30 game-days), and the weather
    // stream consumed exactly 2 x kRegions draws: a twin that draws the
    // same count lands in the same stream state.
    CHECK(fixture.store.region(0)->season.index == static_cast<std::uint32_t>(Season::SPRING));
    core::SeedRng twin;
    twin.seedAll(11u);
    for (std::size_t draw = 0; draw < 2u * kRegions; ++draw)
        (void)twin.pct("weather");
    CHECK(streamHi(rng, "weather") == streamHi(twin, "weather"));

    // Any weather that began during the run began at an hour boundary.
    for (regionId id = 0; id < kRegions; ++id)
    {
        const Region *region = fixture.store.region(id);
        CHECK(region->weather.sinceWorldTime % GAME_HOUR_SECONDS == 0);
        CHECK(region->weather.sinceWorldTime <= clock.worldTime);
    }
}

// ---------------------------------------------------------------------------
// TestHooks_Yields_A52TablesAndProduct
// Hook 1 (owner: 3.4): the A-52 tables and the collapsed farmer-duty
// multiplier (season x weather, R3 wide intermediate).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestHooks_Yields_A52TablesAndProduct"
)
{
    CHECK(seasonYieldMultiplier(Season::SPRING) == 10'000);
    CHECK(seasonYieldMultiplier(Season::SUMMER) == 12'000);
    CHECK(seasonYieldMultiplier(Season::AUTUMN) == 8'000);
    CHECK(seasonYieldMultiplier(Season::WINTER) == 3'000);

    CHECK(weatherYieldMultiplier(Weather::CLEAR) == 10'000); // no modifier
    CHECK(weatherYieldMultiplier(Weather::RAIN) == 11'000);
    CHECK(weatherYieldMultiplier(Weather::STORM) == 7'000);
    CHECK(weatherYieldMultiplier(Weather::FOG) == 10'000);  // no modifier
    CHECK(weatherYieldMultiplier(Weather::SNOW) == 10'000); // snow is not rain (the swap is transition-table only)

    CHECK(cropYieldMultiplier(Season::SPRING, Weather::CLEAR) == 10'000);
    CHECK(cropYieldMultiplier(Season::SUMMER, Weather::RAIN) == 13'200); // 1.2 x 1.1
    CHECK(cropYieldMultiplier(Season::WINTER, Weather::STORM) == 2'100); // 0.3 x 0.7 — the winter famine
    CHECK(cropYieldMultiplier(Season::AUTUMN, Weather::FOG) == 8'000);
}

// ---------------------------------------------------------------------------
// TestHooks_Migration_WinterPullWarmClimatesOnly
// Hook 2 (owner: 3.4): winter adds MIGRATION_WINTER_PULL (1000) toward the
// warm climates only; every other season adds nothing.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestHooks_Migration_WinterPullWarmClimatesOnly"
)
{
    CHECK(migrationClimatePull(Season::WINTER, Climate::COASTAL) == MIGRATION_WINTER_PULL);
    CHECK(migrationClimatePull(Season::WINTER, Climate::TEMPERATE) == MIGRATION_WINTER_PULL);
    CHECK(migrationClimatePull(Season::WINTER, Climate::FOREST) == 0);
    CHECK(migrationClimatePull(Season::WINTER, Climate::SWAMP) == 0);
    CHECK(migrationClimatePull(Season::WINTER, Climate::MOUNTAIN) == 0);

    for (const Season season : {Season::SPRING, Season::SUMMER, Season::AUTUMN})
        for (const std::size_t c : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4}})
            CHECK(migrationClimatePull(season, static_cast<Climate>(c)) == 0);
}

// ---------------------------------------------------------------------------
// TestHooks_WeatherModifier_AffinityLookupByWeatherKey
// Hook 3 (owner: 5.1): the template's weatherAffinity map answers by
// weatherKey; absent keys contribute nothing.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestHooks_WeatherModifier_AffinityLookupByWeatherKey"
)
{
    // The design reference's disaster template: "weatherAffinity": { "storm": 2000 }.
    const std::map<std::string, std::uint32_t> disaster{{"storm", 2'000}};
    CHECK(weatherModifier(Weather::STORM, disaster) == 2'000);
    CHECK(weatherModifier(Weather::RAIN, disaster) == 0);
    CHECK(weatherModifier(Weather::CLEAR, disaster) == 0);

    const std::map<std::string, std::uint32_t> none{};
    for (std::size_t kind = 0; kind < static_cast<std::size_t>(Weather::COUNT); ++kind)
        CHECK(weatherModifier(static_cast<Weather>(kind), none) == 0);

    // The keys are exactly the five weather words, one per kind.
    std::map<std::string, bool> keys;
    for (std::size_t kind = 0; kind < static_cast<std::size_t>(Weather::COUNT); ++kind)
    {
        const char *key = weatherKey(static_cast<Weather>(kind));
        REQUIRE(key != nullptr);
        keys[key] = true;
    }
    CHECK(keys.size() == static_cast<std::size_t>(Weather::COUNT));
    CHECK(keys.count("clear") == 1);
    CHECK(keys.count("rain") == 1);
    CHECK(keys.count("storm") == 1);
    CHECK(keys.count("fog") == 1);
    CHECK(keys.count("snow") == 1);
}

// ---------------------------------------------------------------------------
// TestHooks_Exposure_ActiveStormSnowAndShelterRadius
// Hook 4 (owner: 3.3/3.4, Phase 2): the gate is storm/snow only, the
// per-tick cost is A-53, and D-SHELTER-1's 50 m radius is x/z and inclusive
// at the boundary.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestHooks_Exposure_ActiveStormSnowAndShelterRadius"
)
{
    CHECK(exposureActive(Weather::STORM));
    CHECK(exposureActive(Weather::SNOW));
    CHECK_FALSE(exposureActive(Weather::CLEAR));
    CHECK_FALSE(exposureActive(Weather::RAIN));
    CHECK_FALSE(exposureActive(Weather::FOG));

    CHECK(exposureDamagePerTick() == EXPOSURE_DPS);
    CHECK(EXPOSURE_DPS == 2); // A-53

    const Position anchor{0.0, 0.0, 0.0};
    CHECK(isSheltered(Position{0.0, 0.0, 0.0}, anchor));
    CHECK(isSheltered(Position{30.0, 0.0, 40.0}, anchor));     // exactly 50 m: inclusive
    CHECK(isSheltered(Position{30.0, 1'000.0, 40.0}, anchor)); // y is cosmetic (card 1.6)
    CHECK(isSheltered(Position{0.0, 0.0, 50.0}, anchor));      // the boundary on one axis
    CHECK_FALSE(isSheltered(Position{0.0, 0.0, 50.0001}, anchor));
    CHECK_FALSE(isSheltered(Position{100.0, 0.0, 100.0}, anchor));
}

// ---------------------------------------------------------------------------
// TestHooks_ExposureSweep_UnshelteredOnlyViaApplyDamageSink
// Hook 4's frozen sweep, proven against fakes (the card: exposure is DAMAGE
// through applyDamage with source = 0 — never HP subtraction; the consumer
// tests land with 3.3 in Phase 2). Sheltered candidates are skipped; the
// gate short-circuits before any damage on non-exposed weather.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestHooks_ExposureSweep_UnshelteredOnlyViaApplyDamageSink"
)
{
    SeasonFixture fixture;
    Region *region = fixture.addRegion(0, Climate::TEMPERATE);

    const Position anchor{0.0, 0.0, 0.0};
    const std::vector<ExposureCandidate> candidates{
        {11, Position{0.0, 0.0, 0.0}},     // at the marker: sheltered
        {12, Position{100.0, 0.0, 100.0}}, // ~141 m out: exposed
        {13, Position{30.0, 0.0, 40.0}},   // exactly 50 m: sheltered (inclusive)
        {14, Position{51.0, 0.0, 0.0}},    // just outside: exposed
    };

    struct DamageCall
    {
        characterId who;
        std::uint32_t hp;
        characterId source;
    };
    std::vector<DamageCall> hits;
    const auto sink
        = [&](characterId who, std::uint32_t hp, characterId source) { hits.push_back(DamageCall{who, hp, source}); };

    // Storm: the two unsheltered candidates take EXPOSURE_DPS through the
    // sink, with the environment as the source; nobody else is touched.
    region->weather.kind = static_cast<std::uint32_t>(Weather::STORM);
    applyExposure(*region, candidates, anchor, sink);
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].who == 12);
    CHECK(hits[0].hp == EXPOSURE_DPS);
    CHECK(hits[0].source == kEnvironmentSource);
    CHECK(hits[0].source == 0);
    CHECK(hits[1].who == 14);
    CHECK(hits[1].hp == EXPOSURE_DPS);
    CHECK(hits[1].source == kEnvironmentSource);

    // Snow exposes too.
    hits.clear();
    region->weather.kind = static_cast<std::uint32_t>(Weather::SNOW);
    applyExposure(*region, candidates, anchor, sink);
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].who == 12);
    CHECK(hits[1].who == 14);

    // Clear, rain and fog: the gate short-circuits — the sink is never
    // called even for the unsheltered candidates.
    for (const Weather calm : {Weather::CLEAR, Weather::RAIN, Weather::FOG})
    {
        hits.clear();
        region->weather.kind = static_cast<std::uint32_t>(calm);
        applyExposure(*region, candidates, anchor, sink);
        CHECK(hits.empty());
    }
}
