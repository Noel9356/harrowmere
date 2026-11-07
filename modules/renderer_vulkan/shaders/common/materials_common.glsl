// Shared material access for the geometry pass and the SDF tracing passes.
//
// One texture array per material map kind: layer i belongs to texture set i
// (the renderer uploads sets in the order their names first appear in the
// scene). Per-primitive material indices live in the material table
// (binding 3), one entry per scene primitive in the same order - the
// primitives buffer (scene_common.glsl, binding 1) and this table are the
// same length. A negative layer index means "no texture": the pass falls
// back to the flat values the primitive itself carries.
//
// Data map packing (one RGBA8 layer per set, assembled by the renderer):
//   R roughness, G metallic, B/A tangent-space normal XY (OpenGL +X/+Y).
// The normal Z is reconstructed in the shader.

#ifndef ENGINE_MATERIALS_COMMON_GLSL
#define ENGINE_MATERIALS_COMMON_GLSL

#include "scene_common.glsl"

struct GpuMaterial
{
    ivec4 layers;       // x albedo array layer, y data array layer (-1 = none)
    vec4 scaleAndFlags; // x uv tiling scale, y unused, z unused, w unused
};

layout(std430, set = 0, binding = 3) readonly buffer MaterialBuffer
{
    GpuMaterial materials[];
};

layout(set = 0, binding = 4) uniform sampler2DArray albedoTextureArray; // sRGB decoded on sample
layout(set = 0, binding = 5) uniform sampler2DArray dataTextureArray;  // R rough, G metal, BA normal xy

// ---- anti-tiling variation (rev 23; strengthened rev 24) -------------------
// World-planar UVs tile one texture set every uvScale metres; across a 16 km
// patch the same tile prints thousands of times and the repetition reads as
// wallpaper (the owner's texture-repetition defect, 2026-11-05). Rev 23's
// single-octave drift (±0.7 tile at 96 m) still let the fine grain self-
// align at the meso scale - the owner read the repetition again on rev 23's
// captures. Rev 24 hardens the three layers:
//   * antiTileWarp - TWO drift octaves: a 96 m domain swing (up to ±1.2
//     tiles) plus a 13 m swing (±0.55 tile) that breaks the meso-scale
//     alignment the first octave left;
//   * antiTileTint / terrainTint - deeper macro+meso hue/luminance patches
//     (±22% / ±28% swings, wider hue pull);
//   * antiTileGrain - a 0.55 m fine value-noise luminance jitter (±7%) that
//     decorrelates the speckle itself, the layer the owner's eye was
//     tracking.
// The traced path (materialAt below) and the raster path (scene.frag) call
// the SAME functions at the SAME world point, so the relight-vs-raster
// delta the gates watch stays inside its band.
float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float vnoise2(vec2 p)
{
    const vec2 i = floor(p);
    const vec2 f = fract(p);
    const vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), u.x),
               mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), u.x), u.y);
}

float fbm3(vec2 p)
{
    return 0.5 * vnoise2(p) + 0.25 * vnoise2(p * 2.13 + 19.7) + 0.125 * vnoise2(p * 4.31 + 47.1);
}

// Domain drift in TILE space: uv arrives as world XZ / uvScale, so a drift
// of 3.4 tiles swings the sample point across several tile borders smoothly.
// Rev 25 drama: the far octave deepened 2.4 -> 3.4 tiles and the meso octave
// 1.1 -> 1.6 - the owner's second "repetition still exist" verdict (the rev
// 24 amplitudes left the meadow's fine speckle still self-aligning between
// neighbouring macro cells).
vec2 antiTileWarp(vec2 worldXZ, vec2 uv)
{
    const vec2 driftFar  = vec2(fbm3(worldXZ * (1.0 / 96.0) + 13.1), fbm3(worldXZ * (1.0 / 96.0) + 71.7)) - 0.5;
    const vec2 driftNear = vec2(vnoise2(worldXZ * (1.0 / 13.0) + 5.7), vnoise2(worldXZ * (1.0 / 13.0) + 47.9)) - 0.5;
    return uv + driftFar * 3.4 + driftNear * 1.6;
}

// ---- rev 26: per-sample ROTATION ------------------------------------------
// The warp octaves shift WHERE each tile prints, but every tile kept the
// texture's own axis orientation - the fine speckle still self-aligned
// across neighbouring cells (the owner's third repetition verdict, 2026-11-
// 06 captures). Two overlapping samples at DIFFERENT rotations and scales,
// blended by a 5.7 m noise mask, break the alignment for good: whatever
// repeat pattern survives in one sample is decorrelated by the other, and
// the mask hides the transition. Both sampling paths (materialAt here,
// scene.frag's raster branch) call the same helpers, so the relight-vs-
// raster delta the gates watch stays inside its band.
vec2 antiTileRot(vec2 uv, float sinA, float cosA)
{
    return vec2(uv.x * cosA - uv.y * sinA, uv.x * sinA + uv.y * cosA);
}

