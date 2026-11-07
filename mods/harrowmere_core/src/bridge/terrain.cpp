// Task 5.1.2 - Terrain relief from the world model (implementation).
//
// See terrain.hpp for the contract. This file owns:
//   * The per-cell value-noise hash (splitmix64-style over (seed, cellX, cellZ)).
//   * The octave sum (A-147.a = 5 octaves; A-147.b = 0.8 cycles/km base
//     frequency; A-147.c = 0.5 persistence; A-147.d = 120 m inland max
//     amplitude — the noise amplitude cap for MOUNTAIN regions).
//   * The climate elevation baseline (small, 0-30 m) and the climate noise
//     amplitude cap (8-120 m), both bilinearly interpolated across region
//     centers so region edges are continuous. The baseline deltas are pinned
//     ≤ 30 m so the A-149 gradient cap (40 m / km) holds at every internal
//     region edge: the bilinear interp's gradient at a border is
//     delta_baseline / REGION_SIZE_M = delta / 1 km ≤ 30 m / km.
//   * The coastal clamp (A-147.e = 8 m within 2 km of COASTAL regions): a
//     multiplicative factor on the noise amplitude, 0 inside a COASTAL region,
//     1 far inland, interpolated across the band.
//
// Determinism: every sample is a pure function of (seed_, region_climates_,
// x, z). No global state, no SeedRng stream, no wall clock. The hash function
// is the standard splitmix64 (public-domain, no license header needed) — the
// same algorithm SeedRng uses to seed its streams, but applied directly to
// (seed, cellX, cellZ) here, NOT through SeedRng's stream table.
//
// R5 (no float in game rules): heightAt is presentation, not a game rule, so
// f64 is correct here. The sim never calls it (D-IMG-1's red line).

#include "bridge/terrain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "core/constants.hpp" // REGION_GRID, REGION_SIZE_M, REGION_COUNT (A-40)

namespace harrowmere::bridge
{

namespace
{

// splitmix64 — the standard public-domain 64-bit hash (the same algorithm
// SeedRng uses to seed its streams, applied directly here). Mixing (seed,
// cellX, cellZ) into a u64; the output is mapped to [-1, 1] by the caller.
//
// This is NOT the worldgen RNG stream (D-IMG-1's pitfall): SeedRng's streams
// are sim resources with serialized state; this hash is a pure function of
// its arguments and carries no state.
inline std::uint64_t splitmix64(
    std::uint64_t x
) noexcept
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Map a u64 hash output to [0, 1). Uses the high 53 bits so there is no
// precision loss in f64 (the mantissa is 52 bits).
inline double hashTo01(
    std::uint64_t h
) noexcept
{
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0); // 2^53
}

} // namespace

// ---- TerrainField construction --------------------------------------------

TerrainField::TerrainField(
    std::uint64_t seed, const std::vector<world::Climate> &regionClimates
)
    : seed_(seed)
{
    // The climate grid must cover every region 0..REGION_COUNT-1 (1.6.1 stage
    // 1 assigns a climate to every region). Defensive: pad with TEMPERATE if
    // the input is short (a broken caller would still produce a continuous
    // field, never a crash).
    region_climates_.resize(static_cast<std::size_t>(REGION_COUNT), world::Climate::TEMPERATE);
    const std::size_t copy_n = std::min(regionClimates.size(), region_climates_.size());
    for (std::size_t i = 0; i < copy_n; ++i)
        region_climates_[i] = regionClimates[i];

    buildCoastalDistanceLut();
}

// ---- the ONE height door ---------------------------------------------------

