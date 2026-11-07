// Shared sky model for the composite pass, the cascade compose base and the
// reflection fallback. A vertical gradient (zenith / horizon / ground), an
// optional physically scaled sun disk, a broad sun halo (rev 24) and a
// deterministic ALU cloud layer (rev 24 - the flat grey-white sky was the
// owner's "lighting is boring" defect; the clouds ride the fog density so
// clear skies stay sparse and storms pile up).
//
// Two entry points on purpose:
//   skyBase(direction)     gradient + clouds + the BROAD halo term - the
//                          reflection fallback and any GI consumer use this;
//                          every added term stays low-frequency so coarse
//                          angular texels never alias;
//   skyRadiance(direction) skyBase + the sun disk + the TIGHT halo - direct
//                          view (composite sky pixels) and reflection rays
//                          see the sun itself.
//
// The sun disk radiance is sunColor * sunIntensity / solidAngle: integrating
// the disk over its solid angle reproduces exactly the sun's configured
// irradiance, so the disk is bright enough for ACES to clamp it to white but
// adds no extra energy to the scene.

#ifndef ENGINE_ENVIRONMENT_COMMON_GLSL
#define ENGINE_ENVIRONMENT_COMMON_GLSL

// Descriptor binding of the environment block. Consumers that use the
// technique shared-set layout get the default (6); the composite pass binds
// it at 7 (its bindings 0-6 are the image inputs) and defines the macro
// before the include.
#ifndef ENGINE_ENVIRONMENT_BINDING
#define ENGINE_ENVIRONMENT_BINDING 6
#endif

// std140 mirror of renderer.cpp EnvironmentData (keep both sides in sync).
layout(std140, set = 0, binding = ENGINE_ENVIRONMENT_BINDING) uniform EnvironmentUniform
{
    vec4 skyZenith;        // rgb zenith radiance, w horizon exponent
    vec4 skyHorizon;       // rgb horizon radiance, w ground exponent
    vec4 skyGround;        // rgb ground radiance, w sun disk enabled (0/1)
    vec4 sunDiskDirection; // xyz direction towards the sun, w cos(inner angle)
    vec4 sunDiskRadiance;  // rgb disk radiance, w cos(outer angle)
    vec4 fogDensityTint;   // x density [0,1], yzw linear fog tint
    vec4 moonDirectionIntensity; // xyz direction towards the scene, w intensity
} environment;

// The shared value-noise stack (the same shape materials_common.glsl uses -
// the sky cannot include that file without dragging the material buffers in,
// so the three tiny functions are duplicated verbatim here).
float skyHash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float skyVnoise(vec2 p)
{
    const vec2 i = floor(p);
    const vec2 f = fract(p);
    const vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(skyHash12(i), skyHash12(i + vec2(1.0, 0.0)), u.x),
               mix(skyHash12(i + vec2(0.0, 1.0)), skyHash12(i + vec2(1.0, 1.0)), u.x), u.y);
}

float skyFbm(vec2 p)
{
    return 0.5 * skyVnoise(p) + 0.25 * skyVnoise(p * 2.13 + 19.7) + 0.125 * skyVnoise(p * 4.31 + 47.1)
           + 0.0625 * skyVnoise(p * 8.7 + 3.3);
}

// The sun's look-direction height above the horizon (the disk direction
// points TOWARDS the sun; y > 0 means it is up). Drives the halo's warmth
// (a low sun glows amber) and the clouds' day lift.
float sunHeight()
{
    return normalize(environment.sunDiskDirection.xyz).y;
}

// Coverage from the fog density: CLEAR 0.17 -> ~0.28 cumulus, RAIN 0.35 ->
// ~0.41, STORM 0.55 -> ~0.56 overcast. The rev-25 drama round lowered the
// whole map: the old floor (0.28 + density*0.85) put a 0.42 deck over the
// CLEAR sky - a half-grey ceiling over every day capture (the "lighting is
// boring" sky). The floor keeps a few distant clouds in the clearest air.
float cloudCoverage()
{
    const float density = max(environment.fogDensityTint.x, 0.0);
    return clamp(0.16 + density * 0.72, 0.14, 0.62);
}

