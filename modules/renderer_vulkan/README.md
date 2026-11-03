# renderer_vulkan module

The graphics backend and the owner of the frame. The renderer is a Vulkan 1.3 implementation (vulkan-hpp `vk::raii`, dynamic rendering, sync2) that rasterizes the shared `[scene]` world description into a G-buffer, dispatches the attached lighting technique modules against it, composites the radiance layers into the swapchain, copies captures, and presents. It exposes the `RendererCapability` (capability name `renderer`, published in `renderer_capability.hpp` next to this module; the plugin descriptor carries the implementation-bound member name `renderer.vulkan`) and attaches technique modules through an in-tree interface that is deliberately NOT a capability surface (see below), so the kernel and script mods stay backend-agnostic while C++ technique modules get a typed, zero-overhead integration point.

## Contract

`RendererCapability` is intentionally small: `renderFrame()` renders one frame, `waitUntilIdle()` drains the device (used before teardown and before offscreen reads), `requestFrameCapture()` queues the capture path (the screenshot_png module's programmatic entry point), and `validationErrorsObserved()` reports whether the debug messenger saw a validation error — the flag the app frame loop turns into exit code 3. The published header carries no Vulkan vocabulary; the published-header guard (`cmake/capability_header_guard.cmake`, called from this module's `CMakeLists.txt`) fails the configure if it ever does.

## The technique attach point

Lighting technique modules attach passes to the renderer at construct time through `modules/renderer_vulkan/render_interface.hpp`. This header is in-tree and Vulkan-typed on purpose: it is shared by the renderer and the technique modules, both of which are graphics code, while the kernel and the script-facing surface never see it. Each technique declares which radiance layer it writes (direct diffuse, indirect diffuse, indirect specular), so the renderer creates exactly the offscreen targets that are consumed. Which techniques exist in a run is decided by capability demand: each technique provides a demand marker named after the technique and variant it actually delivers (`pbr_direct_diffuse` for the analytic direct pass; `screen_space_radiance_cascades` / `world_space_radiance_cascades` for the radiance-cascades module's two probe placements — see each technique module's README), and is activated exactly when an active mod demands that marker, with the composite order fixed (direct radiance before indirect) as `direct + albedo * (1 - metallic) * diffuseGI + specular + emissive`. One reserved layer-contract spelling completes the set: `indirect_specular` is the name a future reflection technique's marker joins so the direct pass can split the glossy energy with it (see `modules/pbr/README.md`); no shipped module provides it, and a demanded capability with no provider fails startup precisely instead of rendering a silent black screen. Per-technique settings live in `[lighting.<technique>]` sections.

## Frame pipeline

One command buffer per frame, in order: geometry pass, technique dispatches (direct, then indirect), composite into the swapchain image (through the bloom chain when `bloom` is on), capture copy, present.

- **Geometry pass (G-buffer).** Albedo + roughness as RGBA8, normal + metallic as RGBA16F, emissive as RGBA16F, depth as D32. Scene objects come from the shared `[scene]` configuration (`scene_description.cpp`); `plane` is authoring sugar for a thin box so rasterized and traced geometry stay the same primitive.
- **Technique dispatches.** A compute-to-compute memory barrier is emitted between technique records — one technique's storage-image output is the next technique's sampled input, and unordered access here races passes against each other.
- **Composite.** Blends the radiance layers with the material textures, applies `[renderer].exposure` and the `[renderer].tonemap` operator (`aces`, `reinhard`, `none`), and writes the sky for pixels that miss all geometry (one shared sky model in `assets`-independent shader code — `environment_common.glsl`, also consumed by reflection misses).
- **Capture copy and present.** The capture copy is recorded inside the same command buffer and synchronized with the frame fence — the screenshot_png module never races the compositor.

All offscreen images live in GENERAL layout permanently (written as attachments, read as sampled/storage images), so the frame needs access barriers only, no layout churn. The material texture arrays rest in SHADER_READ_ONLY_OPTIMAL for their lifetime. Semaphore ownership follows the two-scope policy documented in `frame_sync_policy.hpp`: acquire semaphores are owned per frame slot, render-finished (present) semaphores per swapchain image — `frame_sync_policy.hpp` encodes the policy in one place. Frame EXECUTION is serialized on the host: transient targets and the host-written uniforms are single-buffered, so each frame waits for both in-flight fences before any uniform write or command recording — barriers order accesses within a submission, only fences order submissions.

## Materials and textures

