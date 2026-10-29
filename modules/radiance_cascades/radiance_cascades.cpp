#include "radiance_cascades.hpp"

#include "lighting_capability.hpp"
#include "light_uniform.hpp"
#include "render_interface.hpp"
#include "renderer.hpp"
#include "scene_description.hpp"
#include "vulkan_utilities.hpp"

#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace engine
{

namespace
{

constexpr std::string_view moduleContext = "radiance_cascades module";

// Direction lanes packed into one march/compose workgroup (the shaders'
// local_size_z): 8 lanes turn the 16-thread 4x4 tile into a 128-thread
// workgroup - two full warps on 32-lane hardware - and let the march's anchor
// lane amortize one probe anchor across all of them. Dispatch z is
// (directions + lanes - 1) / lanes, so a direction count that is not a
// multiple of 8 simply idles the tail lanes (the shaders' invocation guards
// bound every axis).
constexpr std::uint32_t marchDirectionLanes = 8;

// Technique settings defaults. The Settings struct's member initializers and
// the [lighting.radiance_cascades] fallbacks state the same numbers through
// these constants: the struct defaults previously drifted from the config
// fallbacks (probeStride 8 vs 4, baseRays 64 vs 32, losSteps 16 vs 24) -
// unreachable but a silent trap for the first direct construction. The
// values agree with the module's shipped config.toml (the defaults layer the
// build copies next to every runtime): the code fallback and the shipped
// default state the SAME quality/cost balance, so a runtime whose defaults
// file went missing behaves exactly like the documented one.
constexpr std::uint32_t defaultCascades            = 5;
constexpr float defaultIntervalBase                = 0.5f;
constexpr float defaultIntervalFactor              = 3.0f;
constexpr std::uint32_t defaultProbeStride         = 8;
constexpr std::uint32_t defaultBaseRays            = 128;
constexpr std::uint32_t defaultLosSteps            = 16;
constexpr std::uint32_t defaultMarchSteps          = 48;
constexpr std::uint32_t defaultShadowSteps         = 24;
constexpr std::uint32_t defaultSpatialFilterRadius = 10;

// Angular mapping of the cascade field's directional domain. Every mode
// stores one image layer per direction (the march/compose stages are
// mapping-agnostic - they read the precomputed direction table), the modes
// differ in WHICH directions cover the sphere and how the gather integrates
// them into irradiance:
//
// - octahedron: the direction set of an octahedral equal-basis map texel
//   grid (N x N texels over the sphere, ~base_rays directions). The gather
//   integrates the field with each texel's exact solid angle. DEFAULT -
//   even texel areas keep the quadrature free of the equirect schedule's
//   pole clustering, at the same direction budget.
// - cubemap:    six face grids (6 * F^2 directions, ~base_rays). The gather
//   integrates with each cube texel's exact solid angle.
// - spherical_harmonic: the march runs on the octahedral direction set, but
//   the gather PROJECTS the composed field onto the real spherical harmonics
//   through band 2 and evaluates the irradiance from the coefficients
//   (the Ramamoorthi-Hanrahan truncated cosine-lobe convolution). The field
//   is band-limited: smoother than any texel quadrature, which reads as
//   softly blurred GI at the same march cost.
enum class CascadeAngularMapping : std::uint32_t
{
    Octahedron        = 0,
    Cubemap           = 1,
    SphericalHarmonic = 2,
};

// The [lighting.radiance_cascades].mapping spellings.
constexpr const char *defaultMappingName = "octahedron";

bool parseMappingName(
    const std::string &name, CascadeAngularMapping &outMapping
)
{
    if (name == "octahedron")
    {
        outMapping = CascadeAngularMapping::Octahedron;
        return true;
    }
    if (name == "cubemap")
    {
        outMapping = CascadeAngularMapping::Cubemap;
        return true;
    }
    if (name == "spherical_harmonic")
    {
        outMapping = CascadeAngularMapping::SphericalHarmonic;
        return true;
    }
    return false;
}

// Probe PLACEMENT strategy. The Radiance Cascades algorithm is
// placement-agnostic - WHERE the probes live is a strategy on top of the
// interval/partition scheme the algorithm describes. The module provides one
// demand marker capability per placement ("screen_space_radiance_cascades",
// "world_space_radiance_cascades"), and the run selects the placement by
// demanding exactly one of them - the capability name IS the placement
// contract, so a mod states which transport it wants and no configuration key
// can disagree with the demand. The current build ships BOTH placements:
//
// - screen_space_radiance_cascades: probes anchor on the visible frame
//   (depth buffer). A hit whose projection lands off screen carries the
//   direct term only - the classic screen-space limitation.
//
// - world_space_radiance_cascades: probes anchor in a dense 3D world-space
//   grid (camera-centered, cell size = world_cell_size * 2^cascade). The
//   march traces the SDF from each probe's world position, so probes can
//   sample off-screen geometry (the whole point of world-space RC - the
//   algorithm's name describes the interval hierarchy, not the placement;
//   see the README for the algorithm-true rationale). The grid is small
//   (default 8x8x4 cells per cascade) and 2D-array-image storage with the
//   (cell_y, cell_z) pair flattened along the Y axis keeps layers =
//   directionCount independent of grid dimensions. The gather blends 8
//   trilinear probes per cascade at the pixel's surface world position.

// Light transport bounces inside one frame: bounce 0 relights march hits
// with the direct lights only; bounce b > 0 adds the previous bounce's
// gathered transport at the hit point (the march samples the gather image of
// bounce b-1 through the hit's screen position). 1 = the historical single
// bounce; every extra bounce re-runs the whole cascade sequence, so the cost
// is linear in bounces.
constexpr std::uint32_t defaultBounces = 1;

// Direction table ceiling: Vulkan only guarantees maxImageArrayLayers >= 256,
// and every cascade image carries one layer per direction. The octahedron
// grid stays within that (16 x 16 = 256); the cubemap grid is clamped to six
// texels per face (6 * 36 = 216). The octahedron floor of 3 texels per axis
// keeps every texel's corner quad free of antipodal vertex pairs (with 2 the
// corner texels span both poles, where the spherical-triangle solid angle
// formula degenerates); the cubemap texel formula is exact at any size.
constexpr std::uint32_t maxOctahedronTexelsPerAxis = 16;
constexpr std::uint32_t minOctahedronTexelsPerAxis = 3;
constexpr std::uint32_t maxCubemapFaceTexels       = 6;
constexpr std::uint32_t minCubemapFaceTexels       = 1;

// Push-constant blocks as the .comp shaders see them (file scope so the
// pipeline layouts size their ranges with sizeof; the static_asserts pin the
// shader-side byte counts).
struct MarchPush
{
    std::array<float, 4> interval;
    std::array<std::uint32_t, 4> flags;
    std::array<std::uint32_t, 4> grid;
    std::array<std::uint32_t, 4> extentStride;
    // Base layer of this cascade's anchor pair in the shared probe anchor
    // field (cascade * 2; the march writes anchor+depth on the base layer and
    // the surface normal one layer up, the gather loads both back).
    std::array<std::uint32_t, 4> anchor;
};

struct ComposePush
{
    std::array<float, 4> coarsestFlag; // w 1 when this is the coarsest cascade
    std::array<std::uint32_t, 4> fineGrid;
    std::array<std::uint32_t, 4> coarseGrid;
};

struct GatherPush
{
    std::array<std::uint32_t, 4> counts;
    std::array<float, 4> schedule;
    std::array<std::uint32_t, 4> dirs;
};

struct FilterPush
{
    std::array<std::int32_t, 4> params;
    std::array<float, 4> cameraPlanes;
};

// Push-constant blocks for the WORLD-SPACE placement shaders. The screen-
// space structs above stay byte-identical to their .comp shader counterparts;
// the world-space structs here mirror rc_world_*.comp's push_constant
// declarations byte-for-byte (the shader struct name is local to the shader;
// only the byte layout must match).
//
// World-space march uses a SEPARATE WorldMarchPush struct (not the screen-
// space MarchPush) because the extentStride field's interpretation differs:
// the screen-space shader reads extentStride as uvec4 (extent.width, extent.
// height, probeStride << cascade, probeStride << cascade) while the world-
// space shader reads it as vec4 (gridOrigin.xyz world, cellSize). The byte
// layout is identical (16 bytes), but the C++ type must match the values
// being stored: floats for world-space (origin and cellSize), uints for
// screen-space (extent and stride). Storing floats in a uint field narrows
// them silently (truncation, not bit-cast), so the two placements need
// distinct structs.
struct WorldMarchPush
{
    std::array<float, 4> interval;       // x tStart, y tEnd (world-space depth along the ray, from the probe anchor)
    std::array<std::uint32_t, 4> flags;  // x primitiveCount, y maxSteps, z unused, w bounce index
    std::array<std::uint32_t, 4> grid;   // x gridW, y gridH, z directionCount, w gridD (cells per axis)
    std::array<float, 4> extentStride;   // xyz gridOrigin (world), w cellSize
    std::array<std::uint32_t, 4> anchor; // unused (kept for byte-layout parity with screen-space rc_march.comp)
};

struct WorldComposePush
{
    std::array<float, 4> coarsestFlag;         // w 1 when this is the coarsest cascade (sky base)
    std::array<std::uint32_t, 4> fineGrid;     // x gridW, y gridH, z directionCount, w gridD (cascade being written)
    std::array<std::uint32_t, 4> coarseGrid;   // x gridW, y gridH, z directionCount, w gridD (cascade c+1)
    std::array<float, 4> fineOriginCellSize;   // xyz fine grid origin (world), w fine cell size
    std::array<float, 4> coarseOriginCellSize; // xyz coarse grid origin (world), w coarse cell size
};

struct WorldGatherPush
{
    std::array<std::uint32_t, 4> counts;  // x primitiveCount, y losSteps, z visibilityEnabled, w cascadeCount
    std::array<float, 4> schedule;        // x intervalBase, y intervalFactor, zw unused
    std::array<std::uint32_t, 4> dirs;    // x directionCount, yzw unused
    std::array<float, 4> originCellSize0; // xyz grid origin cascade 0 (world), w cell size cascade 0
    std::array<std::uint32_t, 4> dims0;   // x gridW cascade 0, y gridH cascade 0, z gridD cascade 0, w unused
    std::array<float, 4> originCellSize1; // xyz grid origin cascade 1 (world), w cell size cascade 1
    std::array<std::uint32_t, 4> dims1;   // x gridW cascade 1, y gridH cascade 1, z gridD cascade 1, w unused
    std::array<float, 4> pureTransport;   // x pureTransport (1 = no direct fallback: intermediate bounces), yzw unused
};

static_assert(sizeof(MarchPush) == 80, "rc_march.comp expects an 80-byte push block");
static_assert(sizeof(ComposePush) == 48, "rc_compose.comp expects a 48-byte push block");
static_assert(sizeof(GatherPush) == 48, "rc_gather.comp expects a 48-byte push block");
static_assert(sizeof(FilterPush) == 32, "rc_denoise.comp expects a 32-byte push block");
static_assert(sizeof(WorldMarchPush) == 80, "rc_world_march.comp expects an 80-byte push block");
static_assert(sizeof(WorldComposePush) == 80, "rc_world_compose.comp expects an 80-byte push block");
static_assert(sizeof(WorldGatherPush) == 128, "rc_world_gather.comp expects a 128-byte push block");

// The spatial filter's fixed edge-awareness weights (rc_denoise.comp reads
// params.y/z): the normal power and the depth-band percentage of the
// camera depth range.
constexpr std::int32_t filterNormalPower      = 8;
constexpr std::int32_t filterDepthBandPercent = 4;

// ---- direction tables ----------------------------------------------------------------
//
// One vec4 per direction: xyz the unit direction, w the texel's exact solid
// angle (the gather's quadrature weight). Built once per rebuild on the CPU
// and uploaded to the storage buffer bound at shared set 0 / binding 7
// (cascade_common.glsl); layer order matches the march image layers.

// Solid angle of the spherical triangle spanned by three unit vectors
// (Van Oosterom & Strackee): tan(Omega/2) = |a . (b x c)| / (1 + a.b + b.c + c.a).
float sphericalTriangleSolidAngle(
    const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c
)
{
    const float numerator   = std::abs(glm::dot(a, glm::cross(b, c)));
    const float denominator = 1.0f + glm::dot(a, b) + glm::dot(b, c) + glm::dot(c, a);
    return 2.0f * std::atan2(numerator, denominator);
}

// Octahedral decode: the square [-1,1]^2 folds onto the octahedron (and
// normalized, onto the sphere). Texel centers land strictly inside the
// square; corners may sit on the fold, where the decode is continuous.
glm::vec3 octahedronDecode(
    float u, float v
)
{
    float x = u;
    float y = v;
    float z = 1.0f - std::abs(u) - std::abs(v);
    if (z < 0.0f)
    {
        // The lower pyramid: fold the corner triangles back up.
        const float signX = u >= 0.0f ? 1.0f : -1.0f;
        const float signY = v >= 0.0f ? 1.0f : -1.0f;
        x                 = signX * (1.0f - std::abs(v));
        y                 = signY * (1.0f - std::abs(u));
    }
    return glm::normalize(glm::vec3{x, y, z});
}

// Octahedron mapping: texelsPerAxis^2 directions, one per texel center of
// the octahedral square. Each texel's solid angle is the exact area of its
// spherical quadrilateral (two spherical triangles over the decoded
// corners) - the octahedral map is only approximately equal-area, so the
// quadrature weights must not be assumed uniform.
std::vector<std::array<float, 4>> buildOctahedronDirections(
    std::uint32_t texelsPerAxis
)
{
    const float inverseAxis = 1.0f / static_cast<float>(texelsPerAxis);
    std::vector<std::array<float, 4>> directions;
    directions.reserve(static_cast<std::size_t>(texelsPerAxis) * texelsPerAxis);
    for (std::uint32_t texelY = 0; texelY < texelsPerAxis; ++texelY)
    {
        for (std::uint32_t texelX = 0; texelX < texelsPerAxis; ++texelX)
        {
            // Texel center direction.
            const float u          = (static_cast<float>(texelX) + 0.5f) * 2.0f * inverseAxis - 1.0f;
            const float v          = (static_cast<float>(texelY) + 0.5f) * 2.0f * inverseAxis - 1.0f;
            const glm::vec3 center = octahedronDecode(u, v);
            // Corner grid over the square, mapped exactly like the decode.
            const glm::vec3 corner00 = octahedronDecode(
                static_cast<float>(texelX) * 2.0f * inverseAxis - 1.0f,
                static_cast<float>(texelY) * 2.0f * inverseAxis - 1.0f
            );
            const glm::vec3 corner10 = octahedronDecode(
                static_cast<float>(texelX + 1) * 2.0f * inverseAxis - 1.0f,
                static_cast<float>(texelY) * 2.0f * inverseAxis - 1.0f
            );
            const glm::vec3 corner11 = octahedronDecode(
                static_cast<float>(texelX + 1) * 2.0f * inverseAxis - 1.0f,
                static_cast<float>(texelY + 1) * 2.0f * inverseAxis - 1.0f
            );
            const glm::vec3 corner01 = octahedronDecode(
                static_cast<float>(texelX) * 2.0f * inverseAxis - 1.0f,
                static_cast<float>(texelY + 1) * 2.0f * inverseAxis - 1.0f
            );
            const float solidAngle = sphericalTriangleSolidAngle(corner00, corner10, corner11)
                                   + sphericalTriangleSolidAngle(corner00, corner11, corner01);
            directions.push_back({center.x, center.y, center.z, solidAngle});
        }
    }
    return directions;
}

// Cubemap mapping: six faces of faceSize^2 texels. A face texel's solid
// angle is the exact solid angle of its square on the face plane z = 1
// (the closed form of the 1/(1+x^2+y^2)^(3/2) integral over the rectangle).
std::vector<std::array<float, 4>> buildCubemapDirections(
    std::uint32_t faceSize
)
{
    const float inverseFace = 1.0f / static_cast<float>(faceSize);
    // Face axes: (forward, uBasis, vBasis) per cube face.
    const std::array<std::array<float, 3>, 6> faceForward = {{
        {1.0f, 0.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, -1.0f},
    }};
    const std::array<std::array<float, 3>, 6> faceUBasis  = {{
        {0.0f, 0.0f, -1.0f},
        {0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f},
    }};
    const std::array<std::array<float, 3>, 6> faceVBasis  = {{
        {0.0f, -1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, -1.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
    }};
    std::vector<std::array<float, 4>> directions;
    directions.reserve(static_cast<std::size_t>(6) * faceSize * faceSize);
    for (std::uint32_t face = 0; face < 6; ++face)
    {
        for (std::uint32_t texelY = 0; texelY < faceSize; ++texelY)
        {
            for (std::uint32_t texelX = 0; texelX < faceSize; ++texelX)
            {
                const float u             = (static_cast<float>(texelX) + 0.5f) * 2.0f * inverseFace - 1.0f;
                const float v             = (static_cast<float>(texelY) + 0.5f) * 2.0f * inverseFace - 1.0f;
                const glm::vec3 forward   = glm::make_vec3(faceForward[face].data());
                const glm::vec3 uBasis    = glm::make_vec3(faceUBasis[face].data());
                const glm::vec3 vBasis    = glm::make_vec3(faceVBasis[face].data());
                const glm::vec3 direction = glm::normalize(forward + uBasis * u + vBasis * v);
                // Exact texel solid angle: atan2 terms over the rectangle's
                // corners on the unit face plane.
                const float u0 = static_cast<float>(texelX) * 2.0f * inverseFace - 1.0f;
                const float u1 = static_cast<float>(texelX + 1) * 2.0f * inverseFace - 1.0f;
                const float v0 = static_cast<float>(texelY) * 2.0f * inverseFace - 1.0f;
                const float v1 = static_cast<float>(texelY + 1) * 2.0f * inverseFace - 1.0f;
                const auto cornerTerm
                    = [](float a, float b) { return std::atan2(a * b, std::sqrt(1.0f + a * a + b * b)); };
                const float solidAngle
                    = cornerTerm(u1, v1) - cornerTerm(u0, v1) - cornerTerm(u1, v0) + cornerTerm(u0, v0);
                directions.push_back({direction.x, direction.y, direction.z, solidAngle});
            }
        }
    }
    return directions;
}

// Direction count and grid resolution of one mapping at the configured ray
// budget: every mode spends approximately base_rays directions. Returns the
// ready-to-upload direction table.
std::vector<std::array<float, 4>> buildDirectionTable(
    CascadeAngularMapping mapping, std::uint32_t baseRays
)
{
    switch (mapping)
    {
    case CascadeAngularMapping::Cubemap: {
        const auto faceSize = std::clamp<std::uint32_t>(
            static_cast<std::uint32_t>(std::lround(std::sqrt(static_cast<double>(baseRays) / 6.0))),
            minCubemapFaceTexels,
            maxCubemapFaceTexels
        );
        return buildCubemapDirections(faceSize);
    }
    case CascadeAngularMapping::Octahedron:
    case CascadeAngularMapping::SphericalHarmonic:
    default: {
        const auto texelsPerAxis = std::clamp<std::uint32_t>(
            static_cast<std::uint32_t>(std::lround(std::sqrt(static_cast<double>(baseRays)))),
            minOctahedronTexelsPerAxis,
            maxOctahedronTexelsPerAxis
        );
        return buildOctahedronDirections(texelsPerAxis);
    }
    }
}

struct CascadeImage
{
    std::unique_ptr<vk::raii::Image> image;
    std::unique_ptr<vk::raii::DeviceMemory> memory;
    std::unique_ptr<vk::raii::ImageView> view;
};

} // namespace

// The screen-space radiance cascades pass: march (per cascade interval), compose
// (front-to-back interval chaining, coarsest first), gather (leak-proof
// visibility weighted probe blend). See the .comp files for the technique.
class ScreenSpaceRadianceCascadesPass final : public LightingTechniquePass
{
public:
    struct Settings
    {
        std::uint32_t cascades = defaultCascades;
        float intervalBase     = defaultIntervalBase;
        float intervalFactor   = defaultIntervalFactor;
        // Cost note: the march dispatches (extent/stride)^2 * rays * steps
        // scene-SDF evaluations per frame. Stride 2 at 720p meant 29 million
        // march invocations - multi-second frames and a starved message pump
        // ("not responding"); the configured stride (demo: 8+) trades march
        // cost against smoothness, which the LOS-guarded gather and the
        // spatial filter carry.
        std::uint32_t probeStride = defaultProbeStride;
        std::uint32_t baseRays    = defaultBaseRays;
        bool gatherVisibility     = true;
        std::uint32_t losSteps    = defaultLosSteps;
        std::uint32_t marchSteps  = defaultMarchSteps;
        bool relightShadows       = true;
        std::uint32_t shadowSteps = defaultShadowSteps;
        // Edge-aware spatial filter radius on the gather output (pixels, 0 =
        // pass-through copy). Removes the probe grid's spatial scale; the
        // depth/normal weights keep silhouettes hard.
        std::uint32_t spatialFilterRadius = defaultSpatialFilterRadius;
        // Angular mapping of the field's directional domain (see the enum's
        // comment): octahedron texel grid, cubemap face grid, or SH projection
        // evaluated in the gather. The direction budget stays ~base_rays in
        // every mode.
        CascadeAngularMapping mapping = CascadeAngularMapping::Octahedron;
        // Light transport bounces per frame (1 = the historical single
        // bounce). Every bounce re-runs march + compose + gather; the march
        // of bounce b > 0 adds the previous bounce's gathered transport at
        // its hit points.
        std::uint32_t bounces = defaultBounces;
    };

    ScreenSpaceRadianceCascadesPass(
        Settings settings, LightingCapability *lightingCapability
    )
        : settings(settings), lighting(lightingCapability)
    {}

    const char *techniqueName() const override
    {
        return "radiance_cascades";
    }

    LightingTarget targets() const override
    {
        return LightingTarget::IndirectDiffuse;
    }

    void record(
        vk::raii::CommandBuffer &commandBuffer, const FrameRenderContext &context
    ) override
    {
        if (cascadeCount() == 0 || context.extent != cachedExtent || needsRecreate)
        {
            rebuild(context);
        }

        updateLightUniform();

        const std::uint32_t cascades = cascadeCount();
        // Angular schedule: CONSTANT across cascades. Every cascade carries
        // the same direction count; the spatial grid refines per cascade
        // (probe stride doubles) while the interval partition grows. The
        // former per-cascade halving with a 2x3 floor handed the entire far
        // field six directions, which is where the composed field's angular
        // structure (giant soft blobs) came from.
        const std::uint32_t directions = directionCount;
        // The cascade geometry table (grid dims, intervals, dispatch dims)
        // was filled by rebuild(); record() walks it without recomputing the
        // stride-shifts, ceil-divs and pow() calls per cascade per frame.
        // The march/compose z-dim is constant per rebuild (depends only on
        // directionCount, set in rebuild()).
        for (std::uint32_t bounce = 0; bounce < settings.bounces; ++bounce)
        {
            const bool lastBounce = bounce + 1 == settings.bounces;

            // ---- March: every cascade traces its own interval. ---------
            commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, **marchPipeline);
            commandBuffer.bindDescriptorSets(
                vk::PipelineBindPoint::eCompute, **marchPipelineLayout, 0, **sharedSet, {}
            );
            for (std::uint32_t cascade = 0; cascade < cascades; ++cascade)
            {
                commandBuffer.bindDescriptorSets(
                    vk::PipelineBindPoint::eCompute, **marchPipelineLayout, 1, **marchSets[cascade], {}
                );
                const CascadeGeometry &g = cascadeGeometry[cascade];
                const MarchPush pushValue{
                    .interval     = {g.intervalStart, g.intervalEnd, 0.0f, 0.0f},
                    .flags        = {context.primitiveCount, settings.marchSteps, cascade > 0 ? 1u : 0u, bounce},
                    .grid         = {g.gridWidth, g.gridHeight, directions, 0u},
                    .extentStride = {context.extent.width, context.extent.height, g.stride, g.stride},
                    .anchor       = {cascade * 2u, 0u, 0u, 0u}
                };
                commandBuffer.pushConstants(
                    **marchPipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pushValue), &pushValue
                );
                // The march workgroup packs 8 direction lanes (4x4x8, matching
                // the shader's local_size): the anchor lane computes one probe
                // anchor for all of them, and the 16-thread workgroups the
                // bare 4x4 shape produced schedule as half warps.
                commandBuffer.dispatch(g.dispatchX, g.dispatchY, marchDispatchZ);
            }

            // march[c] writes -> compose reads. The anchor field needs its own
            // dependency: the gather loads it through eShaderStorageRead, an
            // access the march-image barrier below does not cover.
            marchBarriers(cascadeImageScratch);
            barrierCascades(
                commandBuffer,
                cascadeImageScratch,
                vk::PipelineStageFlagBits2::eComputeShader,
                vk::PipelineStageFlagBits2::eComputeShader
            );
            barrierAnchorField(
                commandBuffer,
                vk::AccessFlagBits2::eShaderStorageWrite,
                vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderStorageRead
            );

            // ---- Compose: front-to-back interval chaining. -------------
            commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, **composePipeline);
            commandBuffer.bindDescriptorSets(
                vk::PipelineBindPoint::eCompute, **composePipelineLayout, 0, **sharedSet, {}
            );
            for (std::uint32_t cascade = cascades; cascade-- > 0;)
            {
                commandBuffer.bindDescriptorSets(
                    vk::PipelineBindPoint::eCompute, **composePipelineLayout, 1, **composeSets[cascade], {}
                );
                const CascadeGeometry &g = cascadeGeometry[cascade];
                const ComposePush pushValue{
                    .coarsestFlag = {0.0f, 0.0f, 0.0f, cascade == cascades - 1 ? 1.0f : 0.0f},
                    .fineGrid     = {g.gridWidth, g.gridHeight, directions, 0u},
                    .coarseGrid
                    = {cascade + 1 < cascades ? cascadeGeometry[cascade + 1].gridWidth : 1u,
                        cascade + 1 < cascades ? cascadeGeometry[cascade + 1].gridHeight : 1u,
                        directions,
                        0u}
                };
                commandBuffer.pushConstants(
                    **composePipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pushValue), &pushValue
                );
                // The compose workgroup packs 8 direction lanes per dispatch
                // workgroup (4x4x8, matching the shader's local_size).
                commandBuffer.dispatch(g.dispatchX, g.dispatchY, marchDispatchZ);

                // full[c] write -> full[c-1] read in the next iteration.
                std::array<vk::ImageMemoryBarrier2, 1> barrier;
                barrier[0].setSrcStageMask(vk::PipelineStageFlagBits2::eComputeShader);
                barrier[0].setSrcAccessMask(vk::AccessFlagBits2::eShaderStorageWrite);
                barrier[0].setDstStageMask(vk::PipelineStageFlagBits2::eComputeShader);
                barrier[0].setDstAccessMask(vk::AccessFlagBits2::eShaderRead);
                barrier[0].setOldLayout(vk::ImageLayout::eGeneral);
                barrier[0].setNewLayout(vk::ImageLayout::eGeneral);
                barrier[0].setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
                barrier[0].setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
                barrier[0].setImage(**fullImages[cascade].image);
                barrier[0].setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
                vk::DependencyInfo dependency;
                dependency.setImageMemoryBarriers(barrier);
                commandBuffer.pipelineBarrier2(dependency);
            }

            // ---- Gather: per pixel diffuse irradiance from cascade 0. ---
            // The gather writes the module temp image; the NEXT bounce's
            // march reads it as its transport source (bounce > 0), and the
            // spatial filter of the last bounce turns the final one into the
            // renderer-owned indirect target.
            commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, **gatherPipeline);
            commandBuffer.bindDescriptorSets(
                vk::PipelineBindPoint::eCompute, **gatherPipelineLayout, 0, **sharedSet, {}
            );
            commandBuffer.bindDescriptorSets(
                vk::PipelineBindPoint::eCompute, **gatherPipelineLayout, 1, **gatherSet, {}
            );
            const GatherPush gatherPush{
                .counts = {context.primitiveCount, settings.losSteps, settings.gatherVisibility ? 1u : 0u, cascades},
                .schedule
                = {settings.intervalBase,
                    settings.intervalFactor,
                    static_cast<float>(settings.probeStride),
                    lastBounce ? 0.0f : 1.0f},
                .dirs = {directions, static_cast<std::uint32_t>(settings.mapping), 0u, 0u}
            };
            commandBuffer.pushConstants(
                **gatherPipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(gatherPush), &gatherPush
            );
            commandBuffer.dispatch(gatherDispatchX, gatherDispatchY, 1);

            // gather write -> next reader (the next bounce's march, or the
            // spatial filter below on the last bounce).
            barrierGatherTemp(
                commandBuffer, vk::AccessFlagBits2::eShaderStorageWrite, vk::AccessFlagBits2::eShaderRead
            );
            // Multi-bounce ordering on the anchor field: the gather's
            // imageLoads must complete before the next bounce's anchor lanes
            // overwrite the field. Cross-FRAME reuse needs no barrier here -
            // the host-side frame serialization (both in-flight fences waited
            // before recording) already orders the next frame's stores after
            // this submission's loads.
            if (!lastBounce)
            {
                barrierAnchorField(
                    commandBuffer, vk::AccessFlagBits2::eShaderStorageRead, vk::AccessFlagBits2::eShaderStorageWrite
                );
            }
        }

        // ---- Spatial filter: edge-aware cleanup of the gather output. -----
        // (Runs once, on the last bounce's gather output - intermediate
        // bounces feed the transport chain, not the composite.)
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, **denoisePipeline);
        commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, **denoisePipelineLayout, 0, **sharedSet, {});
        commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, **denoisePipelineLayout, 1, **denoiseSet, {});
        const FilterPush filterPush{
            .params
            = {static_cast<std::int32_t>(settings.spatialFilterRadius), filterNormalPower, filterDepthBandPercent, 0},
            .cameraPlanes = {context.cameraNearPlane, context.cameraFarPlane, 0.0f, 0.0f}
        };
        commandBuffer.pushConstants(
            **denoisePipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(filterPush), &filterPush
        );
        commandBuffer.dispatch(gatherDispatchX, gatherDispatchY, 1);
    }

    void notifyFrameResourcesRecreated() override
    {
        needsRecreate = true;
    }

