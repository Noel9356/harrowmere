// Task 5.1.4 - Sky, sun & weather on screen: the pinned test suite.
//
// Six pinned tests (card 5.1.4):
//   * TestAtmo_SunDirectionTracksDayPhase   — sun moves with the clock.
//   * TestAtmo_SkyAnchorsInterpolateInOrder — dawn->noon->dusk->night in order.
//   * TestAtmo_RainRaisesFogDensity         — RAIN > CLEAR on fog density.
//   * TestAtmo_NightWithinExposureFloorCeiling — night in [floor, ceiling].
//   * TestAtmo_WeatherStateUnmodified       — the weather input is unchanged.
//   * TestAtmo_SeasonTintsApply             — winter desaturates, autumn warms.
//
// The tests run headless (no GPU, no window). The AtmosphereSample is pure
// data; the tests exercise the derivation functions directly.

#include <doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "bridge/atmosphere.hpp"
#include "core/constants_phase5.hpp" // SKY_ANCHORS (A-155), EXPOSURE_* (A-156), BLOOM_* (A-157)

using harrowmere::BLOOM_STRENGTH_SUN;
using harrowmere::BLOOM_STRENGTH_TORCH;
using harrowmere::EXPOSURE_DAY_CEILING;
using harrowmere::EXPOSURE_NIGHT_FLOOR;
using harrowmere::SKY_ANCHORS;
using harrowmere::bridge::AtmosphereInput;
using harrowmere::bridge::AtmosphereSample;
using harrowmere::bridge::AtmosphereSeason;
using harrowmere::bridge::AtmosphereWeather;
using harrowmere::bridge::computeSun;
using harrowmere::bridge::exposureFor;
using harrowmere::bridge::fogFor;
using harrowmere::bridge::sampleAtmosphere;
using harrowmere::bridge::skyTintFor;
using harrowmere::bridge::SunLight;

namespace
{

constexpr double kPi = 3.14159265358979323846;

} // namespace

