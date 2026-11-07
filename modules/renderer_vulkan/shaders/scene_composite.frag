// Composite pass fragment shader: final HDR assembly and tonemap.
//
//   final = direct                                    (Cook-Torrance radiance)
//         + albedo * (1 - metallic) * gi              (diffuse GI irradiance;
//                                                      metals receive none)
//         + indirectSpecular                          (ray traced reflections,
//                                                      Fresnel-weighted by the
//                                                      reflections technique)
//         + emissive                                  (depth < 1: geometry)
//   final = skyRadiance(primary ray direction)        (depth >= 1: sky)
//
// The sky is the shared directional model (environment_common.glsl):
// gradient plus sun disk, the same function reflection rays see, so a
// reflective floor and the sky agree where they meet. Tonemapping and
// exposure are configurable ([renderer] tonemap / exposure).

#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D albedoSampler;   // rgb albedo (linear), a roughness
layout(set = 0, binding = 1) uniform sampler2D emissiveSampler; // rgb emissive radiance
layout(set = 0, binding = 2) uniform sampler2D directSampler;   // rgb direct radiance
layout(set = 0, binding = 3) uniform sampler2D giSampler;       // rgb diffuse GI irradiance / pi
layout(set = 0, binding = 4) uniform sampler2D depthSampler;
layout(set = 0, binding = 5) uniform sampler2D normalSampler;   // rgb world normal, a metallic
layout(set = 0, binding = 6) uniform sampler2D specularSampler; // rgb Fresnel-weighted reflection radiance
// Rev 31 (the layered depth buffer): the transparency layer the composite
// blends over the opaque result - the nearest transparent surface's
// albedo+roughness (9), world normal + authored alpha (10), NDC depth (11).
layout(set = 0, binding = 9) uniform sampler2D transAlbedoSampler; // rgb albedo, a roughness
layout(set = 0, binding = 10) uniform sampler2D transNormalSampler; // rgb world normal, a authored alpha
layout(set = 0, binding = 11) uniform sampler2D transDepthSampler;  // NDC depth (1.0 = no surface)

// The shared sky model (gradient + sun disk). The composite binds the
// environment block at 7: bindings 0-6 are the image inputs above.
#define ENGINE_ENVIRONMENT_BINDING 7
#include "environment_common.glsl"

// std140 mirror of render_interface.hpp CameraData.
layout(std140, set = 0, binding = 8) uniform CameraUniform
{
    mat4 viewProjection;
    mat4 inverseViewProjection;
    vec4 cameraPosition;
} camera;

layout(push_constant) uniform CompositePush
{
    layout(offset = 0) vec4 flagsExposure; // x unused, w exposure
    layout(offset = 16) ivec4 modeFlags;   // x tonemap (0 none, 1 aces, 2 reinhard), y directEnabled, z giEnabled, w specularEnabled
} composite;