private:
    std::uint32_t cascadeCount() const
    {
        return settings.cascades;
    }

    void barrierCascades(
        vk::raii::CommandBuffer &commandBuffer,
        const std::vector<vk::Image> &images,
        vk::PipelineStageFlagBits2 sourceStage,
        vk::PipelineStageFlagBits2 destinationStage
    )
    {
        // Scratch members (below): record() runs every frame and fresh
        // vectors per barrier group were pure heap churn on the record path.
        std::vector<vk::ImageMemoryBarrier2> &barriers = cascadeBarrierScratch;
        barriers.clear();
        for (const vk::Image &image : images)
        {
            vk::ImageMemoryBarrier2 barrier;
            barrier.setSrcStageMask(sourceStage);
            barrier.setSrcAccessMask(vk::AccessFlagBits2::eShaderStorageWrite);
            barrier.setDstStageMask(destinationStage);
            barrier.setDstAccessMask(vk::AccessFlagBits2::eShaderRead);
            barrier.setOldLayout(vk::ImageLayout::eGeneral);
            barrier.setNewLayout(vk::ImageLayout::eGeneral);
            barrier.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            barrier.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            barrier.setImage(image);
            barrier.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
            barriers.push_back(barrier);
        }
        vk::DependencyInfo dependency;
        dependency.setImageMemoryBarriers(barriers);
        commandBuffer.pipelineBarrier2(dependency);
    }

    void marchBarriers(
        std::vector<vk::Image> &images
    )
    {
        images.clear();
        for (const CascadeImage &cascade : marchImages)
        {
            images.push_back(**cascade.image);
        }
    }

    // One image memory barrier on the gather temp (the bounce ping-pong
    // image): every gather writes it, the next bounce's march samples it as
    // its transport source and the last bounce's spatial filter reads it.
    void barrierGatherTemp(
        vk::raii::CommandBuffer &commandBuffer, vk::AccessFlagBits2 sourceAccess, vk::AccessFlagBits2 destinationAccess
    )
    {
        vk::ImageMemoryBarrier2 barrier;
        barrier.setSrcStageMask(vk::PipelineStageFlagBits2::eComputeShader);
        barrier.setSrcAccessMask(sourceAccess);
        barrier.setDstStageMask(vk::PipelineStageFlagBits2::eComputeShader);
        barrier.setDstAccessMask(destinationAccess);
        barrier.setOldLayout(vk::ImageLayout::eGeneral);
        barrier.setNewLayout(vk::ImageLayout::eGeneral);
        barrier.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
        barrier.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
        barrier.setImage(**gatherTemp.image);
        barrier.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
        std::array<vk::ImageMemoryBarrier2, 1> barriers{barrier};
        vk::DependencyInfo dependency;
        dependency.setImageMemoryBarriers(barriers);
        commandBuffer.pipelineBarrier2(dependency);
    }

    // The same one-barrier shape for the probe anchor field: the march's
    // anchor lanes write it, the gather's probeAnchor reads it back
    // (write -> read after the march, and read -> write before the next
    // bounce's march rewrites it - the loads of one bounce must complete
    // before the stores of the next). The access masks arrive as full flag
    // sets because the read side needs READ and STORAGE_READ together.
    void barrierAnchorField(
        vk::raii::CommandBuffer &commandBuffer, vk::AccessFlags2 sourceAccess, vk::AccessFlags2 destinationAccess
    )
    {
        vk::ImageMemoryBarrier2 barrier;
        barrier.setSrcStageMask(vk::PipelineStageFlagBits2::eComputeShader);
        barrier.setSrcAccessMask(sourceAccess);
        barrier.setDstStageMask(vk::PipelineStageFlagBits2::eComputeShader);
        barrier.setDstAccessMask(destinationAccess);
        barrier.setOldLayout(vk::ImageLayout::eGeneral);
        barrier.setNewLayout(vk::ImageLayout::eGeneral);
        barrier.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
        barrier.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
        barrier.setImage(**anchorField.image);
        barrier.setSubresourceRange(
            vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, cascadeCount() * 2)
        );
        std::array<vk::ImageMemoryBarrier2, 1> barriers{barrier};
        vk::DependencyInfo dependency;
        dependency.setImageMemoryBarriers(barriers);
        commandBuffer.pipelineBarrier2(dependency);
    }

    // Allocates the image, memory and view (no layout transition - the batched
    // submission at the end of rebuild() performs every Undefined -> General
    // transition in one shot). The previous per-image setup pool + setup
    // buffer + submit + waitIdle pair was rebuild-only, but it ran once per
    // cascade image (13 separate submissions at cascades=5); batching them
    // collapses 13 round-trips into one.
    void createCascadeImageNoTransition(
        CascadeImage &target,
        const FrameRenderContext &context,
        vk::Extent2D grid,
        std::uint32_t layers,
        vk::ImageViewType viewType = vk::ImageViewType::e2DArray,
        vk::Format format          = vk::Format::eR16G16B16A16Sfloat
    )
    {
        vk::ImageCreateInfo imageInfo;
        imageInfo.setImageType(vk::ImageType::e2D);
        imageInfo.setFormat(format);
        imageInfo.setExtent(vk::Extent3D(grid.width, grid.height, 1));
        imageInfo.setMipLevels(1);
        imageInfo.setArrayLayers(layers);
        imageInfo.setSamples(vk::SampleCountFlagBits::e1);
        imageInfo.setTiling(vk::ImageTiling::eOptimal);
        imageInfo.setUsage(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage);
        imageInfo.setSharingMode(vk::SharingMode::eExclusive);
        imageInfo.setInitialLayout(vk::ImageLayout::eUndefined);
        target.image                              = std::make_unique<vk::raii::Image>(context.device, imageInfo);
        const vk::MemoryRequirements requirements = target.image->getMemoryRequirements();
        vk::MemoryAllocateInfo allocate;
        allocate.setAllocationSize(requirements.size);
        allocate.setMemoryTypeIndex(findMemoryType(
            context.physicalDevice, requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal
        ));
        target.memory = std::make_unique<vk::raii::DeviceMemory>(context.device, allocate);
        target.image->bindMemory(*target.memory, 0);
        vk::ImageViewCreateInfo viewInfo;
        viewInfo.setImage(**target.image);
        viewInfo.setViewType(viewType);
        viewInfo.setFormat(format);
        viewInfo.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers));
        target.view = std::make_unique<vk::raii::ImageView>(context.device, viewInfo);
    }

    // Records one Undefined -> General layout transition into the supplied
    // setup command buffer. Caller drives the begin/end/submit/waitIdle once
    // for the whole batch.
    void recordInitialTransition(
        vk::raii::CommandBuffer &commandBuffer, CascadeImage &target, std::uint32_t layers
    )
    {
        vk::ImageMemoryBarrier2 barrier;
        barrier.setSrcStageMask(vk::PipelineStageFlagBits2::eAllCommands);
        barrier.setSrcAccessMask(vk::AccessFlags2());
        barrier.setDstStageMask(vk::PipelineStageFlagBits2::eAllCommands);
        barrier.setDstAccessMask(vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
        barrier.setOldLayout(vk::ImageLayout::eUndefined);
        barrier.setNewLayout(vk::ImageLayout::eGeneral);
        barrier.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
        barrier.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
        barrier.setImage(**target.image);
        barrier.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers));
        std::array<vk::ImageMemoryBarrier2, 1> barriers{barrier};
        vk::DependencyInfo dependency;
        dependency.setImageMemoryBarriers(barriers);
        commandBuffer.pipelineBarrier2(dependency);
    }

    void updateLightUniform()
    {
        GpuLightUniform uniform;
        const bool hasLighting = lighting != nullptr;
        DirectionalLightDescription sun;
        const bool sunEnabled = hasLighting && lighting->hasSunLight();
        if (sunEnabled)
        {
            sun = lighting->sunLight();
        }
        const std::uint32_t count = hasLighting ? std::min(lighting->pointLightCount(), maxPointLights) : 0;
        std::array<PointLightDescription, maxPointLights> pointLights{};
        for (std::uint32_t index = 0; index < count; ++index)
        {
            pointLights[index] = lighting->pointLight(index);
        }
        fillSunAndPointLights(uniform, sunEnabled, sun, pointLights.data(), count);
        uniform.lightCountsAndFlags
            = {static_cast<std::int32_t>(count),
                settings.relightShadows ? 1 : 0,
                static_cast<std::int32_t>(settings.shadowSteps),
                0};
        // Change-gated write: the mapped buffer keeps its last bytes, so an
        // unchanged light set (the common frame) skips the coherent write.
        // Invalidated on rebuild, where the buffer is created fresh.
        if (!uploadedLightUniformValid || !(uniform == uploadedLightUniform))
        {
            std::memcpy(lightStorage.mapped, &uniform, sizeof(uniform));
            uploadedLightUniform      = uniform;
            uploadedLightUniformValid = true;
        }
    }

    void rebuild(
        const FrameRenderContext &context
    )
    {
        // Teardown in strict reverse dependency order: descriptor sets, pools,
        // pipelines, pipeline layouts, descriptor set layouts, then images,
        // buffers and samplers. Pipelines embed their layouts and layouts
        // embed the descriptor set layouts, so a layout must never die while
        // a pipeline that references it is alive.
        gatherSet.reset();
        gatherPool.reset();
        denoiseSet.reset();
        denoisePool.reset();
        for (auto &set : composeSets)
        {
            set.reset();
        }
        composePool.reset();
        for (auto &set : marchSets)
        {
            set.reset();
        }
        marchPool.reset();
        sharedSet.reset();
        sharedPool.reset();
        gatherPipeline.reset();
        gatherPipelineLayout.reset();
        denoisePipeline.reset();
        denoisePipelineLayout.reset();
        composePipeline.reset();
        composePipelineLayout.reset();
        marchPipeline.reset();
        marchPipelineLayout.reset();
        sharedSetLayout.reset();
        marchSetLayout.reset();
        composeSetLayout.reset();
        gatherSetLayout.reset();
        filterSetLayout.reset();
        for (auto &image : fullImages)
        {
            image = CascadeImage{};
        }
        for (auto &image : marchImages)
        {
            image = CascadeImage{};
        }
        gatherTemp  = CascadeImage{};
        skyImage    = CascadeImage{};
        anchorField = CascadeImage{};
        lightStorage.reset();
        directionStorage.reset();
        composeSampler.reset();
        nearestSampler.reset();

        const std::uint32_t cascades = cascadeCount();
        // Direction table per angular mapping (Settings.mapping): every mode
        // spends approximately base_rays directions - the octahedron texel
        // grid, the cubemap face grid, or the octahedron grid again for the
        // SH projection (the march always runs on discrete directions; the
        // mapping only changes the gather's irradiance evaluation). One vec4
        // per direction - xyz the unit direction, w the exact solid angle -
        // uploaded to the host-visible storage buffer bound at shared set 0 /
        // binding 7 (cascade_common.glsl). Layer order matches the march's
        // image layers.
        const std::vector<std::array<float, 4>> directionTable
            = buildDirectionTable(settings.mapping, settings.baseRays);
        directionCount = static_cast<std::uint32_t>(directionTable.size());
        {
            directionStorage = createMappedHostVisibleBuffer(
                context.device,
                context.physicalDevice,
                static_cast<vk::DeviceSize>(directionTable.size() * sizeof(std::array<float, 4>)),
                vk::BufferUsageFlagBits::eStorageBuffer
            );
            std::memcpy(
                directionStorage.mapped, directionTable.data(), directionTable.size() * sizeof(std::array<float, 4>)
            );
        }

        // ---- Per-cascade geometry cache -----------------------------------
        // Stride, grid dims, view-depth interval and 4x4-tile dispatch dims
        // are pure functions of (settings, extent, cascade index). The
        // record() path walked these through four lambdas per frame, each
        // calling std::pow and a ceil-div per cascade; with cascades=5 that
        // was 20 pow() calls and 10 ceil-divs per frame just to set up the
        // dispatch arguments. Cache them once per rebuild instead - the
        // record() hot path now reads straight out of this vector.
        cascadeGeometry.assign(cascades, CascadeGeometry{});
        for (std::uint32_t cascade = 0; cascade < cascades; ++cascade)
        {
            CascadeGeometry &g         = cascadeGeometry[cascade];
            const std::uint32_t stride = settings.probeStride << cascade;
            g.stride                   = stride;
            g.gridWidth                = std::max(1u, (context.extent.width + stride - 1) / stride);
            g.gridHeight               = std::max(1u, (context.extent.height + stride - 1) / stride);
            g.intervalStart = cascade == 0 ? 0.0f
                                           : settings.intervalBase
                                                 * std::pow(settings.intervalFactor, static_cast<float>(cascade - 1));
            g.intervalEnd   = settings.intervalBase * std::pow(settings.intervalFactor, static_cast<float>(cascade));
            // 4x4 tile workgroup for march/compose (local_size = 4x4x8).
            g.dispatchX = (g.gridWidth + 3) / 4;
            g.dispatchY = (g.gridHeight + 3) / 4;
        }
        // 8x8 tile workgroup for gather/denoise (local_size = 8x8x1).
        gatherDispatchX = (context.extent.width + 7) / 8;
        gatherDispatchY = (context.extent.height + 7) / 8;
        // March/compose z-dim is constant per rebuild: a function of
        // directionCount alone (the shader's local_size_z is 8). Cache it
        // instead of recomputing per cascade per frame.
        marchDispatchZ = (directionCount + marchDirectionLanes - 1) / marchDirectionLanes;

        // Cascade storage: march (radiance + transmittance) and full (composed
        // radiance) per cascade. Sized to the cascade grid, so memory decays
        // by 4x per cascade (memory rule).
        marchImages.resize(cascades);
        fullImages.resize(cascades);
        // First create every image with no layout transition; the batched
        // submission below performs all Undefined -> General transitions in
        // one shot. Previously each image was followed by its own command
        // pool, command buffer, submit and waitIdle - N round-trips to the
        // device for what is one logical step.
        for (std::uint32_t cascade = 0; cascade < cascades; ++cascade)
        {
            const vk::Extent2D grid{cascadeGeometry[cascade].gridWidth, cascadeGeometry[cascade].gridHeight};
            createCascadeImageNoTransition(marchImages[cascade], context, grid, directionCount);
            createCascadeImageNoTransition(fullImages[cascade], context, grid, directionCount);
        }
        createCascadeImageNoTransition(skyImage, context, vk::Extent2D{1, 1}, 1);
        // The probe anchor field: sized to the FINEST cascade grid (every
        // coarser grid fits inside it), two layers per cascade. rgba32f: the
        // anchor positions feed the gather's SDF line-of-sight traces, whose
        // centimetre-scale offsets need the fp32 precision a 16-bit target
        // would quantize away.
        createCascadeImageNoTransition(
            anchorField,
            context,
            vk::Extent2D{cascadeGeometry[0].gridWidth, cascadeGeometry[0].gridHeight},
            cascades * 2,
            vk::ImageViewType::e2DArray,
            vk::Format::eR32G32B32A32Sfloat
        );
        // Gather output temp: full screen extent, single layer, same format
        // as the indirect target. The gather and filter shaders declare plain
        // image2D/sampler2D, so the view is 2D (not the cascade 2D array).
        createCascadeImageNoTransition(
            gatherTemp, context, vk::Extent2D{context.extent.width, context.extent.height}, 1, vk::ImageViewType::e2D
        );

        // One batched setup submission for every cascade image, the anchor
        // field and the gather temp: a single command pool, a single command
        // buffer, a single submit + waitIdle. UNDEFINED -> GENERAL through a
        // one-shot submission on the renderer's graphics queue: storage image
        // writes and sampled reads both require a defined layout before the
        // first dispatch that touches the image.
        {
            vk::CommandPoolCreateInfo poolInfo;
            poolInfo.setFlags(vk::CommandPoolCreateFlagBits::eTransient);
            poolInfo.setQueueFamilyIndex(context.graphicsQueueFamily);
            vk::raii::CommandPool setupPool(context.device, poolInfo);
            vk::CommandBufferAllocateInfo allocateInfo;
            allocateInfo.setCommandPool(*setupPool);
            allocateInfo.setLevel(vk::CommandBufferLevel::ePrimary);
            allocateInfo.setCommandBufferCount(1);
            vk::raii::CommandBuffer setupBuffer
                = std::move(vk::raii::CommandBuffers(context.device, allocateInfo).front());
            setupBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
            for (std::uint32_t cascade = 0; cascade < cascades; ++cascade)
            {
                recordInitialTransition(setupBuffer, marchImages[cascade], directionCount);
                recordInitialTransition(setupBuffer, fullImages[cascade], directionCount);
            }
            recordInitialTransition(setupBuffer, skyImage, 1);
            recordInitialTransition(setupBuffer, anchorField, cascades * 2);
            recordInitialTransition(setupBuffer, gatherTemp, 1);
            setupBuffer.end();
            vk::SubmitInfo submitInfo;
            submitInfo.setCommandBuffers(*setupBuffer);
            context.graphicsQueue.submit(submitInfo);
            context.graphicsQueue.waitIdle();
        }

        nearestSampler = makeNearestClampSampler(context.device);

        vk::SamplerCreateInfo linearInfo;
        linearInfo.setMagFilter(vk::Filter::eLinear);
        linearInfo.setMinFilter(vk::Filter::eLinear);
        linearInfo.setMipmapMode(vk::SamplerMipmapMode::eNearest);
        linearInfo.setAddressModeU(vk::SamplerAddressMode::eClampToEdge);
        linearInfo.setAddressModeV(vk::SamplerAddressMode::eClampToEdge);
        linearInfo.setAddressModeW(vk::SamplerAddressMode::eClampToEdge);
        composeSampler = std::make_unique<vk::raii::Sampler>(context.device, linearInfo);

        // Light uniform buffer (own copy: the GI relight configures shadow
        // behaviour from [lighting.radiance_cascades], independent of the
        // direct module).
        lightStorage = createMappedHostVisibleBuffer(
            context.device, context.physicalDevice, sizeof(GpuLightUniform), vk::BufferUsageFlagBits::eUniformBuffer
        );
        // The buffer is fresh: the next updateLightUniform must write.
        uploadedLightUniformValid = false;

        // Layouts -----------------------------------------------------------
        {
            // Shared set 0: camera UBO, primitives SSBO, light UBO (the two
            // relighting passes), material table SSBO, albedo/data texture
            // arrays (textured relight), environment UBO (the compose sky
            // base), cascade direction table SSBO (binding 7,
            // cascade_common.glsl).
            std::array<vk::DescriptorSetLayoutBinding, 8> bindings;
            bindings[0].setBinding(0).setDescriptorType(vk::DescriptorType::eUniformBuffer).setDescriptorCount(1);
            bindings[0].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[1].setBinding(1).setDescriptorType(vk::DescriptorType::eStorageBuffer).setDescriptorCount(1);
            bindings[1].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[2].setBinding(2).setDescriptorType(vk::DescriptorType::eUniformBuffer).setDescriptorCount(1);
            bindings[2].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[3].setBinding(3).setDescriptorType(vk::DescriptorType::eStorageBuffer).setDescriptorCount(1);
            bindings[3].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[4]
                .setBinding(4)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                .setDescriptorCount(1);
            bindings[4].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[5]
                .setBinding(5)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                .setDescriptorCount(1);
            bindings[5].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[6].setBinding(6).setDescriptorType(vk::DescriptorType::eUniformBuffer).setDescriptorCount(1);
            bindings[6].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[7].setBinding(7).setDescriptorType(vk::DescriptorType::eStorageBuffer).setDescriptorCount(1);
            bindings[7].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            vk::DescriptorSetLayoutCreateInfo setInfo;
            setInfo.setBindings(bindings);
            sharedSetLayout = std::make_unique<vk::raii::DescriptorSetLayout>(context.device, setInfo);
        }
        {
            // March role set: depth, normal (sampled), the cascade storage
            // image (written), and (bounce > 0) the previous bounce's gather
            // output as the transport source (sampled). Bounce 0 never reads
            // it (the push flag gates the sample), but the binding must stay
            // valid. Binding 4 is the probe anchor field the anchor lane
            // publishes (the gather's probeAnchor reads the same image).
            std::array<vk::DescriptorSetLayoutBinding, 5> bindings;
            for (std::uint32_t binding = 0; binding < 2; ++binding)
            {
                bindings[binding]
                    .setBinding(binding)
                    .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                    .setDescriptorCount(1);
                bindings[binding].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            }
            bindings[2].setBinding(2).setDescriptorType(vk::DescriptorType::eStorageImage).setDescriptorCount(1);
            bindings[2].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[3]
                .setBinding(3)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                .setDescriptorCount(1);
            bindings[3].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[4].setBinding(4).setDescriptorType(vk::DescriptorType::eStorageImage).setDescriptorCount(1);
            bindings[4].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            vk::DescriptorSetLayoutCreateInfo setInfo;
            setInfo.setBindings(bindings);
            marchSetLayout = std::make_unique<vk::raii::DescriptorSetLayout>(context.device, setInfo);
        }
        {
            std::array<vk::DescriptorSetLayoutBinding, 3> bindings;
            bindings[0]
                .setBinding(0)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                .setDescriptorCount(1);
            bindings[0].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[1].setBinding(1).setDescriptorType(vk::DescriptorType::eStorageImage).setDescriptorCount(1);
            bindings[1].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[2].setBinding(2).setDescriptorType(vk::DescriptorType::eStorageImage).setDescriptorCount(1);
            bindings[2].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            vk::DescriptorSetLayoutCreateInfo setInfo;
            setInfo.setBindings(bindings);
            composeSetLayout = std::make_unique<vk::raii::DescriptorSetLayout>(context.device, setInfo);
        }
        {
            // Spatial filter: gathered GI, depth, normal + the output storage
            // image.
            std::array<vk::DescriptorSetLayoutBinding, 4> bindings;
            for (std::uint32_t binding = 0; binding < 3; ++binding)
            {
                bindings[binding]
                    .setBinding(binding)
                    .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                    .setDescriptorCount(1);
                bindings[binding].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            }
            bindings[3].setBinding(3).setDescriptorType(vk::DescriptorType::eStorageImage).setDescriptorCount(1);
            bindings[3].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            vk::DescriptorSetLayoutCreateInfo setInfo;
            setInfo.setBindings(bindings);
            filterSetLayout = std::make_unique<vk::raii::DescriptorSetLayout>(context.device, setInfo);
        }
        {
            // Gather: one composed-cascade sampler per cascade slot (the
            // shader switches over the per-pixel cascade index), depth,
            // normal, the gather output storage image, and the march-written
            // probe anchor field (binding 11, imageLoad in probeAnchor).
            std::array<vk::DescriptorSetLayoutBinding, 12> bindings;
            for (std::uint32_t binding = 0; binding < 10; ++binding)
            {
                bindings[binding]
                    .setBinding(binding)
                    .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                    .setDescriptorCount(1);
                bindings[binding].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            }
            bindings[10].setBinding(10).setDescriptorType(vk::DescriptorType::eStorageImage).setDescriptorCount(1);
            bindings[10].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[11].setBinding(11).setDescriptorType(vk::DescriptorType::eStorageImage).setDescriptorCount(1);
            bindings[11].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            vk::DescriptorSetLayoutCreateInfo setInfo;
            setInfo.setBindings(bindings);
            gatherSetLayout = std::make_unique<vk::raii::DescriptorSetLayout>(context.device, setInfo);
        }

        // Pipelines ---------------------------------------------------------
        auto makeComputePipeline = [&context](
                                       const std::vector<char> &code,
                                       const vk::raii::DescriptorSetLayout &sharedLayout,
                                       const vk::raii::DescriptorSetLayout &roleLayout,
                                       std::uint32_t pushSize
                                   ) {
            vk::PushConstantRange pushRange;
            pushRange.setStageFlags(vk::ShaderStageFlagBits::eCompute);
            pushRange.setOffset(0);
            pushRange.setSize(pushSize);
            vk::PipelineLayoutCreateInfo layoutInfo;
            const std::array<vk::DescriptorSetLayout, 2> layouts{*sharedLayout, *roleLayout};
            layoutInfo.setSetLayouts(layouts);
            layoutInfo.setPushConstantRanges(pushRange);
            auto layout = std::make_unique<vk::raii::PipelineLayout>(context.device, layoutInfo);

            vk::ShaderModuleCreateInfo moduleInfo;
            moduleInfo.setCodeSize(code.size()).setPCode(reinterpret_cast<const std::uint32_t *>(code.data()));
            vk::raii::ShaderModule module(context.device, moduleInfo);
            vk::PipelineShaderStageCreateInfo stage;
            stage.setStage(vk::ShaderStageFlagBits::eCompute).setModule(*module).setPName("main");
            vk::ComputePipelineCreateInfo pipelineInfo;
            pipelineInfo.setStage(stage);
            pipelineInfo.setLayout(**layout);
            // The renderer's device-wide pipeline cache, threaded through
            // FrameRenderContext (NVIDIA's "Use pipeline cache" guidance).
            // On rebuilds triggered by swapchain recreate or shader hot
            // reload, the driver can hit the cache for unchanged SPIR-V.
            auto pipeline = std::make_unique<vk::raii::Pipeline>(context.device, *context.pipelineCache, pipelineInfo);
            struct Built
            {
                std::unique_ptr<vk::raii::PipelineLayout> layout;
                std::unique_ptr<vk::raii::Pipeline> pipeline;
            };
            return Built{.layout = std::move(layout), .pipeline = std::move(pipeline)};
        };

        // SPIR-V is read from disk on every rebuild() - rebuild() runs only
        // when frame resources were recreated (swapchain resize) or on the
        // first record call, never on the per-frame hot path. Re-reading
        // here keeps the live shader bytes honest: a developer who recompiles
        // a .comp and triggers a rebuild (window resize, or - with the
        // renderer's hot-reload path - a watched .spv change that now also
        // notifies attached techniques) sees the new SPIR-V on the next
        // rebuild. A byte cache here would save a few KB of file I/O per
        // rebuild but break that dev-loop unless invalidated everywhere
        // rebuild could be triggered, which is exactly the surface the
        // hot-reload path covers - so the cache would either duplicate or
        // risk that surface. Not caching is the simpler correct shape.
        auto marchBuilt = makeComputePipeline(
            readShaderBytes(context.shaderDirectory / "rc_march.comp.spv", moduleContext),
            *sharedSetLayout,
            *marchSetLayout,
            sizeof(MarchPush)
        );
        marchPipelineLayout = std::move(marchBuilt.layout);
        marchPipeline       = std::move(marchBuilt.pipeline);
        auto composeBuilt   = makeComputePipeline(
            readShaderBytes(context.shaderDirectory / "rc_compose.comp.spv", moduleContext),
            *sharedSetLayout,
            *composeSetLayout,
            sizeof(ComposePush)
        );
        composePipelineLayout = std::move(composeBuilt.layout);
        composePipeline       = std::move(composeBuilt.pipeline);
        auto gatherBuilt      = makeComputePipeline(
            readShaderBytes(context.shaderDirectory / "rc_gather.comp.spv", moduleContext),
            *sharedSetLayout,
            *gatherSetLayout,
            sizeof(GatherPush)
        );
        gatherPipelineLayout = std::move(gatherBuilt.layout);
        gatherPipeline       = std::move(gatherBuilt.pipeline);
        // The filter pass has its own small layout (3 sampled images + 1
        // storage image) - the gather layout grew per-cascade sampler
        // bindings the filter does not use. Push block: params + camera
        // planes (32 bytes) - the planes linearize buffer depth for the
        // edge-aware depth band.
        auto denoiseBuilt = makeComputePipeline(
            readShaderBytes(context.shaderDirectory / "rc_denoise.comp.spv", moduleContext),
            *sharedSetLayout,
            *filterSetLayout,
            sizeof(FilterPush)
        );
        denoisePipelineLayout = std::move(denoiseBuilt.layout);
        denoisePipeline       = std::move(denoiseBuilt.pipeline);

        // Descriptor sets ---------------------------------------------------
        {
            std::array<vk::DescriptorPoolSize, 3> poolSizes{
                vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, 3),
                vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 3),
                vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 2)
            };
            vk::DescriptorPoolCreateInfo poolInfo;
            poolInfo.setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet);
            poolInfo.setMaxSets(1);
            poolInfo.setPoolSizes(poolSizes);
            sharedPool = std::make_unique<vk::raii::DescriptorPool>(context.device, poolInfo);
            vk::DescriptorSetAllocateInfo allocate;
            allocate.setDescriptorPool(**sharedPool);
            allocate.setSetLayouts(**sharedSetLayout);
            sharedSet = std::make_unique<vk::raii::DescriptorSet>(
                std::move(vk::raii::DescriptorSets(context.device, allocate).front())
            );

            vk::DescriptorBufferInfo cameraInfo(context.cameraBuffer, 0, sizeof(CameraData));
            vk::DescriptorBufferInfo primitivesInfo(context.primitivesBuffer, 0, vk::WholeSize);
            vk::DescriptorBufferInfo lightInfo(**lightStorage.buffer, 0, sizeof(GpuLightUniform));
            vk::DescriptorBufferInfo materialsInfo(context.materialsBuffer, 0, vk::WholeSize);
            vk::DescriptorImageInfo albedoArrayInfo(
                **nearestSampler, context.albedoArrayView, vk::ImageLayout::eShaderReadOnlyOptimal
            );
            vk::DescriptorImageInfo dataArrayInfo(
                **nearestSampler, context.dataArrayView, vk::ImageLayout::eShaderReadOnlyOptimal
            );
            vk::DescriptorBufferInfo environmentInfo(context.environmentBuffer, 0, vk::WholeSize);
            vk::DescriptorBufferInfo directionsInfo(**directionStorage.buffer, 0, vk::WholeSize);
            std::array<vk::WriteDescriptorSet, 8> writes;
            writes[0].setDstSet(**sharedSet).setDstBinding(0).setDescriptorType(vk::DescriptorType::eUniformBuffer);
            writes[0].setBufferInfo(cameraInfo);
            writes[1].setDstSet(**sharedSet).setDstBinding(1).setDescriptorType(vk::DescriptorType::eStorageBuffer);
            writes[1].setBufferInfo(primitivesInfo);
            writes[2].setDstSet(**sharedSet).setDstBinding(2).setDescriptorType(vk::DescriptorType::eUniformBuffer);
            writes[2].setBufferInfo(lightInfo);
            writes[3].setDstSet(**sharedSet).setDstBinding(3).setDescriptorType(vk::DescriptorType::eStorageBuffer);
            writes[3].setBufferInfo(materialsInfo);
            writes[4]
                .setDstSet(**sharedSet)
                .setDstBinding(4)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            writes[4].setImageInfo(albedoArrayInfo);
            writes[5]
                .setDstSet(**sharedSet)
                .setDstBinding(5)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            writes[5].setImageInfo(dataArrayInfo);
            writes[6].setDstSet(**sharedSet).setDstBinding(6).setDescriptorType(vk::DescriptorType::eUniformBuffer);
            writes[6].setBufferInfo(environmentInfo);
            writes[7].setDstSet(**sharedSet).setDstBinding(7).setDescriptorType(vk::DescriptorType::eStorageBuffer);
            writes[7].setBufferInfo(directionsInfo);
            context.device.updateDescriptorSets(writes, {});
        }

        const vk::ImageView dummyView = *skyImage.view;
        marchSets.resize(cascades);
        {
            std::array<vk::DescriptorPoolSize, 2> poolSizes{
                vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, cascades * 3),
                vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, cascades * 2)
            };
            vk::DescriptorPoolCreateInfo poolInfo;
            poolInfo.setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet);
            poolInfo.setMaxSets(cascades);
            poolInfo.setPoolSizes(poolSizes);
            marchPool = std::make_unique<vk::raii::DescriptorPool>(context.device, poolInfo);
            for (std::uint32_t cascade = 0; cascade < cascades; ++cascade)
            {
                vk::DescriptorSetAllocateInfo allocate;
                allocate.setDescriptorPool(**marchPool);
                allocate.setSetLayouts(**marchSetLayout);
                marchSets[cascade] = std::make_unique<vk::raii::DescriptorSet>(
                    std::move(vk::raii::DescriptorSets(context.device, allocate).front())
                );
                vk::DescriptorImageInfo depthInfo(**nearestSampler, context.depthView, vk::ImageLayout::eGeneral);
                vk::DescriptorImageInfo normalInfo(**nearestSampler, context.normalView, vk::ImageLayout::eGeneral);
                vk::DescriptorImageInfo outInfo(
                    **nearestSampler, *marchImages[cascade].view, vk::ImageLayout::eGeneral
                );
                // The bounce source: the gather temp (full extent 2D view).
                // Bounce 0's march never samples it - the push flags.w gates
                // the read - but the descriptor stays bound.
                vk::DescriptorImageInfo bounceSourceInfo(**nearestSampler, *gatherTemp.view, vk::ImageLayout::eGeneral);
                // The anchor lane's publication target: the shared probe
                // anchor field (every cascade set binds the same image; the
                // layer addressing travels in the push constants).
                vk::DescriptorImageInfo anchorInfo(**nearestSampler, *anchorField.view, vk::ImageLayout::eGeneral);
                std::array<vk::WriteDescriptorSet, 5> writes;
                writes[0]
                    .setDstSet(**marchSets[cascade])
                    .setDstBinding(0)
                    .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
                writes[0].setImageInfo(depthInfo);
                writes[1]
                    .setDstSet(**marchSets[cascade])
                    .setDstBinding(1)
                    .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
                writes[1].setImageInfo(normalInfo);
                writes[2]
                    .setDstSet(**marchSets[cascade])
                    .setDstBinding(2)
                    .setDescriptorType(vk::DescriptorType::eStorageImage);
                writes[2].setImageInfo(outInfo);
                writes[3]
                    .setDstSet(**marchSets[cascade])
                    .setDstBinding(3)
                    .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
                writes[3].setImageInfo(bounceSourceInfo);
                writes[4]
                    .setDstSet(**marchSets[cascade])
                    .setDstBinding(4)
                    .setDescriptorType(vk::DescriptorType::eStorageImage);
                writes[4].setImageInfo(anchorInfo);
                context.device.updateDescriptorSets(writes, {});
            }
        }

        composeSets.resize(cascades);
        {
            std::array<vk::DescriptorPoolSize, 2> poolSizes{
                vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, cascades),
                vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, cascades * 2)
            };
            vk::DescriptorPoolCreateInfo poolInfo;
            poolInfo.setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet);
            poolInfo.setMaxSets(cascades);
            poolInfo.setPoolSizes(poolSizes);
            composePool = std::make_unique<vk::raii::DescriptorPool>(context.device, poolInfo);
            for (std::uint32_t cascade = 0; cascade < cascades; ++cascade)
            {
                vk::DescriptorSetAllocateInfo allocate;
                allocate.setDescriptorPool(**composePool);
                allocate.setSetLayouts(**composeSetLayout);
                composeSets[cascade] = std::make_unique<vk::raii::DescriptorSet>(
                    std::move(vk::raii::DescriptorSets(context.device, allocate).front())
                );
                // The coarsest cascade has no coarser neighbor: bind the sky
                // dummy (the shader branches to the sky constant before it
                // could ever read it).
                const vk::ImageView coarseView = cascade + 1 < cascades ? *fullImages[cascade + 1].view : dummyView;
                vk::DescriptorImageInfo coarseInfo(**composeSampler, coarseView, vk::ImageLayout::eGeneral);
                vk::DescriptorImageInfo outInfo(**nearestSampler, *fullImages[cascade].view, vk::ImageLayout::eGeneral);
                vk::DescriptorImageInfo marchInfo(
                    **nearestSampler, *marchImages[cascade].view, vk::ImageLayout::eGeneral
                );
                std::array<vk::WriteDescriptorSet, 3> writes;
                writes[0]
                    .setDstSet(**composeSets[cascade])
                    .setDstBinding(0)
                    .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
                writes[0].setImageInfo(coarseInfo);
                writes[1]
                    .setDstSet(**composeSets[cascade])
                    .setDstBinding(1)
                    .setDescriptorType(vk::DescriptorType::eStorageImage);
                writes[1].setImageInfo(outInfo);
                writes[2]
                    .setDstSet(**composeSets[cascade])
                    .setDstBinding(2)
                    .setDescriptorType(vk::DescriptorType::eStorageImage);
                writes[2].setImageInfo(marchInfo);
                context.device.updateDescriptorSets(writes, {});
            }
        }

        {
            std::array<vk::DescriptorPoolSize, 2> poolSizes{
                vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 10),
                vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, 2)
            };
            vk::DescriptorPoolCreateInfo poolInfo;
            poolInfo.setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet);
            poolInfo.setMaxSets(1);
            poolInfo.setPoolSizes(poolSizes);
            gatherPool = std::make_unique<vk::raii::DescriptorPool>(context.device, poolInfo);
            vk::DescriptorSetAllocateInfo allocate;
            allocate.setDescriptorPool(**gatherPool);
            allocate.setSetLayouts(**gatherSetLayout);
            gatherSet = std::make_unique<vk::raii::DescriptorSet>(
                std::move(vk::raii::DescriptorSets(context.device, allocate).front())
            );
            // Bindings 0..7: the composed cascade of that index; slots beyond
            // the configured cascade count bind the sky stand-in (the gather
            // selection clamps to the configured count and never reads them).
            // The image info array must outlive updateDescriptorSets: the
            // write structs only reference it.
            std::array<vk::WriteDescriptorSet, 12> writes;
            std::array<vk::DescriptorImageInfo, 8> cascadeInfos;
            for (std::uint32_t cascade = 0; cascade < 8; ++cascade)
            {
                const vk::ImageView cascadeView = cascade < cascades ? *fullImages[cascade].view : *skyImage.view;
                cascadeInfos[cascade]
                    = vk::DescriptorImageInfo(**nearestSampler, cascadeView, vk::ImageLayout::eGeneral);
                writes[cascade]
                    .setDstSet(**gatherSet)
                    .setDstBinding(cascade)
                    .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
                writes[cascade].setImageInfo(cascadeInfos[cascade]);
            }
            vk::DescriptorImageInfo depthInfo(**nearestSampler, context.depthView, vk::ImageLayout::eGeneral);
            vk::DescriptorImageInfo normalInfo(**nearestSampler, context.normalView, vk::ImageLayout::eGeneral);
            // The gather writes the MODULE temp; the filter pass turns it
            // into the renderer-owned indirect target.
            vk::DescriptorImageInfo outInfo(**nearestSampler, *gatherTemp.view, vk::ImageLayout::eGeneral);
            writes[8]
                .setDstSet(**gatherSet)
                .setDstBinding(8)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            writes[8].setImageInfo(depthInfo);
            writes[9]
                .setDstSet(**gatherSet)
                .setDstBinding(9)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            writes[9].setImageInfo(normalInfo);
            writes[10].setDstSet(**gatherSet).setDstBinding(10).setDescriptorType(vk::DescriptorType::eStorageImage);
            writes[10].setImageInfo(outInfo);
            // The march-written probe anchor field (rc_gather.comp binding 11,
            // probeAnchor's imageLoad source).
            vk::DescriptorImageInfo anchorInfo(**nearestSampler, *anchorField.view, vk::ImageLayout::eGeneral);
            writes[11].setDstSet(**gatherSet).setDstBinding(11).setDescriptorType(vk::DescriptorType::eStorageImage);
            writes[11].setImageInfo(anchorInfo);
            context.device.updateDescriptorSets(writes, {});
        }

        {
            std::array<vk::DescriptorPoolSize, 2> poolSizes{
                vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 3),
                vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, 1)
            };
            vk::DescriptorPoolCreateInfo poolInfo;
            poolInfo.setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet);
            poolInfo.setMaxSets(1);
            poolInfo.setPoolSizes(poolSizes);
            denoisePool = std::make_unique<vk::raii::DescriptorPool>(context.device, poolInfo);
            vk::DescriptorSetAllocateInfo allocate;
            allocate.setDescriptorPool(**denoisePool);
            allocate.setSetLayouts(**filterSetLayout);
            denoiseSet = std::make_unique<vk::raii::DescriptorSet>(
                std::move(vk::raii::DescriptorSets(context.device, allocate).front())
            );
            vk::DescriptorImageInfo giInfo(**nearestSampler, *gatherTemp.view, vk::ImageLayout::eGeneral);
            vk::DescriptorImageInfo depthInfo(**nearestSampler, context.depthView, vk::ImageLayout::eGeneral);
            vk::DescriptorImageInfo normalInfo(**nearestSampler, context.normalView, vk::ImageLayout::eGeneral);
            vk::DescriptorImageInfo outInfo(**nearestSampler, context.indirectRadianceView, vk::ImageLayout::eGeneral);
            std::array<vk::WriteDescriptorSet, 4> writes;
            writes[0]
                .setDstSet(**denoiseSet)
                .setDstBinding(0)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            writes[0].setImageInfo(giInfo);
            writes[1]
                .setDstSet(**denoiseSet)
                .setDstBinding(1)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            writes[1].setImageInfo(depthInfo);
            writes[2]
                .setDstSet(**denoiseSet)
                .setDstBinding(2)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            writes[2].setImageInfo(normalInfo);
            writes[3].setDstSet(**denoiseSet).setDstBinding(3).setDescriptorType(vk::DescriptorType::eStorageImage);
            writes[3].setImageInfo(outInfo);
            context.device.updateDescriptorSets(writes, {});
        }

        cachedExtent  = context.extent;
        needsRecreate = false;
    }

    Settings settings;
    LightingCapability *lighting = nullptr;

    // Direction count of the active angular mapping (the direction table's
    // size): the march/compose third dispatch dimension, the cascade image
    // layer count, and the gather's quadrature loop bound.
    std::uint32_t directionCount = 0;
    vk::Extent2D cachedExtent{};
    bool needsRecreate = false;

    // Per-cascade geometry cache (computed once per rebuild, read by
    // record()). The lambdas that produced these values per frame were the
    // record() hot path's only per-cascade computation; with cascades=5 that
    // was 20 std::pow() calls and 10 ceil-divs per frame for the dispatch
    // arguments alone. The cache also exposes the stride directly to the
    // march push block (record() used to recompute `settings.probeStride <<
    // cascade` four times per cascade per frame).
    struct CascadeGeometry
    {
        std::uint32_t stride     = 0;
        std::uint32_t gridWidth  = 0;
        std::uint32_t gridHeight = 0;
        float intervalStart      = 0.0f;
        float intervalEnd        = 0.0f;
        std::uint32_t dispatchX  = 0;
        std::uint32_t dispatchY  = 0;
    };
    std::vector<CascadeGeometry> cascadeGeometry;
    std::uint32_t marchDispatchZ  = 0;
    std::uint32_t gatherDispatchX = 0;
    std::uint32_t gatherDispatchY = 0;

    std::vector<CascadeImage> marchImages;
    std::vector<CascadeImage> fullImages;
    CascadeImage gatherTemp; // full extent gather output, filtered into the target
    CascadeImage skyImage;   // 1x1x1 stand-in for the coarsest compose binding
    // The probe anchor field: one (anchor.xyz, view depth) + one (normal.xyz)
    // layer pair per cascade, written once per probe by the march's anchor
    // lane, read back by the gather's probeAnchor. rgba32f keeps the values
    // bit-exact against the fp32 math that used to run in both stages.
    CascadeImage anchorField;

    std::unique_ptr<vk::raii::Sampler> nearestSampler;
    std::unique_ptr<vk::raii::Sampler> composeSampler;
    // Last uniform bytes written into lightStorage (the change gate in
    // updateLightUniform); reset by rebuild so a fresh buffer always
    // receives the first write.
    GpuLightUniform uploadedLightUniform{};
    bool uploadedLightUniformValid = false;
    MappedHostVisibleBuffer lightStorage;
    MappedHostVisibleBuffer directionStorage;

    std::unique_ptr<vk::raii::DescriptorSetLayout> sharedSetLayout;
    std::unique_ptr<vk::raii::DescriptorSetLayout> marchSetLayout;
    std::unique_ptr<vk::raii::DescriptorSetLayout> composeSetLayout;
    std::unique_ptr<vk::raii::DescriptorSetLayout> gatherSetLayout;
    std::unique_ptr<vk::raii::DescriptorSetLayout> filterSetLayout;
    std::unique_ptr<vk::raii::DescriptorPool> sharedPool;
    std::unique_ptr<vk::raii::DescriptorSet> sharedSet;
    std::unique_ptr<vk::raii::DescriptorPool> marchPool;
    std::vector<std::unique_ptr<vk::raii::DescriptorSet>> marchSets;
    std::unique_ptr<vk::raii::DescriptorPool> composePool;
    std::vector<std::unique_ptr<vk::raii::DescriptorSet>> composeSets;
    std::unique_ptr<vk::raii::DescriptorPool> gatherPool;
    std::unique_ptr<vk::raii::DescriptorSet> gatherSet;

    std::unique_ptr<vk::raii::PipelineLayout> marchPipelineLayout;
    std::unique_ptr<vk::raii::Pipeline> marchPipeline;
    std::unique_ptr<vk::raii::PipelineLayout> composePipelineLayout;
    std::unique_ptr<vk::raii::Pipeline> composePipeline;
    std::unique_ptr<vk::raii::PipelineLayout> gatherPipelineLayout;
    std::unique_ptr<vk::raii::Pipeline> gatherPipeline;
    std::unique_ptr<vk::raii::PipelineLayout> denoisePipelineLayout;
    std::unique_ptr<vk::raii::Pipeline> denoisePipeline;
    std::unique_ptr<vk::raii::DescriptorPool> denoisePool;
    std::unique_ptr<vk::raii::DescriptorSet> denoiseSet;

    // Record-path scratch reused across frames (see barrierCascades above).
    std::vector<vk::ImageMemoryBarrier2> cascadeBarrierScratch;
    std::vector<vk::Image> cascadeImageScratch;
};