// The cloud layer: a dome-projected 4-octave fbm field. Deterministic (no
// time input - R8), smooth at reflection-texel frequencies, faded in at the
// horizon so the sky band stays clean.
float cloudLayer(vec3 direction)
{
    const float height = direction.y;
    if (height <= 0.004)
    {
        return 0.0;
    }
    const vec2 p = direction.xz / (height + 0.22);
    const float field = skyFbm(p * 1.15 + 31.7) * 0.72 + skyVnoise(p * 3.4 + 7.7) * 0.28;
    const float coverage = cloudCoverage();
    const float shape = smoothstep(1.0 - coverage, 1.0 - coverage + 0.30, field);
    // Fade with altitude so the dome's pole does not smear. Rev 26: the fade
    // starts at 0.006 (was 0.015) - a horizon camera (the coast staging) kept
    // clipping the whole deck out of frame, and a blue sky with a bare
    // horizon read as a dead gradient.
    const float fade = smoothstep(0.004, 0.10, height) * (1.0 - 0.35 * smoothstep(0.55, 1.0, height));
    return shape * fade;
}

// The cloud radiance: lit faces lean towards the sun (silver lining), shadow
// undersides pull the sky's own color down; at night the whole layer falls
// to a dark silhouette against the moon gradient. The lit peak stays BELOW
// the composite's bloom threshold (1.0) - a cloud that blooms washes every
// capture pale (the rev-24 bisect: bright cloud decks crossed the threshold
// and the 0.15 bloom strength lifted the whole frame).
vec3 cloudRadiance(vec3 direction, float cloud)
{
    if (cloud <= 0.001)
    {
        return vec3(0.0);
    }
    const vec3 sunDir = normalize(environment.sunDiskDirection.xyz);
    const float sunUp = smoothstep(-0.08, 0.25, sunHeight());
    const float facing = max(dot(normalize(direction), sunDir), 0.0);
    // Golden-hour tint: a low sun lights the cloud decks amber, noon whites
    // them out. Rev 25: the lit term rises (0.30 + 0.32) and the shadow
    // floor lifts 0.58 -> 0.66 - the old deck read as a grey lid; the deck
    // now reads as SUNLIT cumulus against the blue.
    const vec3 litTint = mix(vec3(1.10, 0.84, 0.60), vec3(0.98, 0.96, 0.92), smoothstep(0.06, 0.45, sunHeight()));
    const vec3 shadow = mix(environment.skyZenith.rgb, environment.skyHorizon.rgb, 0.45) * 0.66;
    const vec3 lit = shadow + litTint * (0.30 + 0.32 * facing * facing);
    return mix(shadow, lit, clamp(0.25 + 0.75 * sunUp, 0.0, 1.0)) * cloud;
}

// Rev 29 (the anti-mauve round): a luma-preserving saturation lift for the
// PAINTED gradient. ACES desaturates bright blues towards grey, and the
// anchor blends between the day blues and the dusk reds pass through
// low-chroma violets - the 16.5h frontier capture printed the whole dome
// as one flat mauve sheet. Lifting the gradient's chroma (not its luma,
// so every luma gate and the fog's energy bookkeeping stay put) returns
// the sky its blue; the halo and the clouds pile on top unchanged.
vec3 skySaturate(vec3 color)
{
    const float luma = dot(color, vec3(0.299, 0.587, 0.114));
    return max(mix(vec3(luma), color, 1.30), vec3(0.0));
}

// Rev 29: the dusk repaint, shared by skyBase and skyGradient so the fog
// anchor meets the painted sky on ONE color (the rev-27 lesson). While the
// sun sits near the horizon the CPU blend is at the dusk anchor - a flat
// red - and repainting the band is what turns it into a sunset: a warm
// orange band concentrated towards the sun's azimuth, a rose anti-sun
// side, and a zenith that deepens towards slate so the transit never owns
// the high sky as mauve.
vec3 duskRepaint(vec3 result, vec3 direction)
{
    const vec3 sunDir = normalize(environment.sunDiskDirection.xyz);
    const float sh = sunHeight();
    // Active from deep twilight (sh = -0.22) to a sun two thirds up; the
    // rev-29 follow-up widened the upper edge (0.14/0.40 -> 0.20/0.62):
    // the golden-hour dome needs the warm-band + slate-zenith counterpoint
    // ALREADY at a 22-38 degree sun, or the whole dome renders as one flat
    // cream sheet (the 16.5h frontier frame).
    const float band = (1.0 - smoothstep(0.20, 0.62, sh)) * smoothstep(-0.22, -0.04, sh);
    if (band <= 0.001)
    {
        return result;
    }
    const float height = direction.y;
    if (height < 0.0)
    {
        return result;
    }
    const float facing = max(dot(normalize(direction), sunDir), 0.0);
    // The warm band hugs the horizon, orange on the sun's side, rose away
    // from it; a weak all-azimuth share keeps the whole horizon luminous
    // the way a real twilight is.
    const vec3 sunSide = vec3(1.00, 0.46, 0.18);
    const vec3 awaySide = vec3(0.72, 0.42, 0.42);
    const vec3 warm = mix(awaySide, sunSide, pow(facing, 1.5));
    const float horiz = 1.0 - smoothstep(0.02, 0.40, height);
    result = mix(result, warm * (0.52 + 0.38 * pow(facing, 2.5)),
                 band * horiz * (0.55 + 0.45 * pow(facing, 2.0)));
    // The zenith deepens towards slate so the high sky reads as dusk, not
    // as washed violet.
    const vec3 slate = vec3(0.145, 0.19, 0.33);
    result = mix(result, slate + result * 0.18, band * smoothstep(0.16, 0.70, height) * 0.62);
    return result;
}

