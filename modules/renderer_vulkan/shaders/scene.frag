#version 450
#extension GL_GOOGLE_include_directive : require
// Geometry pass fragment shader: writes the G-buffer. Three attachments keep
// the formats minimal (memory rule): albedo+roughness in RGBA8, world normal
// + metallic in RGBA16F, emissive radiance in RGBA16F (HDR values would clip
// in the 8-bit attachment). The depth attachment is bound by the pipeline.
//
// Materials: when the object references a texture set, albedo/roughness/
// metallic/normal come from the texture arrays (materials_common.glsl);
// otherwise the flat configured values pass through. UVs and the tangent
// frame come from the shared parameterization, so a GI bounce or a
// reflection ray landing on the same point samples identical texels.


#include "scene_common.glsl"
#include "materials_common.glsl"

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec3 inLocalPosition;
layout(location = 3) in vec3 inLocalNormal;
// The model's axis scales from the vertex stage (the non-mesh half extents).
layout(location = 4) in vec3 inHalfExtents;

layout(location = 0) out vec4 outEmissive;     // RGBA16F
layout(location = 1) out vec4 outAlbedo;       // RGBA8_UNORM
layout(location = 2) out vec4 outNormal;       // RGBA16F

layout(push_constant) uniform ObjectPush
{
    layout(offset = 0) mat4 model;
    layout(offset = 64) vec4 albedoRoughness;          // rgb albedo, a roughness
    layout(offset = 80) vec4 metallicEmissiveStrength; // x metallic, y emissive strength
    layout(offset = 96) vec4 materialLayers;           // x albedo layer, y data layer (-1 none), z uv scale, w primitive type
    layout(offset = 112) vec4 meshBox;                 // mesh objects: the registered mesh's bbox half extents
} object;