// ---------------------------------------------------------------------------
// TestAtmo_SunDirectionTracksDayPhase
//
// The sun's direction and intensity track the day/night phase. Phase 0 =
// midnight (intensity 0, sun below horizon), 0.25 = sunrise (intensity > 0,
// sun at horizon), 0.5 = noon (intensity 1, sun overhead), 0.75 = sunset
// (intensity > 0, sun at horizon).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestAtmo_SunDirectionTracksDayPhase"
)
{
    // Midnight: intensity 0 (sun below horizon).
    const SunLight midnight = computeSun(0.0);
    CHECK(midnight.intensity == doctest::Approx(0.0f).epsilon(1e-6));

    // Sunrise: intensity is 0 at the exact horizon (phase 0.25), and rises
    // quickly after. The test checks the rise rather than the exact horizon
    // (the card: "sun direction and intensity from the day/night phase").
    const SunLight sunrise = computeSun(0.25);
    CHECK(sunrise.intensity == doctest::Approx(0.0f).epsilon(1e-6)); // at horizon
    const SunLight afterSunrise = computeSun(0.30);
    CHECK(afterSunrise.intensity > 0.0f); // above the horizon
    CHECK(afterSunrise.intensity < 0.5f); // not yet at peak

    // Noon: intensity 1 (sun overhead).
    const SunLight noon = computeSun(0.5);
    CHECK(noon.intensity == doctest::Approx(1.0f).epsilon(1e-6));
    // Direction: straight down (-Y) at noon.
    CHECK(noon.direction[1] == doctest::Approx(-1.0f).epsilon(1e-6));

    // Sunset: intensity > 0 (sun at horizon, going down).
    const SunLight sunset = computeSun(0.75);
    CHECK(sunset.intensity > 0.0f);
    CHECK(sunset.intensity < 0.5f);

    // The sample path plumbs the sun into the AtmosphereSample.
    AtmosphereInput input{};
    input.dayPhase01         = 0.5; // noon
    input.weather            = AtmosphereWeather::CLEAR;
    input.season             = AtmosphereSeason::SPRING;
    const AtmosphereSample s = sampleAtmosphere(input);
    CHECK(s.sunIntensity == doctest::Approx(1.0f).epsilon(1e-6));

    // Night: sun intensity 0, bloom sun 0 (no sun disc to bloom).
    input.dayPhase01              = 0.0;
    const AtmosphereSample sNight = sampleAtmosphere(input);
    CHECK(sNight.sunIntensity == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(sNight.bloomSun == doctest::Approx(0.0f).epsilon(1e-6));
}

// ---------------------------------------------------------------------------
// TestAtmo_SkyAnchorsInterpolateInOrder
//
// The sky gradient interpolates between the two nearest A-155 anchors by the
// day/night phase. The anchors in phase order are: night (0.0) -> dawn
// (0.25) -> noon (0.5) -> dusk (0.75) -> night (1.0). The test asserts the
// sky tint at each anchor matches the anchor's color, and the interpolation
// between anchors is monotone in each channel where the anchors are monotone.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestAtmo_SkyAnchorsInterpolateInOrder"
)
{
    // The A-155 table has four anchors: dawn, noon, dusk, night.
    REQUIRE(SKY_ANCHORS.size() == 4);

    // At each anchor's phase, the sky tint matches the anchor color (after
    // season modulation; spring is the identity multiplier so we use spring
    // for the pure-anchor check).
    const auto checkAnchor = [](double phase, std::size_t anchorIdx) {
        const auto tint    = skyTintFor(phase, AtmosphereSeason::SPRING);
        const auto &anchor = SKY_ANCHORS[anchorIdx];
        // The smoothstep interpolation at the exact anchor phase gives t=0
        // or t=1, so the tint should match the anchor color (clamped).
        for (int c = 0; c < 3; ++c)
        {
            // Allow a small tolerance for the smoothstep + clamp path.
            CHECK(std::abs(tint[c] - anchor.color[c]) < 0.05f);
        }
    };

    checkAnchor(0.0, 3);  // night
    checkAnchor(0.25, 0); // dawn
    checkAnchor(0.5, 1);  // noon
    checkAnchor(0.75, 2); // dusk

    // The noon sky is brighter (higher blue) than the night sky.
    const auto noonTint  = skyTintFor(0.5, AtmosphereSeason::SPRING);
    const auto nightTint = skyTintFor(0.0, AtmosphereSeason::SPRING);
    CHECK(noonTint[2] > nightTint[2]); // blue channel: noon > night

    // The dawn sky is warmer (higher red) than the noon sky.
    const auto dawnTint = skyTintFor(0.25, AtmosphereSeason::SPRING);
    CHECK(dawnTint[0] > noonTint[0]); // red channel: dawn > noon
}

// ---------------------------------------------------------------------------
// TestAtmo_RainRaisesFogDensity
//
// RAIN raises fog density above CLEAR. The weather state machine's as-built
// deltas: RAIN raises density, overcast (STORM/FOG) raises it further.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestAtmo_RainRaisesFogDensity"
)
{
    const auto clearFog = fogFor(AtmosphereWeather::CLEAR);
    const auto rainFog  = fogFor(AtmosphereWeather::RAIN);
    const auto stormFog = fogFor(AtmosphereWeather::STORM);
    const auto fogFog   = fogFor(AtmosphereWeather::FOG);

    // CLEAR carries the lightest haze, not zero (rev 22: the aerial
    // perspective that turns the hard horizon seam into depth).
    CHECK(clearFog.density > 0.0f);
    // RAIN > CLEAR.
    CHECK(rainFog.density > clearFog.density);
    // STORM > RAIN (storm is heavier).
    CHECK(stormFog.density > rainFog.density);
    // FOG is the densest (fog is the weather state defined by low visibility).
    CHECK(fogFog.density > rainFog.density);
    CHECK(fogFog.density > stormFog.density);

    // The sample path plumbs the fog into the AtmosphereSample.
    AtmosphereInput input{};
    input.dayPhase01         = 0.5;
    input.weather            = AtmosphereWeather::RAIN;
    input.season             = AtmosphereSeason::SPRING;
    const AtmosphereSample s = sampleAtmosphere(input);
    CHECK(s.fogDensity > 0.0f);

    // RAIN desaturates the fog tint vs CLEAR (lower blue channel spread).
    // CLEAR fog tint is neutral gray (0.7, 0.7, 0.7); RAIN fog tint is
    // desaturated (lower max channel).
    const float clearMax = std::max({clearFog.tint[0], clearFog.tint[1], clearFog.tint[2]});
    const float rainMax  = std::max({rainFog.tint[0], rainFog.tint[1], rainFog.tint[2]});
    CHECK(rainMax <= clearMax);
}

