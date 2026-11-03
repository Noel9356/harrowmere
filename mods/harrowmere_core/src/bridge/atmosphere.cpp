// Task 5.1.4 - Sky, sun & weather on screen (implementation).
//
// See atmosphere.hpp for the contract. This file owns:
//   * The sun direction + intensity from dayPhase01 (the same math
//     scene_bridge.cpp's computeSun does, centralized here so the atmosphere
//     module owns the sun path).
//   * The sky gradient: interpolation between the two nearest A-155 anchors
//     by the day/night phase, then modulation by the season tint.
//   * The fog density + tint from the weather state (1.6.3's as-built
//     deltas: RAIN raises density, overcast desaturates, snow blue-shifts).
//   * The exposure tone target clamped to [A-156 floor, A-156 ceiling].
//   * The bloom strength for the sun disc (A-157 = 0.35 when the sun is up).
//
// D-IMG-3: no new engine module, no kernel edit. The bridge publishes the
// AtmosphereSample per frame; the renderer's existing sun/sky/fog path
// carries it. radiance_cascades stays deleted.
//
// Determinism: every output is a pure function of (dayPhase01, weather,
// season). No wall clock (R8/D-TIME-1 — the card's pitfall). No RNG stream.

#include "bridge/atmosphere.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace harrowmere::bridge
{

namespace
{

// Linear interpolation between two floats.
inline float lerp(
    float a, float b, float t
) noexcept
{
    return a + (b - a) * t;
}

// Linear interpolation between two RGB triples (component-wise).
inline std::array<float, 3> lerpColor(
    const std::array<float, 3> &a, const std::array<float, 3> &b, float t
) noexcept
{
    return {lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t)};
}

// Multiply two RGB triples (component-wise) — used for the season tint.
inline std::array<float, 3> mulColor(
    const std::array<float, 3> &a, const std::array<float, 3> &b
) noexcept
{
    return {a[0] * b[0], a[1] * b[1], a[2] * b[2]};
}

// The smoothstep fade for anchor interpolation (C1-continuous so the sky
// gradient does not have a hard edge at the anchor boundaries).
inline float smoothstep(
    float t
) noexcept
{
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

// Find the two nearest anchors for a given phase, and the interpolation
// factor between them. The anchors are at phases {0.25 (dawn), 0.5 (noon),
// 0.75 (dusk), 0.0 (night)} — the night anchor wraps around. Returns the
// (lo, hi, t) where lo is the anchor BEFORE the phase and hi is the anchor
// AFTER, t in [0, 1].
struct AnchorPair
{
    const SkyAnchorRow *lo;
    const SkyAnchorRow *hi;
    float t;
};

AnchorPair nearestAnchors(
    double dayPhase01
) noexcept
{
    // The four anchors in phase order: night (0.0), dawn (0.25), noon (0.5),
    // dusk (0.75). The cycle wraps: after dusk comes night again (at phase
    // 1.0 == 0.0).
    //
    // The SKY_ANCHORS table is in (dawn, noon, dusk, night) order; the
    // SKY_ANCHOR_PHASES table mirrors that order. For the interpolation, we
    // walk the phases in cyclic order: night -> dawn -> noon -> dusk ->
    // night.
    constexpr std::array<const SkyAnchorRow *, 5> cycle = {
        &SKY_ANCHORS[3], // night (phase 0.0)
        &SKY_ANCHORS[0], // dawn  (phase 0.25)
        &SKY_ANCHORS[1], // noon  (phase 0.5)
        &SKY_ANCHORS[2], // dusk  (phase 0.75)
        &SKY_ANCHORS[3], // night (phase 1.0, wraps)
    };
    constexpr std::array<double, 5> cyclePhases = {0.0, 0.25, 0.5, 0.75, 1.0};

    const double phase = std::clamp(dayPhase01, 0.0, 1.0);

    // Find the segment [cyclePhases[i], cyclePhases[i+1]] that contains phase.
    for (std::size_t i = 0; i < 4; ++i)
    {
        if (phase >= cyclePhases[i] && phase <= cyclePhases[i + 1])
        {
            const double span = cyclePhases[i + 1] - cyclePhases[i];
            const float t     = span > 0.0 ? static_cast<float>((phase - cyclePhases[i]) / span) : 0.0f;
            return {cycle[i], cycle[i + 1], smoothstep(t)};
        }
    }
    // Fallback (should not happen after clamp).
    return {&SKY_ANCHORS[3], &SKY_ANCHORS[3], 0.0f};
}

} // namespace

// ---- sun ------------------------------------------------------------------

SunLight computeSun(
    double dayPhase01
) noexcept
{
    // Map phase to an angle where phase 0.25 (sunrise) -> 0, phase 0.5
    // (noon) -> pi/2, phase 0.75 (sunset) -> pi, phase 0 (midnight) -> -pi/2.
    using std::numbers::pi_v;
    const double angle = (std::clamp(dayPhase01, 0.0, 1.0) - 0.25) * 2.0 * pi_v<double>;

    const float cosA = static_cast<float>(std::cos(angle));
    const float sinA = static_cast<float>(std::sin(angle));

    // Sun position relative to scene origin: (cosA, sinA, 0).
    // Light travels from sun toward scene: direction = -position.
    SunLight s;
    s.direction = {-cosA, -sinA, 0.0f};
    // Intensity tracks the sun's elevation: 0 at/below the horizon, 1 at
    // noon. max(0, sinA) clamps night-time negatives to zero.
    s.intensity = std::max(0.0f, sinA);
    return s;
}

// ---- sky tint -------------------------------------------------------------

std::array<float, 3> skyTintFor(
    double dayPhase01, AtmosphereSeason season
) noexcept
{
    const auto [lo, hi, t]    = nearestAnchors(dayPhase01);
    std::array<float, 3> tint = lerpColor(lo->color, hi->color, t);

    // Season modulation (winter desaturates, autumn warms). The season
    // index maps to SEASON_TINTS (0..3: spring, summer, autumn, winter).
    const std::uint32_t seasonIdx = static_cast<std::uint32_t>(season);
    if (seasonIdx < static_cast<std::uint32_t>(AtmosphereSeason::COUNT))
    {
        tint = mulColor(tint, SEASON_TINTS[seasonIdx].multiplier);
    }

    // Clamp to [0, 1] (the season multipliers can push components above 1.0).
    return {
        std::clamp(tint[0], 0.0f, 1.0f),
        std::clamp(tint[1], 0.0f, 1.0f),
        std::clamp(tint[2], 0.0f, 1.0f),
    };
}

// ---- fog ------------------------------------------------------------------

FogSample fogFor(
    AtmosphereWeather weather
) noexcept
{
    // The card: "fog density and tint from the weather state machine (1.6.3's
    // as-built deltas: RAIN raises density, overcast desaturates)". The
    // values are presentation-only (the card did not pin per-weather
    // numbers beyond the directions); they live here and follow the same
    // promotion pattern as scene_bridge.hpp's presentation tints.
    switch (weather)
    {
    case AtmosphereWeather::CLEAR:
        return {0.0f, {0.7f, 0.7f, 0.7f}};
    case AtmosphereWeather::RAIN:
        // Rain raises density, desaturates slightly.
        return {0.35f, {0.55f, 0.55f, 0.58f}};
    case AtmosphereWeather::STORM:
        // Storm raises density further, desaturates more.
        return {0.55f, {0.40f, 0.40f, 0.45f}};
    case AtmosphereWeather::SNOW:
        // Snow: moderate density, blue-shifted tint.
        return {0.30f, {0.80f, 0.85f, 0.95f}};
    case AtmosphereWeather::FOG:
        // Fog: high density, neutral tint.
        return {0.75f, {0.65f, 0.65f, 0.65f}};
    default:
        return {0.0f, {0.7f, 0.7f, 0.7f}};
    }
}

// ---- exposure -------------------------------------------------------------

double exposureFor(
    double dayPhase01
) noexcept
{
    // The exposure tracks the sun's elevation (same shape as the sun
    // intensity): 0 at night, 1 at noon, clamped to
    // [EXPOSURE_NIGHT_FLOOR, EXPOSURE_DAY_CEILING]. The night floor (0.08)
    // is the "dark but playable" floor — the card's pitfall: "a 'realistic'
    // black night fails the capture checklist by design".
    using std::numbers::pi_v;
    const double angle = (std::clamp(dayPhase01, 0.0, 1.0) - 0.25) * 2.0 * pi_v<double>;
    const double sinA  = std::sin(angle);
    const double raw   = std::max(0.0, sinA);
    // Map [0, 1] -> [EXPOSURE_NIGHT_FLOOR, EXPOSURE_DAY_CEILING].
    const double mapped = EXPOSURE_NIGHT_FLOOR + raw * (EXPOSURE_DAY_CEILING - EXPOSURE_NIGHT_FLOOR);
    return std::clamp(mapped, EXPOSURE_NIGHT_FLOOR, EXPOSURE_DAY_CEILING);
}

// ---- the sample -----------------------------------------------------------

AtmosphereSample sampleAtmosphere(
    const AtmosphereInput &input
) noexcept
{
    AtmosphereSample s;

    const auto sun = computeSun(input.dayPhase01);
    s.sunDirection = sun.direction;
    s.sunIntensity = sun.intensity;

    s.skyTint = skyTintFor(input.dayPhase01, input.season);

    const auto fog = fogFor(input.weather);
    s.fogDensity   = fog.density;
    s.fogTint      = fog.tint;

    s.exposureTarget = exposureFor(input.dayPhase01);

    // Bloom: the sun disc blooms when the sun is up (intensity > 0);
    // torch-range bloom is always on (A-157 = 0.35). The card: "bloom as a
    // light cue, not a wash" — bloom is NOT applied to the full scene, only
    // to the sun disc and torch-range lights. The bridge's renderer path
    // scopes the bloom to those lights; this module only publishes the
    // strength.
    s.bloomSun   = sun.intensity > 0.0f ? BLOOM_STRENGTH_SUN : 0.0f;
    s.bloomTorch = BLOOM_STRENGTH_TORCH;

    return s;
}

} // namespace harrowmere::bridge
