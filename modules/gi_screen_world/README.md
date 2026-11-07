# gi_screen_world

A lighting TECHNIQUE module named after its algorithm (rev 33): a per-pixel
diffuse GI final gather whose every ray resolves in TWO spaces - marched
through the SCREEN-space depth buffer first (the rasterized truth), then,
for whatever the screen cannot answer, sphere-traced against the EXACT
analytic scene SDF (the world fallback).

Name history, all three pinned by the owner's own bar: rev 30 `gi_traced`
(rejected: any GI is traced - too generic), rev 31 `gi_sdf_hemisphere`
(described the cosine-hemisphere SDF gather), rev 33 `gi_screen_world`
(rev 32 made every ray a two-space ray, so the rev-31 name described only
the fallback stage; the new name states the technique's defining structure -
screen first, world second). A name like `lumen` was considered and
REJECTED on the research below: this is not Lumen, and naming it so would
overclaim.

## What the research actually says about Lumen (rev 33)

Verified against Epic's documentation, the SIGGRAPH 2022 Advances course
notes (Wright et al., "Lumen: Real-Time Global Illumination in Unreal
Engine 5"), the radiance-cache course materials, and multiple independent
deep dives (see FINDINGS.md round 33 for the source list). Lumen's actual
shape:

1. **Trace ordering** - every gather ray is resolved through a chain:
   screen-space traces FIRST (against the frame's depth/color, half-res for
   diffuse rays), then short Mesh-SDF traces (the first two metres), then
   Global-SDF traces, then skylight on a final miss. Screen traces exist to
   cover mismatches between the triangle scene and Lumen's proxy scene and
   to catch geometry the proxy representations miss (skinned meshes in
   software mode).
2. **The final gather is PROBE-based, not per-pixel** - the Screen Space
   Radiance Cache places probes on pixels at ~1/16 resolution (adaptive,
   subdividing near detailed geometry); each probe traces an octahedral ray
   set (r.Lumen.ScreenProbeGather.TracingOctahedronResolution, default 8 ->
   8x8 rays), importance-samples directions using LAST frame's reprojected
   cache (product importance sampling), and pixels interpolate from probes
   in the same plane. A World Space Radiance Cache (32x32 octahedral probes
   in a clipmap) backs it for distant lighting. Lumen explicitly REJECTED
   per-pixel tracing + full-res screen-space denoising as the expensive end
   of the spectrum.
3. **Surface cache, not hit lighting** - ray hits are relit from a card-
   based cache of pre-relit scene surfaces (amortized over frames), NOT by
   shading the hit directly (hit lighting exists only as the expensive
   hardware-RT option).
4. **Temporal filter** - full-res, depth+normal history rejection, no
   neighborhood clamp, capped accumulation
   (r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated, default 10),
   accelerated when traces hit fast-moving objects.
5. **Short-range occlusion** - a 16 px screen-space occlusion band applied
   AFTER the temporal filter (so it carries no lag), repairing the contact
   shadow detail the downsampled cache loses (r.Lumen.ScreenProbeGather.
   ShortRangeAO).
6. **Monte Carlo, not cone tracing** - Lumen 4 replaced prefiltered cone
   traces with MC integration and moved the noise problem into the final
   gather + filters.

## What this module shares with Lumen (verified, not vibes)

- **The trace ordering** (rev 32): screen march first, world field second,
  sky on a true miss - Lumen's chain with the mesh/global SDF stages
  collapsed into one EXACT analytic field.
- **The reason for the screen stage**: the detail source for screen-visible
  geometry (exact silhouettes, real rasterized materials), not a fallback.
- **A radiance memory for the second bounce**: Lumen's two-level radiance
  cache vs our one-fetch reprojection of last frame's gather - same
  principle, different reach.
- **Temporal accumulation with reprojection-validated history**, now with
  normal rejection and a MaxFramesAccumulated-style cap (rev 33).
- **Post-temporal short-range occlusion** (rev 33, off by default).
- **Monte Carlo gather + firefly clamp** (our indirect_clamp plays the role
  the probe prefiltering plays there).

## Where this module deliberately differs (and why)

- **Per-pixel gather instead of screen probes**: probes amortize tracing
  across pixels at the cost of interpolated (flat-ish) indirect light; a
  per-pixel gather pays rays for pixel-fidelity indirect shadows. Our scene
  sizes and the analytic field make the per-pixel end affordable; Lumen's
  open worlds make probes the only answer. Not better - a different point
  on the same curve.
- **The world field is EXACT**: no voxelization, no distance-field texture,
  no global-SDF merge - by construction the trace cannot leak through a
  wall the raster path drew (Lumen's biggest software-mode disease: thin
  walls, SDF expand, over-occlusion). This stage of ours is the one piece
  Lumen cannot match, because our geometry IS an SDF.
- **Hit lighting everywhere** instead of a surface cache: every hit is
  relit with the same textured, shadowed direct lighting the G-buffer pass
  uses (Lumen's hardware-RT "Hit Lighting" mode, as the default rather
  than the luxury option). No card capture exists to go stale or pink.
- **No probe hierarchy / product importance sampling**: the one-fetch
  second bounce is what a per-pixel gather can afford; the hierarchy is
  what probe amortization buys. Recorded as the honest gap.

## What it computes

Per pixel, per frame (gather + spatial filter at HALF resolution, one
target-to-history copy, one bilateral upsample):

1. **Final gather** - `rays_per_pixel` (1-8) cosine-distributed hemisphere
   rays, stratified per-ray, jittered by interleaved gradient noise
   (freezable: `animated_jitter = 0` pins the noise phase for
   deterministic captures), resolved in two spaces:
   - **Screen-space stage** (`screen_trace`, rev 32) - the ray is marched
     through THIS frame's depth buffer (`screen_steps` exponential strides
     from 5 cm to `screen_max_distance`, `screen_refine` bisect steps per
     crossing, `screen_thickness` + 5%/m thickness tolerance). Off = the
     world-only A/B (Lumen's Screen Traces checkbox).
   - **World-space fallback** - a miss (off-screen, behind the camera, the
     march budget spent, thickness guard tripped) sphere-traces the exact
     SDF after the containment escape (three overlaps out of the owning
     occluder proxy).
2. **Hit relight** - an analytic-surface hit (|SDF| < 5 cm) relights by
   `surfaceOutgoingRadianceOwned` (textured, shadowed, emissive-included -
   the chamber's panels keep their light); a mesh hit shades from the
   G-buffer's albedo/normal with the shared `directIrradianceOverPi`. Every
   ray's radiance passes the `indirect_clamp` firefly limit.
3. **Second bounce** (`second_bounce`) - the hit point reprojects into last
   frame's gathered irradiance, fetched once, albedo-weighted,
   depth/normal-confidence-gated.
4. **Miss = sky** - `skyBase` (no sun/moon disks - those are the direct
   pass's keys), scaled by `sky_miss_scale`.
5. **Temporal reuse** - reprojection-validated blend at
   `max(temporal_alpha, min(1 - 1/(frame+1), N/(N+1)))` where N =
   `temporal_max_frames`; `temporal_normal_rejection` adds the G-buffer
   normal cone to the confidence gate.
6. **Short-range occlusion** (`short_range_ao_radius`, default off) - an
   8-tap depth ring at the configured px radius, applied AFTER the
   temporal filter (Lumen's ordering), capped at 75% darkening.
7. **Spatial filter** (`gi_screen_world_denoise.comp`) - a Gaussian
   depth/normal bilateral with a neighborhood-outlier clamp;
   `filter_radius` 1 = the shipped 3x3, 0 = unfiltered A/B, 2-4 = wider
   support. The upsample lifts the filtered half-res result to the
   full-res target depth/normal-aware.

The target holds irradiance/pi (the composite multiplies albedo into it),
scaled by `intensity` AFTER the temporal blend (the history keeps the
unscaled transport, so the scale never compounds through the bounce read).
The alpha channel carries the pixel's view depth - the confidence metric
both temporal consumers share.

## Wiring

- Demand marker: `gi_screen_world_diffuse` (the mod's `capabilities` list
  pulls the module in and attaches the pass - the exact contract
  `pbr_direct_diffuse` follows).
- Family member: `lighting.gi_screen_world`.
- The pass fills the renderer's `indirectRadiance` target; the composite's
  `modeFlags.z` gates the term in automatically (renderer-side, no
  composite edit needed for attach/detach).
- Lights are read through the acquired `LightingCapability`
  (light_uniform.hpp is the single fill definition); the shadow policy
  mirrors the direct pass's `[lighting.pbr]` keys so transport and direct
  shading can never disagree about occlusion.

## Configuration

`[lighting.gi_screen_world]` (every key mirrored in gi_capability.hpp and
the shipped config.toml):

| Key | Default | Maps to (Lumen) |
| --- | --- | --- |
| `enabled` | true | (master switch) |
| `screen_trace` | true | `r.Lumen.ScreenProbeGather.ScreenTraces` |
| `screen_steps` | 24 | `...ScreenTraces.HZBTraversal.MaxIterations` |
| `screen_refine` | 6 | (bisect depth; HZB has no direct analogue) |
| `screen_max_distance` | 24.0 | "Max Trace Distance" (screen half) |
| `screen_thickness` | 0.12 | (linear-step thickness leak guard) |
| `rays_per_pixel` | 2 | final-gather quality (probe octahedron res there) |
| `indirect_clamp` | 8.0 | (firefly clamp; probes prefilter there) |
| `second_bounce` | true | (radiance-cache bounce) |
| `temporal_alpha` | 0.125 | (history floor) |
| `temporal_max_frames` | 64 | `...Temporal.MaxFramesAccumulated` (10) |
| `temporal_normal_rejection` | true | (depth+normal rejection) |
| `short_range_ao_radius` | 0 | `r.Lumen.ScreenProbeGather.ShortRangeAO` |
| `short_range_ao_strength` | 1.0 | (occlusion gain) |
| `filter_radius` | 1 | `...SpatialFilterNumPasses` (probe space there) |
| `max_distance` | 60.0 | "Max Trace Distance" (world half) |
| `march_steps` | 96 | (world SDF march budget) |
| `intensity` | 1.0 | (output scale) |
| `sky_miss_scale` | 1.0 | (sky-light energy on miss) |
| `animated_jitter` | true | (freeze for deterministic captures) |

## Known limits (documented on purpose)

- The relight's sun shadow ignores the terrain heightfield grid (the shared
  relight path is SDF-only - the documented relight_common.glsl limit), so
  terrain self-shadowing enters GI one bounce brighter than direct shows.
  The analytic occluders (buildings, props, characters, the chamber) shadow
  exactly.
- The screen march answers only what the depth buffer holds: rays that
  leave the frustum (or cross a surface the stride undersampled, beyond the
  thickness tolerance) fall back to the SDF trace - the world stage, not
  the screen, owns occluded and off-screen light.
- The screen hit's material split keys on |SDF| < 5 cm at the hit point: a
  mesh standing closer than 5 cm to an analytic surface shades through the
  analytic branch (harmless: both carry the same direct lighting), and a
  grazing analytic hit reconstructed worse than 5 cm from the depth buffer
  shades through the G-buffer branch (loses the emissive term; the chamber's
  panels sit well inside the band).
- `rays_per_pixel` is the cost dial: each ray is a screen march
  (`screen_steps` strides + bisection) and, on a screen miss, a full SDF
  march. 2 is the shipped default; the temporal filter remains the primary
  denoiser for a static camera, and a camera cut (teleport) reprojects to
  garbage confidence and converges within the running-average ramp's first
  frames.
- The second bounce is a screen-space memory, not a field: hit points far
  outside the previous frustum fall back to one-bounce lighting (correct,
  just darker in deep concavities).
- `short_range_ao_radius` reads the HALF-res gather grid's depth (the ring
  works in gather resolution), so its effective footprint is 2x the
  configured px at output resolution.