vec3 albedoAntiTile(int layer, vec2 uv, vec2 worldXZ)
{
    // Sample A: the warped uv as before. Sample B: scaled 1.37x, rotated
    // ~54 degrees, offset half a tile - never aligns with A.
    const vec2 uvA = uv;
    const vec2 uvB = antiTileRot(uv * 1.37 + vec2(0.5, 0.25), 0.809, 0.588);
    const vec3 sA  = textureLod(albedoTextureArray, vec3(uvA, float(layer)), 0.0).rgb;
    const vec3 sB  = textureLod(albedoTextureArray, vec3(uvB, float(layer)), 0.0).rgb;
    const float m  = smoothstep(0.32, 0.68, fbm3(worldXZ * (1.0 / 5.7) + 3.7));
    return mix(sA, sB, m);
}

vec4 dataAntiTile(int layer, vec2 uv, vec2 worldXZ)
{
    const vec2 uvA = uv;
    const vec2 uvB = antiTileRot(uv * 1.37 + vec2(0.5, 0.25), 0.809, 0.588);
    const vec4 sA  = textureLod(dataTextureArray, vec3(uvA, float(layer)), 0.0);
    const vec4 sB  = textureLod(dataTextureArray, vec3(uvB, float(layer)), 0.0);
    const float m  = smoothstep(0.32, 0.68, fbm3(worldXZ * (1.0 / 5.7) + 3.7));
    return mix(sA, sB, m);
}

// The 0.55 m fine grain: ±4.5% luminance jitter (rev 29: ±7% read as
// sensor static at capture distance - the speckle decorated every meadow
// with fixed noise), the layer that decorrelates
// the texture's own speckle between neighbouring tiles.
float antiTileGrain(vec2 worldXZ)
{
    return 0.955 + 0.09 * vnoise2(worldXZ * (1.0 / 0.55));
}

// Dry-vs-lush macro patches with a deeper luminance swing (rev 24: ±22%).
vec3 antiTileTint(vec2 worldXZ)
{
    const float macro = fbm3(worldXZ * (1.0 / 160.0));
    const float meso  = fbm3(worldXZ * (1.0 / 27.0) + 41.3);
    const vec3 dry    = vec3(1.16, 1.03, 0.80);
    const vec3 lush   = vec3(0.80, 0.97, 0.90);
    const vec3 tint   = mix(lush, dry, smoothstep(0.24, 0.76, macro * 0.65 + meso * 0.35));
    return tint * (0.86 + 0.20 * (macro - 0.5) + 0.14 * (meso - 0.5));
}

// The terrain's stronger swing (mesh objects whose bbox half extent exceeds
// 1 km - only the heightfield patch is that large). Rev 24 note: the first
// draft's dry end (1.24, 1.06, 0.72) bleached the whole meadow to straw in
// the coast capture - the macro field sits mid-range across most of the
// map, so the dry tint dominated. The dry end now stays inside a pale-olive
// pull and the smoothstep window keeps most of the land in the green half.
vec3 terrainTint(vec2 worldXZ)
{
    const float macro = fbm3(worldXZ * (1.0 / 190.0));
    const float meso  = fbm3(worldXZ * (1.0 / 31.0) + 41.3);
    // Rev 25 drama: both ends pull greener - the CC0 grass photo is already
    // yellow-olive (linear 0.63/0.47/0.21) and the old dry end (1.08, 1.01,
    // 0.90) pushed whole meadows to savanna. The patch now reads as a lush
    // temperate valley with dry patches, not a wheat field.
    // Rev 29 (the meadow grade): the lush end eases (0.74, 1.00, 0.74) ->
    // (0.80, 0.99, 0.79) - the pure green multiplier over the yellow-olive
    // photo keyed every sunlit field to a fluorescent yellow-green (the
    // spawn_day aerial), and the mean stays put (both ends bracket 1.0).
    const vec3 dry    = vec3(1.02, 1.00, 0.90);
    const vec3 lush   = vec3(0.80, 0.99, 0.79);
    return mix(lush, dry, smoothstep(0.30, 0.74, macro * 0.6 + meso * 0.4))
           * (0.88 + 0.20 * (macro - 0.5) + 0.14 * (meso - 0.5));
}

// ---- triplanar mapping for mesh objects (rev 24) ---------------------------
// Registered meshes (trees) previously sampled one world-PLANAR uv: on a
// vertical trunk both u and v derive from world XZ, so climbing the trunk
// changed neither coordinate - the whole column sampled ONE texture line
// (vertical stripes). True triplanar projects along the surface's dominant
// axis; the weights come from the world normal raised to the 4th power so
// the blend stays tight around the axis transitions.
struct TriplanarUv
{
    vec2 uvX;      // projection onto the YZ plane (for +/-X faces)
    vec2 uvY;      // projection onto the XZ plane (for +/-Y faces)
    vec2 uvZ;      // projection onto the XY plane (for +/-Z faces)
    vec3 weights;  // the blend weights (sum to 1)
};

