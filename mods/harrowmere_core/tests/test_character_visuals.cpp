// Task 5.1.3 - Characters beyond the token: the pinned test suite.
//
// Five pinned tests (card 5.1.3):
//   * TestCharVis_SilhouettePerSpecies       — nine rows consumed.
//   * TestCharVis_YawFollowsVelocity         — yaw derives from movement dir.
//   * TestCharVis_BobPhaseFromDistance       — phase from traveled distance.
//   * TestCharVis_CorpseFlattenGrayUnchanged — corpse flag carries; the
//                                              bridge's flatten-and-gray rule
//                                              stays as-built.
//   * TestCharVis_NoSimStateTouched          — store snapshots identical
//                                              across a render frame.
//
// The tests run headless (no GPU, no window). The CharacterVisual is pure
// data; the tests exercise the derivation functions directly.

#include <doctest.h>

#include <array>
#include <cmath>
#include <cstdint>

#include "bridge/character_visuals.hpp"
#include "core/constants_phase5.hpp" // SPECIES_SILHOUETTES (A-153), WALK_BOB_HZ (A-154)

using harrowmere::Position;
using harrowmere::SPECIES_SILHOUETTES;
using harrowmere::speciesId;
using harrowmere::WALK_BOB_AMPLITUDE_M;
using harrowmere::WALK_BOB_HZ;
using harrowmere::bridge::CharacterVisual;
using harrowmere::bridge::CharacterVisualInput;
using harrowmere::bridge::computeCharacterVisual;
using harrowmere::bridge::speciesSilhouette;
using harrowmere::bridge::walkBobOffset;
using harrowmere::bridge::yawFromVelocity;

// ---------------------------------------------------------------------------
// TestCharVis_SilhouettePerSpecies
//
// Nine rows consumed: every species id 0..8 has a silhouette row, and the
// lookup returns the right row. The fallback (unknown id) returns the
// human-scale row.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestCharVis_SilhouettePerSpecies"
)
{
    // The A-153 table has exactly nine rows (one per species id 0..8).
    REQUIRE(SPECIES_SILHOUETTES.size() == 9);

    // Every species id 0..8 resolves to its own row.
    for (std::uint16_t s = 0; s < 9; ++s)
    {
        const auto &row = speciesSilhouette(speciesId{s});
        CHECK(row.species == s);
        CHECK(row.heightRatio > 0.0);
        CHECK(row.girthRatio > 0.0);
        CHECK(row.rationale != nullptr);
    }

    // Distinct silhouettes: at least the dragon (id 6) is larger than the
    // wolf (id 8) — the card's "characters read at a glance" demand.
    const auto &dragon = speciesSilhouette(speciesId{6});
    const auto &wolf   = speciesSilhouette(speciesId{8});
    CHECK(dragon.heightRatio > wolf.heightRatio);
    CHECK(dragon.girthRatio > wolf.girthRatio);

    // Unknown species id falls back to the human-scale row (id 1).
    const auto &fallback = speciesSilhouette(speciesId{999});
    const auto &human    = speciesSilhouette(speciesId{1});
    CHECK(fallback.species == human.species);
    CHECK(fallback.heightRatio == human.heightRatio);
    CHECK(fallback.girthRatio == human.girthRatio);

    // The compute path plumbs the silhouette into the visual.
    CharacterVisualInput input{};
    input.species           = speciesId{6}; // dragon
    input.isMoving          = false;
    const CharacterVisual v = computeCharacterVisual(input);
    CHECK(v.heightScale == dragon.heightRatio);
    CHECK(v.girthScale == dragon.girthRatio);
}