// ---- World-space placement ----------------------------------------------------------
//
// The world-space variant: probes live in a dense 3D grid (camera-centered,
// cell size = world_cell_size * 2^cascade). Storage reuses the 2D array
// image pattern from screen-space, but the (cell_y, cell_z) pair is flattened
// along the Y axis: image extent is (gridW, gridH * gridD), layers =
// directionCount. Texel (x, y, layer) holds the marched/composed (rgb
// radiance, a transmittance) for probe (cell_x=x, cell_y=y % gridH,
// cell_z=y / gridH) and direction = layer. The cell-flattening keeps the
// layer count = directionCount, independent of grid dimensions, so the
// world-space base_rays stays configurable up to Vulkan's maxImageArrayLayers
// (>= 256).
//
// The march traces the analytic scene SDF from each probe's world-space cell
// center (no depth-buffer dependency) - the whole point of world-space RC:
// probes can sample off-screen geometry. The gather blends 8 trilinear
// probes per cascade at the pixel's surface world position (vs screen-space's
// 4 bilinear probes per cascade). The denoise shader is SHARED with
// screen-space (it operates on the gather output image, which is screen-space
// regardless of where the probes lived).
//
// The grid follows the camera between frames (the origin is recomputed per
// rebuild from context.cameraPosition). Multi-bounce: same pattern as
// screen-space (gather temp ping-pong, last bounce keeps the direct fallback).
class WorldSpaceRadianceCascadesPass final : public LightingTechniquePass
{
public:
    struct Settings
    {
        std::uint32_t cascades = defaultCascades;
        float intervalBase     = defaultIntervalBase;
        float intervalFactor   = defaultIntervalFactor;
        // World-space grid: cells per axis for cascade 0. Each coarser
        // cascade doubles the cell size (cellSize_c = worldCellSize * 2^c).
        // The grid dimensions stay constant per cascade (the coarser grid
        // covers a 2x larger world extent).
        float worldCellSize               = 1.0f;
        std::uint32_t gridW               = 8;
        std::uint32_t gridH               = 8;
        std::uint32_t gridD               = 4;
        std::uint32_t baseRays            = defaultBaseRays;
        bool gatherVisibility             = true;
        std::uint32_t losSteps            = defaultLosSteps;
        std::uint32_t marchSteps          = defaultMarchSteps;
        bool relightShadows               = true;
        std::uint32_t shadowSteps         = defaultShadowSteps;
        std::uint32_t spatialFilterRadius = defaultSpatialFilterRadius;
        CascadeAngularMapping mapping     = CascadeAngularMapping::Octahedron;
        std::uint32_t bounces             = defaultBounces;
    };