double TerrainField::heightAt(
    double x, double z
) const noexcept
{
    // Out-of-world positions return 0 (the renderer clips them; 5.2.2's water
    // plane and the camera floor never see out-of-world samples).
    if (x < 0.0 || z < 0.0 || x >= TERRAIN_WORLD_EXTENT_M || z >= TERRAIN_WORLD_EXTENT_M)
        return 0.0;

    // 1. The climate elevation baseline (small, 0-30 m), bilinearly interpolated
    //    across the four nearest region centers. The baseline deltas are
    //    pinned ≤ 30 m so the A-149 gradient cap (40 m / km) holds at every
    //    internal region edge (the bilinear interp's gradient at a border is
    //    delta / REGION_SIZE_M = delta / 1 km).
    const double baseline = climateBaselineAt(x, z);

    // 2. The noise amplitude cap (8-120 m), bilinearly interpolated across
    //    region centers, then multiplied by the coastal clamp factor.
    const double amplitude_cap       = climateNoiseAmplitudeAt(x, z);
    const double coastal_clamp       = coastalClampAt(x, z);
    const double effective_amplitude = amplitude_cap * coastal_clamp;

    // 3. The noise octave sum (value-noise with smoothstep). Each octave
    //    doubles frequency and multiplies amplitude by persistence. The
    //    base frequency (A-147.b = 0.8 cycles/km) sets the coarsest octave's
    //    wavelength at 1.25 km.
    double noise_sum = 0.0;
    double amplitude = 1.0;
    double total_amp = 0.0;
    for (std::uint32_t octave = 0; octave < TERRAIN_NOISE_OCTAVES; ++octave)
    {
        const double frequency = TERRAIN_NOISE_BASE_FREQUENCY * std::pow(2.0, static_cast<double>(octave)) / 1000.0;
        const double sample    = valueNoiseAt(seed_, x * frequency, z * frequency, octave);
        noise_sum += sample * amplitude;
        total_amp += amplitude;
        amplitude *= TERRAIN_NOISE_PERSISTENCE;
    }
    if (total_amp > 0.0)
        noise_sum /= total_amp; // normalize to [-1, 1]

    // 4. Combine: baseline + noise * effective_amplitude. Clamped to the
    //    basin floor (rev 24): the old clamp at exactly 0 flattened every
    //    basin to the datum, and with the water plane below the datum the
    //    world had NO visible water anywhere (the rev-24 world scan: minimum
    //    0.00 across the whole patch). The floor now sits TERRAIN_BASIN_FLOOR
    //    below the datum, so the noise's natural basins hold real lakes: the
    //    water plane (A-151 datum - 0.3 m) shows wherever the ground falls
    //    below it, the datum-level flats stay dry shore, and the scatter's
    //    SEA_LEVEL rejection keeps props and cottages out of the water.
    double height = baseline + noise_sum * effective_amplitude;
    height = std::max(TERRAIN_BASIN_FLOOR_M, height);

    // 5. Rev 25 (the drama round): the settlement pads. Inside SETTLEMENT
    //    FLAT RADIUS the ground is exactly the pad's level; a smoothstep
    //    band hands back to the field. Pads sit dry (the caller clamps the
    //    base above the sea level) so a village never floats or drowns, and
    //    the C2 blend keeps the A-149 gradient promise outside the band.
    for (const SettlementFlat &flat : settlement_flats_)
    {
        const double dx  = x - flat.x;
        const double dz  = z - flat.z;
        const double dSq = dx * dx + dz * dz;
        if (dSq >= TERRAIN_SETTLEMENT_BLEND_R2_M) // 92 m squared
        {
            continue;
        }
        const double d     = std::sqrt(dSq);
        const double t     = d <= TERRAIN_SETTLEMENT_FLAT_RADIUS_M
                                 ? 0.0
                                 : smoothstep((d - TERRAIN_SETTLEMENT_FLAT_RADIUS_M)
                                              / (TERRAIN_SETTLEMENT_BLEND_RADIUS_M - TERRAIN_SETTLEMENT_FLAT_RADIUS_M));
        height = flat.baseHeight + (height - flat.baseHeight) * t;
    }
    return height;
}

// ---- mesh grid (the bridge's ground claim) --------------------------------

std::vector<double> TerrainField::meshGrid() const
{
    // A-148 = 512 steps across the world patch. The grid spans [0,
    // TERRAIN_WORLD_EXTENT_M] on both axes; one cell is
    // TERRAIN_WORLD_EXTENT_M / TERRAIN_GRID_STEPS metres (~31.25 m at A-40's
    // 16 km world).
    const std::size_t steps = TERRAIN_GRID_STEPS;
    const double step_m     = TERRAIN_WORLD_EXTENT_M / static_cast<double>(steps);
    std::vector<double> grid;
    grid.resize(3 * steps * steps);
    for (std::size_t row = 0; row < steps; ++row)
    {
        const double z = static_cast<double>(row) * step_m;
        for (std::size_t col = 0; col < steps; ++col)
        {
            const double x        = static_cast<double>(col) * step_m;
            const double y        = heightAt(x, z);
            const std::size_t idx = (row * steps + col) * 3;
            grid[idx + 0]         = x;
            grid[idx + 1]         = y;
            grid[idx + 2]         = z;
        }
    }
    return grid;
}