// ---------------------------------------------------------------------------
// TestCharVis_YawFollowsVelocity
//
// The yaw derives from the movement direction. 0 = -Z (forward),
// pi/2 = +X (right), pi = +Z (backward), -pi/2 = -X (left). Stationary =
// 0.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestCharVis_YawFollowsVelocity"
)
{
    const double kPi = 3.14159265358979323846;

    // Stationary: yaw 0.
    CHECK(yawFromVelocity(Position{0.0, 0.0, 0.0}) == doctest::Approx(0.0).epsilon(1e-9));

    // Forward (-Z): yaw 0.
    CHECK(yawFromVelocity(Position{0.0, 0.0, -1.0}) == doctest::Approx(0.0).epsilon(1e-9));

    // Right (+X): yaw pi/2.
    CHECK(yawFromVelocity(Position{1.0, 0.0, 0.0}) == doctest::Approx(kPi / 2.0).epsilon(1e-6));

    // Backward (+Z): yaw pi.
    CHECK(yawFromVelocity(Position{0.0, 0.0, 1.0}) == doctest::Approx(kPi).epsilon(1e-6));

    // Left (-X): yaw -pi/2.
    CHECK(yawFromVelocity(Position{-1.0, 0.0, 0.0}) == doctest::Approx(-kPi / 2.0).epsilon(1e-6));

    // The compute path plumbs the yaw into the visual.
    CharacterVisualInput input{};
    input.species           = speciesId{1};
    input.velocity          = Position{1.0, 0.0, 0.0};
    input.isMoving          = true;
    const CharacterVisual v = computeCharacterVisual(input);
    CHECK(v.yawRad == doctest::Approx(kPi / 2.0).epsilon(1e-6));

    // Vertical-only velocity (y axis): treated as stationary (yaw 0).
    // The card's convention is planar (x, z) — y is cosmetic.
    CHECK(yawFromVelocity(Position{0.0, 1.0, 0.0}) == doctest::Approx(0.0).epsilon(1e-9));
}

// ---------------------------------------------------------------------------
// TestCharVis_BobPhaseFromDistance
//
// The walk bob's phase comes from traveled distance, not wall time. The bob
// is a sine wave at WALK_BOB_HZ (2.2 Hz) at the reference walk speed (4 m/s).
// Stationary or corpse: 0.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestCharVis_BobPhaseFromDistance"
)
{
    // Stationary: 0.
    CHECK(walkBobOffset(0.0, false, false) == doctest::Approx(0.0).epsilon(1e-9));
    // Corpse: 0 (even if "moving").
    CHECK(walkBobOffset(100.0, true, true) == doctest::Approx(0.0).epsilon(1e-9));

    // Distance 0: sin(0) = 0.
    CHECK(walkBobOffset(0.0, true, false) == doctest::Approx(0.0).epsilon(1e-9));

    // The bob is periodic: at the reference speed, one full cycle is
    // referenceSpeed / WALK_BOB_HZ = 4 / 2.2 ≈ 1.818 m. After that distance,
    // the bob returns to 0.
    const double referenceSpeed = 4.0; // A-57
    const double cycleDistance  = referenceSpeed / WALK_BOB_HZ;
    CHECK(walkBobOffset(cycleDistance, true, false) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(walkBobOffset(2.0 * cycleDistance, true, false) == doctest::Approx(0.0).epsilon(1e-6));

    // Half a cycle: sin(pi) = 0 (the bob crosses zero).
    CHECK(walkBobOffset(cycleDistance / 2.0, true, false) == doctest::Approx(0.0).epsilon(1e-6));

    // Quarter cycle: sin(pi/2) = 1 (the bob peaks at the amplitude).
    const double quarter = cycleDistance / 4.0;
    const double bob     = walkBobOffset(quarter, true, false);
    CHECK(bob == doctest::Approx(WALK_BOB_AMPLITUDE_M).epsilon(1e-6));

    // The bob's magnitude never exceeds the amplitude.
    for (double d = 0.0; d < 10.0; d += 0.1)
    {
        const double b = walkBobOffset(d, true, false);
        CHECK(std::abs(b) <= WALK_BOB_AMPLITUDE_M + 1e-9);
    }

    // The compute path plumbs the bob into the visual.
    CharacterVisualInput input{};
    input.species           = speciesId{1};
    input.traveledDistanceM = quarter;
    input.isMoving          = true;
    const CharacterVisual v = computeCharacterVisual(input);
    CHECK(v.bobOffsetM == doctest::Approx(WALK_BOB_AMPLITUDE_M).epsilon(1e-6));
}

// ---------------------------------------------------------------------------
// TestCharVis_CorpseFlattenGrayUnchanged
//
// The corpse flag carries through the CharacterVisual; the bridge's
// flatten-and-gray rendering rule stays as-built (this module does NOT
// touch the corpse shape — it only carries the flag so the bridge can
// switch to the corpse shape without a separate query). The test asserts
// the flag carries and that a corpse has no walk bob.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestCharVis_CorpseFlattenGrayUnchanged"
)
{
    CharacterVisualInput input{};
    input.species           = speciesId{1};
    input.isCorpse          = true;
    input.isMoving          = true; // even if "moving", a corpse does not bob
    input.traveledDistanceM = 100.0;
    const CharacterVisual v = computeCharacterVisual(input);
    CHECK(v.isCorpse);
    CHECK(v.bobOffsetM == doctest::Approx(0.0).epsilon(1e-9)); // no bob for corpses

    // A living character has the same silhouette as a corpse of the same
    // species (the silhouette is per-species, not per-living-state). The
    // bridge's flatten-and-gray is a RENDERING choice, not a silhouette
    // change — this module carries the flag, the bridge applies the shape.
    CharacterVisualInput living   = input;
    living.isCorpse               = false;
    const CharacterVisual vLiving = computeCharacterVisual(living);
    CHECK_FALSE(vLiving.isCorpse);
    CHECK(vLiving.heightScale == v.heightScale);
    CHECK(vLiving.girthScale == v.girthScale);
}