    WorldSpaceRadianceCascadesPass(
        Settings settings, LightingCapability *lightingCapability
    )
        : settings(settings), lighting(lightingCapability)
    {}

    const char *techniqueName() const override
    {
        return "radiance_cascades";
    }

    LightingTarget targets() const override
    {
        return LightingTarget::IndirectDiffuse;
    }

    void record(
        vk::raii::CommandBuffer &commandBuffer, const FrameRenderContext &context
    ) override
    {
        // Resource teardown/recreation only answers structural changes: the
        // swapchain resized, the frame resources were recreated, or this is
        // the first frame. A camera move is NOT structural - the cascade
        // images, descriptor sets and pipelines are all origin-independent
        // (the origin reaches the shaders through push constants, see the
        // march/compose/gather push blocks below), so a moving camera only
        // re-centers the per-cascade grid geometry, a pure CPU loop. Rebuilding
        // here would tear down and recreate ~2*cascadeCount images (each with
        // its own vkAllocateMemory), every descriptor pool/set and every
        // pipeline, and round-trip a submit + waitIdle - per frame, while
        // flying - for data the push constants already carry.
        if (cascadeCount() == 0 || context.extent != cachedExtent || needsRecreate)
        {
            rebuild(context);
        } else if (cameraPositionCache != context.cameraPosition)
        {
            updateWorldGeometry(context);
        }

        updateLightUniform();

        const std::uint32_t cascades   = cascadeCount();
        const std::uint32_t directions = directionCount;
        for (std::uint32_t bounce = 0; bounce < settings.bounces; ++bounce)
        {
            const bool lastBounce = bounce + 1 == settings.bounces;

            // ---- March: every cascade traces its own interval from each
            // probe's world-space cell center. -----------
            commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, **marchPipeline);
            commandBuffer.bindDescriptorSets(
                vk::PipelineBindPoint::eCompute, **marchPipelineLayout, 0, **sharedSet, {}
            );
            for (std::uint32_t cascade = 0; cascade < cascades; ++cascade)
            {
                commandBuffer.bindDescriptorSets(
                    vk::PipelineBindPoint::eCompute, **marchPipelineLayout, 1, **marchSets[cascade], {}
                );
                const WorldCascadeGeometry &g = worldGeometry[cascade];
                const WorldMarchPush pushValue{
                    .interval     = {g.intervalStart, g.intervalEnd, 0.0f, 0.0f},
                    .flags        = {context.primitiveCount, settings.marchSteps, 0u, bounce},
                    .grid         = {settings.gridW, settings.gridH, directions, settings.gridD},
                    .extentStride = {g.origin[0], g.origin[1], g.origin[2], g.cellSize},
                    .anchor       = {0u, 0u, 0u, 0u}
                };
                commandBuffer.pushConstants(
                    **marchPipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pushValue), &pushValue
                );
                // Workgroup 4x4x8 = 128 threads, matching the screen-space
                // march's tile shape. The dispatch's Y axis covers
                // (gridH * gridD) probes (the cell-flattening along Y). The Z
                // axis is the cached marchDispatchZ (set in rebuild() - a
                // function of directionCount alone, like the screen-space
                // pass's cache).
                commandBuffer.dispatch(
                    (settings.gridW + 3) / 4, (settings.gridH * settings.gridD + 3) / 4, marchDispatchZ
                );
            }

