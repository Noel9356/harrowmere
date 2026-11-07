#ifndef ENGINE_MODULES_GI_SCREEN_WORLD_GI_CAPABILITY_HPP
#define ENGINE_MODULES_GI_SCREEN_WORLD_GI_CAPABILITY_HPP

#include <cstdint>

// Published capability surface of the gi_screen_world module. The module is
// a lighting TECHNIQUE provider (see render_interface.hpp): its demand marker
// ("gi_screen_world_diffuse") attaches the two-space diffuse GI gather pass
// to the renderer; the family member ("lighting.gi_screen_world") exists so
// consumers can name the provider precisely. The settings struct documents
// the technique's tunables for tooling; the module reads the same values
// from configuration.
//
// Rev 33: every stage of the pass is configurable, each knob mapped onto the
// control Lumen exposes for the same stage (the mapping is documented in
// README.md and FINDINGS.md round 33):
//   screenTrace            ~ r.Lumen.ScreenProbeGather.ScreenTraces
//   screenSteps            ~ r.Lumen.ScreenProbeGather.ScreenTraces.HZBTraversal.MaxIterations
//   screenMaxDistance      ~ Post Process "Max Trace Distance" (screen stage)
//   screenThickness        ~ the screen-march thickness tolerance (linear-step leak guard)
//   temporalMaxFrames      ~ r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated
//   temporalNormalRejection~ Lumen's depth+normal history rejection
//   shortRangeAoRadius     ~ r.Lumen.ScreenProbeGather.ShortRangeAO (16 px band)
//   raysPerPixel           ~ the final gather's quality dial (Lumen: probe octahedron res)
//   maxDistance            ~ Post Process "Max Trace Distance" (world stage)
//   marchSteps             ~ the world-stage sphere-trace budget
//   indirectClamp          ~ the firefly limit (no Lumen equivalent needed: probes
//                            prefilter; our per-pixel estimator does not)

namespace engine
{

// Tunables of the two-space diffuse GI technique. Mirrored by the shipped
// config.toml defaults ([lighting.gi_screen_world]) - keep both sides in step.
struct GiScreenWorldSettings
{
    bool enabled = true; // false keeps the module idle even when demanded

    // ---- the world-space stage (the exact analytic SDF fallback) ----------
    float maxDistance = 60.0f;       // world ray bound in metres (a cost dial, not correctness)
    std::uint32_t marchSteps = 96;   // sphere-tracing step budget per world ray

    // ---- the screen-space stage (the rasterized-truth detail source) ------
    bool screenTrace = true;         // false = world-only A/B (Lumen's "Screen Traces" off)
    std::uint32_t screenSteps = 24;  // exponential stride budget (5 cm first stride)
    std::uint32_t screenRefine = 6;  // bisect steps per depth crossing
    float screenMaxDistance = 24.0f; // the screen march's bound in metres
    float screenThickness = 0.12f;   // thickness tolerance base (m; +5%/m slope in-shader)

    // ---- the final gather --------------------------------------------------
    // Stratified rays per pixel, each with its own jitter. Variance drops
    // ~sqrt(n); cost scales linearly (each ray is a screen march + possible
    // world march). 1-8; 2 is the shipped default.
    std::uint32_t raysPerPixel = 2;
    float indirectClamp = 8.0f;      // per-ray radiance clamp (the firefly limit)

    // ---- the second bounce (the screen-space radiance memory) -------------
    bool secondBounce = true;        // previous frame's gathered irradiance at the hit

    // ---- temporal accumulation (the reprojection-validated blend) ---------
    float temporalAlpha = 0.125f;    // history blend floor per frame (interactive motion)
    // Cap on the running-average ramp (alpha <= N/(N+1)). Lumen defaults to
    // 10; this rig's static-camera captures rely on longer convergence, so
    // the shipped default is 64 (near-identical to the uncapped ramp in the
    // capture window, but the history never hardens to a frozen average).
    std::uint32_t temporalMaxFrames = 64;
    bool temporalNormalRejection = true; // reject history where the G-buffer normal disagrees

    // ---- short-range screen-space occlusion (Lumen "ShortRangeAO") --------
    // Applied AFTER the temporal filter (Lumen's ordering): a ring of depth
    // taps around the pixel darkens the gathered term where the screen shows
    // nearer geometry inside the radius. 0 = off (the rev-32 look); Lumen's
    // band is 16 px.
    std::uint32_t shortRangeAoRadius = 0; // px (0 disables the term)
    float shortRangeAoStrength = 1.0f;    // occlusion gain

    // ---- spatial filter (the gather's companion bilateral) -----------------
    // Kernel radius in gather-resolution px. 1 = the shipped 3x3 Gaussian
    // bilateral; 0 disables the filter (the raw estimate passes through);
    // 2-4 widen the support for heavier estimators (rays_per_pixel = 1).
    std::uint32_t filterRadius = 1;

    // ---- output shaping -----------------------------------------------------
    float intensity = 1.0f;          // linear scale on the stored irradiance
    float skyMissScale = 1.0f;       // scale on the sky radiance a gather miss returns
    bool animatedJitter = true;      // false freezes the IGN frame phase (deterministic captures)
};

} // namespace engine

#endif // ENGINE_MODULES_GI_SCREEN_WORLD_GI_CAPABILITY_HPP
