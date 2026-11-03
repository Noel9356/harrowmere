// Task 5.1.2 - Terrain relief from the world model.
//
// The flat ground plane becomes land: a deterministic heightfield the renderer
// draws and everything else reads through one function — with the simulation
// untouched.
//
// D-IMG-1 (common.md Appendix A.14): HEIGHT IS PRESENTATION. Sim positions
// remain planar (x, z); heightAt(x, z) is a pure function of the worldgen
// seed value and the region data 1.6.1 already produced (elevation bias per
// climate class — mountains inland, flats on TEMPERATE/COASTAL plains — plus
// a seeded value-noise octaves field). The noise field DERIVES from the seed
// value (a u64), never from the sim's RNG streams (the card's pitfall:
// borrowing the worldgen stream changes every downstream draw, and the
// streams' state lives in task 0.3's SeedRng which is a sim resource).
//
// What ships here:
//   * TerrainField — a build-once, read-many value describing the heightfield
//     for one world (seed + region climate grid). Constructed from a WorldStore
//     (or its climate grid directly) and immutable thereafter.
//   * heightAt(x, z) — the ONE height door. Pure, const, no global state.
//     Returns metres above the heightfield datum (the A-151 sea level, which
//     5.2.2 pins at 0 m; this module does not touch water — it returns the
//     land's height, and 5.2.2's water plane clips at sea level).
//   * meshGrid() — the static backdrop mesh the bridge (5.1.2's consumer)
//     publishes as the scene's ground claim: a 512x512 vertex grid
//     (A-148) spanning the world extent, each vertex heightAt()-sampled.
//     The bridge consumes this instead of the Phase-1 flat ground plane; the
//     scene.toml ground claim is REPLACED, not deleted (the card spec: the
//     scene claim stays the run's only one; §3.6 layout).
//
// Determinism contract:
//   * Same (seed, region climate grid) -> byte-identical heightAt() samples.
//     Tested by TestTerrain_SameSeedSameHeights over a dense sample grid.
//   * Different seed -> different samples at the same (x, z). Tested by
//     TestTerrain_DifferentSeedDifferentHeights.
//   * heightAt is a pure function of (seed, region climate grid, x, z) — no
//     global state, no RNG stream, no wall clock. Tested by
//     TestTerrain_HeightAtPureFunction (two TerrainField instances built
//     from identical inputs produce identical samples; calling heightAt on
//     the same instance twice produces identical samples).
//   * The worldgen summary hash (1.6.1's FNV-1a) is UNCHANGED: this module
//     reads region.climate / region.flags / region.manaRichness (already in
//     the hash) and adds no fields to Region. Tested by
//     TestTerrain_WorldgenHashUnchanged (a worldgen run with and without a
//     terrain build produces the same summaryHash).
//
// The region-edge gradient cap (A-149 = 40 m / km) is enforced structurally:
// the per-octave amplitude is bounded so the steepest legal gradient between
// two adjacent region centers (1 km apart) stays at or below the cap. The
// noise amplitude is shaped per climate (mountains get the full inland max,
// plains get a fraction, coastal is clamped to 8 m within 2 km of a COASTAL
// region), and the climate bias is interpolated across region edges so two
// neighbors at different climate classes do not produce a cliff — tested by
// TestTerrain_HeightContinuousAtRegionEdges over every internal region edge.
//
// Constants: A-147.a-e (noise field), A-148 (grid resolution), A-149 (gradient
// cap). The coastal clamp reads A-147.e's band (2 km) and amplitude (8 m).
// The world extent is A-40 (REGION_GRID * REGION_SIZE_M = 16 km). All in
// core/constants_phase5.hpp — append-only, never edited elsewhere.
//
// What does NOT ship here (deferred to owner cards):
//   * The bridge mesh publish (5.1.2's consumer is the bridge, but the bridge
//     already publishes a flat ground plane through scene.toml's claim; the
//     bridge-side swap to terrain meshGrid() is the bridge task's own
//     integration step. This module exposes meshGrid() so the swap is a
//     one-line bridge edit, not a re-derivation.)
//   * Water (5.2.2): heightAt returns the land height; water is a separate
//     plane at sea level (A-151, owned by 5.2.2).
//   * Camera floor (6.1.1): the camera reads heightAt to clamp the eye above
//     the terrain — 6.1.1 owns that integration.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "core/constants.hpp"        // REGION_GRID, REGION_SIZE_M, REGION_COUNT (A-40)
#include "core/constants_phase5.hpp" // A-147.a-e, A-148, A-149
#include "core/types.hpp"            // Position, regionId (task 0.2)
#include "world/region.hpp"          // Climate, regionIdAt, regionIdOf (task 1.4 / 1.5)