            marchBarriers(cascadeImageScratch);
            barrierCascades(
                commandBuffer,
                cascadeImageScratch,
                vk::PipelineStageFlagBits2::eComputeShader,
                vk::PipelineStageFlagBits2::eComputeShader
            );
            // The previous bounce's gather temp is read by the next bounce's
            // march (when bounce > 0). The first bounce's march does not
            // read prevGi (the push flag gates the sample), so this barrier
            // is a no-op until the gather writes the temp image.
            barrierGatherTemp(commandBuffer, vk::AccessFlagBits2::eShaderRead, vk::AccessFlagBits2::eShaderRead);

            // ---- Compose: front-to-back interval chaining in world space.
            commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, **composePipeline);
            commandBuffer.bindDescriptorSets(
                vk::PipelineBindPoint::eCompute, **composePipelineLayout, 0, **sharedSet, {}
            );
            for (std::uint32_t cascade = cascades; cascade-- > 0;)
            {
                commandBuffer.bindDescriptorSets(
                    vk::PipelineBindPoint::eCompute, **composePipelineLayout, 1, **composeSets[cascade], {}
                );
                const WorldCascadeGeometry &g      = worldGeometry[cascade];
                const WorldCascadeGeometry *coarse = (cascade + 1 < cascades) ? &worldGeometry[cascade + 1] : nullptr;
                const WorldComposePush pushValue{
                    .coarsestFlag       = {0.0f, 0.0f, 0.0f, cascade == cascades - 1 ? 1.0f : 0.0f},
                    .fineGrid           = {settings.gridW, settings.gridH, directions, settings.gridD},
                    .coarseGrid         = {settings.gridW, settings.gridH, directions, settings.gridD},
                    .fineOriginCellSize = {g.origin[0], g.origin[1], g.origin[2], g.cellSize},
                    .coarseOriginCellSize
                    = coarse
                        ? std::
                              array<float, 4>{coarse->origin[0], coarse->origin[1], coarse->origin[2], coarse->cellSize}
                        : std::array<float, 4>{g.origin[0], g.origin[1], g.origin[2], g.cellSize}
                };
                commandBuffer.pushConstants(
                    **composePipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pushValue), &pushValue
                );
                commandBuffer.dispatch(
                    (settings.gridW + 3) / 4, (settings.gridH * settings.gridD + 3) / 4, marchDispatchZ
                );

                // full[c] write -> full[c-1] read in the next iteration.
                std::array<vk::ImageMemoryBarrier2, 1> barrier;
                barrier[0].setSrcStageMask(vk::PipelineStageFlagBits2::eComputeShader);
                barrier[0].setSrcAccessMask(vk::AccessFlagBits2::eShaderStorageWrite);
                barrier[0].setDstStageMask(vk::PipelineStageFlagBits2::eComputeShader);
                barrier[0].setDstAccessMask(vk::AccessFlagBits2::eShaderRead);
                barrier[0].setOldLayout(vk::ImageLayout::eGeneral);
                barrier[0].setNewLayout(vk::ImageLayout::eGeneral);
                barrier[0].setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
                barrier[0].setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
                barrier[0].setImage(**fullImages[cascade].image);
                barrier[0].setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
                vk::DependencyInfo dependency;
                dependency.setImageMemoryBarriers(barrier);
                commandBuffer.pipelineBarrier2(dependency);
            }

            // ---- Gather: per pixel diffuse irradiance from world-space cascades.
            commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, **gatherPipeline);
            commandBuffer.bindDescriptorSets(
                vk::PipelineBindPoint::eCompute, **gatherPipelineLayout, 0, **sharedSet, {}
            );
            commandBuffer.bindDescriptorSets(
                vk::PipelineBindPoint::eCompute, **gatherPipelineLayout, 1, **gatherSet, {}
            );
            const WorldGatherPush gatherPush{
                .counts = {context.primitiveCount, settings.losSteps, settings.gatherVisibility ? 1u : 0u, cascades},
                .schedule = {settings.intervalBase, settings.intervalFactor, 0.0f, 0.0f},
                .dirs = {directions, 0u, 0u, 0u},
                .originCellSize0 = {worldGeometry[0].origin[0], worldGeometry[0].origin[1], worldGeometry[0].origin[2], worldGeometry[0].cellSize},
                .dims0 = {settings.gridW, settings.gridH, settings.gridD, 0u},
                .originCellSize1 = cascades > 1
                    ? std::array<float, 4>{worldGeometry[1].origin[0], worldGeometry[1].origin[1], worldGeometry[1].origin[2], worldGeometry[1].cellSize}
                    : std::array<float, 4>{worldGeometry[0].origin[0], worldGeometry[0].origin[1], worldGeometry[0].origin[2], worldGeometry[0].cellSize},
                .dims1 = {settings.gridW, settings.gridH, settings.gridD, 0u},
                .pureTransport = {lastBounce ? 0.0f : 1.0f, 0.0f, 0.0f, 0.0f}
            };
            commandBuffer.pushConstants(
                **gatherPipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(gatherPush), &gatherPush
            );
            commandBuffer.dispatch(gatherDispatchX, gatherDispatchY, 1);

            barrierGatherTemp(
                commandBuffer, vk::AccessFlagBits2::eShaderStorageWrite, vk::AccessFlagBits2::eShaderRead
            );
        }

        // ---- Spatial filter: edge-aware cleanup (SHARED with screen-space
        // path; same shader rc_denoise.comp). -----
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, **denoisePipeline);
        commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, **denoisePipelineLayout, 0, **sharedSet, {});
        commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, **denoisePipelineLayout, 1, **denoiseSet, {});
        const FilterPush filterPush{
            .params
            = {static_cast<std::int32_t>(settings.spatialFilterRadius), filterNormalPower, filterDepthBandPercent, 0},
            .cameraPlanes = {context.cameraNearPlane, context.cameraFarPlane, 0.0f, 0.0f}
        };
        commandBuffer.pushConstants(
            **denoisePipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(filterPush), &filterPush
        );
        commandBuffer.dispatch(gatherDispatchX, gatherDispatchY, 1);
    }

    void notifyFrameResourcesRecreated() override
    {
        needsRecreate = true;
    }

