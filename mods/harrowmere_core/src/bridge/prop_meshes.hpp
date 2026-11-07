#ifndef HARROWMERE_CORE_BRIDGE_PROP_MESHES_HPP
#define HARROWMERE_CORE_BRIDGE_PROP_MESHES_HPP

// bridge/prop_meshes.hpp — the procedural prop-mesh vocabulary (rev 24).
//
// The owner's defect: trees were one box + one sphere ("lollipops"). These
// builders generate proper low-poly tree GEOMETRY — tapered branching
// trunks, layered or blob-clustered canopies — as triangle meshes the
// renderer rasterizes with its real vertices (mesh primitives: the analytic
// tracer keeps skipping them, the known no-shadow limit).
//
// Every builder is a PURE function of its seed (splitmix64, D-IMG-2 — no
// RNG stream, no clock). All meshes are authored in METERS, base at y = 0,
// +Y up (the renderer's mesh contract: instances translate + rotateY only).
// Vertex layout: position xyz + normal xyz interleaved (6 floats), indices
// u32 — the scene registry's SceneMeshDescriptor layout verbatim.

#include <array>
#include <cstdint>
#include <vector>

namespace harrowmere
{
namespace bridge
{

// One generated prop mesh (SceneMeshDescriptor's vertex/index payload).
struct PropMeshData
{
    std::vector<float> vertices;          // 6 floats per vertex: pos xyz, normal xyz
    std::vector<std::uint32_t> indices;
};

// The registered mesh names (the sink + the adapter address them by name).
inline constexpr const char *kMeshOakTrunkLg      = "prop_oak_trunk_lg";
inline constexpr const char *kMeshOakTrunkSm      = "prop_oak_trunk_sm";
inline constexpr const char *kMeshOakCanopyLg     = "prop_oak_canopy_lg";
inline constexpr const char *kMeshOakCanopySm     = "prop_oak_canopy_sm";
inline constexpr const char *kMeshPineTrunkLg     = "prop_pine_trunk_lg";
inline constexpr const char *kMeshPineTrunkSm     = "prop_pine_trunk_sm";
inline constexpr const char *kMeshPineCanopyLg    = "prop_pine_canopy_lg";
inline constexpr const char *kMeshPineCanopySm    = "prop_pine_canopy_sm";
inline constexpr const char *kMeshBirchTrunkLg    = "prop_birch_trunk_lg";
inline constexpr const char *kMeshBirchTrunkSm    = "prop_birch_trunk_sm";
inline constexpr const char *kMeshBirchCanopyLg   = "prop_birch_canopy_lg";
inline constexpr const char *kMeshBirchCanopySm   = "prop_birch_canopy_sm";
inline constexpr const char *kMeshDeadTree        = "prop_dead_tree";

// The species builders (each seed makes a distinct but stable shape).
PropMeshData buildOakTrunk(std::uint64_t seed, float scale);
PropMeshData buildOakCanopy(std::uint64_t seed, float scale);
PropMeshData buildPineTrunk(std::uint64_t seed, float scale);
PropMeshData buildPineCanopy(std::uint64_t seed, float scale);
PropMeshData buildBirchTrunk(std::uint64_t seed, float scale);
PropMeshData buildBirchCanopy(std::uint64_t seed, float scale);
PropMeshData buildDeadTree(std::uint64_t seed, float scale);

// The deterministic per-placement seed: a splitmix64 pass over the
// placement's planar position quantized to centimeters (D-IMG-2 — pure
// function of the placement, never the worldgen RNG stream).
inline std::uint64_t propMeshSeed(
    double x, double z
) noexcept
{
    const auto quantize = [](double v) {
        const auto q = static_cast<std::int64_t>(v * 100.0);
        return static_cast<std::uint64_t>(q);
    };
    std::uint64_t h = 0x9E3779B97F4A7C15ull;
    h = (h ^ (quantize(x) * 0xBF58476D1CE4E5B9ull)) + 0x9E3779B97F4A7C15ull;
    h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull;
    h = (h ^ (quantize(z) * 0x94D049BB133111EBull)) + 0x9E3779B97F4A7C15ull;
    h = (h ^ (h >> 27)) * 0x94D049BB133111EBull;
    return h ^ (h >> 31);
}

} // namespace bridge
} // namespace harrowmere

#endif // HARROWMERE_CORE_BRIDGE_PROP_MESHES_HPP
