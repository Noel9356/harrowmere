// bridge/prop_meshes.cpp — the procedural tree geometry builders.
//
// See prop_meshes.hpp for the contract. The vocabulary is three species +
// one dead tree, each with a large (mature) and — except the dead tree — a
// small variant, all authored in meters at their final size:
//
//   oak   — tapered bent trunk, three rising branches, canopy of eight
//           noise-deformed blobs clustered around the crown;
//   pine  — straight trunk, four stacked skirted tiers climbing to a tip;
//   birch — slim pale trunk, three small blobs high on the stem;
//   dead  — bare trunk, two broken branch stubs, no canopy.
//
// Geometry helpers: appendTube (an arbitrary oriented tapered tube between
// two points — trunk and branches), appendCone (a tier skirt), appendBlob
// (a noise-deformed low-poly sphere). Normals are computed per primitive
// (radial for tubes/cones, the normalized offset for blobs) — no post-pass
// welding, the flat-shaded facets are the low-poly style.

#include "bridge/prop_meshes.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace harrowmere
{
namespace bridge
{

namespace
{

// splitmix64 (the presentation hash — D-IMG-2, no RNG streams).
inline std::uint64_t splitmix64(
    std::uint64_t x
) noexcept
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

inline double hashTo01(
    std::uint64_t h
) noexcept
{
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

struct MeshAccumulator
{
    std::vector<float> vertices;
    std::vector<std::uint32_t> indices;

    std::uint32_t vertex(
        float x, float y, float z, float nx, float ny, float nz
    )
    {
        vertices.push_back(x);
        vertices.push_back(y);
        vertices.push_back(z);
        vertices.push_back(nx);
        vertices.push_back(ny);
        vertices.push_back(nz);
        return static_cast<std::uint32_t>(vertices.size() / 6 - 1);
    }

    void triangle(
        std::uint32_t a, std::uint32_t b, std::uint32_t c
    )
    {
        indices.push_back(a);
        indices.push_back(b);
        indices.push_back(c);
    }
};

// An arbitrary-oriented tapered tube between two points (the trunk and the
// branch builder). The radial frame comes from a stable up vector: the
// axis least parallel to the segment, crossed against it, then the second
// axis closes the frame. 7 radial segments keeps the trunk facets visible
// in the low-poly style without wasting vertices.
void appendTube(
    MeshAccumulator &mesh, double x0, double y0, double z0, double x1, double y1, double z1,
    double r0, double r1, std::uint64_t &hash
)
{
    const double ax = x1 - x0;
    const double ay = y1 - y0;
    const double az = z1 - z0;
    const double length = std::sqrt(ax * ax + ay * ay + az * az);
    if (length < 1e-6)
        return;
    const double nx = ax / length;
    const double ny = ay / length;
    const double nz = az / length;

    // A stable frame: pick the world axis least aligned with the segment.
    double ux = 0.0, uy = 1.0, uz = 0.0;
    if (std::fabs(ny) > 0.9)
    {
        ux = 1.0;
        uy = 0.0;
    }
    // u = normalize(cross(n, up)), v = cross(n, u).
    double cx = ny * uz - nz * uy;
    double cy = nz * ux - nx * uz;
    double cz = nx * uy - ny * ux;
    const double cl = std::sqrt(cx * cx + cy * cy + cz * cz);
    cx /= cl;
    cy /= cl;
    cz /= cl;
    const double vx = ny * cz - nz * cy;
    const double vy = nz * cx - nx * cz;
    const double vz = nx * cy - ny * cx;

    constexpr int kSegments = 7;
    std::uint32_t baseRing[kSegments + 1];
    std::uint32_t topRing[kSegments + 1];
    for (int s = 0; s <= kSegments; ++s)
    {
        const double angle = (static_cast<double>(s) / kSegments) * 2.0 * std::numbers::pi_v<double>;
        const double ca = std::cos(angle);
        const double sa = std::sin(angle);
        // Radial direction in the tube frame.
        const double rx = cx * ca + vx * sa;
        const double ry = cy * ca + vy * sa;
        const double rz = cz * ca + vz * sa;
        baseRing[s] = mesh.vertex(
            static_cast<float>(x0 + rx * r0), static_cast<float>(y0 + ry * r0),
            static_cast<float>(z0 + rz * r0), static_cast<float>(rx), static_cast<float>(ry),
            static_cast<float>(rz));
        topRing[s] = mesh.vertex(
            static_cast<float>(x1 + rx * r1), static_cast<float>(y1 + ry * r1),
            static_cast<float>(z1 + rz * r1), static_cast<float>(rx), static_cast<float>(ry),
            static_cast<float>(rz));
    }
    // Slight per-tube twist: rotate the top ring's winding start so the
    // facets do not stripe vertically on every branch.
    const int twist = static_cast<int>(hashTo01(hash) * kSegments);
    std::uint32_t topTwisted[kSegments + 1];
    for (int s = 0; s <= kSegments; ++s)
    {
        topTwisted[s] = topRing[(s + twist) % kSegments];
    }
    for (int s = 0; s < kSegments; ++s)
    {
        mesh.triangle(baseRing[s], baseRing[s + 1], topTwisted[s]);
        mesh.triangle(baseRing[s + 1], topTwisted[s + 1], topTwisted[s]);
    }
    ++hash;
}

// A skirted cone tier (the pine builder): base ring at `baseY` with radius
// `r`, tip at `tipY`, ring vertices displaced by a small hash jitter so the
// tiers do not read as machined cones.
void appendCone(
    MeshAccumulator &mesh, double x, double z, double baseY, double tipY, double r, std::uint64_t &hash
)
{
    constexpr int kSegments = 8;
    const std::uint32_t tip = mesh.vertex(
        static_cast<float>(x), static_cast<float>(tipY), static_cast<float>(z), 0.0f, 1.0f, 0.0f);
    std::uint32_t ring[kSegments + 1];
    for (int s = 0; s <= kSegments; ++s)
    {
        const double angle = (static_cast<double>(s) / kSegments) * 2.0 * std::numbers::pi_v<double>;
        const double ca = std::cos(angle);
        const double sa = std::sin(angle);
        const double jitter = 0.85 + 0.3 * hashTo01(hash++);
        const double rr = r * jitter;
        // Skirt: the base ring sits BELOW baseY so the tier's lower edge
        // droops like foliage.
        const double y = baseY - 0.35 * jitter * r;
        ring[s] = mesh.vertex(
            static_cast<float>(x + ca * rr), static_cast<float>(y), static_cast<float>(z + sa * rr),
            static_cast<float>(ca), 0.45f, static_cast<float>(sa));
    }
    for (int s = 0; s < kSegments; ++s)
    {
        mesh.triangle(ring[s], ring[s + 1], tip);
        // An underside triangle so the skirt's lower edge never shows the
        // sky through the tier (backface-culled otherwise).
        mesh.triangle(ring[s + 1], ring[s], tip);
    }
}

// A noise-deformed low-poly sphere blob (the canopy builder): 7x5 lat/long
// grid, radius deformed ±12% by a smooth-ish hash so the blobs stop reading
// as billiard balls.
void appendBlob(
    MeshAccumulator &mesh, double cx, double cy, double cz, double radius, std::uint64_t &hash
)
{
    constexpr int kLon = 7;
    constexpr int kLat = 5;
    double deform[kLat + 1][kLon + 1];
    for (int la = 0; la <= kLat; ++la)
    {
        for (int lo = 0; lo <= kLon; ++lo)
        {
            deform[la][lo] = 0.88 + 0.24 * hashTo01(hash++);
        }
    }
    // Wrap the seam columns so the deformation closes around the sphere.
    for (int la = 0; la <= kLat; ++la)
    {
        deform[la][kLon] = deform[la][0];
    }

    std::uint32_t grid[kLat + 1][kLon + 1];
    for (int la = 0; la <= kLat; ++la)
    {
        const double polar = (static_cast<double>(la) / kLat) * std::numbers::pi_v<double>;
        const double sp = std::sin(polar);
        const double cp = std::cos(polar);
        for (int lo = 0; lo <= kLon; ++lo)
        {
            const double azimuth = (static_cast<double>(lo) / kLon) * 2.0 * std::numbers::pi_v<double>;
            const double nx = sp * std::cos(azimuth);
            const double ny = cp;
            const double nz = sp * std::sin(azimuth);
            const double r = radius * deform[la][lo];
            grid[la][lo] = mesh.vertex(
                static_cast<float>(cx + nx * r), static_cast<float>(cy + ny * r),
                static_cast<float>(cz + nz * r), static_cast<float>(nx), static_cast<float>(ny),
                static_cast<float>(nz));
        }
    }
    // The winding matches the engine's unit-sphere convention
    // (mesh_generation.cpp: (a, a+1, b) with a=(stack,slice), b=(stack+1,
    // slice) is outward-facing) - the blob's parameterization is that
    // sphere's verbatim, so the same order applies.
    for (int la = 0; la < kLat; ++la)
    {
        for (int lo = 0; lo < kLon; ++lo)
        {
            mesh.triangle(grid[la][lo], grid[la][lo + 1], grid[la + 1][lo]);
            mesh.triangle(grid[la][lo + 1], grid[la + 1][lo + 1], grid[la + 1][lo]);
        }
    }
}

} // namespace

PropMeshData buildOakTrunk(
    const std::uint64_t seed, const float scale
)
{
    MeshAccumulator mesh;
    std::uint64_t hash = splitmix64(seed ^ 0x0A0Aull);
    // The trunk: base at y=0, rising to the crown with a slight hash bend.
    const double h = 2.9 * scale;
    const double bendX = (hashTo01(hash++) - 0.5) * 0.5 * scale;
    const double bendZ = (hashTo01(hash++) - 0.5) * 0.5 * scale;
    appendTube(mesh, 0, 0, 0, bendX * 0.35, h * 0.55, bendZ * 0.35, 0.24 * scale, 0.16 * scale, hash);
    appendTube(mesh, bendX * 0.35, h * 0.55, bendZ * 0.35, bendX, h, bendZ, 0.16 * scale, 0.10 * scale, hash);
    // Three rising branches to carry the canopy blobs' silhouette.
    for (int b = 0; b < 3; ++b)
    {
        const double azimuth = static_cast<double>(b) * 2.094 + hashTo01(hash) * 0.9;
        const double startH = h * (0.52 + 0.14 * hashTo01(hash++));
        const double reach = (1.15 + 0.45 * hashTo01(hash++)) * scale;
        const double tipH = startH + reach * 0.72;
        appendTube(mesh, bendX * 0.4, startH, bendZ * 0.4, bendX + std::cos(azimuth) * reach, tipH,
            bendZ + std::sin(azimuth) * reach, 0.085 * scale, 0.03 * scale, hash);
    }
    return {std::move(mesh.vertices), std::move(mesh.indices)};
}

PropMeshData buildOakCanopy(
    const std::uint64_t seed, const float scale
)
{
    MeshAccumulator mesh;
    std::uint64_t hash = splitmix64(seed ^ 0x0B0Bull);
    // Rev 25 (the drama round): the crown HUGS the trunk top - the old
    // 4.1-scale crown rode 1.2 m above the 2.9-scale trunk, and at capture
    // distance the gap read as a lollipop stick. Center blob enlarged, ring
    // tightened and dropped so the whole mass overlaps into one crown with
    // a lumpy, lit silhouette (the faceted look the low-poly style wants).
    const double crownY = 3.5 * scale;
    appendBlob(mesh, 0, crownY, 0, 1.65 * scale, hash);
    for (int b = 0; b < 6; ++b)
    {
        const double azimuth = static_cast<double>(b) * 1.047 + hashTo01(hash) * 0.7;
        const double ring = (0.85 + 0.45 * hashTo01(hash++)) * scale;
        const double y = crownY + (hashTo01(hash++) - 0.55) * 1.3 * scale;
        appendBlob(mesh, std::cos(azimuth) * ring, y, std::sin(azimuth) * ring,
            (0.95 + 0.55 * hashTo01(hash++)) * scale, hash);
    }
    const double topX = (hashTo01(hash++) - 0.5) * 0.8 * scale;
    const double topZ = (hashTo01(hash++) - 0.5) * 0.8 * scale;
    appendBlob(mesh, topX, crownY + 1.05 * scale, topZ, 1.1 * scale, hash);
    return {std::move(mesh.vertices), std::move(mesh.indices)};
}

PropMeshData buildPineTrunk(
    const std::uint64_t seed, const float scale
)
{
    MeshAccumulator mesh;
    std::uint64_t hash = splitmix64(seed ^ 0x0C0Cull);
    const double h = 5.6 * scale;
    appendTube(mesh, 0, 0, 0, 0, h, 0, 0.19 * scale, 0.06 * scale, hash);
    return {std::move(mesh.vertices), std::move(mesh.indices)};
}

PropMeshData buildPineCanopy(
    const std::uint64_t seed, const float scale
)
{
    MeshAccumulator mesh;
    std::uint64_t hash = splitmix64(seed ^ 0x0D0Dull);
    // Four stacked tiers climbing to a tip: wide skirted cones, the lower
    // edges drooping (appendCone's skirt).
    const double tipY = 7.0 * scale;
    appendCone(mesh, 0, 0, 3.1 * scale, tipY, 2.05 * scale, hash);
    appendCone(mesh, 0, 0, 4.15 * scale, tipY, 1.6 * scale, hash);
    appendCone(mesh, 0, 0, 5.15 * scale, tipY, 1.15 * scale, hash);
    appendCone(mesh, 0, 0, 6.05 * scale, tipY, 0.7 * scale, hash);
    return {std::move(mesh.vertices), std::move(mesh.indices)};
}

PropMeshData buildBirchTrunk(
    const std::uint64_t seed, const float scale
)
{
    MeshAccumulator mesh;
    std::uint64_t hash = splitmix64(seed ^ 0x0E0Eull);
    const double h = 4.0 * scale;
    const double bendX = (hashTo01(hash++) - 0.5) * 0.4 * scale;
    appendTube(mesh, 0, 0, 0, bendX * 0.5, h * 0.5, 0, 0.115 * scale, 0.085 * scale, hash);
    appendTube(mesh, bendX * 0.5, h * 0.5, 0, bendX, h, 0, 0.085 * scale, 0.045 * scale, hash);
    return {std::move(mesh.vertices), std::move(mesh.indices)};
}

PropMeshData buildBirchCanopy(
    const std::uint64_t seed, const float scale
)
{
    MeshAccumulator mesh;
    std::uint64_t hash = splitmix64(seed ^ 0x0F0Full);
    // Three small blobs high on the slim stem.
    appendBlob(mesh, 0, 4.05 * scale, 0, 0.85 * scale, hash);
    appendBlob(mesh, 0.55 * scale, 4.35 * scale, 0.2 * scale, 0.6 * scale, hash);
    appendBlob(mesh, -0.4 * scale, 4.55 * scale, -0.3 * scale, 0.55 * scale, hash);
    return {std::move(mesh.vertices), std::move(mesh.indices)};
}

PropMeshData buildDeadTree(
    const std::uint64_t seed, const float scale
)
{
    MeshAccumulator mesh;
    std::uint64_t hash = splitmix64(seed ^ 0x1010ull);
    // A bare leaning trunk with two broken branch stubs — no canopy.
    const double h = 3.2 * scale;
    const double leanX = (hashTo01(hash++) - 0.5) * 1.1 * scale;
    const double leanZ = (hashTo01(hash++) - 0.5) * 1.1 * scale;
    appendTube(mesh, 0, 0, 0, leanX * 0.45, h * 0.5, leanZ * 0.45, 0.16 * scale, 0.11 * scale, hash);
    appendTube(mesh, leanX * 0.45, h * 0.5, leanZ * 0.45, leanX, h, leanZ, 0.11 * scale, 0.02 * scale, hash);
    appendTube(mesh, leanX * 0.35, h * 0.42, leanZ * 0.35, leanX * 0.35 + 0.9 * scale, h * 0.62,
        leanZ * 0.35 - 0.3 * scale, 0.07 * scale, 0.02 * scale, hash);
    appendTube(mesh, leanX * 0.6, h * 0.68, leanZ * 0.6, leanX * 0.6 - 0.7 * scale, h * 0.85,
        leanZ * 0.6 + 0.25 * scale, 0.06 * scale, 0.02 * scale, hash);
    return {std::move(mesh.vertices), std::move(mesh.indices)};
}

} // namespace bridge
} // namespace harrowmere