private:
    std::uint32_t cascadeCount() const
    {
        return settings.cascades;
    }

    void barrierCascades(
        vk::raii::CommandBuffer &commandBuffer,
        const std::vector<vk::Image> &images,
        vk::PipelineStageFlagBits2 sourceStage,
        vk::PipelineStageFlagBits2 destinationStage
    )
    {
        std::vector<vk::ImageMemoryBarrier2> &barriers = cascadeBarrierScratch;
        barriers.clear();
        for (const vk::Image &image : images)
        {
            vk::ImageMemoryBarrier2 barrier;
            barrier.setSrcStageMask(sourceStage);
            barrier.setSrcAccessMask(vk::AccessFlagBits2::eShaderStorageWrite);
            barrier.setDstStageMask(destinationStage);
            barrier.setDstAccessMask(vk::AccessFlagBits2::eShaderRead);
            barrier.setOldLayout(vk::ImageLayout::eGeneral);
            barrier.setNewLayout(vk::ImageLayout::eGeneral);
            barrier.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            barrier.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            barrier.setImage(image);
            barrier.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
            barriers.push_back(barrier);
        }
        vk::DependencyInfo dependency;
        dependency.setImageMemoryBarriers(barriers);
        commandBuffer.pipelineBarrier2(dependency);
    }

    void marchBarriers(
        std::vector<vk::Image> &images
    )
    {
        images.clear();
        for (const CascadeImage &cascade : marchImages)
        {
            images.push_back(**cascade.image);
        }
    }

    void barrierGatherTemp(
        vk::raii::CommandBuffer &commandBuffer, vk::AccessFlagBits2 sourceAccess, vk::AccessFlagBits2 destinationAccess
    )
    {
        vk::ImageMemoryBarrier2 barrier;
        barrier.setSrcStageMask(vk::PipelineStageFlagBits2::eComputeShader);
        barrier.setSrcAccessMask(sourceAccess);
        barrier.setDstStageMask(vk::PipelineStageFlagBits2::eComputeShader);
        barrier.setDstAccessMask(destinationAccess);
        barrier.setOldLayout(vk::ImageLayout::eGeneral);
        barrier.setNewLayout(vk::ImageLayout::eGeneral);
        barrier.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
        barrier.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
        barrier.setImage(**gatherTemp.image);
        barrier.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
        std::array<vk::ImageMemoryBarrier2, 1> barriers{barrier};
        vk::DependencyInfo dependency;
        dependency.setImageMemoryBarriers(barriers);
        commandBuffer.pipelineBarrier2(dependency);
    }

    // Allocates the image, memory and view (no layout transition - the batched
    // submission at the end of rebuild() performs every Undefined -> General
    // transition in one shot). Same shape as the screen-space helper.
    void createCascadeImageNoTransition(
        CascadeImage &target,
        const FrameRenderContext &context,
        vk::Extent2D grid,
        std::uint32_t layers,
        vk::ImageViewType viewType = vk::ImageViewType::e2DArray,
        vk::Format format          = vk::Format::eR16G16B16A16Sfloat
    )
    {
        vk::ImageCreateInfo imageInfo;
        imageInfo.setImageType(vk::ImageType::e2D);
        imageInfo.setFormat(format);
        imageInfo.setExtent(vk::Extent3D(grid.width, grid.height, 1));
        imageInfo.setMipLevels(1);
        imageInfo.setArrayLayers(layers);
        imageInfo.setSamples(vk::SampleCountFlagBits::e1);
        imageInfo.setTiling(vk::ImageTiling::eOptimal);
        imageInfo.setUsage(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage);
        imageInfo.setSharingMode(vk::SharingMode::eExclusive);
        imageInfo.setInitialLayout(vk::ImageLayout::eUndefined);
        target.image                              = std::make_unique<vk::raii::Image>(context.device, imageInfo);
        const vk::MemoryRequirements requirements = target.image->getMemoryRequirements();
        vk::MemoryAllocateInfo allocate;
        allocate.setAllocationSize(requirements.size);
        allocate.setMemoryTypeIndex(findMemoryType(
            context.physicalDevice, requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal
        ));
        target.memory = std::make_unique<vk::raii::DeviceMemory>(context.device, allocate);
        target.image->bindMemory(*target.memory, 0);
        vk::ImageViewCreateInfo viewInfo;
        viewInfo.setImage(**target.image);
        viewInfo.setViewType(viewType);
        viewInfo.setFormat(format);
        viewInfo.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers));
        target.view = std::make_unique<vk::raii::ImageView>(context.device, viewInfo);
    }

    void recordInitialTransition(
        vk::raii::CommandBuffer &commandBuffer, CascadeImage &target, std::uint32_t layers
    )
    {
        vk::ImageMemoryBarrier2 barrier;
        barrier.setSrcStageMask(vk::PipelineStageFlagBits2::eAllCommands);
        barrier.setSrcAccessMask(vk::AccessFlags2());
        barrier.setDstStageMask(vk::PipelineStageFlagBits2::eAllCommands);
        barrier.setDstAccessMask(vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
        barrier.setOldLayout(vk::ImageLayout::eUndefined);
        barrier.setNewLayout(vk::ImageLayout::eGeneral);
        barrier.setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
        barrier.setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
        barrier.setImage(**target.image);
        barrier.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers));
        std::array<vk::ImageMemoryBarrier2, 1> barriers{barrier};
        vk::DependencyInfo dependency;
        dependency.setImageMemoryBarriers(barriers);
        commandBuffer.pipelineBarrier2(dependency);
    }

    void updateLightUniform()
    {
        GpuLightUniform uniform;
        const bool hasLighting = lighting != nullptr;
        DirectionalLightDescription sun;
        const bool sunEnabled = hasLighting && lighting->hasSunLight();
        if (sunEnabled)
        {
            sun = lighting->sunLight();
        }
        const std::uint32_t count = hasLighting ? std::min(lighting->pointLightCount(), maxPointLights) : 0;
        std::array<PointLightDescription, maxPointLights> pointLights{};
        for (std::uint32_t index = 0; index < count; ++index)
        {
            pointLights[index] = lighting->pointLight(index);
        }
        fillSunAndPointLights(uniform, sunEnabled, sun, pointLights.data(), count);
        uniform.lightCountsAndFlags
            = {static_cast<std::int32_t>(count),
                settings.relightShadows ? 1 : 0,
                static_cast<std::int32_t>(settings.shadowSteps),
                0};
        // Change-gated write: the mapped buffer keeps its last bytes, so an
        // unchanged light set (the common frame) skips the coherent write.
        // Invalidated on rebuild, where the buffer is created fresh.
        if (!uploadedLightUniformValid || !(uniform == uploadedLightUniform))
        {
            std::memcpy(lightStorage.mapped, &uniform, sizeof(uniform));
            uploadedLightUniform      = uniform;
            uploadedLightUniformValid = true;
        }
    }

    // Recomputes the per-cascade grid geometry from the current camera
    // position: the only frame-varying inputs of the world-space pass. Pure
    // CPU work (cascadeCount iterations of arithmetic, no Vulkan calls), so
    // record() runs it whenever the camera moves without paying the resource
    // rebuild - the origin reaches the shaders through the march/compose/
    // gather push constants, which read worldGeometry after this runs.
    void updateWorldGeometry(
        const FrameRenderContext &context
    )
    {
        // World-space grid: camera-centered origin, cell size grows by 2x
        // per cascade. The grid origin is offset by half a cell stride per
        // cascade so coarser grids' corners align with finer grids' centres -
        // the standard radiance-cascades nesting rule.
        //
        // The camera position is read from context.cameraPosition (a clean
        // typed value the renderer fills from its own CameraData each frame,
        // avoiding any host-mapped buffer trick on the technique pass side).
        cameraPositionCache          = context.cameraPosition;
        const std::uint32_t cascades = cascadeCount();
        worldGeometry.assign(cascades, WorldCascadeGeometry{});
        for (std::uint32_t cascade = 0; cascade < cascades; ++cascade)
        {
            WorldCascadeGeometry &g = worldGeometry[cascade];
            const float cellSize    = settings.worldCellSize * static_cast<float>(1u << cascade);
            g.cellSize              = cellSize;
            // Grid origin: camera position offset so the camera sits in the
            // centre of the grid (each axis covers half the grid on either
            // side of the camera), and offset by half a cell per cascade so
            // coarser grids' corners align with finer grids' centres.
            const float halfExtentX  = static_cast<float>(settings.gridW) * cellSize * 0.5f;
            const float halfExtentY  = static_cast<float>(settings.gridH) * cellSize * 0.5f;
            const float halfExtentZ  = static_cast<float>(settings.gridD) * cellSize * 0.5f;
            const float coarseOffset = (cascade > 0) ? (cellSize * 0.5f) : 0.0f;
            g.origin
                = {cameraPositionCache[0] - halfExtentX + coarseOffset,
                    cameraPositionCache[1] - halfExtentY + coarseOffset,
                    cameraPositionCache[2] - halfExtentZ + coarseOffset};
            g.intervalStart = cascade == 0 ? 0.0f
                                           : settings.intervalBase
                                                 * std::pow(settings.intervalFactor, static_cast<float>(cascade - 1));
            g.intervalEnd   = settings.intervalBase * std::pow(settings.intervalFactor, static_cast<float>(cascade));
        }
    }

    void rebuild(
        const FrameRenderContext &context
    )
    {
        // Teardown in strict reverse dependency order: descriptor sets, pools,
        // pipelines, pipeline layouts, descriptor set layouts, then images,
        // buffers and samplers. Same discipline as screen-space.
        gatherSet.reset();
        gatherPool.reset();
        denoiseSet.reset();
        denoisePool.reset();
        for (auto &set : composeSets)
        {
            set.reset();
        }
        composePool.reset();
        for (auto &set : marchSets)
        {
            set.reset();
        }
        marchPool.reset();
        sharedSet.reset();
        sharedPool.reset();
        gatherPipeline.reset();
        gatherPipelineLayout.reset();
        denoisePipeline.reset();
        denoisePipelineLayout.reset();
        composePipeline.reset();
        composePipelineLayout.reset();
        marchPipeline.reset();
        marchPipelineLayout.reset();
        sharedSetLayout.reset();
        marchSetLayout.reset();
        composeSetLayout.reset();
        gatherSetLayout.reset();
        filterSetLayout.reset();
        for (auto &image : fullImages)
        {
            image = CascadeImage{};
        }
        for (auto &image : marchImages)
        {
            image = CascadeImage{};
        }
        gatherTemp = CascadeImage{};
        skyImage   = CascadeImage{};
        lightStorage.reset();
        directionStorage.reset();
        composeSampler.reset();
        nearestSampler.reset();

        const std::uint32_t cascades = cascadeCount();
        // Direction table (SAME builder as screen-space - octahedron,
        // cubemap or SH projection). The world-space base_rays is
        // configurable; the only storage constraint is directionCount <=
        // Vulkan's maxImageArrayLayers (>= 256), which the octahedron mapping
        // respects at base_rays up to 256 (16*16 = 256).
        const std::vector<std::array<float, 4>> directionTable
            = buildDirectionTable(settings.mapping, settings.baseRays);
        directionCount   = static_cast<std::uint32_t>(directionTable.size());
        directionStorage = createMappedHostVisibleBuffer(
            context.device,
            context.physicalDevice,
            static_cast<vk::DeviceSize>(directionTable.size() * sizeof(std::array<float, 4>)),
            vk::BufferUsageFlagBits::eStorageBuffer
        );
        std::memcpy(
            directionStorage.mapped, directionTable.data(), directionTable.size() * sizeof(std::array<float, 4>)
        );

        // ---- Per-cascade world geometry cache --------------------------------
        updateWorldGeometry(context);
        gatherDispatchX = (context.extent.width + 7) / 8;
        gatherDispatchY = (context.extent.height + 7) / 8;
        marchDispatchZ  = (directionCount + marchDirectionLanes - 1) / marchDirectionLanes;

        // Cascade storage: march + full per cascade, same 2D array image
        // pattern as screen-space. Texel (x, y, layer) holds data for
        // probe (cell_x=x, cell_y=y % gridH, cell_z=y / gridH) and
        // direction=layer. Image extent is (gridW, gridH * gridD).
        marchImages.resize(cascades);
        fullImages.resize(cascades);
        for (std::uint32_t cascade = 0; cascade < cascades; ++cascade)
        {
            const vk::Extent2D grid{settings.gridW, settings.gridH * settings.gridD};
            createCascadeImageNoTransition(marchImages[cascade], context, grid, directionCount);
            createCascadeImageNoTransition(fullImages[cascade], context, grid, directionCount);
        }
        createCascadeImageNoTransition(skyImage, context, vk::Extent2D{1, 1}, 1);
        createCascadeImageNoTransition(
            gatherTemp, context, vk::Extent2D{context.extent.width, context.extent.height}, 1, vk::ImageViewType::e2D
        );

        // Batched layout transitions for every cascade image + sky +
        // gather temp (same pattern as screen-space: one pool, one buffer,
        // one submit + waitIdle for the whole set).
        {
            vk::CommandPoolCreateInfo poolInfo;
            poolInfo.setFlags(vk::CommandPoolCreateFlagBits::eTransient);
            poolInfo.setQueueFamilyIndex(context.graphicsQueueFamily);
            vk::raii::CommandPool setupPool(context.device, poolInfo);
            vk::CommandBufferAllocateInfo allocateInfo;
            allocateInfo.setCommandPool(*setupPool);
            allocateInfo.setLevel(vk::CommandBufferLevel::ePrimary);
            allocateInfo.setCommandBufferCount(1);
            vk::raii::CommandBuffer setupBuffer
                = std::move(vk::raii::CommandBuffers(context.device, allocateInfo).front());
            setupBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
            for (std::uint32_t cascade = 0; cascade < cascades; ++cascade)
            {
                recordInitialTransition(setupBuffer, marchImages[cascade], directionCount);
                recordInitialTransition(setupBuffer, fullImages[cascade], directionCount);
            }
            recordInitialTransition(setupBuffer, skyImage, 1);
            recordInitialTransition(setupBuffer, gatherTemp, 1);
            setupBuffer.end();
            vk::SubmitInfo submitInfo;
            submitInfo.setCommandBuffers(*setupBuffer);
            context.graphicsQueue.submit(submitInfo);
            context.graphicsQueue.waitIdle();
        }

        nearestSampler = makeNearestClampSampler(context.device);
        vk::SamplerCreateInfo linearInfo;
        linearInfo.setMagFilter(vk::Filter::eLinear);
        linearInfo.setMinFilter(vk::Filter::eLinear);
        linearInfo.setMipmapMode(vk::SamplerMipmapMode::eNearest);
        linearInfo.setAddressModeU(vk::SamplerAddressMode::eClampToEdge);
        linearInfo.setAddressModeV(vk::SamplerAddressMode::eClampToEdge);
        linearInfo.setAddressModeW(vk::SamplerAddressMode::eClampToEdge);
        composeSampler = std::make_unique<vk::raii::Sampler>(context.device, linearInfo);

        lightStorage = createMappedHostVisibleBuffer(
            context.device, context.physicalDevice, sizeof(GpuLightUniform), vk::BufferUsageFlagBits::eUniformBuffer
        );
        // The buffer is fresh: the next updateLightUniform must write.
        uploadedLightUniformValid = false;

        // ---- Descriptor set layouts (same shape as screen-space, minus the
        // probe anchor field binding - world-space probes are deterministic).
        {
            // Shared set 0: camera UBO, primitives SSBO, light UBO, materials
            // SSBO, albedo/data texture arrays, environment UBO, cascade
            // direction table SSBO (binding 7, cascade_common.glsl).
            std::array<vk::DescriptorSetLayoutBinding, 8> bindings;
            bindings[0].setBinding(0).setDescriptorType(vk::DescriptorType::eUniformBuffer).setDescriptorCount(1);
            bindings[0].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[1].setBinding(1).setDescriptorType(vk::DescriptorType::eStorageBuffer).setDescriptorCount(1);
            bindings[1].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[2].setBinding(2).setDescriptorType(vk::DescriptorType::eUniformBuffer).setDescriptorCount(1);
            bindings[2].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[3].setBinding(3).setDescriptorType(vk::DescriptorType::eStorageBuffer).setDescriptorCount(1);
            bindings[3].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[4]
                .setBinding(4)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                .setDescriptorCount(1);
            bindings[4].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[5]
                .setBinding(5)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                .setDescriptorCount(1);
            bindings[5].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[6].setBinding(6).setDescriptorType(vk::DescriptorType::eUniformBuffer).setDescriptorCount(1);
            bindings[6].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[7].setBinding(7).setDescriptorType(vk::DescriptorType::eStorageBuffer).setDescriptorCount(1);
            bindings[7].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            vk::DescriptorSetLayoutCreateInfo setInfo;
            setInfo.setBindings(bindings);
            sharedSetLayout = std::make_unique<vk::raii::DescriptorSetLayout>(context.device, setInfo);
        }
        // March role set: depth, normal (sampled - kept for layout parity
        // with the screen-space gather descriptor set), the cascade
        // storage image (written), the previous bounce's gather output
        // (sampled, gated by the push flag). 4 bindings (no probe anchor
        // field - world anchors are deterministic). Binding order matches
        // rc_world_march.comp: 0=depth, 1=normal, 2=cascadeMarch (storage),
        // 3=prevGi (sampled).
        {
            std::array<vk::DescriptorSetLayoutBinding, 4> bindings;
            bindings[0]
                .setBinding(0)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                .setDescriptorCount(1);
            bindings[0].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[1]
                .setBinding(1)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                .setDescriptorCount(1);
            bindings[1].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[2].setBinding(2).setDescriptorType(vk::DescriptorType::eStorageImage).setDescriptorCount(1);
            bindings[2].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[3]
                .setBinding(3)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                .setDescriptorCount(1);
            bindings[3].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            vk::DescriptorSetLayoutCreateInfo setInfo;
            setInfo.setBindings(bindings);
            marchSetLayout = std::make_unique<vk::raii::DescriptorSetLayout>(context.device, setInfo);
        }
        {
            std::array<vk::DescriptorSetLayoutBinding, 3> bindings;
            bindings[0]
                .setBinding(0)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                .setDescriptorCount(1);
            bindings[0].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[1].setBinding(1).setDescriptorType(vk::DescriptorType::eStorageImage).setDescriptorCount(1);
            bindings[1].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            bindings[2].setBinding(2).setDescriptorType(vk::DescriptorType::eStorageImage).setDescriptorCount(1);
            bindings[2].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            vk::DescriptorSetLayoutCreateInfo setInfo;
            setInfo.setBindings(bindings);
            composeSetLayout = std::make_unique<vk::raii::DescriptorSetLayout>(context.device, setInfo);
        }
        {
            // Spatial filter: same layout as screen-space (giSampler,
            // depthSampler, normalSampler, output storage image). The denoise
            // shader is SHARED.
            std::array<vk::DescriptorSetLayoutBinding, 4> bindings;
            for (std::uint32_t binding = 0; binding < 3; ++binding)
            {
                bindings[binding]
                    .setBinding(binding)
                    .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                    .setDescriptorCount(1);
                bindings[binding].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            }
            bindings[3].setBinding(3).setDescriptorType(vk::DescriptorType::eStorageImage).setDescriptorCount(1);
            bindings[3].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            vk::DescriptorSetLayoutCreateInfo setInfo;
            setInfo.setBindings(bindings);
            filterSetLayout = std::make_unique<vk::raii::DescriptorSetLayout>(context.device, setInfo);
        }
        {
            // Gather: 8 cascade samplers + depth + normal + output. 11
            // bindings (vs screen-space's 12 - no probe anchor field).
            std::array<vk::DescriptorSetLayoutBinding, 11> bindings;
            for (std::uint32_t binding = 0; binding < 10; ++binding)
            {
                bindings[binding]
                    .setBinding(binding)
                    .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                    .setDescriptorCount(1);
                bindings[binding].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            }
            bindings[10].setBinding(10).setDescriptorType(vk::DescriptorType::eStorageImage).setDescriptorCount(1);
            bindings[10].setStageFlags(vk::ShaderStageFlagBits::eCompute);
            vk::DescriptorSetLayoutCreateInfo setInfo;
            setInfo.setBindings(bindings);
            gatherSetLayout = std::make_unique<vk::raii::DescriptorSetLayout>(context.device, setInfo);
        }

        // ---- Pipelines (using the rc_world_*.comp shaders for march/compose/
        // gather; the SHARED rc_denoise.comp for the filter stage).
        auto makeComputePipeline = [&context](
                                       const std::vector<char> &code,
                                       const vk::raii::DescriptorSetLayout &sharedLayout,
                                       const vk::raii::DescriptorSetLayout &roleLayout,
                                       std::uint32_t pushSize
                                   ) {
            vk::PushConstantRange pushRange;
            pushRange.setStageFlags(vk::ShaderStageFlagBits::eCompute);
            pushRange.setOffset(0);
            pushRange.setSize(pushSize);
            vk::PipelineLayoutCreateInfo layoutInfo;
            const std::array<vk::DescriptorSetLayout, 2> layouts{*sharedLayout, *roleLayout};
            layoutInfo.setSetLayouts(layouts);
            layoutInfo.setPushConstantRanges(pushRange);
            auto layout = std::make_unique<vk::raii::PipelineLayout>(context.device, layoutInfo);

            vk::ShaderModuleCreateInfo moduleInfo;
            moduleInfo.setCodeSize(code.size()).setPCode(reinterpret_cast<const std::uint32_t *>(code.data()));
            vk::raii::ShaderModule module(context.device, moduleInfo);
            vk::PipelineShaderStageCreateInfo stage;
            stage.setStage(vk::ShaderStageFlagBits::eCompute).setModule(*module).setPName("main");
            vk::ComputePipelineCreateInfo pipelineInfo;
            pipelineInfo.setStage(stage);
            pipelineInfo.setLayout(**layout);
            // The renderer's device-wide pipeline cache (NVIDIA's "Use
            // pipeline cache" guidance), threaded through FrameRenderContext.
            auto pipeline = std::make_unique<vk::raii::Pipeline>(context.device, *context.pipelineCache, pipelineInfo);
            struct Built
            {
                std::unique_ptr<vk::raii::PipelineLayout> layout;
                std::unique_ptr<vk::raii::Pipeline> pipeline;
            };
            return Built{.layout = std::move(layout), .pipeline = std::move(pipeline)};
        };

        auto marchBuilt = makeComputePipeline(
            readShaderBytes(context.shaderDirectory / "rc_world_march.comp.spv", moduleContext),
            *sharedSetLayout,
            *marchSetLayout,
            sizeof(WorldMarchPush)
        );
        marchPipelineLayout = std::move(marchBuilt.layout);
        marchPipeline       = std::move(marchBuilt.pipeline);
        auto composeBuilt   = makeComputePipeline(
            readShaderBytes(context.shaderDirectory / "rc_world_compose.comp.spv", moduleContext),
            *sharedSetLayout,
            *composeSetLayout,
            sizeof(WorldComposePush)
        );
        composePipelineLayout = std::move(composeBuilt.layout);
        composePipeline       = std::move(composeBuilt.pipeline);
        auto gatherBuilt      = makeComputePipeline(
            readShaderBytes(context.shaderDirectory / "rc_world_gather.comp.spv", moduleContext),
            *sharedSetLayout,
            *gatherSetLayout,
            sizeof(WorldGatherPush)
        );
        gatherPipelineLayout = std::move(gatherBuilt.layout);
        gatherPipeline       = std::move(gatherBuilt.pipeline);
        // Denoise: SHARED rc_denoise.comp shader (operates on the gather
        // output image, which is screen-space regardless of where the probes
        // lived). Push block: 32 bytes (FilterPush).
        auto denoiseBuilt = makeComputePipeline(
            readShaderBytes(context.shaderDirectory / "rc_denoise.comp.spv", moduleContext),
            *sharedSetLayout,
            *filterSetLayout,
            sizeof(FilterPush)
        );
        denoisePipelineLayout = std::move(denoiseBuilt.layout);
        denoisePipeline       = std::move(denoiseBuilt.pipeline);

        // ---- Descriptor sets (same wiring pattern as screen-space, minus
        // the probe anchor field binding for the march role set).
        {
            std::array<vk::DescriptorPoolSize, 3> poolSizes{
                vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, 3),
                vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 3),
                vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 2)
            };
            vk::DescriptorPoolCreateInfo poolInfo;
            poolInfo.setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet);
            poolInfo.setMaxSets(1);
            poolInfo.setPoolSizes(poolSizes);
            sharedPool = std::make_unique<vk::raii::DescriptorPool>(context.device, poolInfo);
            vk::DescriptorSetAllocateInfo allocate;
            allocate.setDescriptorPool(**sharedPool);
            allocate.setSetLayouts(**sharedSetLayout);
            sharedSet = std::make_unique<vk::raii::DescriptorSet>(
                std::move(vk::raii::DescriptorSets(context.device, allocate).front())
            );

            vk::DescriptorBufferInfo cameraInfo(context.cameraBuffer, 0, sizeof(CameraData));
            vk::DescriptorBufferInfo primitivesInfo(context.primitivesBuffer, 0, vk::WholeSize);
            vk::DescriptorBufferInfo lightInfo(**lightStorage.buffer, 0, sizeof(GpuLightUniform));
            vk::DescriptorBufferInfo materialsInfo(context.materialsBuffer, 0, vk::WholeSize);
            vk::DescriptorImageInfo albedoArrayInfo(
                **nearestSampler, context.albedoArrayView, vk::ImageLayout::eShaderReadOnlyOptimal
            );
            vk::DescriptorImageInfo dataArrayInfo(
                **nearestSampler, context.dataArrayView, vk::ImageLayout::eShaderReadOnlyOptimal
            );
            vk::DescriptorBufferInfo environmentInfo(context.environmentBuffer, 0, vk::WholeSize);
            vk::DescriptorBufferInfo directionsInfo(**directionStorage.buffer, 0, vk::WholeSize);
            std::array<vk::WriteDescriptorSet, 8> writes;
            writes[0].setDstSet(**sharedSet).setDstBinding(0).setDescriptorType(vk::DescriptorType::eUniformBuffer);
            writes[0].setBufferInfo(cameraInfo);
            writes[1].setDstSet(**sharedSet).setDstBinding(1).setDescriptorType(vk::DescriptorType::eStorageBuffer);
            writes[1].setBufferInfo(primitivesInfo);
            writes[2].setDstSet(**sharedSet).setDstBinding(2).setDescriptorType(vk::DescriptorType::eUniformBuffer);
            writes[2].setBufferInfo(lightInfo);
            writes[3].setDstSet(**sharedSet).setDstBinding(3).setDescriptorType(vk::DescriptorType::eStorageBuffer);
            writes[3].setBufferInfo(materialsInfo);
            writes[4]
                .setDstSet(**sharedSet)
                .setDstBinding(4)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            writes[4].setImageInfo(albedoArrayInfo);
            writes[5]
                .setDstSet(**sharedSet)
                .setDstBinding(5)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            writes[5].setImageInfo(dataArrayInfo);
            writes[6].setDstSet(**sharedSet).setDstBinding(6).setDescriptorType(vk::DescriptorType::eUniformBuffer);
            writes[6].setBufferInfo(environmentInfo);
            writes[7].setDstSet(**sharedSet).setDstBinding(7).setDescriptorType(vk::DescriptorType::eStorageBuffer);
            writes[7].setBufferInfo(directionsInfo);
            context.device.updateDescriptorSets(writes, {});
        }

        const vk::ImageView dummyView = *skyImage.view;
        marchSets.resize(cascades);
        {
            std::array<vk::DescriptorPoolSize, 2> poolSizes{
                vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, cascades * 3),
                vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, cascades)
            };
            vk::DescriptorPoolCreateInfo poolInfo;
            poolInfo.setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet);
            poolInfo.setMaxSets(cascades);
            poolInfo.setPoolSizes(poolSizes);
            marchPool = std::make_unique<vk::raii::DescriptorPool>(context.device, poolInfo);
            for (std::uint32_t cascade = 0; cascade < cascades; ++cascade)
            {
                vk::DescriptorSetAllocateInfo allocate;
                allocate.setDescriptorPool(**marchPool);
                allocate.setSetLayouts(**marchSetLayout);
                marchSets[cascade] = std::make_unique<vk::raii::DescriptorSet>(
                    std::move(vk::raii::DescriptorSets(context.device, allocate).front())
                );
                vk::DescriptorImageInfo depthInfo(**nearestSampler, context.depthView, vk::ImageLayout::eGeneral);
                vk::DescriptorImageInfo normalInfo(**nearestSampler, context.normalView, vk::ImageLayout::eGeneral);
                vk::DescriptorImageInfo outInfo(
                    **nearestSampler, *marchImages[cascade].view, vk::ImageLayout::eGeneral
                );
                vk::DescriptorImageInfo bounceSourceInfo(**nearestSampler, *gatherTemp.view, vk::ImageLayout::eGeneral);
                std::array<vk::WriteDescriptorSet, 4> writes;
                writes[0]
                    .setDstSet(**marchSets[cascade])
                    .setDstBinding(0)
                    .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
                writes[0].setImageInfo(depthInfo);
                writes[1]
                    .setDstSet(**marchSets[cascade])
                    .setDstBinding(1)
                    .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
                writes[1].setImageInfo(normalInfo);
                // Binding 2 is the cascadeMarch STORAGE image (the shader
                // writes the marched field here); binding 3 is prevGi (the
                // previous bounce's gathered transport, sampled).
                writes[2]
                    .setDstSet(**marchSets[cascade])
                    .setDstBinding(2)
                    .setDescriptorType(vk::DescriptorType::eStorageImage);
                writes[2].setImageInfo(outInfo);
                writes[3]
                    .setDstSet(**marchSets[cascade])
                    .setDstBinding(3)
                    .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
                writes[3].setImageInfo(bounceSourceInfo);
                context.device.updateDescriptorSets(writes, {});
            }
        }

        composeSets.resize(cascades);
        {
            std::array<vk::DescriptorPoolSize, 2> poolSizes{
                vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, cascades),
                vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, cascades * 2)
            };
            vk::DescriptorPoolCreateInfo poolInfo;
            poolInfo.setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet);
            poolInfo.setMaxSets(cascades);
            poolInfo.setPoolSizes(poolSizes);
            composePool = std::make_unique<vk::raii::DescriptorPool>(context.device, poolInfo);
            for (std::uint32_t cascade = 0; cascade < cascades; ++cascade)
            {
                vk::DescriptorSetAllocateInfo allocate;
                allocate.setDescriptorPool(**composePool);
                allocate.setSetLayouts(**composeSetLayout);
                composeSets[cascade] = std::make_unique<vk::raii::DescriptorSet>(
                    std::move(vk::raii::DescriptorSets(context.device, allocate).front())
                );
                const vk::ImageView coarseView = cascade + 1 < cascades ? *fullImages[cascade + 1].view : dummyView;
                vk::DescriptorImageInfo coarseInfo(**composeSampler, coarseView, vk::ImageLayout::eGeneral);
                vk::DescriptorImageInfo outInfo(**nearestSampler, *fullImages[cascade].view, vk::ImageLayout::eGeneral);
                vk::DescriptorImageInfo marchInfo(
                    **nearestSampler, *marchImages[cascade].view, vk::ImageLayout::eGeneral
                );
                std::array<vk::WriteDescriptorSet, 3> writes;
                writes[0]
                    .setDstSet(**composeSets[cascade])
                    .setDstBinding(0)
                    .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
                writes[0].setImageInfo(coarseInfo);
                writes[1]
                    .setDstSet(**composeSets[cascade])
                    .setDstBinding(1)
                    .setDescriptorType(vk::DescriptorType::eStorageImage);
                writes[1].setImageInfo(outInfo);
                writes[2]
                    .setDstSet(**composeSets[cascade])
                    .setDstBinding(2)
                    .setDescriptorType(vk::DescriptorType::eStorageImage);
                writes[2].setImageInfo(marchInfo);
                context.device.updateDescriptorSets(writes, {});
            }
        }

        {
            std::array<vk::DescriptorPoolSize, 2> poolSizes{
                vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 10),
                vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, 1)
            };
            vk::DescriptorPoolCreateInfo poolInfo;
            poolInfo.setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet);
            poolInfo.setMaxSets(1);
            poolInfo.setPoolSizes(poolSizes);
            gatherPool = std::make_unique<vk::raii::DescriptorPool>(context.device, poolInfo);
            vk::DescriptorSetAllocateInfo allocate;
            allocate.setDescriptorPool(**gatherPool);
            allocate.setSetLayouts(**gatherSetLayout);
            gatherSet = std::make_unique<vk::raii::DescriptorSet>(
                std::move(vk::raii::DescriptorSets(context.device, allocate).front())
            );
            std::array<vk::WriteDescriptorSet, 11> writes;
            std::array<vk::DescriptorImageInfo, 8> cascadeInfos;
            for (std::uint32_t cascade = 0; cascade < 8; ++cascade)
            {
                const vk::ImageView cascadeView = cascade < cascades ? *fullImages[cascade].view : *skyImage.view;
                cascadeInfos[cascade]
                    = vk::DescriptorImageInfo(**nearestSampler, cascadeView, vk::ImageLayout::eGeneral);
                writes[cascade]
                    .setDstSet(**gatherSet)
                    .setDstBinding(cascade)
                    .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
                writes[cascade].setImageInfo(cascadeInfos[cascade]);
            }
            vk::DescriptorImageInfo depthInfo(**nearestSampler, context.depthView, vk::ImageLayout::eGeneral);
            vk::DescriptorImageInfo normalInfo(**nearestSampler, context.normalView, vk::ImageLayout::eGeneral);
            vk::DescriptorImageInfo outInfo(**nearestSampler, *gatherTemp.view, vk::ImageLayout::eGeneral);
            writes[8]
                .setDstSet(**gatherSet)
                .setDstBinding(8)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            writes[8].setImageInfo(depthInfo);
            writes[9]
                .setDstSet(**gatherSet)
                .setDstBinding(9)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            writes[9].setImageInfo(normalInfo);
            writes[10].setDstSet(**gatherSet).setDstBinding(10).setDescriptorType(vk::DescriptorType::eStorageImage);
            writes[10].setImageInfo(outInfo);
            context.device.updateDescriptorSets(writes, {});
        }

        {
            std::array<vk::DescriptorPoolSize, 2> poolSizes{
                vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 3),
                vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, 1)
            };
            vk::DescriptorPoolCreateInfo poolInfo;
            poolInfo.setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet);
            poolInfo.setMaxSets(1);
            poolInfo.setPoolSizes(poolSizes);
            denoisePool = std::make_unique<vk::raii::DescriptorPool>(context.device, poolInfo);
            vk::DescriptorSetAllocateInfo allocate;
            allocate.setDescriptorPool(**denoisePool);
            allocate.setSetLayouts(**filterSetLayout);
            denoiseSet = std::make_unique<vk::raii::DescriptorSet>(
                std::move(vk::raii::DescriptorSets(context.device, allocate).front())
            );
            vk::DescriptorImageInfo giInfo(**nearestSampler, *gatherTemp.view, vk::ImageLayout::eGeneral);
            vk::DescriptorImageInfo depthInfo(**nearestSampler, context.depthView, vk::ImageLayout::eGeneral);
            vk::DescriptorImageInfo normalInfo(**nearestSampler, context.normalView, vk::ImageLayout::eGeneral);
            vk::DescriptorImageInfo outInfo(**nearestSampler, context.indirectRadianceView, vk::ImageLayout::eGeneral);
            std::array<vk::WriteDescriptorSet, 4> writes;
            writes[0]
                .setDstSet(**denoiseSet)
                .setDstBinding(0)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            writes[0].setImageInfo(giInfo);
            writes[1]
                .setDstSet(**denoiseSet)
                .setDstBinding(1)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            writes[1].setImageInfo(depthInfo);
            writes[2]
                .setDstSet(**denoiseSet)
                .setDstBinding(2)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler);
            writes[2].setImageInfo(normalInfo);
            writes[3].setDstSet(**denoiseSet).setDstBinding(3).setDescriptorType(vk::DescriptorType::eStorageImage);
            writes[3].setImageInfo(outInfo);
            context.device.updateDescriptorSets(writes, {});
        }

        cachedExtent  = context.extent;
        needsRecreate = false;
    }

    Settings settings;
    LightingCapability *lighting = nullptr;

    std::uint32_t directionCount = 0;
    vk::Extent2D cachedExtent{};
    bool needsRecreate = false;
    std::array<float, 3> cameraPositionCache{0.0f, 0.0f, 0.0f};

    struct WorldCascadeGeometry
    {
        std::array<float, 3> origin = {0.0f, 0.0f, 0.0f};
        float cellSize              = 1.0f;
        float intervalStart         = 0.0f;
        float intervalEnd           = 0.0f;
    };
    std::vector<WorldCascadeGeometry> worldGeometry;
    std::uint32_t marchDispatchZ  = 0;
    std::uint32_t gatherDispatchX = 0;
    std::uint32_t gatherDispatchY = 0;

    std::vector<CascadeImage> marchImages;
    std::vector<CascadeImage> fullImages;
    CascadeImage gatherTemp;
    CascadeImage skyImage;

    std::unique_ptr<vk::raii::Sampler> nearestSampler;
    std::unique_ptr<vk::raii::Sampler> composeSampler;
    // Last uniform bytes written into lightStorage (the change gate in
    // updateLightUniform); reset by rebuild so a fresh buffer always
    // receives the first write.
    GpuLightUniform uploadedLightUniform{};
    bool uploadedLightUniformValid = false;
    MappedHostVisibleBuffer lightStorage;
    MappedHostVisibleBuffer directionStorage;

    std::unique_ptr<vk::raii::DescriptorSetLayout> sharedSetLayout;
    std::unique_ptr<vk::raii::DescriptorSetLayout> marchSetLayout;
    std::unique_ptr<vk::raii::DescriptorSetLayout> composeSetLayout;
    std::unique_ptr<vk::raii::DescriptorSetLayout> gatherSetLayout;
    std::unique_ptr<vk::raii::DescriptorSetLayout> filterSetLayout;
    std::unique_ptr<vk::raii::DescriptorPool> sharedPool;
    std::unique_ptr<vk::raii::DescriptorSet> sharedSet;
    std::unique_ptr<vk::raii::DescriptorPool> marchPool;
    std::vector<std::unique_ptr<vk::raii::DescriptorSet>> marchSets;
    std::unique_ptr<vk::raii::DescriptorPool> composePool;
    std::vector<std::unique_ptr<vk::raii::DescriptorSet>> composeSets;
    std::unique_ptr<vk::raii::DescriptorPool> gatherPool;
    std::unique_ptr<vk::raii::DescriptorSet> gatherSet;

    std::unique_ptr<vk::raii::PipelineLayout> marchPipelineLayout;
    std::unique_ptr<vk::raii::Pipeline> marchPipeline;
    std::unique_ptr<vk::raii::PipelineLayout> composePipelineLayout;
    std::unique_ptr<vk::raii::Pipeline> composePipeline;
    std::unique_ptr<vk::raii::PipelineLayout> gatherPipelineLayout;
    std::unique_ptr<vk::raii::Pipeline> gatherPipeline;
    std::unique_ptr<vk::raii::PipelineLayout> denoisePipelineLayout;
    std::unique_ptr<vk::raii::Pipeline> denoisePipeline;
    std::unique_ptr<vk::raii::DescriptorPool> denoisePool;
    std::unique_ptr<vk::raii::DescriptorSet> denoiseSet;

    std::vector<vk::ImageMemoryBarrier2> cascadeBarrierScratch;
    std::vector<vk::Image> cascadeImageScratch;
};

