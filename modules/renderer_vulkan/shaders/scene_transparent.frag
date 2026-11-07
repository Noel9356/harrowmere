#version 450
#extension GL_GOOGLE_include_directive : require
// Transparency pass fragment shader (rev 31, the layered depth buffer):
// writes the SECOND depth layer's material payload. The vertex stage is the
// geometry pass's scene.vert; this fragment fills three attachments:
//
//   location 0  RGBA8   rgb albedo, a roughness
//   location 1  RGBA16F  rgb world normal, a authored opacity multiplier
//   location 2  R32F    gl_FragCoord.z (the NDC depth, the composite's
//                       transparent-layer depth)
//
// The pipeline depth-tests against the OPAQUE depth (write off): a
// transparent surface behind a wall never reaches the blend. The composite
// computes the per-pixel opacity from the world-space depth delta between
// this layer and layer 0 (Beer-Lambert absorption along the view ray) times
// the authored alpha stored here, shades the surface with its own specular +
// ambient, and blends it over the refracted view behind.
//
// Materials: the first citizen is the water plane - the SAME dark body +
// multi-octave ripple normal the opaque pass's water branch built (moved
// here; the water no longer draws opaquely). Any other semi-transparent set
// takes the flat configured albedo at its authored alpha.

#include "scene_common.glsl"
#include "materials_common.glsl"

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec3 inLocalPosition;
layout(location = 3) in vec3 inLocalNormal;
layout(location = 4) in vec3 inHalfExtents;

layout(location = 0) out vec4 outTransAlbedo; // RGBA8: rgb albedo, a roughness
layout(location = 1) out vec4 outTransNormal; // RGBA16F: rgb world normal, a alpha
layout(location = 2) out float outTransDepth; // R32F: NDC depth

layout(push_constant) uniform ObjectPush
{
    layout(offset = 0) mat4 model;
    layout(offset = 64) vec4 albedoRoughness;          // rgb albedo, a roughness
    layout(offset = 80) vec4 metallicEmissiveStrength; // x metallic, y emissive, z OPACITY multiplier
    layout(offset = 96) vec4 materialLayers;           // x albedo layer, y data layer (-1 none), z uv scale, w primitive type
    layout(offset = 112) vec4 meshBox;                 // mesh objects: the registered mesh's bbox half extents
} object;

void main()
{
    const uint primitiveType = uint(object.materialLayers.w + 0.5);
    const bool isWater = primitiveType == primitiveTypeMesh && object.meshBox.w > 0.5;
    const int albedoLayer = int(floor(object.materialLayers.x + 0.5));
    const float uvScale = max(object.materialLayers.z, 1e-4);
    const float opacity = clamp(object.metallicEmissiveStrength.z, 0.0, 1.0);
    // Fully transparent (alpha 0) contributes nothing: discard before any
    // write so the layer's depth stays clear behind such a surface too.
    if (opacity <= 0.004)
    {
        discard;
    }

    vec3 albedo = object.albedoRoughness.rgb;
    float roughness = object.albedoRoughness.a;
    vec3 worldNormal = normalize(inWorldNormal);

    if (isWater)
    {
        // The water body: the rev-27/28 dark blue-green mix with the ripple
        // texture share (moved verbatim from the opaque pass's water branch;
        // the surface's look does not change with its depth layer).
        const vec3 waterTex = albedoLayer >= 0
                                  ? texture(albedoTextureArray, vec3(inWorldPosition.xz / uvScale, float(albedoLayer))).rgb
                                  : vec3(1.0);
        albedo = mix(vec3(0.04, 0.10, 0.13), waterTex * 0.72, 0.40);
        roughness = 0.28;
        // The ripple normal: finite differences of a 3-octave height field
        // (7 m swell, 2.3 m chop, 0.9 m sparkle); the direct pass's GGX lobe
        // did the glitter work when the surface shaded opaquely - now the
        // composite's sun lobe reads this normal.
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
        const float slope = 1.5; // ripple steepness (rev 28's eased value)
        worldNormal = normalize(vec3(-(rippleX - rippleC) * slope, 1.0, -(rippleZ - rippleC) * slope));
    }

    outTransAlbedo = vec4(albedo, roughness);
    outTransNormal = vec4(worldNormal, opacity);
    outTransDepth  = gl_FragCoord.z;
}