// ---- per-cell value-noise hash --------------------------------------------

std::uint64_t TerrainField::hashCell(
    std::uint64_t seed, std::int64_t cellX, std::int64_t cellZ
) noexcept
{
    // Mix seed, cellX, cellZ into one u64. The order is fixed forever so two
    // builds of the same seed sample identically (the determinism contract).
    std::uint64_t h = seed;
    h               = splitmix64(h ^ (static_cast<std::uint64_t>(cellX) * 0x9E3779B97F4A7C15ull));
    h               = splitmix64(h ^ (static_cast<std::uint64_t>(cellZ) * 0xD1B54A32D192ED03ull));
    return h;
}

double TerrainField::valueNoiseAt(
    std::uint64_t seed, double x, double z, std::uint32_t octave
) noexcept
{
    // The four cell corners surrounding (x, z). Each corner's value is
    // hashTo01(hashCell(seed, cellX, cellZ, octave)) mapped to [-1, 1].
    // The octave index is mixed into the seed so two octaves do not read
    // the same value.
    const auto hash01 = [seed, octave](std::int64_t cx, std::int64_t cz) noexcept -> double {
        const std::uint64_t octave_seed = seed ^ (static_cast<std::uint64_t>(octave) * 0x9E3779B97F4A7C15ull);
        return hashTo01(hashCell(octave_seed, cx, cz)) * 2.0 - 1.0;
    };

    const std::int64_t x0 = static_cast<std::int64_t>(std::floor(x));
    const std::int64_t z0 = static_cast<std::int64_t>(std::floor(z));
    const std::int64_t x1 = x0 + 1;
    const std::int64_t z1 = z0 + 1;

    const double v00 = hash01(x0, z0);
    const double v10 = hash01(x1, z0);
    const double v01 = hash01(x0, z1);
    const double v11 = hash01(x1, z1);

    const double fx = smoothstep(x - static_cast<double>(x0));
    const double fz = smoothstep(z - static_cast<double>(z0));

    return bilinear(v00, v10, v01, v11, fx, fz);
}

// ---- climate baseline & noise amplitude -----------------------------------

double TerrainField::climateBaselineAt(
    double x, double z
) const noexcept
{
    return climateInterp(x, z, climateBaselineM);
}

double TerrainField::climateNoiseAmplitudeAt(
    double x, double z
) const noexcept
{
    return climateInterp(x, z, climateNoiseAmplitudeM);
}

double TerrainField::climateBaselineM(
    world::Climate c
) noexcept
{
    // The per-climate baseline elevation (the heightfield's "sea-level
    // reference" for each climate). Small values so the A-149 gradient cap
    // holds at every internal region edge: the bilinear interp's gradient at
    // a border is delta_baseline / REGION_SIZE_M (1 km), so with max delta
    // 30 m (MOUNTAIN to COASTAL) the gradient is 30 m / km ≤ 40 m / km ✓.
    switch (c)
    {
    case world::Climate::MOUNTAIN:
        return 30.0; // alpine valleys
    case world::Climate::FOREST:
        return 18.0; // rolling woodland
    case world::Climate::SWAMP:
        return 4.0; // low wetlands
    case world::Climate::TEMPERATE:
        return 10.0; // gentle plains
    case world::Climate::COASTAL:
        return 0.0; // sea-level shore
    default:
        return 10.0;
    }
}