RadianceCascadesModule::RadianceCascadesModule()
{
    componentManifest = ComponentManifest{
        .name    = "radiance_cascades",
        .kind    = ComponentKind::Module,
        .version = "0.1.0",
        // One demand marker per probe placement: the run demands
        // screen_space_radiance_cascades or world_space_radiance_cascades when the
        // GI transport pass should attach to the renderer, and the demanded name
        // selects the placement (the markers carry no call interface - the
        // implementation behind either is the technique pass).
        .providedCapabilities = {"screen_space_radiance_cascades", "world_space_radiance_cascades"},
        .requiredCapabilities = {"logging", "renderer"},
        // "lighting" is OPTIONAL (the first manifest use of the mechanism): the
        // pass relights march hits with the scene lights when a provider is
        // active and degrades to emissive + sky transport without one. Declaring
        // it through optionalCapabilities gives the module everything the old
        // hand-rolled pattern (an explicit capability.lighting permission plus a
        // try/catch around the acquire) could not: the resolver orders this
        // module after the lighting provider when one exists - construct's
        // acquire below is a property of the assembly, never of plugin scan
        // order - and the capability.<name> permission is derived from the
        // declaration. Optional capabilities never join the demand closure, so
        // this declaration does not pull a light provider into runs that have no
        // other use for one.
        .optionalCapabilities = {"lighting"},
        .requiredComponents   = {},
        .requiredPermissions  = {"capability.logging", "capability.renderer"},
        // The pass settings the module reads from [lighting.radiance_cascades].
        // The screen-space-specific keys (probe_stride) and the world-space-
        // specific keys (world_cell_size, world_grid_w/h/d) are both declared
        // so the unknown-key warnings stay quiet for either placement's
        // overrides - the demanded placement capability selects which keys
        // actually apply.
        .knownConfigKeys = {
            "lighting.radiance_cascades.cascades",
            "lighting.radiance_cascades.interval_base",
            "lighting.radiance_cascades.interval_factor",
            "lighting.radiance_cascades.probe_stride",
            "lighting.radiance_cascades.base_rays",
            "lighting.radiance_cascades.mapping",
            "lighting.radiance_cascades.bounces",
            "lighting.radiance_cascades.gather_visibility",
            "lighting.radiance_cascades.los_steps",
            "lighting.radiance_cascades.march_steps",
            "lighting.radiance_cascades.relight_shadows",
            "lighting.radiance_cascades.shadow_steps",
            "lighting.radiance_cascades.spatial_filter",
            "lighting.radiance_cascades.world_cell_size",
            "lighting.radiance_cascades.world_grid_w",
            "lighting.radiance_cascades.world_grid_h",
            "lighting.radiance_cascades.world_grid_d",
        },
    };
}