Scene objects reference PBR texture sets: `[scene.objects].texture` names a folder under `[renderer].texture_directory` as `<name>/` with `Color.png`, `NormalGL.png`, `Roughness.png` and optional `Metalness.png`. The engine ships no content: the directory default comes from the content package (`mods/test_scene/config.toml` points it at the package's own `assets/textures/`, CC0 sets from ambientCG), and a scene that names texture sets with no directory configured fails with a precise error. The renderer uploads the referenced sets as two 2D texture arrays with a full mip chain — albedo in sRGB, data packed as R roughness / G metallic / B,A normal XY — plus a per-primitive material table. The normal map's X and Y are decoded from the RED and GREEN channels of `NormalGL.png` respectively. The decode itself is parallel by default: each set's PNG maps are CPU-bound and per-set independent, so the startup decode fans out across the machine's cores through the kernel's `parallelFor` (`kernel/include/engine/kernel/parallel_for.hpp`) and joins before any shared state — the cross-set resolution contract, the GPU upload — is checked in set order on the main thread. A failed decode (a set whose maps disagree with its own color map) still fails startup precisely.

UVs and tangent frames come from ONE shared parameterization (`scene_common.glsl`) consumed by the geometry pass and every SDF tracing consumer, so a GI bounce or a reflection ray landing on a point samples exactly the texels the camera sees; `uv_scale` is world meters per texture tile. The unit box mesh spans +-1 and the model matrix scales by the primitive's HALF EXTENTS, so a scaled unit box occupies exactly the box the analytic SDF describes — a half-unit mesh would render a half-size room inside the SDF's closed room: sky through the walls, GI, shadows and reflections all disagreeing. Under ROTATION the agreement is equally exact: the SDF converts world points to primitive-local space through `worldToLocalY`, the inverse of the model matrix's rotation; the forward rotation applied there would trace every rotated box as a mirrored ghost at `-rotationY`.

The upload paths for the texture arrays are explicit — every upload names its array layer, and the texel channels are filled per plane rather than through one interleaved fill, so each channel is written exactly where it belongs.

## Camera

The renderer integrates the free-fly controller (`camera_controller.hpp`) every frame from the `InputCapability` when present: the configured `[scene.camera]` pose is the STARTING pose, then WASD flies, the mouse looks while the cursor is captured (Esc releases, F or left-click re-captures), Space/E rise, Ctrl/Q sink, Shift boosts, and the wheel scales the fly speed; camera state is re-uploaded per frame. The input and scene capabilities are OPTIONAL DECLARATIONS (`optionalCapabilities`, no hand-rolled try/catch): minimal builds without them render the static `[scene]` world from a static camera, and when the modules ARE present the resolver orders them before this module, so the resolves are a property of the assembly. The fly camera's keys are data too: each axis also accepts a semantic `[input.actions]` entry (`camera_forward`, `camera_back`, `camera_left`, `camera_right`, `camera_up`, `camera_down`, plus the already-wired `boost` and `release_cursor`) that replaces the fixed WASD/Shift/Escape keys when defined — a gamepad binding flies the camera with zero code, and absent keys keep the historical bindings. `free_fly = false` pins the camera to the configured pose. While the ui_imgui module's overlay claims the pointer (`UiCapability::wantsMouse`, the modal rule) the controller pauses its look/fly handling and drains the accumulators, so a debug window never fights the camera for motion. Camera keys and their meanings live in the `[scene]` section of `docs/configuration.md`.

## The mesh path

Registry objects of the mesh type instance a registered mesh (`SceneCapability::addMesh`); the static `[scene]` table stays analytic (box, sphere, plane). New handles upload ONCE through the same staging discipline as every other bulk resource (device idle on the serialized frame path) and draw in the geometry pass as REAL TRIANGLES with the engine's one interleaved vertex layout (position xyz + normal xyz, `meshFloatsPerVertex` = 6 floats): one vertex input state serves unit primitives and registered meshes alike. The analytic consumers (GI, reflections, shadows) never trace the triangles - each mesh object contributes its ORIENTED BOUNDING BOX as an exact box primitive carrying the object's material (the proxy contract, `scene_capability.hpp`), so rasterized and traced geometry agree by construction and the UV parameterization stays the shared one.

## The overlay seam

The module publishes a second capability member, `renderer.vulkan.overlay` (`renderer_overlay_capability.hpp`): the in-frame UI recording seam. One registration (`setOverlayRecorder`), one per-frame virtual call (`recordOverlay`), invoked after the composite pass wrote the swapchain image and before the capture/present transitions - so a screenshot of a frame with a debug UI shows the debug UI. The capability header stays vendor-neutral (opaque handles only, guarded like every published header); the Vulkan-specific handle bundle the ui_imgui module initializes against is documented field-for-field in the header. With no UI module in the run the recorder pointer stays null and the seam costs one branch per frame.

## Configuration

| Key | Type | Default | Meaning |
|---|---|---|---|
| `enable_validation` | bool | `false` | Vulkan validation layers plus debug messenger. `false` keeps interactive runs fast: the layer taxes every dispatch heavily on the CPU, and the app loop polls window events only between frames — multi-second frames starve the message pump and the OS marks the window "not responding". Diagnostics-oriented and automated runs re-enable it so VUID violations fail through exit code 3 |
| `fail_on_validation_error` | bool | `false` | abort the frame loop with a diagnostic at the first observed validation error instead of continuing (development runs; the app frame loop surfaces the same condition through its exit code without this key) |
| `vsync` | bool | `true` | present mode selection |
| `prefer_discrete_device` | bool | `true` | device selection: `true` scores discrete GPUs highest, `false` prefers integrated GPUs; CPU/software devices always score lowest |
| `max_frames_in_flight` | integer | `2` | frame slots (command buffers, fences, acquire semaphores). Frames do NOT overlap on the GPU: transient targets and host-written uniforms are single-buffered, so the loop serializes execution by waiting both fences before recording; the second slot only decouples recording from the presentation engine |
| `shader_directory` | string | `"shaders"` | where compiled SPIR-V is loaded from (resolved relative to the executable; module build output lands in `runtime/shaders`) |
| `texture_maps.color` | string | `"Color.png"` | per-set color/albedo file name (the ambientCG layout) |
| `texture_maps.roughness` | string | `"Roughness.png"` | per-set roughness file name (packed into the data layer's R channel) |
| `texture_maps.metalness` | string | `"Metalness.png"` | per-set metalness file name (data layer G channel) |
| `texture_maps.normal` | string | `"NormalGL.png"` | per-set normal file name (data layer BA channels) |
| `texture_directory` | string | (empty) | where scene texture sets are loaded from; shipped empty because content belongs to packages — the content mod's defaults provide it, and a scene referencing texture sets without a directory fails precisely |
| `exposure` | float | `1.15` | HDR exposure applied in the composite pass |
| `tonemap` | string | `"aces"` | tonemap operator: `aces`, `reinhard`, `none` |
| `bloom` | bool | `true` | the bloom post-chain: bright pass, separable Gaussian, add-back before the tonemap. Set `false` for the scene as authored (changing it is a restart; see the live-reload note below) |
| `bloom_threshold` | float | `1.0` | radiance lifted into the bloom chain only above this value |
| `bloom_strength` | float | `0.15` | how much of the blurred chain the final stage adds back |

Vulkan 1.3 features are enabled explicitly, not assumed; the graphics pipeline creation path asserts every stage it configured actually exists, because a silently dropped vertex stage produces an empty pipeline. A failing shader compile is a hard startup error — a silent failure would keep the previous frame's visuals on screen.

Live reload: `exposure`, `tonemap`, `bloom_threshold` and `bloom_strength` are composite push-constant values — they re-read when the kernel's `ConfigurationReloadedEvent` names them (see `docs/configuration.md`, "Live configuration reload"), and the very next frame consumes them with no GPU resource created or destroyed. Keep-last-good on invalid values (an unknown tonemap mode logs and keeps the previous one). The `bloom` toggle is deliberately NOT reloadable: it selects pipeline structure at initialize; changing it is a restart.

## Files

One state struct, several translation units split by concern (the module-private `renderer_state.hpp` is the only place they meet):

- `renderer.hpp` — the module interface (capability implementation, technique attach point)
- `renderer_capability.hpp` — the published capability surface every consumer includes
- `renderer_state.hpp` — module-private shared state: named constants, GPU-side structs, `RendererState` with every method declaration
- `renderer.cpp` — the module lifecycle: construct/start/stop, capability pointer, technique attach and ordering, frame event publish
- `renderer_device.cpp` — instance, physical-device selection, device, surface, swapchain, frame images and layout transitions
- `renderer_scene.cpp` — scene geometry upload, texture-array loading/mip chain, environment and camera uniform buffers
- `renderer_pipelines.cpp` — scene and composite pipelines with their descriptor sets
- `renderer_frames.cpp` — command/sync objects, capture buffers, per-frame command recording (barriers, geometry pass, techniques, composite) and the frame loop phases
- `render_interface.hpp` — the technique attach point (in-tree, Vulkan-typed)
- `camera_controller.hpp` — the free-fly camera controller (consumer of `InputCapability`)
- `frame_sync_policy.hpp` — the semaphore ownership policy for the render loop
- `vulkan_utilities.hpp` — Vulkan helpers shared with the lighting technique modules (shader bytes, memory type, samplers, mapped buffers)
- `scene_description.hpp` / `scene_description.cpp` — `[scene]` parsing into primitives, materials, lights
- `mesh_generation.hpp` / `mesh_generation.cpp` — the unit-mesh contract: primitive geometry generated as `meshFloatsPerVertex` interleaved floats, the same layout registered meshes upload
- `scene_math.hpp` — shared scene-side math on GLM (perspective with Vulkan's [0, 1] depth, lookAt, inverse, model transforms); glm is acquired through the `engine_glm_bindings` wrapper, module-internal, never on a published capability header
- `light_uniform.hpp` — light upload structures shared with the lighting modules
- `shaders/` — GLSL sources: `scene.vert`/`scene.frag`, the bloom chain (`scene_bloom_extract.comp`, `scene_bloom_blur.comp`), `scene_composite.vert`/`scene_composite.frag`/`scene_composite_final.frag`, `common/` (`scene_common.glsl`, `environment_common.glsl`, `lighting_common.glsl`, `materials_common.glsl`, `relight_common.glsl`); compiled via the shared `engine_compile_module_shaders` CMake helper into `runtime/shaders/`
- `module_plugin.cpp` — the static plugin descriptor (name, factory, provided capabilities)
- `CMakeLists.txt` — target and option wiring (`ENGINE_BUILD_MODULE_RENDERER_VULKAN`)