double TerrainField::climateNoiseAmplitudeM(
    world::Climate c
) noexcept
{
    // The per-climate noise amplitude cap. A-147.d = 120 m is the inland max
    // (MOUNTAIN). A-147.e = 8 m is the coastal clamp (COASTAL). Others scale
    // between. These are the AMPLITUDE of the value-noise modulation around
    // the baseline; the noise itself is C2-continuous (smoothstep), so it
    // contributes no discontinuity at region borders — the gradient cap is
    // structural on the baseline, not the noise.
    switch (c)
    {
    case world::Climate::MOUNTAIN:
        return TERRAIN_INLAND_MAX_AMPLITUDE_M; // 120 m (A-147.d)
    case world::Climate::FOREST:
        // Rev 25 (the drama round): the old caps (48/24/18 m) drew a plain
        // the camera could not tell from a table top - the owner's "this
        // scene is boring" verdict. The caps rise so the rolling hills
        // actually read (and feed the rev-25 heightfield shadows).
        return TERRAIN_INLAND_MAX_AMPLITUDE_M * 0.62; // 74 m
    case world::Climate::SWAMP:
        return TERRAIN_INLAND_MAX_AMPLITUDE_M * 0.22; // 26 m
    case world::Climate::TEMPERATE:
        return TERRAIN_INLAND_MAX_AMPLITUDE_M * 0.42; // 50 m
    case world::Climate::COASTAL:
        return TERRAIN_COASTAL_CLAMP_M; // 8 m (A-147.e)
    default:
        return TERRAIN_INLAND_MAX_AMPLITUDE_M * 0.42;
    }
}

// Bilinear interpolation of a per-climate scalar across region centers.
// Used for both the baseline and the noise amplitude. The interpolation
// window is REGION_SIZE_M (1 km) — the distance between adjacent region
// centers — so the gradient at a region border is delta / 1 km, structurally
// bounded by the A-149 cap when the deltas are ≤ 40 m.
double TerrainField::climateInterp(
    double x, double z, double (*valueAt)(world::Climate)
) const noexcept
{
    const double region_size = static_cast<double>(REGION_SIZE_M);
    // Position relative to region centers: each region center is at
    // (i + 0.5) * region_size (region.hpp's regionCenter()).
    const double x_rel = (x - region_size * 0.5) / region_size;
    const double z_rel = (z - region_size * 0.5) / region_size;
    if (x_rel < 0.0 || z_rel < 0.0)
        return valueAt(world::Climate::TEMPERATE);

    const std::int64_t cx = static_cast<std::int64_t>(std::floor(x_rel));
    const std::int64_t cz = static_cast<std::int64_t>(std::floor(z_rel));

    const auto climateValue = [this, valueAt](std::int64_t ix, std::int64_t iz) noexcept -> double {
        const std::uint32_t ux = static_cast<std::uint32_t>(std::clamp<std::int64_t>(ix, 0, REGION_GRID - 1));
        const std::uint32_t uz = static_cast<std::uint32_t>(std::clamp<std::int64_t>(iz, 0, REGION_GRID - 1));
        const regionId r       = world::regionIdOf(ux, uz);
        if (r >= REGION_COUNT)
            return valueAt(world::Climate::TEMPERATE);
        return valueAt(region_climates_[r]);
    };

    const double v00 = climateValue(cx, cz);
    const double v10 = climateValue(cx + 1, cz);
    const double v01 = climateValue(cx, cz + 1);
    const double v11 = climateValue(cx + 1, cz + 1);

    const double fx = smoothstep(x_rel - static_cast<double>(cx));
    const double fz = smoothstep(z_rel - static_cast<double>(cz));

    return bilinear(v00, v10, v01, v11, fx, fz);
}

// ---- coastal clamp ---------------------------------------------------------

double TerrainField::coastalClampAt(
    double x, double z
) const noexcept
{
    // The coastal clamp factor: 1.0 far from any COASTAL region, 0.0 inside
    // a COASTAL region, interpolated across the A-147.e 2 km band. Uses the
    // precomputed coastal_distance_m_ LUT (one BFS at construction).
    const double region_size = static_cast<double>(REGION_SIZE_M);
    const double x_rel       = (x - region_size * 0.5) / region_size;
    const double z_rel       = (z - region_size * 0.5) / region_size;
    if (x_rel < 0.0 || z_rel < 0.0)
        return 1.0;

    const std::int64_t cx = static_cast<std::int64_t>(std::floor(x_rel));
    const std::int64_t cz = static_cast<std::int64_t>(std::floor(z_rel));

    const auto coastalDist = [this](std::int64_t ix, std::int64_t iz) noexcept -> double {
        const std::uint32_t ux = static_cast<std::uint32_t>(std::clamp<std::int64_t>(ix, 0, REGION_GRID - 1));
        const std::uint32_t uz = static_cast<std::uint32_t>(std::clamp<std::int64_t>(iz, 0, REGION_GRID - 1));
        const regionId r       = world::regionIdOf(ux, uz);
        if (r >= REGION_COUNT)
            return 1e9;
        return coastal_distance_m_[r];
    };

    const double d00 = coastalDist(cx, cz);
    const double d10 = coastalDist(cx + 1, cz);
    const double d01 = coastalDist(cx, cz + 1);
    const double d11 = coastalDist(cx + 1, cz + 1);

    const double fx = smoothstep(x_rel - static_cast<double>(cx));
    const double fz = smoothstep(z_rel - static_cast<double>(cz));

    const double dist = bilinear(d00, d10, d01, d11, fx, fz);

    // 0 inside a COASTAL region (dist 0), 1 far away (dist ≥ band), linear
    // across the band.
    if (dist <= 0.0)
        return 0.0;
    if (dist >= TERRAIN_COASTAL_BAND_M)
        return 1.0;
    return dist / TERRAIN_COASTAL_BAND_M;
}