RadianceCascadesModule::~RadianceCascadesModule()
{
    stop();
}

void RadianceCascadesModule::construct(
    ComponentContext &context
)
{
    logger = &context.acquireCapability<LoggerCapability>();

    // Probe PLACEMENT follows DEMAND, not configuration: the module provides
    // one demand marker per placement, the run selects the placement by
    // demanding exactly one of them, and the demanded name is the placement
    // contract - there is no separate config key that could disagree with it.
    // The placement-specific settings (probe_stride for screen,
    // world_cell_size + grid extents for world) are gated by the resolved
    // placement; the shared settings below (cascades, intervals, base_rays,
    // mapping, bounces, gather_visibility, los_steps, march_steps,
    // relight_shadows, shadow_steps, spatial_filter) apply to both.
    const bool screenSpaceDemanded = context.isCapabilityDemanded("screen_space_radiance_cascades");
    const bool worldSpaceDemanded  = context.isCapabilityDemanded("world_space_radiance_cascades");
    if (screenSpaceDemanded && worldSpaceDemanded)
    {
        throw std::runtime_error(
            "radiance_cascades module: the run demands both radiance cascade placements "
            "('screen_space_radiance_cascades' and 'world_space_radiance_cascades'); the two are "
            "alternative transports for one indirect diffuse radiance layer and one module instance "
            "attaches one pass - demand at most one of them"
        );
    }
    const bool worldSpacePlacement = worldSpaceDemanded;
    configuredPlacement            = screenSpaceDemanded ? "screen" : (worldSpaceDemanded ? "world" : "none");

    // Angular mapping: octahedron (default), cubemap, or spherical_harmonic.
    // An unknown spelling is a precise startup error, not a silent fallback:
    // the mapping changes the GI's look, so a typo must never quietly render
    // with a different one. The mapping applies to BOTH placements - the
    // direction table builder and the gather's irradiance evaluation are
    // placement-agnostic.
    const std::string mappingName
        = context.configurationStringInSection("lighting", "radiance_cascades.mapping").value_or(defaultMappingName);
    CascadeAngularMapping mapping = CascadeAngularMapping::Octahedron;
    if (!parseMappingName(mappingName, mapping))
    {
        throw std::runtime_error(
            "radiance_cascades module: [lighting.radiance_cascades] mapping '" + mappingName
            + "' is not a known mapping (octahedron, cubemap, spherical_harmonic)"
        );
    }
    configuredMappingName = mappingName;

    // Shared settings: cascade count, interval schedule, base_rays, gather
    // visibility, march / shadow step budgets, spatial filter radius, bounce
    // count. These apply to both placements; the placement-specific settings
    // (probe_stride for screen, world_cell_size + grid_w/h/d for world) are
    // read inside the placement branches below.
    const std::uint32_t cascades = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        context.configurationIntegerInSection("lighting", "radiance_cascades.cascades").value_or(defaultCascades), 1, 8
    ));
    const float intervalBase
        = static_cast<float>(context.configurationFloatingInSection("lighting", "radiance_cascades.interval_base")
                .value_or(defaultIntervalBase));
    const float intervalFactor
        = static_cast<float>(context.configurationFloatingInSection("lighting", "radiance_cascades.interval_factor")
                .value_or(defaultIntervalFactor));
    if (intervalBase <= 0.0f || intervalFactor < 1.0f)
    {
        throw std::runtime_error("radiance_cascades module: interval_base must be > 0 and interval_factor >= 1");
    }
    const std::uint32_t baseRays = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        context.configurationIntegerInSection("lighting", "radiance_cascades.base_rays").value_or(defaultBaseRays),
        4,
        256
    ));
    const bool gatherVisibility
        = context.configurationBooleanInSection("lighting", "radiance_cascades.gather_visibility").value_or(true);
    const std::uint32_t losSteps   = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        context.configurationIntegerInSection("lighting", "radiance_cascades.los_steps").value_or(defaultLosSteps),
        4,
        256
    ));
    const std::uint32_t marchSteps = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        context.configurationIntegerInSection("lighting", "radiance_cascades.march_steps").value_or(defaultMarchSteps),
        4,
        512
    ));
    const bool relightShadows
        = context.configurationBooleanInSection("lighting", "radiance_cascades.relight_shadows").value_or(true);
    const std::uint32_t shadowSteps = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        context.configurationIntegerInSection("lighting", "radiance_cascades.shadow_steps")
            .value_or(defaultShadowSteps),
        4,
        256
    ));
    // Filter radius clamp follows the shader's (rc_denoise.comp clamps 0..16):
    // the shipped demo asks for 10 (one probe cell spans 8 pixels at stride 8,
    // so the window must average ACROSS cells), and an 8-cap here silently
    // applied 8 for 10 - the same stale dense-probe-era limit the shader side
    // already shed. The filter operates on the gather output image which is
    // screen-space regardless of placement, so the radius applies to both.
    const std::uint32_t spatialFilterRadius = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        context.configurationIntegerInSection("lighting", "radiance_cascades.spatial_filter")
            .value_or(defaultSpatialFilterRadius),
        0,
        16
    ));
    // Light transport bounces: 1 reproduces the historical single bounce;
    // every extra bounce re-runs the cascade sequence (cost linear in
    // bounces). There is NO upper bound by design: the bounce count is a
    // quality/performance trade the configuration owns. The only validation
    // is the structural floor - 0 bounces would disable the transport this
    // pass exists for, and negative counts are a typo.
    const std::int64_t configuredBouncesValue
        = context.configurationIntegerInSection("lighting", "radiance_cascades.bounces").value_or(defaultBounces);
    if (configuredBouncesValue < 1)
    {
        throw std::runtime_error(
            "radiance_cascades module: [lighting.radiance_cascades] bounces must be >= 1 (0 would disable the "
            "light transport; drop the radiance cascade demand from the run instead of configuring the pass empty)"
        );
    }
    const std::uint32_t bounces = static_cast<std::uint32_t>(configuredBouncesValue);
    configuredBounces           = bounces;

    // The lighting capability is optional (declared in the manifest): the
    // resolver ordered this module after the provider when one is active, so
    // the probe below is a stable answer, never scan-order luck. A denied or
    // missing provider only disables the first-bounce relight.
    LightingCapability *lighting = nullptr;
    if (context.isCapabilityAvailable(LightingCapability::capabilityName))
    {
        lighting = &context.acquireCapability<LightingCapability>();
    }

    // Attachment follows demand: this module only activates when the run
    // demanded one of the placement markers it provides, so an active module
    // always attaches (the defensive flag keeps the start log honest if a
    // future assembly change ever activates a module without demand). Asking
    // about one's OWN capabilities is the one question the demand set answers.
    passAttached      = screenSpaceDemanded || worldSpaceDemanded;
    lightingAvailable = lighting != nullptr;

    // Placement dispatch: build the per-placement Settings and create the
    // matching pass. The two pass classes share the algorithm's structure
    // (march / compose / gather / denoise) but differ in storage layout,
    // probe anchoring, and shader set. Both expose the same LightingTarget
    // (IndirectDiffuse); the demanded marker names the placement the pass
    // implements.
    if (worldSpacePlacement)
    {
        WorldSpaceRadianceCascadesPass::Settings settings;
        settings.cascades       = cascades;
        settings.intervalBase   = intervalBase;
        settings.intervalFactor = intervalFactor;
        // World-space grid: cell size grows 2x per cascade. The grid
        // dimensions stay constant per cascade (the coarser grid covers a
        // 2x larger world extent at 2x the cell stride). Defaults: 8x8x4
        // cells per cascade - small enough that gridW * gridH * gridD
        // directionCount stays under Vulkan's maxImageArrayLayers (>= 256)
        // for direction counts up to 256 (octahedron 16*16 = 256).
        settings.worldCellSize = static_cast<float>(
            context.configurationFloatingInSection("lighting", "radiance_cascades.world_cell_size").value_or(1.0)
        );
        if (settings.worldCellSize <= 0.0f)
        {
            throw std::runtime_error(
                "radiance_cascades module: [lighting.radiance_cascades] world_cell_size must be > 0"
            );
        }
        settings.gridW               = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            context.configurationIntegerInSection("lighting", "radiance_cascades.world_grid_w").value_or(8), 1, 64
        ));
        settings.gridH               = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            context.configurationIntegerInSection("lighting", "radiance_cascades.world_grid_h").value_or(8), 1, 64
        ));
        settings.gridD               = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            context.configurationIntegerInSection("lighting", "radiance_cascades.world_grid_d").value_or(4), 1, 64
        ));
        settings.baseRays            = baseRays;
        settings.gatherVisibility    = gatherVisibility;
        settings.losSteps            = losSteps;
        settings.marchSteps          = marchSteps;
        settings.relightShadows      = relightShadows;
        settings.shadowSteps         = shadowSteps;
        settings.spatialFilterRadius = spatialFilterRadius;
        settings.mapping             = mapping;
        settings.bounces             = bounces;
        pass                         = std::make_shared<WorldSpaceRadianceCascadesPass>(settings, lighting);
    } else
    {
        ScreenSpaceRadianceCascadesPass::Settings settings;
        settings.cascades            = cascades;
        settings.intervalBase        = intervalBase;
        settings.intervalFactor      = intervalFactor;
        settings.probeStride         = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            context.configurationIntegerInSection("lighting", "radiance_cascades.probe_stride")
                .value_or(defaultProbeStride),
            1,
            64
        ));
        settings.baseRays            = baseRays;
        settings.gatherVisibility    = gatherVisibility;
        settings.losSteps            = losSteps;
        settings.marchSteps          = marchSteps;
        settings.relightShadows      = relightShadows;
        settings.shadowSteps         = shadowSteps;
        settings.spatialFilterRadius = spatialFilterRadius;
        settings.mapping             = mapping;
        settings.bounces             = bounces;
        pass                         = std::make_shared<ScreenSpaceRadianceCascadesPass>(settings, lighting);
    }
    if (passAttached)
    {
        RendererCapability &rendererCapability = context.acquireCapability<RendererCapability>();
        auto *rendererModule                   = static_cast<RendererModule *>(&rendererCapability);
        rendererModule->attachLightingTechnique(pass);
    }
}

void RadianceCascadesModule::start()
{
    logger->logMessage(
        LogLevel::Information,
        std::string("radiance_cascades module started; technique ")
            + (passAttached ? "attached (indirect radiance)" : "no placement demanded; idle") + ", placement "
            + configuredPlacement + ", light capability "
            + (lightingAvailable ? "present" : "absent (no first-bounce relight)") + ", mapping "
            + configuredMappingName + ", bounces " + std::to_string(configuredBounces)
    );
}

void RadianceCascadesModule::stop()
{
    passAttached = false;
    pass.reset();
    logger = nullptr;
}

void *RadianceCascadesModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    // The placement markers are demand markers: they carry no call interface,
    // so the implementation behind either is the module object itself. The
    // kernel type-erases the pointer; whoever demands a marker consumes the
    // work the module attaches to the frame, not a cast.
    if (capabilityName == "screen_space_radiance_cascades" || capabilityName == "world_space_radiance_cascades")
    {
        return this;
    }
    return nullptr;
}

} // namespace engine