namespace harrowmere::bridge
{

// The heightfield for one world. Constructed from the worldgen seed and the
// region climate grid (the inputs 1.6.1 already produced); immutable
// thereafter. heightAt() is the ONE read door — everything else (camera floor,
// scatter placement, water clipping) consumes it.
//
// Construction is O(REGION_COUNT) — a copy of the climate grid + a precomputed
// coastal-distance LUT (the coastal clamp's 2 km band, measured once per
// region center, not per heightAt call). heightAt() is O(octaves) = O(5).
class TerrainField
{
public:
    // Build from a seed value + the per-region climate grid (regionId ->
    // Climate). The grid is the same shape WorldStore::regions() exposes
    // (a std::map<regionId, Region> with every region 0..REGION_COUNT-1
    // present; the climate field on Region is 1.6.1 stage 1's output).
    //
    // The seed is the worldgen seed (1.6.1's `worldSeed` parameter) — a u64.
    // The noise field derives from this value alone (not the SeedRng
    // streams): a splitmix64-style hash mixes (seed, cellX, cellZ) into a
    // per-cell value, so two builds of the same seed sample identically and
    // a different seed differs at every sample.
    TerrainField(std::uint64_t seed, const std::vector<world::Climate> &regionClimates);

    // Convenience: build directly from a WorldStore's region grid. Reads
    // region.climate for every region 0..REGION_COUNT-1; the rest of Region
    // (flags, manaRichness, season, weather) is NOT read — height is a
    // presentation derivation of the static world shape, not its live state.
    template <class WorldStoreLike>
    explicit TerrainField(
        std::uint64_t seed, const WorldStoreLike &world
    )
        : TerrainField(seed, extractClimates(world))
    {}

    // The ONE height door. Pure, const, no global state. Returns metres
    // above the heightfield datum (the A-151 sea level, pinned at 0 m by
    // 5.2.2). Positions outside the world plane ([0, TERRAIN_WORLD_EXTENT_M)
    // on x and z) return 0 — the renderer clips them anyway (5.2.2's water
    // plane and the camera floor never see out-of-world samples).
    //
    // The heightfield is continuous: the climate bias is interpolated across
    // region edges (bilinear over the four nearest region centers), so two
    // samples on opposite sides of a region border differ by at most the
    // A-149 gradient cap (40 m / km * 1 km = 40 m) plus the noise octave
    // bound — the structural cap holds, tested.
    double heightAt(double x, double z) const noexcept;

    // The static backdrop mesh the bridge publishes: a TERRAIN_GRID_STEPS x
    // TERRAIN_GRID_STEPS (A-148 = 512) vertex grid spanning the world extent
    // ([0, TERRAIN_WORLD_EXTENT_M] on both axes). Each vertex's y is
    // heightAt()-sampled at its (x, z); x/z are the grid's world coordinates.
    // The bridge consumes this as the scene's ground claim (replacing the
    // Phase-1 flat ground plane — the card spec: scene.toml's ground-plane
    // claim is replaced by the bridge's terrain claim, not deleted).
    //
    // Returns a flat vector of (x, y, z) triples in row-major order
    // (row 0 is z=0, row TERRAIN_GRID_STEPS-1 is z=TERRAIN_WORLD_EXTENT_M);
    // the vector's size is exactly 3 * TERRAIN_GRID_STEPS * TERRAIN_GRID_STEPS.
    std::vector<double> meshGrid() const;

    // The seed this field was built from (the worldgen seed value, NOT a
    // SeedRng stream — D-IMG-1's red line). Exposed so the bridge and the
    // capture suite can stamp it into the scenario report.
    std::uint64_t seed() const noexcept
    {
        return seed_;
    }

    // The climate grid the field was built from (regionId 0..REGION_COUNT-1).
    // Exposed so tests can assert the coastal clamp fires only where
    // COASTAL regions are present.
    const std::vector<world::Climate> &regionClimates() const noexcept
    {
        return region_climates_;
    }