vec3 acesTonemap(vec3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

// Rev 31: the sky/hemispheric ambient for the transparency surface. The same
// sky-derived pair the opaque branch builds below (horizon band for vertical
// normals, zenith for up), at the flat weight the water reads naturally.
vec3 transAmbient(vec3 worldNormal, float upOpenness)
{
    const vec3 ambientSky = mix(environment.skyGround.rgb, environment.skyZenith.rgb, 0.55);
    const vec3 ambientHorizon = mix(environment.skyHorizon.rgb, environment.skyZenith.rgb, 0.25);
    return mix(ambientHorizon, ambientSky, upOpenness * upOpenness) * vec3(1.06, 0.98, 0.88) * (0.13 + 0.16 * upOpenness);
}

// The transparency surface's reflection: a Fresnel-weighted SKY reflection
// in the ripple normal. This is the term that makes water read as water -
// at nadir the Fresnel is ~2-4% (the dark body rules), at grazing angles it
// climbs to 1 and the surface mirrors the sky, and the SUN DISK inside
// skyRadiance becomes the glitter path the ripple normals break up. The
// rev-30 look's direct-pass GGX lobe was a stand-in for exactly this; the
// layered-depth move transferred the surface's shading here, so the
// reflection comes from the same sky model every other surface agrees on.
// (No separate sun-specular term: the disk inside the reflection IS the sun
// specular - adding both would double-count the disk's energy.)
vec3 transSurfaceReflection(vec3 normal, vec3 viewDir, float roughness)
{
    const float noV = max(dot(normal, viewDir), 1e-3);
    const vec3 fresnel = vec3(0.02) + 0.98 * pow(1.0 - noV, 5.0);
    vec3 reflected = reflect(-viewDir, normal);
    // A ripple can fold the reflection below the horizon: fold it back up
    // (the horizon band's color), never into the seabed.
    if (reflected.y < 0.0)
    {
        reflected.y = -reflected.y * 0.25;
    }
    // The ripple roughness breaks the mirror: rougher surfaces mix the
    // sharp sky towards its average (a broad sheen instead of a crisp
    // image; the water's 0.28 keeps most of the sharp read).
    const vec3 sky = skyRadiance(normalize(reflected));
    const vec3 skyAverage = mix(environment.skyHorizon.rgb, environment.skyZenith.rgb, 0.4);
    return mix(sky, skyAverage, clamp(roughness * 1.5, 0.0, 0.85)) * fresnel;
}

void main()
{
    const float depth = texture(depthSampler, inUv).r;
    vec3 color;
    if (depth >= 1.0)
    {
        const vec4 target = camera.inverseViewProjection * vec4(inUv * 2.0 - vec2(1.0), 1.0, 1.0);
        // The shared skyRadiance reads the raw direction's y for the
        // gradient height, so normalize here (the ray is reconstructed).
        color = skyRadiance(normalize(target.xyz / target.w - camera.cameraPosition.xyz));
    }
    else
    {
        const vec4 albedoRoughness = texture(albedoSampler, inUv);
        const vec3 albedo = albedoRoughness.rgb;
        const vec3 emissive = texture(emissiveSampler, inUv).rgb;
        const float metallic = texture(normalSampler, inUv).a;
        const vec3 worldNormal = normalize(texture(normalSampler, inUv).rgb);

        // Hemispheric ambient (A-190, the graphics-uplift round): the sky's
        // own light wraps every surface, weighted by how much the normal
        // opens towards the zenith. Without it the unlit hemisphere is PURE
        // black - the roof undersides, north walls and shadow cores read as
        // holes in the pastel world (the radiance-cascade GI that would own
        // this term is not part of this build, so the composite carries the
        // floor itself).
        // Rev 25 drama: the ambient leans SKY-BLUE (mix 0.35 -> 0.55) so the
        // shadowed side of every object reads cool against the amber sun key
        // - warm light / cool shadow is the contrast pair that stopped the
        // frames reading as one muddy sheet.
        const vec3 ambientSky = mix(environment.skyGround.rgb, environment.skyZenith.rgb, 0.55);
        // Rev 29 (the unlit-facade round): a VERTICAL wall opens towards the
        // horizon band, not the zenith - the pure zenith mix left every
        // north-facing cottage wall reading near-black under the morning key
        // (the rev-27 debt note's "unlit-side ambient is thin"). The sky
        // term now blends the (brighter, warmer) horizon color in as the
        // normal turns vertical, and the hemispheric weight rises a touch
        // with it. The term stays sky-derived: at night both colors fall to
        // the near-black gradient and the moon floor below rules exactly as
        // before, so the day/night gate keeps its margin.
        const vec3 ambientHorizon = mix(environment.skyHorizon.rgb, environment.skyZenith.rgb, 0.25);
        const float upOpenness = clamp(worldNormal.y * 0.5 + 0.5, 0.0, 1.0);
        const vec3 skyTerm = mix(ambientHorizon, ambientSky, upOpenness * upOpenness);
        // The floor is constant, not sky-derived (rev 22 amendment): after
        // dusk the sky gradient itself is near black, so a sky-proportional
        // ambient vanished with it and the night terrain read as a hole. A
        // deep cool floor keeps the shapes alive under the moon key light
        // without washing the day. Rev 24: the floor now SCALES WITH THE
        // MOON - the night captures (spawn_night / corpse_aftermath) read as
        // a black field because the floor term was 4x too small for the
        // terrain's dark albedo; the moon-driven term lifts the night ground
        // to a readable blue while vanishing exactly when the moon does (the
        // day keeps the rev-22 constant as its floor). Rev 29: the moon
        // share eases 0.11 -> 0.06 with A-186's key at 1.2 - the 3.2-key era
        // pumped the night ground to daylight through the 12.5x night
        // exposure gain; the floor follows the key down so the night reads
        // as moonlit, not floodlit.
        const vec3 nightFloor = vec3(1.0, 1.1, 1.5) * clamp(environment.moonDirectionIntensity.w * 0.06, 0.0, 1.0);
        // Rev 25 (the drama round): the hemispheric weight drops from
        // (0.18 + 0.22*up) to (0.10 + 0.15*up). The old floor lifted the
        // shadow side of every object to within a stop of the sun side -
        // with the rev-25 shadows arriving (mesh proxies + terrain
        // heightfield) the flat ambient erased exactly the contrast those
        // shadows draw. The warm bounce tint (a ground-bounce lean, not the
        // sky's blue) keeps shadow interiors readable instead of grey.
        const vec3 bounceTint = vec3(1.06, 0.98, 0.88);
        // Rev 29: the weight rises (0.10 + 0.15*up) -> (0.13 + 0.16*up) - the
        // unlit facades need the fill, and the sunlit side keeps its contrast
        // because the direct key dwarfs the ambient there.
        // Rev 30 (the GI round): when the run's GI technique owns the
        // indirect term, the analytic ambient is a GHOST term - the GI's own
        // sky miss + bounce transport already carries the sky fill, and the
        // ambient would double it, washing the very contrast (corner
        // darkening, colored bleed) the GI exists to draw. The night floor
        // term stays at full strength (the GI's night sky miss is near
        // black; the floor is what keeps the night readable, exactly the
        // rev-24 amendment's job).
        // Rev 31: the share eases 0.25 -> 0.40. The street-level crowd
        // frame showed why a quarter is too thin: a shaded figure's
        // camera-facing hemisphere gathers mostly SHADOWED ground and walls
        // inside the cottage ring (the sky is a small solid angle there),
        // so its GI fill measured ~a tenth of the sunlit plaza beside it -
        // the crowd read as silhouettes again. The 0.40 share keeps the GI
        // as the dominant transport (the chamber's colored bleed and corner
        // occlusion stay) while the sky-derived ambient lifts every shaded
        // vertical surface off the black point.
        const float ambientShare = composite.modeFlags.z != 0 ? 0.40 : 1.0;
        const vec3 ambient = max(skyTerm * bounceTint * (0.13 + 0.16 * upOpenness) * ambientShare,
                                 max(vec3(0.036, 0.042, 0.060), nightFloor));

        vec3 direct = vec3(0.0);
        vec3 gi = vec3(0.0);
        vec3 specular = vec3(0.0);
        if (composite.modeFlags.y != 0)
        {
            direct = texture(directSampler, inUv).rgb;
        }
        if (composite.modeFlags.z != 0)
        {
            gi = texture(giSampler, inUv).rgb;
        }
        if (composite.modeFlags.w != 0)
        {
            specular = texture(specularSampler, inUv).rgb;
        }
        color = direct + albedo * (1.0 - metallic) * (gi + ambient) + specular + emissive;

        // Distance fog on geometry: reconstruct the world position from the
        // depth the same way the sky branch reconstructs its ray, then blend
        // towards the mod-driven fog tint by view distance (the composite
        // runs before tonemapping, so the fog color stays linear).
        const vec4 target = camera.inverseViewProjection * vec4(inUv * 2.0 - vec2(1.0), depth, 1.0);
        color = applyDistanceFog(color, target.xyz / target.w, camera.cameraPosition.xyz);
    }

    // ---- rev 31: the layered depth buffer's transparency blend -----------
    // A transparent surface IN FRONT of layer 0 (or in front of the sky)
    // blends its own shading over the behind-color. The per-pixel opacity is
    // Beer-Lambert on the view-ray path length through the medium (the world
    // distance between the two layers' surfaces): shallow shore water stays
    // see-through, the basin floor goes opaque within a few metres. The sun
    // SPECULAR rides on top unscaled - a reflection does not attenuate with
    // the water's depth.
    const float transDepth = texture(transDepthSampler, inUv).r;
    if (transDepth < min(depth, 0.999999))
    {
        const vec4 transAlbedoRough = texture(transAlbedoSampler, inUv);
        const vec4 transNormalAlpha = texture(transNormalSampler, inUv);
        const float authoredAlpha = transNormalAlpha.a;
        if (authoredAlpha > 0.004 && transAlbedoRough.a > 0.0)
        {
            // World positions from the two NDC depths (the shared
            // reconstruction the fog and sky branches use).
            const vec4 behindClip = camera.inverseViewProjection * vec4(inUv * 2.0 - vec2(1.0), depth, 1.0);
            const vec4 transClip  = camera.inverseViewProjection * vec4(inUv * 2.0 - vec2(1.0), transDepth, 1.0);
            const vec3 behindPos  = behindClip.xyz / behindClip.w;
            const vec3 transPos   = transClip.xyz / transClip.w;

            const vec3 transNormal = normalize(transNormalAlpha.rgb);
            const float upOpenness = clamp(transNormal.y * 0.5 + 0.5, 0.0, 1.0);
            const vec3 viewDir = normalize(camera.cameraPosition.xyz - transPos);

            // The path length through the medium, and the absorption.
            const float pathMeters = distance(behindPos, transPos);
            float opacity = 1.0 - exp(-0.42 * pathMeters);
            opacity = clamp(opacity, 0.06, 0.97) * authoredAlpha;

            // The surface body: the dark albedo under the sky ambient (the
            // GI at these pixels was gathered for the REFRACTED surface
            // behind, not for the water skin - the ambient is the honest
            // term), plus its own sky reflection added unscaled after the
            // mix (a reflection does not attenuate with the water depth).
            const vec3 surfaceBody = transAlbedoRough.rgb * transAmbient(transNormal, upOpenness) * 1.7;
            const vec3 surfaceReflection = transSurfaceReflection(transNormal, viewDir, transAlbedoRough.a);

            color = mix(color, surfaceBody, opacity) + surfaceReflection;

            // Fog anchors at the WATER surface (the closer depth), not the
            // seabed behind it.
            const vec4 fogTarget = camera.inverseViewProjection * vec4(inUv * 2.0 - vec2(1.0), transDepth, 1.0);
            color = applyDistanceFog(color, fogTarget.xyz / fogTarget.w, camera.cameraPosition.xyz);
        }
    }

    color *= composite.flagsExposure.w;
    if (composite.modeFlags.x == 1)
    {
        color = acesTonemap(color);
    }
    else if (composite.modeFlags.x == 2)
    {
        color = color / (1.0 + color);
    }
    outColor = vec4(color, 1.0);
}
