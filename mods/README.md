# Mod authoring guide

This guide explains how to build a mod for the engine: what a package looks like, what the kernel does with it, and what the scripting bridge allows. It is the entry point referenced from the engine README, the architecture overview and the scripting module documentation.

## What a mod is

A mod is a directory containing a `mod.toml` manifest plus its content: Lua scripts, a TOML scene, and (optionally) packaged assets under `assets/`. The kernel discovers mod packages on disk: EVERY subdirectory of the configured mod directory (`[engine].mod_directory`, default `mods`) is a mod package of the application - there is no list and nothing to register. A subdirectory without a `mod.toml` cannot be read as a package and is reported as a broken install (one warning line, the directory skipped) instead of being silently ignored; loose files are not packages. During the build, the root CMakeLists copies every `mods/<id>/` directory that contains a `mod.toml` into `build/runtime/mods/<id>/`, including its `scene.toml`, `config.toml`, `*.lua` files and `assets/` content. The engine itself ships no content; everything the demo app shows comes from the `test_scene` package.

Two kinds of mods exist:

1. **Script mods** (`type = "script"`): the manifest names a host capability (`host_capability`, usually `scripting`) among its required capabilities; the kernel hands the package to that capability's provider, which runs the script inside an isolated Lua state. This is the normal way to add gameplay behavior.
2. **Native mods** (`type = "native"`): a CMake target that links the kernel and exports the same plugin entry point a module uses (`kernel/include/engine/kernel/module_api.hpp`). The manifest's `entry` is the library base name (`lib<entry>.so` / `<entry>.dll`); a package that ships its own `CMakeLists.txt` is discovered by the root build the same way module directories are, and its library lands in the package's runtime directory through `LIBRARY_OUTPUT_DIRECTORY` - the same plugin scanner that finds engine modules loads it. Use this for content or features that need compiled code.

Mods never name a module: they name the **capabilities** they require, and the dependency graph picks the providers. Which providers win when several implementations of one capability family are present is decided by the `[providers]` section of the configuration. The capability directions are one rule short of symmetric (see `docs/architecture.md`, "Capability directions"): **a mod consumes capabilities from modules AND from other mods; only a module consumes module capabilities.** A native mod becomes a PROVIDER by declaring `provides` (below) - other mods may then require and acquire those capabilities, ordered before them by the graph; a script mod consumes only.

## The manifest: `mod.toml`

The manifest is authoritative for the mod's identity and wiring; the kernel validates it before touching anything. Every field of the schema (from `kernel/include/engine/kernel/mod_manifest.hpp`):

| field              | required | meaning |
| ------------------ | -------- | ------- |
| `id`               | yes      | package id, `[a-zA-Z0-9_]`, unique |
| `version`          | no       | the package's own version, informational identity the graph report prints (defaults to `"0.1.0"`) |
| `engine_api`       | yes      | must match the engine's interface version (currently `1`) |
| `type`             | yes      | `"native"` or `"script"` |
| `entry`            | yes      | script: script file relative to the package; native: library base name |
| `display_name`     | no       | defaults to `id` |
| `description`      | no       | free text |
| `author`           | no       | free text |
| `capabilities`     | for graph| required capabilities (graph edges); each also derives its `capability.<name>` permission grant |
| `provides`         | no       | capabilities this native mod PROVIDES to the rest of the assembly (mods may consume capabilities from modules and mods alike). Native mods only - a script mod has no instance that could implement a capability, so declaring `provides` on one is a manifest error. The compiled manifest must agree with the list: the same drift check a module's descriptor and manifest keep between them, and the declaration is what the graph and the `--validate` dry run resolve from |
| `optional_capabilities` | no  | capabilities used when a provider is active, never demanded: a missing provider is a valid state, an existing one is ordered before the mod, and each entry still derives its `capability.<name>` grant. The mod probes availability at construct time (`ComponentContext::isCapabilityAvailable`) |
| `host_capability`  | script   | which ONE required capability's provider hosts the script runtime |
| `permissions`      | no       | extra permission grants beyond the derived ones (deniable in configuration) |
| `requires`         | no       | component names to order after (any mod type) |
| `config_keys`      | no       | configuration keys the mod reads, `"<section>.<dotted path>"` each; feeds the known-key registry so startup validation recognizes them |
| `scene`            | no       | world content file the mod ships, relative to the package (see "Scenes") |

The shipped `mods/test_scene/mod.toml` is the reference example. A minimal behavior mod:

```toml
id = "my_mod"
version = "0.1.0"
engine_api = 1
type = "script"
entry = "main.lua"

display_name = "My Mod"
description = "Spawns a bouncing sphere at startup."
author = "you"

host_capability = "scripting"
capabilities = ["scripting", "logging", "scene"]
permissions = []
```

The `capabilities` list is the demand root of the whole run: every capability a mod requires pulls its provider module (and that module's own requirements pull further providers transitively). A capability nobody requires stays dormant - its plugin binary is never even instantiated. `optional_capabilities` deliberately stay out of this: an optional use never pulls a provider into the run. The demo run works this way: the `test_scene` package's `capabilities` list is what pulls every module the demo pipeline needs into the run, and exactly those modules load.

## Permissions

Every capability acquisition is gated by the permission system. Requiring a capability in the manifest derives its `capability.<name>` grant, so a well-declared mod needs no second list; `permissions` adds grants beyond those (for example `filesystem.write` for a mod that saves files), and any grant can be denied per component in configuration:

```toml
[permissions]
denied = [["my_mod", "capability.screenshot"]]
```

A denied permission fails startup with a precise diagnostic naming the component and the permission. Enforcement lives at that startup boundary and in the sandbox, not on the call path: a script mod runs in a state that opens only the libraries the host whitelists, and everything a script can reach goes through the bridge surface the scripting host installed - a bridge call runs through pointers its provider captured at construct, so the per-call cost is the call itself (no per-call grant re-check). What a mod DECLARES still decides what the kernel pulls into the run and what its component may acquire through its own context.

## Risk advisories and the trust gate

Before any mod or module code is used, the kernel risk-assesses every active component on structural signals: the package runs native machine code (native mods), it requests permissions outside the `capability.*` namespace (the `permissions` list above - `filesystem.write` and friends), or it acquires a broad set of capabilities. Every flagged component is announced as a warning on every run - install third-party mods knowing exactly what they asked for.

Warning is not permission. A flagged mod is USED only when the assembly's `[trust]` section covers it (`trustMods = ["my_mod"]` names the package, `trustAllMods = true` covers every mod) or the user agrees at startup: an interactive run asks once per launch - `Proceed with these components? [y/N]` - and a headless run (scripts, CI) cannot ask, so it fails with the trust keys as the instructions. The question is answered before any of the mod's construct or start code runs, and before a native mod's library is even mapped. Standing decisions belong in `[trust]`, never in prompt answers; the full gate contract (including why a mod's own `config.toml` cannot define `[trust]`) is documented in `docs/architecture.md`, "Risk advisories and the trust gate", and the keys in `docs/configuration.md`.

## Scenes: `scene.toml`

A package's optional `scene` key points at a `[scene]`-only TOML file. The kernel enforces that the file defines ONLY the `[scene]` section, and applies it wholesale - partial configurations can never merge. Precedence: an explicitly staged scene file > the assembly's own `[scene]` section in `engine.toml` > a single claimed content mod's scene file > code defaults. Two or more mods claiming a scene with no explicit world source is a startup error (the world would be ambiguous).

The section's shape (see `mods/test_scene/scene.toml` for a fully tuned example): `[scene]` carries the flat `sky_radiance` plus the objects array; `[scene.sky]` the vertical gradient and sun disk; `[scene.camera]` the starting pose and free-fly tuning; `[scene.sun]` the directional light. Objects are spheres and boxes (a `plane` is normalized into a thin box at parse time so rasterized and traced geometry can never disagree):

```toml
[scene]
sky_radiance = [0.22, 0.30, 0.48]

[[scene.objects]]
type = "sphere"
position = [0.0, 1.0, -0.5]
size = [1.0, 0.0, 0.0]              # sphere: radius in x (y/z ignored)
albedo = [0.9, 0.7, 0.2]
metallic = 1.0
roughness = 0.15

[[scene.objects]]
type = "plane"                       # convenience type: normalized into a thin box
position = [0.0, 0.0, 0.0]
size = [3.5, 0.01, 3.5]              # half extents
texture = "WoodFloor055"
uv_scale = 0.9
```

Through the SCRIPT bridge (`engine.spawn_object`) the shape is friendlier: `radius = 0.5` for spheres and `half_extents = {0.5, 1, 0.5}` for boxes are accepted there and normalized into the same descriptor.