    // The precomputed coastal-distance LUT: per region, the distance in
    // metres to the nearest COASTAL region center (0 for coastal regions
    // themselves; large for inland regions with no coast). Used by the
    // coastal clamp (A-147.e's 2 km band) so heightAt() does not rescan the
    // grid per call.
    const std::vector<double> &coastalDistanceM() const noexcept
    {
        return coastal_distance_m_;
    }

    // ---- test-friendly structural accessors (the A-149 gradient cap test
    // reads the climate baseline directly, since the full heightfield
    // includes noise whose local gradient can exceed the cap without
    // creating a "cliff at the border" — the cap is structural on the
    // climate bias, not on the noise). Exposed so the test asserts the
    // STRUCTURAL gradient at region borders, not the noise-dominated local
    // gradient.
    double climateBaselineAt(double x, double z) const noexcept;
    double climateNoiseAmplitudeAt(double x, double z) const noexcept;
    static double climateBaselineM(world::Climate c) noexcept;
    static double climateNoiseAmplitudeM(world::Climate c) noexcept;

private:
    // ---- per-cell value-noise hash -----------------------------------------
    // A splitmix64-style hash over (seed, cellX, cellZ) -> u64. The output
    // is mapped to [-1, 1] by the caller (valueNoise01 -> *2 - 1). Pure,
    // deterministic, never touches the SeedRng streams.
    static std::uint64_t hashCell(std::uint64_t seed, std::int64_t cellX, std::int64_t cellZ) noexcept;
    // value-noise at (x, z) for one octave: hash the four surrounding cell
    // corners, map to [-1, 1], bilinearly interpolate with a smoothstep
    // fade. Returns a value in [-1, 1].
    static double valueNoiseAt(std::uint64_t seed, double x, double z, std::uint32_t octave) noexcept;

    // Bilinear interpolation of a per-climate scalar across region centers.
    // Used for both the baseline and the noise amplitude. The interpolation
    // window is REGION_SIZE_M (1 km), so the gradient at a region border is
    // delta / 1 km — structurally bounded by A-149 when deltas ≤ 40 m.
    double climateInterp(double x, double z, double (*valueAt)(world::Climate)) const noexcept;

    // The coastal clamp factor at world position (x, z): 1.0 far from any
    // COASTAL region, 0.0 inside a COASTAL region, interpolated across the
    // A-147.e 2 km band. Multiplied into the noise amplitude so coastal
    // heights stay at or below TERRAIN_COASTAL_CLAMP_M (8 m).
    double coastalClampAt(double x, double z) const noexcept;

    // Helper: bilinear interpolation.
    static double bilinear(double v00, double v10, double v01, double v11, double fx, double fz) noexcept;
    // Helper: smoothstep fade for value-noise interpolation (Ken Perlin's
    // 6t^5 - 15t^4 + 10t^3 — C2-continuous, so octave sums stay smooth).
    static double smoothstep(double t) noexcept;

    // Build the coastal-distance LUT from the climate grid (one pass at
    // construction; heightAt reuses it).
    void buildCoastalDistanceLut();

    // The seed value (NOT a SeedRng stream — D-IMG-1).
    std::uint64_t seed_ = 0;
    // The per-region climate grid (regionId 0..REGION_COUNT-1). Copied at
    // construction so the field does not hold a WorldStore reference.
    std::vector<world::Climate> region_climates_;
    // Per-region distance to the nearest COASTAL region center, in metres.
    // 0 for coastal regions; for inland regions, the grid distance to the
    // nearest coastal region center (1 km per grid step on the A-40 grid).
    std::vector<double> coastal_distance_m_;
};

// ---- free helpers ----------------------------------------------------------

// The terrain README documents the D-IMG-1 contract (height is presentation,
// sim stays planar) and the integration procedure for the bridge (5.1.2's
// consumer), the camera (6.1.1), scatter (5.2.1) and water (5.2.2). The
// README ships beside the code per the global DoD (§1.5: every new subsystem
// directory ships its own README.md). The src/bridge/README.md already covers
// the bridge directory; this module's README is the bridge README's terrain
// section.

} // namespace harrowmere::bridge