// ---- helpers ---------------------------------------------------------------

double TerrainField::bilinear(
    double v00, double v10, double v01, double v11, double fx, double fz
) noexcept
{
    const double a = v00 + (v10 - v00) * fx;
    const double b = v01 + (v11 - v01) * fx;
    return a + (b - a) * fz;
}

double TerrainField::smoothstep(
    double t
) noexcept
{
    // Ken Perlin's fade: 6t^5 - 15t^4 + 10t^3. C2-continuous, so octave sums
    // stay smooth (no discontinuities at cell boundaries).
    if (t <= 0.0)
        return 0.0;
    if (t >= 1.0)
        return 1.0;
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

// ---- coastal-distance LUT -------------------------------------------------

void TerrainField::buildCoastalDistanceLut()
{
    // Multi-source BFS over the region grid: COASTAL regions are the sources,
    // distance is grid-step count * REGION_SIZE_M (close enough for the 2 km
    // band; the bilinear interpolation in coastalClampAt smooths the discrete
    // grid into a continuous field). O(REGION_COUNT) — trivial at 256 regions.
    coastal_distance_m_.assign(static_cast<std::size_t>(REGION_COUNT), 1e9);

    std::vector<std::pair<std::uint32_t, std::uint32_t>> queue;
    for (std::uint32_t z = 0; z < REGION_GRID; ++z)
    {
        for (std::uint32_t x = 0; x < REGION_GRID; ++x)
        {
            const regionId r = world::regionIdOf(x, z);
            if (r >= REGION_COUNT)
                continue;
            if (region_climates_[r] == world::Climate::COASTAL)
            {
                coastal_distance_m_[r] = 0.0;
                queue.emplace_back(x, z);
            }
        }
    }

    // No COASTAL region: every distance stays at the sentinel; the clamp
    // factor is 1.0 everywhere (no coastal flattening). The world still has
    // relief from the inland amplitude and climate baseline.
    if (queue.empty())
        return;

    const double step_m = static_cast<double>(REGION_SIZE_M);
    // Simple BFS with a work list (no deque needed; the grid is small).
    std::size_t head = 0;
    while (head < queue.size())
    {
        const auto [x, z] = queue[head++];
        const regionId r  = world::regionIdOf(x, z);
        if (r >= REGION_COUNT)
            continue;
        const double base_dist = coastal_distance_m_[r];

        const std::array<std::pair<std::int32_t, std::int32_t>, 4> neighbours = {{
            {static_cast<std::int32_t>(x) - 1, static_cast<std::int32_t>(z)},
            {static_cast<std::int32_t>(x) + 1, static_cast<std::int32_t>(z)},
            {static_cast<std::int32_t>(x), static_cast<std::int32_t>(z) - 1},
            {static_cast<std::int32_t>(x), static_cast<std::int32_t>(z) + 1},
        }};
        for (const auto &[nx, nz] : neighbours)
        {
            if (nx < 0 || nz < 0 || nx >= static_cast<std::int32_t>(REGION_GRID)
                || nz >= static_cast<std::int32_t>(REGION_GRID))
                continue;
            const regionId nr = world::regionIdOf(static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(nz));
            if (nr >= REGION_COUNT)
                continue;
            const double new_dist = base_dist + step_m;
            if (new_dist < coastal_distance_m_[nr])
            {
                coastal_distance_m_[nr] = new_dist;
                queue.emplace_back(static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(nz));
            }
        }
    }
}

} // namespace harrowmere::bridge