The kernel's `validate()` checks a package without opening a window or a backend - and without loading a native mod's library (the dry run resolves the graph from the manifests alone, so a native package's `provides` list must be complete in `mod.toml`): failure prints the offending file, table and key.

The static `[scene]` world is the INITIAL state. The `scene` capability is the runtime object registry: scripts and native mods spawn, transform and remove objects through it, and the renderer plus the lighting techniques rebuild their GPU data incrementally (see `modules/scene_registry/README.md`).

## Scripts: the Lua bridge

A script mod runs inside an isolated Lua state created by the scripting module. The script's top level executes once at startup, after every module constructed; per-frame behavior goes through `subscribe_frame_begin`. The bridge lives under the `engine` table:

**Logging** (requires `logging`): `engine.log_trace(message)`, `engine.log_debug(message)`, `engine.log_information(message)`, `engine.log_warning(message)`, `engine.log_error(message)` - routed to the logging capability with the mod's component name.

**Input** (requires `input`): `engine.is_key_down("W")`, `engine.is_mouse_down("left")` - polled state, safe to call from any callback; `engine.consume_mouse_delta()` and `engine.consume_scroll_delta()` return the accumulated motion since the last call (two numbers each, x right / y down); `engine.window_size()` returns `[width, height]`.

**Scene objects** (requires `scene`): `engine.spawn_object(table)` returns a handle - the table accepts `type` (`"sphere"` / `"box"`), `position`, `radius` or `half_extents`, `rotation` (radians around Y), `albedo`, `roughness`, `metallic`, `emissive`, `uv_scale`; `engine.remove_object(handle)`; `engine.set_object_position(handle, x, y, z)`; `engine.set_object_rotation(handle, radians)`; `engine.set_object_material(handle, table)` (same material fields as spawn); `engine.object_count()`. Changes rebuild the GPU scene data incrementally at the start of the next frame (see the scene_registry module README for the cost model).

**Lights** (requires `lighting`): `engine.set_sun_light(table)` with `direction` (towards the scene), `color`, `intensity`; `engine.clear_sun_light()`; `engine.add_point_light(table)` with `position`, `color`, `intensity`, `radius` returns a handle; `engine.remove_point_light(handle)`.

**Camera** (requires `renderer`): `engine.camera_position()` and `engine.camera_look_at()` return the current free-fly camera pose (three numbers each); `engine.set_camera_position(x, y, z)` and `engine.set_camera_look_at(x, y, z)` move it.

**Tweens** (requires `animation`): `engine.animate_object(handle, table)` and `engine.animate_light(handle, table)` start a tween and return its handle; `engine.cancel_tween(handle)` stops one. The table fields: `channel` (`0` position, `1` rotation, `2` emissive for objects; `3` intensity, `4` radius for lights), `from_position`/`to_position` (position tweens), `from_value`/`to_value` (scalar channels), `duration` seconds, `delay` seconds, `easing` (`0` linear, `1` ease-in, `2` ease-out, `3` ease-in-out), `mode` (`0` once, `1` loop, `2` ping-pong). A tween writes through the scene registry every frame, so the renderer rebuild picks it up with no extra path.

**Timers** (always available, self-contained in the scripting module): `engine.create_timer(interval_seconds, repeating, callback)` returns a handle; `engine.cancel_timer(handle)`. Timers fire between frames; a timer error is logged and the timer cancelled instead of taking the mod down.

**The frame clock** (always available, self-contained in the scripting module): `engine.frame_delta_seconds()` returns the wall-clock delta of the most recent loop iteration and `engine.frame_elapsed_seconds()` the accumulated time since the loop began (both 0.0 before the first tick). Scripts cannot measure time themselves (`os`/`io` are never opened), so the host answers from its own heartbeat subscription - the same numbers every simulation module ticks from, headless included.

**Screenshots** (requires `screenshot`): `engine.request_screenshot()` queues one capture of the next rendered frame.

**glTF import** (requires `asset`): `engine.import_gltf(path, fidelity)` imports one glTF 2.0 file (the `.gltf` JSON form or the `.glb` binary container) into the registry and returns the spawned object count. Fidelity is `"mesh"` (the default - real triangles registered as a registry mesh plus one mesh object per primitive) or `"bbox"` (the bounding-proxies contract). A failed import returns 0 and logs precisely.

**Persistence** (requires `persistence`): `engine.save_scene(path)` writes every live object to one TOML file, `engine.load_scene(path)` spawns the rows of a file back as NEW objects (returns the count; mesh objects restore by registered mesh name, so re-import the model before loading a save that references it), and `engine.clear_scene()` removes every live object (meshes stay - they are assets, not instances).

**Physics** (requires `physics`): `engine.add_physics_body(object_handle, table)` turns a scene object into a dynamic body and returns its handle - the table accepts `restitution`, plus the registry object's shape supplies the collider; `engine.remove_physics_body(handle)`; `engine.set_body_velocity(handle, x, y, z)` and `engine.body_velocity(handle)` (returns three numbers); `engine.set_gravity(x, y, z)`; `engine.body_count()`; `engine.set_body_sleeping(handle, sleeping)` and `engine.is_body_awake(handle)`; `engine.raycast(ox, oy, oz, dx, dy, dz, max_distance)` returns `nil` or a table with `object`, `position` (three numbers) and `normal` (three numbers) for the nearest hit.

**Audio** (requires `audio`): `engine.play_sound(path, volume)` plays fire-and-forget; `engine.set_master_volume(v)`; `engine.play_sound_3d(path, x, y, z, volume)` positions one shot spatially; `engine.set_listener_position(x, y, z)` and `engine.set_listener_orientation(fx, fy, fz, ux, uy, uz)` place the listener the spatialization is heard through. A missing device degrades to silent no-ops (one logged warning), never a script error.

**Text input** (requires `input`): `engine.subscribe_text_input(callback)` receives the platform's layout-aware character stream as Unicode codepoints (`"Shift+a"` arrives as one codepoint, not two key events).

**Actions** (requires `input`): `engine.action_strength("jump")` returns the strongest currently-held binding of the named `[input.actions]` action (0.0 for an unconfigured name) - the bind-by-meaning query; the shipped demo content binds `boost` and `release_cursor`, and the free-fly camera consumes the semantic `camera_*` actions when an assembly defines them.

Key events (requires `input`): `engine.subscribe_key_pressed(callback)` and `engine.subscribe_key_released(callback)` receive the key name (`"W"`, `"Escape"`, ...); `engine.subscribe_frame_begin(callback)` receives the frame index, with the iteration's delta and elapsed seconds as optional second and third arguments (`function(frame_index, delta_seconds, elapsed_seconds)` - a one-argument handler keeps working, Lua ignores extra arguments). The frame tick is the composition root's loop heartbeat (`EngineFrameEvent`), so per-frame script behavior runs in a headless assembly exactly like a rendered one. `engine.subscribe_fixed_update(callback)` (self-contained) receives the constant step delta whenever the assembly configured `[engine].fixed_update_hz` - the deterministic lane a simulation script advances by (see `docs/architecture.md`, "The frame clock"). Subscriptions are released automatically when the mod's state is destroyed.

Scripts must not error on the hot path: an error in a frame handler is logged and the handler skipped for the rest of the run, but an error at load time fails startup with a precise report naming the mod and the script.

## Configuration and assets

A package may ship `config.toml` - its defaults layer under the assembly's `engine.toml` (the layer stack is documented in `docs/configuration.md`). Content assets live under `assets/` inside the package and reach the runtime layout with the rest of it (the build copies the package whole); the modules that consume them open the files - the demo package's shipped defaults point the renderer at its texture directory, and the demo script passes the package-relative dragon path to `engine.import_gltf`.

## Packaging checklist

1. `mods/my_mod/mod.toml` - the manifest (identity, requirements, host).
2. `mods/my_mod/main.lua` - the entry script (script mods).
3. `mods/my_mod/scene.toml` - optional world content, referenced by `scene`.
4. `mods/my_mod/config.toml` - optional shipped configuration defaults.
5. `mods/my_mod/assets/` - optional content assets (textures, sounds).
6. Rebuild (or re-run the build) so the packaging rule copies the package into `build/runtime/mods/my_mod/`.
7. `validate()` - the kernel's dry run; success means the package is well-formed.

## Portability

Script mods are held to a portability contract: everything under the `engine` table is implementation-, platform- and vendor-agnostic, so a script cannot tell whether the window behind it is GLFW or something else, and whether the renderer is Vulkan today or another API tomorrow. Native (C++) mods are not held to this: they may consume engine internals directly, at the cost of portability. The capability headers published by each module (`*_capability.hpp`) are the mod-facing surface; the per-module purity guard in every module's CMakeLists enforces that these headers stay free of platform and vendor includes mechanically.

## Kernel surfaces a mod can use

Beyond capability acquisition, the kernel exposes these component-facing surfaces a native mod (or a module) can reach through its `ComponentContext` and the event bus:

- **Event priorities.** Every subscription optionally takes a priority (higher dispatches first; equal priorities keep subscription order, which is what the plain subscription produces). Ordering a reaction around another component's - "drain capture completions BEFORE the consumer of the saved files" - is a subscription-time statement, never a re-registration dance. Dispatch cost is unchanged: the handler vector is priority-ordered at subscribe time (construct phase, off the frame path).
- **Deferred event delivery.** `channel<Event>().publishDeferred(event)` (or the bus-level `publishDeferred`) queues the event on the main thread and publishes it at the composition root's flush point - once per frame, next to the scheduler tick. FIFO per channel; an event deferred from inside a flush lands in the next flush. This is how a handler answers an event with an event of the same type without re-entering the scan it is running in, and how a producer batches notifications into one visible point of the frame. A run that defers nothing pays one atomic check per frame.
- **Worker-to-main event handoff.** `context.eventBus().createWorkerQueue<Event>()` (construct time, main thread) returns a shared queue whose `enqueue` any thread may call and whose flush the frame-point drives - the events themselves cross the thread boundary, so a worker that finishes a decode enqueues its done-event and never writes a capturing completion lambda. Release the shared_ptr in the mod's stop phase, like any subscription.
- **Multi-provider acquisition.** `acquireEachCapability(name)` (typed: `acquireEachCapability<T>()`) resolves EVERY active implementation of the family - exact providers first, then family members - instead of forcing the assembly to pick one in `[providers]`. This is what turns a family into an extension point several mods can fill at once: a consumer that wants every scene decorator takes the plural form; zero providers resolve to an empty vector, so a plural request degrades gracefully. The permission model is the singular acquire's: the same `capability.<name>` grant covers the whole resolution.
- **Non-throwing acquisition.** `tryAcquireCapability<T>()` answers "is it there" in one walk - the implementation pointer when exactly one provider resolves and the permission is granted, nullptr otherwise, never an exception for absence or ambiguity. The natural form for opportunistic consumers that degrade when a provider is missing; a provider that declares the capability but returns no implementation still throws (a contract breach must be visible).
- **The main-thread scheduler.** `context.scheduler().scheduleAfter(seconds, task)` / `scheduleEvery(seconds, task)` return RAII handles (`ScheduledTask`) that cancel on release; the composition root ticks the scheduler every frame-loop iteration, whether or not a renderer publishes frame events. The timing twin of the async pool below - and the right tool for periodic work a mod would otherwise hand-roll around a frame subscription.
- **Live configuration reload.** With `[engine].config_reload_poll_seconds` > 0, the kernel re-assembles the configuration on file changes and publishes `ConfigurationReloadedEvent` carrying exactly the changed dotted key paths (declared in `component_interface.hpp`; see `docs/configuration.md`, "Live configuration reload"). A mod subscribes, re-reads the keys it owns from its context (keep-last-good on invalid values - a handler on the live bus must not throw), and ignores the rest: a reload costs nothing for a mod that does not subscribe. The `ConfigurationWatchHub` utility (`kernel/include/engine/kernel/configuration_watch.hpp`) wraps the whole pattern - one watch per key, one subscription for all of them.
- **The async worker pool.** `AsyncTaskDispatcher` (`kernel/include/engine/kernel/async_dispatcher.hpp`) is the kernel utility for work that must leave the frame path (file IO, decode): `post` / `postWithCompletion` run work off-thread and re-enter the single-threaded world through `drainCompletedTasks` on the frame path; `start()` sizes the pool from the machine's hardware threads (parallel by default - independent batch tasks spread across the cores), and `start(1)` pins the historical single worker for ordered tasks (see `docs/architecture.md`, "Kernel utilities"). A mod that owns a dispatcher instance obeys the same contract the modules do: worker threads never touch GPU objects and never publish on the bus. For one coarse CPU-heavy batch (decode this list of images now, block until done), the kernel's `parallelFor` (`kernel/include/engine/kernel/parallel_for.hpp`) is the fork-join shape: no dispatcher lifecycle, results complete when the call returns.

## Extensibility

The capability-family pattern (`[providers]` swapping) keeps the provider set open: a physics provider stepping scene bodies, an animation provider tweening object transforms and lights, an audio provider playing into the event bus, an asset provider importing glTF content into the scene registry — each slots in as another family member without touching existing mods. Module hot-swap stays out of scope by design: the ABI gate (kernel refuses any plugin whose reported ABI version differs) trades in-place swapping for the absence of an entire mismatched-vtable bug class.