void main()
{
    // The model's column lengths are the axis scales = the primitive's half
    // extents (and the sphere radius) - the exact numbers materialAt() reads
    // from the primitives buffer on the tracing side. Registered meshes arrive
    // with an identity-scale model (vertices are authored in meters): their
    // half extents ride in meshBox, the same numbers the tracing proxy box
    // carries, so both paths parameterize the surface identically. The
    // non-mesh extents arrive as a varying the vertex stage built once per
    // vertex (inHalfExtents) instead of three per-fragment sqrt calls.
    const uint primitiveType = uint(object.materialLayers.w + 0.5);
    const vec3 halfExtents = primitiveType == primitiveTypeMesh
                                 ? object.meshBox.xyz
                                 : inHalfExtents;
    const float uvScale = max(object.materialLayers.z, 1e-4);
    // floor(x + 0.5) rounds HALF AWAY FROM ZERO for negatives too: int()
    // truncates toward zero, which would turn the -1 sentinel into 0.
    const int albedoLayer = int(floor(object.materialLayers.x + 0.5));
    const int dataLayer = int(floor(object.materialLayers.y + 0.5));
    // Rev 25: the water plane rides a 1.0 in meshBox.w (ObjectDrawCache
    // waterFlag -> the geometry pass). The shader has no texture-set names,
    // so the flag is the only reliable water key - the layer ORDER of the
    // "water" set is an implementation detail of the runtime merge.
    const bool isWater = primitiveType == primitiveTypeMesh && object.meshBox.w > 0.5;

    SurfaceMaterial material;
    material.albedo = object.albedoRoughness.rgb;
    material.roughness = object.albedoRoughness.a;
    material.metallic = object.metallicEmissiveStrength.x;
    material.hasNormalMap = false;
    material.normalXY = vec2(0.5);

    if (albedoLayer >= 0 || dataLayer >= 0)
    {
        // Mesh UVs are TRIPLANAR (rev 24): the world-planar projection this
        // replaced derives both u and v from world XZ, so on any vertical
        // surface (a tree trunk) climbing the surface changed neither
        // coordinate - the whole column sampled one texture line, printed
        // as vertical stripes. True triplanar projects along the dominant
        // world axis per fragment; the heightfield's cliffs gain proper side
        // projection too. The anti-tiling warp keys on the same world point
        // every path shares. The tracing consumers skip mesh primitives (the
        // analytic SDF field carries boxes and spheres only), so no relight
        // path reproduces this mapping today.
        if (primitiveType == primitiveTypeMesh)
        {
            // ---- rev 25: the water branch -----------------------------------
            // The water plane previously took the TERRAIN branch (its bbox
            // half extents exceed 1 km) - it shaded with terrainTint and the
            // grass normal map, and read as a flat saturated sheet. Water
            // instead: dark body color, a static multi-octave ripple normal
            // (the direct pass's GGX specular turns the ripples into sun
            // glitter at low sun angles), and a roughness floor that keeps
            // the highlight tight.
            if (isWater)
            {
                const vec3 waterTex = albedoLayer >= 0
                                          ? texture(albedoTextureArray, vec3(inWorldPosition.xz / uvScale, float(albedoLayer))).rgb
                                          : vec3(1.0);
                // Deep body: the texture's own color pushed towards a dark
                // blue-green - a mirror-rough surface keeps most of its
                // radiance in the specular lobe, and a bright body under it
                // washed the highlight out (the coast capture's flat sheet).
                // Rev 27: the body lifts (0.02/0.09/0.10 -> 0.05/0.15/0.16,
                // the texture share 0.5 -> 0.72) - a camera looking DOWN at
                // water reads almost no Fresnel reflection (the nadir
                // reflectance of water is ~2-4%), so the old near-black
                // body was the whole answer and the day water read as an
                // ink sheet from the spawn aerial. The GGX lobe
                // (roughness 0.10) is albedo-independent, so the sun
                // glitter keeps its contrast.
                // Rev 28 (the owner's "the ground is too reflective"): the
                // low-sun glitter ran a broad pale streak across the whole
                // basin (the walk_frontier capture). Roughness 0.10 -> 0.28
                // narrows the lobe's reach, the ripple slope eases (the
                // streak's angular spread) and the body pulls bluer - the
                // water reads as a river dimpling in the light, not a
                // mirror path.
                material.albedo = mix(vec3(0.04, 0.10, 0.13), waterTex * 0.72, 0.40);
                material.roughness = 0.28;
                material.metallic = 0.0;
                material.hasNormalMap = false;
            }
            else
            {
            const TriplanarUv tp = triplanarUv(inWorldPosition, normalize(inWorldNormal), uvScale);
            // The heightfield is the ONLY mesh whose bbox half extent exceeds
            // 1 km (rev 24: the old second clause - model y > 10 - never
            // fired, because the heightfield object sits at the origin; the
            // terrain then fell through to the prop path, which shades with
            // the geometric normal and DROPPED the grass normal map - the
            // flat-normal sheen washed every capture pale).
            const bool terrain = max(max(object.meshBox.x, object.meshBox.y), object.meshBox.z) > 1000.0;
            // rev 23: the macro/meso tint breaks the repeated-tile read; the
            // heightfield gets the stronger dry-vs-lush swing, every prop the
            // subtle one. The fine grain decorrelates the speckle itself.
            const vec3 variation = (terrain ? terrainTint(inWorldPosition.xz) : antiTileTint(inWorldPosition.xz))
                                   * antiTileGrain(inWorldPosition.xz);
            if (albedoLayer >= 0)
            {
                // The A-189 tinted-photo mix (rev 21: the palette owns the
                // hue at the pinned 55% strength, the photo owns the grain).
                // Rev 26: the sampling goes through albedoAntiTile - the
                // two-rotation two-sample blend (materials_common.glsl) that
                // breaks the fine speckle's axis alignment; each triplanar
                // projection blends its own rotated pair before the weights.
                const vec3 texX = albedoAntiTile(albedoLayer, tp.uvX, inWorldPosition.xz);
                const vec3 texY = albedoAntiTile(albedoLayer, tp.uvY, inWorldPosition.xz);
                const vec3 texZ = albedoAntiTile(albedoLayer, tp.uvZ, inWorldPosition.xz);
                const vec3 tex = texX * tp.weights.x + texY * tp.weights.y + texZ * tp.weights.z;
                material.albedo = mix(vec3(1.0), object.albedoRoughness.rgb, 0.55) * tex * variation;
            }
            if (dataLayer >= 0)
            {
                const vec4 dataX = dataAntiTile(dataLayer, tp.uvX, inWorldPosition.xz);
                const vec4 dataY = dataAntiTile(dataLayer, tp.uvY, inWorldPosition.xz);
                const vec4 dataZ = dataAntiTile(dataLayer, tp.uvZ, inWorldPosition.xz);
                const vec4 data  = dataX * tp.weights.x + dataY * tp.weights.y + dataZ * tp.weights.z;
                // Rev 28 (the matte-ground round): the terrain FLOORS at
                // roughness 0.90. The CC0 maps sit at ~0.68-0.77, and every
                // point below ~0.8 paints a grazing sheen the direct pass's
                // GGX lobe turns into the wet-plastic band the street-level
                // captures show. Grass and dirt are Lambertian scatterers.
                material.roughness = max(data.r, 0.90);
                material.metallic = 0.0;
                // Rev 25 fix (the dark-ground round): the TERRAIN shades with
                // its geometric normal. The grass set's NormalGL.png is a
                // flat neutral placeholder (measured 0.5/0.5/1.0 everywhere),
                // and the heightfield's relief lives entirely in the vertex
                // normals the bridge computes - enabling the map path here
                // sampled a garbage tangent frame (diagnosed 2026-11-06: the
                // final normal tilted ~40 degrees off up, nDotL went to zero
                // across the whole ground, every land capture read as a
                // murky olive floor). Prop meshes already shaded with the
                // geometric normal - the low-poly style needs no bark bumps.
                material.hasNormalMap = false;
                material.normalXY = vec2(0.5);
            }
            } // the non-water mesh branch
        }
        else
        {
        const vec2 uv = primitiveType == primitiveTypeSphere
                            ? antiTileWarp(inWorldPosition.xz, sphereSurfaceUv(inLocalPosition, halfExtents.x, uvScale))
                            : antiTileWarp(inWorldPosition.xz, boxSurfaceUv(inLocalPosition, halfExtents, uvScale));
        if (albedoLayer >= 0)
        {
            // The pastel look's tinted-photo mix (A-189): the CC0 photo
            // texture carries the detail, the object's palette tint (A-187)
            // pulls the hue at a pinned 55% strength (rev 21: 35% left every
            // meadow olive; the palette owns the hue, the photo owns the
            // grain). The GI field's analytic materials keep the tint as
            // authored - the relight-vs-raster delta stays under the gates.
            const vec3 variation = antiTileTint(inWorldPosition.xz) * antiTileGrain(inWorldPosition.xz);
            // Rev 26: albedoAntiTile's two-rotation blend replaces the single
            // sample (the traced path's materialAt does the same).
            material.albedo = mix(vec3(1.0), object.albedoRoughness.rgb, 0.55)
                              * albedoAntiTile(albedoLayer, uv, inWorldPosition.xz) * variation;
        }
        if (dataLayer >= 0)
        {
            // A-204 (rev 27, diagnosed with a two-stage shader probe on the
            // market square): the DATA array's .ba half carries NO valid
            // tangent-space normal for the analytic path - the sets' data
            // slots are single-channel roughness uploads, so .ba reads as
            // garbage and the perturbed shading normal tilted the plaza's
            // up-facing tops ~90 degrees off up. nDotL collapsed and the
            // whole market square rendered as mud (measured plaza luma
            // 0.066-0.090 against 0.24 for the sunlit grass beside it; the
            // raw-texel probe rendered the same slabs bright). This is the
            // rev-25 terrain disease (A-197: "the map path bought nothing
            // and cost the ground its sun") surviving on the analytic
            // branch. The low-poly vocabulary shades with its geometric
            // normal here exactly like the terrain and the prop meshes do;
            // the scalar roughness keeps its read, metallic pins 0 (the
            // grayscale uploads carry no metallic channel to read).
            const vec4 data = dataAntiTile(dataLayer, uv, inWorldPosition.xz);
            material.roughness = data.r;
            material.metallic = 0.0;
            material.hasNormalMap = false;
        }
        }
    }

    vec3 worldNormal = normalize(inWorldNormal);
    if (isWater)
    {
        // The ripple normal: finite differences of a 3-octave height field
        // (7 m swell, 2.3 m chop, 0.9 m sparkle). Static (no clock uniform
        // reaches this pass) - between scenarios the camera moves, and the
        // multi-octave field never reads as one flat mirror. The direct
        // pass's GGX lobe at roughness 0.10 does the actual glitter work.
        const vec2 wxz = inWorldPosition.xz;
        const float swell = vnoise2(wxz * (1.0 / 7.0) + vec2(3.1, 7.7));
        const float chop  = vnoise2(wxz * (1.0 / 2.3) + vec2(11.7, 1.3));
        const float spark = vnoise2(wxz * (1.0 / 0.9) + vec2(23.1, 5.3));
        const float rippleC = swell * 0.55 + chop * 0.30 + spark * 0.15;
        const float rippleX = vnoise2((wxz + vec2(0.4, 0.0)) * (1.0 / 7.0) + vec2(3.1, 7.7)) * 0.55
                              + vnoise2((wxz + vec2(0.4, 0.0)) * (1.0 / 2.3) + vec2(11.7, 1.3)) * 0.30
                              + vnoise2((wxz + vec2(0.4, 0.0)) * (1.0 / 0.9) + vec2(23.1, 5.3)) * 0.15;
        const float rippleZ = vnoise2((wxz + vec2(0.0, 0.4)) * (1.0 / 7.0) + vec2(3.1, 7.7)) * 0.55
                              + vnoise2((wxz + vec2(0.0, 0.4)) * (1.0 / 2.3) + vec2(11.7, 1.3)) * 0.30
                              + vnoise2((wxz + vec2(0.0, 0.4)) * (1.0 / 0.9) + vec2(23.1, 5.3)) * 0.15;
        const float slope = 1.5; // ripple steepness (rev 28: eased 2.6 -> 1.5, narrows the glitter streak)
        worldNormal = normalize(vec3(-(rippleX - rippleC) * slope, 1.0, -(rippleZ - rippleC) * slope));
    }
    else if (material.hasNormalMap)
    {
        vec3 tangent;
        vec3 bitangent;
        surfaceTbn(worldNormal, inLocalNormal, mat3(object.model), primitiveType, tangent, bitangent);
        worldNormal = materialWorldNormal(material, worldNormal, tangent, bitangent);
    }

    outEmissive = vec4(material.albedo * object.metallicEmissiveStrength.y, 1.0);
    outAlbedo = vec4(material.albedo, material.roughness);
    outNormal = vec4(worldNormal, material.metallic);
}