vec3 skyBase(vec3 direction)
{
    const float height = direction.y;
    const vec3 ground = environment.skyGround.rgb;
    const vec3 horizon = environment.skyHorizon.rgb;
    const vec3 zenith = environment.skyZenith.rgb;
    vec3 result;
    if (height >= 0.0)
    {
        const float t = pow(clamp(height, 0.0, 1.0), max(environment.skyZenith.w, 0.001));
        result = skySaturate(mix(horizon, zenith, t));
    }
    else
    {
        const float t = pow(clamp(-height, 0.0, 1.0), max(environment.skyHorizon.w, 0.001));
        result = mix(horizon, ground, t);
    }
    result = duskRepaint(result, direction);
    // The BROAD halo (rev 25 drama): a wide warm glow that keeps the sky
    // reading as a lit atmosphere, plus a horizon-hugging term so a low sun
    // paints the horizon band the way a real golden hour does. Rev 29: both
    // terms TIGHTEN (pow 4 -> 6, pow 2 -> 3.5) - the wide lobes washed the
    // whole dome warm on every sun-facing camera, and blue sky + pale warm
    // wash is exactly the flat mauve the 16.5h frontier capture showed. The
    // warmth now owns the sun's sector; the rest of the dome keeps the
    // gradient's blue.
    const vec3 sunDir = normalize(environment.sunDiskDirection.xyz);
    const float sunUp = smoothstep(-0.02, 0.18, sunHeight());
    if (sunUp > 0.0)
    {
        const float facing = max(dot(normalize(direction), sunDir), 0.0);
        const vec3 haloTint = mix(vec3(1.0, 0.62, 0.34), vec3(1.0, 0.90, 0.76), smoothstep(0.05, 0.45, sunHeight()));
        result += haloTint * 0.15 * pow(facing, 6.0) * sunUp;
        result += haloTint * 0.20 * pow(facing, 3.5)
                  * (1.0 - smoothstep(0.0, 0.45, height)) * sunUp;
    }
    // The cloud deck (rev 25: under half strength so the gradient owns the
    // sky's average radiance). Rev 26: 0.28 -> 0.36 - the deck had dropped
    // below visibility in every capture frame, and a cloudless blue read as
    // a dead gradient; the coverage floor and the lit peak both stay below
    // the composite's bloom threshold, so the deck decorates without washing.
    const float cloud = cloudLayer(normalize(direction));
    result = mix(result, cloudRadiance(direction, cloud), 0.36);
    return result;
}

