// Task 5.1.3 - Characters beyond the token (implementation).
//
// See character_visuals.hpp for the contract. This file owns:
//   * The species silhouette lookup (A-153, nine rows).
//   * The yaw-from-velocity derivation (atan2 of the velocity's x/z).
//   * The walk-bob phase from traveled distance (A-154: 2.2 Hz sine wave).
//
// Determinism: every output is a pure function of the input. No wall clock
// (the bob's phase comes from traveled distance, not real time — R8/D-TIME-1).
// No RNG stream (the silhouette is data; the yaw and bob are pure math).
//
// R5: the visual transform is presentation, not a game rule, so f64 is
// correct here. The sim never reads this module's output.

#include "bridge/character_visuals.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "core/constants.hpp" // for the player walk speed (A-57) used by the bob reference

namespace harrowmere::bridge
{

namespace
{

// The A-57 player walk speed (4 m/s) — the reference speed for the walk bob.
// The bob's phase advances at WALK_BOB_HZ (2.2 Hz) AT this speed, so the
// phase = traveledDistance / referenceSpeed * WALK_BOB_HZ. A sprinting
// character (8 m/s) bobs at ~4.4 Hz; a stationary one does not bob.
// (A-57 lives in core/constants.hpp as MOVE_SPEED_MPS but that constant
// lives in the UI mod's input_mapper.hpp — the core constants.hpp carries
// the value here as a local mirror to avoid the cross-mod include.)
inline constexpr double kReferenceWalkSpeedMps = 4.0; // A-57

} // namespace

// ---- species silhouette lookup --------------------------------------------

const SpeciesSilhouetteRow &speciesSilhouette(
    speciesId s
) noexcept
{
    // The table is sorted by species id (0..8); a linear scan is trivial at
    // nine rows. Returns the human-scale fallback (row 1) for unknown ids.
    for (const auto &row : SPECIES_SILHOUETTES)
    {
        if (row.species == s)
            return row;
    }
    // Fallback: the human row (index 1). The table's index 1 is human by
    // construction (A-153 row 1).
    return SPECIES_SILHOUETTES[1];
}

// ---- yaw from velocity ----------------------------------------------------

double yawFromVelocity(
    const Position &velocity
) noexcept
{
    // The yaw faces the movement direction. Convention: 0 = -Z (forward),
    // pi/2 = +X (right), pi = +Z (backward), -pi/2 = -X (left).
    //
    // atan2(x, -z) gives this convention: when velocity is (0, 0, -1)
    // (forward), atan2(0, 1) = 0; when (1, 0, 0) (right), atan2(1, 0) = pi/2.
    const double vx = velocity.x;
    const double vz = velocity.z;
    // Stationary (or pure-vertical velocity): yaw 0.
    if (vx == 0.0 && vz == 0.0)
        return 0.0;
    return std::atan2(vx, -vz);
}

// ---- walk bob offset ------------------------------------------------------

double walkBobOffset(
    double traveledDistanceM, bool isMoving, bool isCorpse
) noexcept
{
    // The bob is a sine wave: amplitude * sin(2 * pi * phase).
    // phase = traveledDistance / referenceSpeed * WALK_BOB_HZ.
    // 0 when not moving or a corpse (the card: "a two-pose walk bob" —
    // stationary characters do not bob; corpses do not bob).
    if (!isMoving || isCorpse)
        return 0.0;
    const double phase = (traveledDistanceM / kReferenceWalkSpeedMps) * WALK_BOB_HZ;
    return WALK_BOB_AMPLITUDE_M * std::sin(2.0 * 3.14159265358979323846 * phase);
}

// ---- the visual transform -------------------------------------------------

CharacterVisual computeCharacterVisual(
    const CharacterVisualInput &input
) noexcept
{
    CharacterVisual v;
    const auto &silhouette = speciesSilhouette(input.species);
    v.heightScale          = silhouette.heightRatio;
    v.girthScale           = silhouette.girthRatio;
    v.yawRad               = yawFromVelocity(input.velocity);
    v.bobOffsetM           = walkBobOffset(input.traveledDistanceM, input.isMoving, input.isCorpse);
    v.isCorpse             = input.isCorpse;
    return v;
}

} // namespace harrowmere::bridge