// ---------------------------------------------------------------------------
// TestCharVis_NoSimStateTouched
//
// The visual derivation touches NO sim state. The test builds a
// CharacterVisualInput (a copy of the sim's per-character state), runs the
// derivation, and asserts the input is unchanged. The bridge composes the
// visual AFTER reading the sim's position, never before writing it — the
// walk bob must not modulate the collision/position the sim sees.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestCharVis_NoSimStateTouched"
)
{
    CharacterVisualInput input{};
    input.species           = speciesId{3}; // taur
    input.position          = Position{100.0, 0.0, 200.0};
    input.velocity          = Position{1.5, 0.0, -2.0};
    input.traveledDistanceM = 42.5;
    input.isMoving          = true;
    input.isCorpse          = false;

    // Snapshot the input.
    const auto snapshot = input;

    // Run the derivation.
    const CharacterVisual v = computeCharacterVisual(input);

    // The input is unchanged: no field mutated.
    CHECK(input.species == snapshot.species);
    CHECK(input.position.x == snapshot.position.x);
    CHECK(input.position.y == snapshot.position.y);
    CHECK(input.position.z == snapshot.position.z);
    CHECK(input.velocity.x == snapshot.velocity.x);
    CHECK(input.velocity.y == snapshot.velocity.y);
    CHECK(input.velocity.z == snapshot.velocity.z);
    CHECK(input.traveledDistanceM == snapshot.traveledDistanceM);
    CHECK(input.isMoving == snapshot.isMoving);
    CHECK(input.isCorpse == snapshot.isCorpse);

    // The visual is non-trivial (the derivation produced output).
    CHECK(v.heightScale > 0.0);
    CHECK(v.yawRad != 0.0); // velocity is non-zero
    // The bob is some value (sin of the phase); the exact value depends on
    // the phase, but it is bounded by the amplitude.
    CHECK(std::abs(v.bobOffsetM) <= WALK_BOB_AMPLITUDE_M + 1e-9);
}
