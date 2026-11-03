// Task 5.1.4 - Sky, sun & weather on screen.
//
// The sky stops being flat: time of day, season and weather — all already
// simulated — become what the player sees, through the renderer's existing
// sun/sky/fog path.
//
// D-IMG-3 (common.md Appendix A.14): NO NEW ENGINE MODULE, NO KERNEL EDIT.
// The bridge publishes per frame: sun direction and intensity from the
// day/night phase (1.2.2's clock), a sky gradient keyed to that phase
// (dawn/noon/dusk/night anchor colors pinned A-155, interpolated), fog
// density and tint from the weather state machine (1.6.3's as-built deltas:
// RAIN raises density, overcast desaturates), and an exposure/tone target
// so night is dark without going black (floor/ceiling pinned A-156). The
// renderer's bloom pass (already in the tree) is enabled for the sun disc
// and torch-range lights only — its strength pinned A-157. Season modulates
// palette (winter desaturates, autumn warms) through the same anchor tables.
//
// What ships here:
//   * AtmosphereSample — a pure-data record describing one frame's atmosphere:
//     sun direction + intensity, sky tint, fog density + tint, exposure
//     target, bloom strength. The bridge reads this and pushes it to the
//     scene sink (setSunLight, setSkyTint, and the renderer's fog/exposure/
//     bloom seams).
//   * sampleAtmosphere(...) — a pure function: given the day/night phase,
//     the weather state, and the season, returns the AtmosphereSample. No
//     sim state is touched (TestAtmo_WeatherStateUnmodified asserts the
//     weather store is identical before and after a frame).
//
// What does NOT ship here:
//   * The renderer's fog/exposure/bloom implementation (the renderer's
//     existing path carries it — D-IMG-3: no new engine module).
//   * The weather state machine (1.6.3 owns it; this module reads it).
//   * The day/night clock (1.2.2 owns it; this module reads dayPhase01).
//
// Determinism: every output is a pure function of (dayPhase01, weatherKind,
// seasonIndex). No wall clock (the card's pitfall: "Interpolating between
// anchors needs the PHASE, not wall time — importing real time here breaks
// determinism reviews and the D-TIME-1 two-clock rule"). No RNG stream.

#pragma once

#include <array>
#include <cstdint>

#include "core/constants_phase5.hpp" // SKY_ANCHORS (A-155), EXPOSURE_* (A-156), BLOOM_* (A-157)

namespace harrowmere::bridge
{

// The weather state the atmosphere reads. Mirrors the weather kinds 1.6.3's
// state machine produces (CLEAR, RAIN, STORM, SNOW, FOG). The kind ordinal
// is 1.6.3's Weather enum; this module stays independent of that header so
// it unit-tests without the world model.
enum class AtmosphereWeather : std::uint32_t
{
    CLEAR = 0,
    RAIN  = 1,
    STORM = 2,
    SNOW  = 3,
    FOG   = 4,
    COUNT
};

// The season index (0..3: spring, summer, autumn, winter). Mirrors 1.6.3's
// season list; this module reads the index, not the season state machine.
enum class AtmosphereSeason : std::uint32_t
{
    SPRING = 0,
    SUMMER = 1,
    AUTUMN = 2,
    WINTER = 3,
    COUNT
};

// One frame's atmosphere. Pure data — the bridge pushes this to the scene
// sink (setSunLight, setSkyTint) and the renderer's fog/exposure/bloom seams.
struct AtmosphereSample
{
    // Sun light: direction is a unit vector pointing in the direction light
    // travels (from sun toward scene); intensity is in [0, 1]. 0 at night.
    std::array<float, 3> sunDirection = {0.0f, -1.0f, 0.0f};
    float sunIntensity                = 0.0f;

    // Sky tint (Linear-RGB, components in [0, 1]). Interpolated between the
    // two nearest A-155 anchors by the day/night phase, then modulated by
    // the season tint.
    std::array<float, 3> skyTint = {0.5f, 0.5f, 0.5f};

    // Fog density (in [0, 1], 0 = no fog). Raised by RAIN and FOG; the
    // card: "fog density and tint from the weather state machine (1.6.3's
    // as-built deltas: RAIN raises density, overcast desaturates)".
    float fogDensity = 0.0f;
    // Fog tint (Linear-RGB). Desaturated by overcast/storm; blue-shifted by
    // snow.
    std::array<float, 3> fogTint = {0.7f, 0.7f, 0.7f};

    // Exposure tone target (in [EXPOSURE_NIGHT_FLOOR, EXPOSURE_DAY_CEILING]).
    // Night is dark (floor 0.08) but never black; day is bright (ceiling 1.0)
    // but never blown out. The card's pitfall: "a 'realistic' black night
    // fails the capture checklist by design".
    double exposureTarget = 1.0;

    // Bloom strength for the sun disc and torch-range lights (A-157 = 0.35).
    // 0 at night (no sun disc); 0.35 during the day. Torch-range bloom is
    // always 0.35 (a torch is a torch, day or night).
    float bloomSun   = 0.0f;
    float bloomTorch = 0.35f; // A-157
};

// The input the atmosphere derivation needs. The bridge reads this from the
// clock (dayPhase01), the weather state machine (primaryWeather), and the
// season state (seasonIndex).
struct AtmosphereInput
{
    double dayPhase01         = 0.5; // 1.2.2's clock: [0, 1) within the day
    AtmosphereWeather weather = AtmosphereWeather::CLEAR;
    AtmosphereSeason season   = AtmosphereSeason::SPRING;
};

// Compute the atmosphere sample for one frame. Pure: no sim state touched,
// no global state read. The bridge calls this once per frame, then pushes
// the result to the scene sink and the renderer's fog/exposure/bloom seams.
AtmosphereSample sampleAtmosphere(const AtmosphereInput &input) noexcept;

// The sun direction + intensity from dayPhase01 (the same math scene_bridge.cpp's
// computeSun does, exposed here so the atmosphere module owns the sun path).
// Phase 0 = midnight, 0.25 = sunrise (east), 0.5 = noon (overhead), 0.75 =
// sunset (west). Intensity is 0 when the sun is below the horizon.
struct SunLight
{
    std::array<float, 3> direction; // unit vector, light-travel direction
    float intensity;                // [0, 1]
};
SunLight computeSun(double dayPhase01) noexcept;

// The sky tint interpolated between the two nearest A-155 anchors by the
// day/night phase, then modulated by the season tint. Pure.
std::array<float, 3> skyTintFor(double dayPhase01, AtmosphereSeason season) noexcept;

// The fog density + tint from the weather state. Pure.
// Returns: (density, tint) — density in [0, 1], tint Linear-RGB.
struct FogSample
{
    float density;
    std::array<float, 3> tint;
};
FogSample fogFor(AtmosphereWeather weather) noexcept;

// The exposure tone target from the day/night phase. Clamped to
// [EXPOSURE_NIGHT_FLOOR, EXPOSURE_DAY_CEILING]. Pure.
double exposureFor(double dayPhase01) noexcept;

} // namespace harrowmere::bridge