TriplanarUv triplanarUv(vec3 worldPos, vec3 worldNormal, float uvScale)
{
    const vec3 n = normalize(worldNormal);
    const vec3 w = pow(abs(n), vec3(4.0));
    TriplanarUv tp;
    tp.weights  = w / max(w.x + w.y + w.z, 1e-5);
    tp.uvX      = antiTileWarp(worldPos.zy, worldPos.zy / uvScale);
    tp.uvY      = antiTileWarp(worldPos.xz, worldPos.xz / uvScale);
    tp.uvZ      = antiTileWarp(worldPos.xy, worldPos.xy / uvScale);
    return tp;
}

struct SurfaceMaterial
{
    vec3 albedo;
    float roughness;
    float metallic;
    bool hasNormalMap;
    vec2 normalXY;
};

// Material of one primitive at one point. primitiveIndex indexes BOTH the
// primitives and the material table; localPoint is the primitive-local
// position (already rotated by the primitive's rotationY).
SurfaceMaterial materialAt(uint primitiveIndex, vec3 localPoint)
{
    const GpuPrimitive primitive = primitives[primitiveIndex];
    const GpuMaterial material = materials[primitiveIndex];
    const float uvScale = max(material.scaleAndFlags.x, 1e-4);

    // rev 23: the world point drives the anti-tiling variation (the warp
    // and the macro tint key on world XZ, not the tile-local uv, so the
    // same spot traces and rasterizes identically).
    const vec2 worldXZ = (primitive.positionType.xyz + localPoint).xz;
    vec2 uv;
    if (primitive.positionType.w < 0.5)
    {
        uv = antiTileWarp(worldXZ, boxSurfaceUv(localPoint, primitive.sizeRotation.xyz, uvScale));
    }
    else
    {
        uv = antiTileWarp(worldXZ, sphereSurfaceUv(localPoint, primitive.sizeRotation.x, uvScale));
    }

    SurfaceMaterial result;
    result.albedo = primitive.albedoRoughness.rgb;
    result.roughness = primitive.albedoRoughness.a;
    result.metallic = primitive.metallicEmissiveStrength.x;
    result.hasNormalMap = false;
    result.normalXY = vec2(0.5);

#ifdef ENGINE_RELIGHT_FLAT_ALBEDO
    // The GI relight's compute path samples NO texture arrays: the capture
    // reference rasterizer (lavapipe 25.0.7) segfaults executing arrayed
    // texture fetches inside compute kernels (diagnosed 2026-11-06 with a
    // four-variant shader probe; unorm and srgb arrays crash alike, the
    // raster path handles the same views fine). The bounce albedo is the
    // primitive's flat palette tint scaled by the ALU-only anti-tiling
    // variation - a documented, bounded approximation: the direct pass keeps
    // the full photo texture, the bounce carries its palette hue. The GI
    // chamber (flat albedos by design) is EXACT under this path.
    // Albedo stays the primitive's flat palette color: the ALU anti-tiling
    // variation exists to break TEXTURE repetition, a flat color cannot
    // repeat, and tinting it printed the checker pattern the first chamber
    // capture showed. Roughness/metallic read the flat values when the set
    // carries a data layer.
    if (material.layers.y >= 0)
    {
        result.roughness = primitive.albedoRoughness.a;
        result.metallic  = primitive.metallicEmissiveStrength.x;
    }
#else
    if (material.layers.x >= 0)
    {
        result.albedo = albedoAntiTile(material.layers.x, uv, worldXZ)
                        * antiTileTint(worldXZ) * antiTileGrain(worldXZ);
    }
    if (material.layers.y >= 0)
    {
        const vec4 data = dataAntiTile(material.layers.y, uv, worldXZ);
        result.roughness = data.r;
        result.metallic = data.g;
        result.hasNormalMap = true;
        result.normalXY = data.ba;
    }
#endif
    return result;
}

// World normal perturbed by the material's normal map. Geometric fallback:
// the interpolated/geometric normal itself when the set has no normal map.
vec3 materialWorldNormal(const SurfaceMaterial material, vec3 geometricNormal, vec3 tangent, vec3 bitangent)
{
    if (!material.hasNormalMap)
    {
        return geometricNormal;
    }
    const vec2 xy = material.normalXY * 2.0 - vec2(1.0);
    const float z = sqrt(max(1.0 - dot(xy, xy), 0.0));
    return normalize(tangent * xy.x + bitangent * xy.y + geometricNormal * z);
}

#endif // ENGINE_MATERIALS_COMMON_GLSL