vec3 skyRadiance(vec3 direction)
{
    vec3 result = skyBase(direction);
    const vec3 sunDir = normalize(environment.sunDiskDirection.xyz);
    const float sunUp = smoothstep(-0.02, 0.18, sunHeight());
    // The TIGHT halo: the direct view gets the hot core around the disk the
    // base deliberately keeps low-frequency.
    if (sunUp > 0.0)
    {
        const float facing = max(dot(normalize(direction), sunDir), 0.0);
        const vec3 haloTint = mix(vec3(1.0, 0.58, 0.28), vec3(1.0, 0.88, 0.72), smoothstep(0.05, 0.45, sunHeight()));
        result += haloTint * 0.26 * pow(facing, 32.0) * sunUp;
    }
    if (environment.skyGround.w > 0.5)
    {
        const float cosine = dot(normalize(direction), sunDir);
        // Smoothstep between the outer and inner cosine angles gives a small
        // antialiased limb instead of a hard circle.
        const float disk = smoothstep(environment.sunDiskRadiance.w, environment.sunDiskDirection.w, cosine);
        result += environment.sunDiskRadiance.rgb * disk;
    }
    // The moon: a small soft disk opposite the sun's arc. The intensity is
    // irradiance-scaled (A-186), so the disk glows without washing the
    // night sky; w == 0 (day, or no override) adds nothing.
    if (environment.moonDirectionIntensity.w > 0.0)
    {
        const vec3 moonDir = normalize(environment.moonDirectionIntensity.xyz);
        const float cosine = dot(normalize(direction), moonDir);
        // Fixed angular size: slightly larger than the sun disk reads as a
        // stylized full moon at this world's scale.
        const float disk = smoothstep(0.9994, 0.99975, cosine);
        result += vec3(0.85, 0.90, 1.0) * environment.moonDirectionIntensity.w * 0.35 * disk;
        // A soft halo so the moon reads through the gradient at dusk.
        result += vec3(0.55, 0.62, 0.80) * environment.moonDirectionIntensity.w * 0.02
                  * pow(clamp(cosine, 0.0, 1.0), 24.0);
        // The moon lights the cloud deck's top edge at night (rev 24): the
        // day term in cloudRadiance already fell to the dark silhouette;
        // this adds the faint moonlit rim so night clouds never vanish.
        const float cloud = cloudLayer(normalize(direction));
        const float facing = max(dot(normalize(direction), moonDir), 0.0);
        result += vec3(0.30, 0.36, 0.50) * (0.10 + 0.14 * facing) * cloud
                  * environment.moonDirectionIntensity.w * 0.16;
    }
    return result;
}

// The pure gradient (no clouds, no halo, no disk) - the aerial-perspective
// anchor rev 27 adds. Far geometry must melt into THE SKY behind it, not
// into a fixed weather sheet: the afternoon captures printed a grey band
// across the horizon because the fog tint stayed neutral grey while the
// anchor blend painted the sky sunset-mauve, so the fully-fogged ground
// met the sky on two different colors. Evaluating the gradient along the
// view ray ties the fog's far color to whatever the sky is doing now.
vec3 skyGradient(vec3 direction)
{
    const float height = direction.y;
    const vec3 ground  = environment.skyGround.rgb;
    const vec3 horizon = environment.skyHorizon.rgb;
    const vec3 zenith  = environment.skyZenith.rgb;
    if (height >= 0.0)
    {
        const float t = pow(clamp(height, 0.0, 1.0), max(environment.skyZenith.w, 0.001));
        // Rev 29: the same saturation lift and dusk repaint skyBase paints,
        // so the fogged far field meets the sky on one color at every hour
        // (the 60% yield in applyDistanceFog below anchors on THIS).
        return duskRepaint(skySaturate(mix(horizon, zenith, t)), direction);
    }
    const float t = pow(clamp(-height, 0.0, 1.0), max(environment.skyHorizon.w, 0.001));
    return mix(horizon, ground, t);
}

// Exp2 distance fog over world metres (the mod-driven density [0, 1] is a
// per-kilometre extinction; 0 = clear). Applied on the composite path only,
// AFTER lighting: the fog color is the weather tint the atmosphere module
// pushed, blended by view distance so the far field melts into it.
vec3 applyDistanceFog(vec3 color, vec3 worldPosition, vec3 cameraPosition)
{
    const float density = max(environment.fogDensityTint.x, 0.0);
    if (density <= 0.0)
    {
        return color;
    }
    const vec3 ray       = worldPosition - cameraPosition;
    const float distanceM = length(ray);
    const float fogFactor = 1.0 - exp2(-density * density * distanceM * distanceM * 1e-6);
    // Rev 27: the fog color yields 60% to the sky gradient along the ray.
    // The weather tint keeps the mood (rain stays grey, storm stays dark),
    // but at the horizon - where the fog factor saturates - the sky's own
    // color dominates, so far ground and sky meet on ONE color instead of
    // printing the grey band.
    const vec3 skyColor = skyGradient(ray / max(distanceM, 1e-4));
    const vec3 fogColor = mix(environment.fogDensityTint.yzw, skyColor, 0.6);
    return mix(color, fogColor, clamp(fogFactor, 0.0, 1.0));
}

#endif // ENGINE_ENVIRONMENT_COMMON_GLSL