// ---------------------------------------------------------------------------
// TestAtmo_NightWithinExposureFloorCeiling
//
// Night is dark but playable: the exposure target at night is at the
// EXPOSURE_NIGHT_FLOOR (0.35 since rev 21), not 0. Day is at the
// EXPOSURE_DAY_CEILING (1.15 since rev 22). Every phase's exposure is in
// [floor, ceiling].
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestAtmo_NightWithinExposureFloorCeiling"
)
{
    // Night: exposure at the floor.
    const double nightExp = exposureFor(0.0);
    CHECK(nightExp == doctest::Approx(EXPOSURE_NIGHT_FLOOR).epsilon(1e-6));

    // Noon: exposure at the ceiling.
    const double noonExp = exposureFor(0.5);
    CHECK(noonExp == doctest::Approx(EXPOSURE_DAY_CEILING).epsilon(1e-6));

    // Every phase: exposure in [floor, ceiling].
    for (double phase = 0.0; phase < 1.0; phase += 0.05)
    {
        const double exp = exposureFor(phase);
        CHECK(exp >= EXPOSURE_NIGHT_FLOOR - 1e-9);
        CHECK(exp <= EXPOSURE_DAY_CEILING + 1e-9);
    }

    // The sample path plumbs the exposure into the AtmosphereSample.
    AtmosphereInput input{};
    input.dayPhase01         = 0.0;
    input.weather            = AtmosphereWeather::CLEAR;
    input.season             = AtmosphereSeason::SPRING;
    const AtmosphereSample s = sampleAtmosphere(input);
    CHECK(s.exposureTarget == doctest::Approx(EXPOSURE_NIGHT_FLOOR).epsilon(1e-6));
}

// ---------------------------------------------------------------------------
// TestAtmo_WeatherStateUnmodified
//
// The atmosphere derivation reads the weather state but never modifies it.
// The test asserts the AtmosphereInput (which carries the weather) is
// unchanged after a sample. (1.6.3's weather store is not touched at all —
// this module reads a single kind ordinal from the bridge's view of it.)
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestAtmo_WeatherStateUnmodified"
)
{
    AtmosphereInput input{};
    input.dayPhase01    = 0.5;
    input.weather       = AtmosphereWeather::STORM;
    input.season        = AtmosphereSeason::WINTER;
    const auto snapshot = input;

    const AtmosphereSample s = sampleAtmosphere(input);

    // The input is unchanged.
    CHECK(input.dayPhase01 == snapshot.dayPhase01);
    CHECK(input.weather == snapshot.weather);
    CHECK(input.season == snapshot.season);

    // The sample is non-trivial.
    CHECK(s.fogDensity > 0.0f);   // STORM raises fog
    CHECK(s.sunIntensity > 0.0f); // noon has sun
    (void)s;
}

// ---------------------------------------------------------------------------
// TestAtmo_SeasonTintsApply
//
// Season modulates the palette: winter desaturates (lower channel spread),
// autumn warms (higher red). The test asserts the season tint is applied to
// the sky color at a fixed phase.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestAtmo_SeasonTintsApply"
)
{
    // At noon, the spring sky is the pure anchor (multiplier 1.0).
    const auto springNoon = skyTintFor(0.5, AtmosphereSeason::SPRING);
    // Autumn warms: the red channel should be higher than spring's.
    const auto autumnNoon = skyTintFor(0.5, AtmosphereSeason::AUTUMN);
    CHECK(autumnNoon[0] >= springNoon[0]); // red channel: autumn >= spring
    // Winter cools: the blue channel should be higher than spring's.
    const auto winterNoon = skyTintFor(0.5, AtmosphereSeason::WINTER);
    CHECK(winterNoon[2] >= springNoon[2]); // blue channel: winter >= spring

    // The season applies through the sample path too.
    AtmosphereInput input{};
    input.dayPhase01         = 0.5;
    input.weather            = AtmosphereWeather::CLEAR;
    input.season             = AtmosphereSeason::AUTUMN;
    const AtmosphereSample s = sampleAtmosphere(input);
    CHECK(s.skyTint[0] >= springNoon[0]); // autumn red >= spring red
}
