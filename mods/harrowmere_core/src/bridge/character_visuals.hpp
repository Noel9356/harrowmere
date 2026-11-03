// Task 5.1.3 - Characters beyond the token.
//
// A character reads as a creature of its species at a glance — silhouette,
// facing, motion — while staying a cheap primitive batch the budget can
// afford for thousands.
//
// What ships here:
//   * CharacterVisual — a pure-data record describing one character's visual
//     transform for one frame: the silhouette scale (height/girth from the
//     per-species A-153 table), the yaw (from movement direction), and the
//     walk-bob offset (phase from traveled distance, A-154). The bridge
//     composes this transform AFTER reading the sim's position, never before
//     writing it — the walk bob must not modulate the collision/position the
//     sim sees (the card's pitfall).
//   * computeCharacterVisual(...) — a pure function: given the sim's
//     per-character state (species, position, velocity, traveled distance,
//     isMoving, isCorpse), returns the CharacterVisual. No sim state is
//     touched (TestCharVis_NoSimStateTouched asserts the store is identical
//     before and after a render frame).
//   * The species silhouette table (A-153) — nine rows, one per species.
//   * The walk bob rate (A-154) — 2.2 Hz, phase from traveled distance.
//
// What does NOT ship here:
//   * The bridge-side rendering (the bridge's scene_bridge.cpp consumes the
//     CharacterVisual and drives the scene sink — the same path that today
//     spawns the species-colored sphere + faction ring). This module is the
//     pure-data derivation; the bridge applies it.
//   * The faction ring (its colors are data, unchanged from 2.5.3).
//   * The corpse flatten-and-gray rule (unchanged from 2.5.3; this module
//     carries the corpse flag but the bridge's corpse rendering stays as-built).
//
// Determinism: every output is a pure function of (species, position,
// velocity, traveledDistance, isMoving, isCorpse). No wall clock (R8: the
// walk bob's phase comes from traveled distance, not real time — the card's
// pitfall: "Interpolating between anchors needs the PHASE, not wall time —
// importing real time here breaks determinism reviews and the D-TIME-1
// two-clock rule"). No RNG stream — the silhouette is data, the yaw is a
// pure function of velocity, the bob is a pure function of distance.

#pragma once

#include <array>
#include <cstdint>

#include "core/types.hpp"            // Position, speciesId, characterId (task 0.2)
#include "core/constants_phase5.hpp" // SPECIES_SILHOUETTES (A-153), WALK_BOB_HZ (A-154)

namespace harrowmere::bridge
{

// The visual transform for one character in one frame. Pure data — the
// bridge composes this into the scene sink's spawnSphere/setObjectPosition
// calls. The transform is applied AFTER reading the sim's position, so the
// walk bob never modulates the collision/position the sim sees.
struct CharacterVisual
{
    // The silhouette scale: (height, girth) multipliers on the 0.8 m sphere
    // baseline. From the per-species A-153 table.
    double heightScale = 1.0;
    double girthScale  = 1.0;

    // The yaw (radians, 0 = facing -Z) the character faces. Derived from
    // movement direction (velocity); 0 when stationary.
    double yawRad = 0.0;

    // The walk bob: a vertical offset (metres) added to the visual position.
    // Phase from traveled distance (A-154: 2.2 Hz at the reference walk
    // speed). 0 when stationary or a corpse.
    double bobOffsetM = 0.0;

    // True iff this character is a corpse (the bridge's corpse rendering
    // stays as-built: flatten and gray). The CharacterVisual carries the
    // flag so the bridge's per-character path can switch to the corpse
    // shape without a separate query.
    bool isCorpse = false;
};

// The per-character state the visual derivation needs. The bridge reads this
// from the CharactersViewForBridge (2.5.3's view) plus a per-character
// traveled-distance accumulator (the bridge tracks the distance each
// character has moved since it was last seen, for the walk-bob phase).
struct CharacterVisualInput
{
    speciesId species = 0;
    Position position{};
    Position velocity{};            // metres per second (the bridge already has this)
    double traveledDistanceM = 0.0; // cumulative distance since spawn (for bob phase)
    bool isMoving            = false;
    bool isCorpse            = false;
};

// Compute the visual transform for one character. Pure: no sim state
// touched, no global state read. The bridge calls this per present
// character per frame, then composes the result into the scene sink.
CharacterVisual computeCharacterVisual(const CharacterVisualInput &input) noexcept;

// Look up the species silhouette row (A-153). Returns the fallback
// (human-scale) row for unknown species ids. Exposed for the
// TestCharVis_SilhouettePerSpecies test.
const SpeciesSilhouetteRow &speciesSilhouette(speciesId s) noexcept;

// Compute the yaw (radians) from a velocity vector. 0 when stationary.
// The yaw faces the movement direction (0 = -Z, pi/2 = +X, pi = +Z,
// -pi/2 = -X). Pure.
double yawFromVelocity(const Position &velocity) noexcept;

// Compute the walk bob offset (metres) from the traveled distance and the
// A-154 rate. The bob is a sine wave: amplitude * sin(2 * pi * hz * distance
// / referenceSpeed). The reference speed is the A-57 player walk speed
// (4 m/s) so 2.2 Hz at that speed is one full bob cycle per ~1.8 m. 0 when
// not moving or a corpse. Pure.
double walkBobOffset(double traveledDistanceM, bool isMoving, bool isCorpse) noexcept;

} // namespace harrowmere::bridge
