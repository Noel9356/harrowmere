# Configuration reference

Default file: `config/engine.toml`, resolved relative to the executable. The executable takes the optional command line documented under "Command line" below (`--config`, `--set`, `--scene`, `--mods-dir`, `--validate`, `--print-graph`); everything a run can vary without one lives in the configuration layers below. Parsed with toml++, so type errors report file and line. Startup performs the full load/resolve/permission pass and warns about keys that no component declares; the kernel's `validate()` runs the same pass without opening a window (an embedder's dry run).

## Where the settings live

The engine's configuration is distributed: every module owns its settings and ships them next to its code, and the assembly file only owns what an assembly owns.

| Layer | File | Owns |
|---|---|---|
| assembly configuration | `config/engine.toml` | runtime layout, provider wiring, permission policy, and (optionally) overrides of anything below |
| module defaults | `modules/<name>/config.toml`, shipped by the modules that carry defaults | the module's own settings |
| mod defaults | `mods/<id>/config.toml`, per active package | the package's content-scale settings and bindings |

`config/engine.toml` deliberately defines no module sections: every module runs on its own shipped defaults without a single line in the assembly file. To tune a module for one assembly, add its section to `engine.toml` — anything written there wins over every defaults file. To change a module's out-of-the-box behavior everywhere, edit the module's own `config.toml`. The header of `config/engine.toml` lists where each module's settings live.

## The layer stack

The effective configuration is assembled from layers, in decreasing precedence:

| Layer | Provided by | Merge rule |
|---|---|---|
| staged overrides | assignments a composition root stages through the kernel before `construct()`; the shipped executable stages none | replaces; highest authority |
| engine configuration | `config/engine.toml` | replaces |
| scene file | the world source: a composition root's explicit scene file, or a content mod's declared scene file when the assembly defines no `[scene]` of its own (see below) | replaces the whole `[scene]` section, nothing else |
| module defaults | `modules/<name>/config.toml`, one shipped by each module that carries defaults | fills gaps only |
| mod defaults | `<mod package>/config.toml`, per active package | fills gaps only |
| code defaults | each component's `value_or(...)` in source | reached when no layer above provided the key |

Rules the stack guarantees:

- **The engine configuration always wins over shipped files.** A module's `config.toml` or a mod's `config.toml` can never override a value the assembly provides; defaults only FILL keys nobody above them set. Between two defaults files providing the same key, the first merge wins — module defaults before mod defaults, each group in sorted name order.
- **Tables deep-merge; arrays and `[[scene.objects]]` rows replace wholesale.** A higher layer that sets `[[scene.objects]]` replaces the entire object list, never appends to it. That is what keeps scene authoring predictable: the objects a run uses are the objects the winning layer lists.
- **The scene file owns `[scene]`, and the world has exactly one source.** Precedence: an explicitly staged scene file (a composition root's explicit intent) > the assembly's own `[scene]` > an installed content mod's declared scene file (`scene = ...` in `mod.toml`) > code defaults. A scene file swaps the world section out from under the assembly configuration wholesale: a key the assembly's `[scene]` had is gone unless the scene file lists it too. A scene file may define ONLY `[scene]` (sub-tables and array-of-tables rows included); any other top-level key, or a missing/`[scene]`-as-value, is a precise startup error. This is how an assembly replaces the shipped demo world: the content mod's demo objects do not leak into your world. Content mods may coexist freely: with an explicit world source their claims are simply not applied (a warning says so), and only claims that would decide the world must be unambiguous — two or more active claims with no explicit source fail startup with a report naming every claim. World content never arrives through a defaults layer: a `config.toml` that defines `[scene]` fails startup with a precise error, because per-key default merging would drip scene values into configurations that did not ask for them (a sealed room whose zero sky must stay zero no matter what content is installed).
- **Staged overrides are absolute.** A composition root stages `section.path=value` assignments before any backend starts (a malformed assignment fails right there) and they are applied after every other layer. The shipped executable stages nothing — an assembly's world is what its files say — but the layer exists so an embedder can vary a run without rewriting files. Keys are dotted paths of bare segments (`scene.sun.intensity`); values use TOML syntax — numbers, booleans, quoted strings, arrays, inline tables — and any raw value TOML rejects becomes a literal string, so `window.title=My Room` works without quotes.
- **The stack is assembled once for startup — and reloadable live, explicitly.** Nothing re-reads a file unless the run asks for it: with `[engine].config_reload_poll_seconds` > 0 the frame loop stats the assembly file at that interval, and a changed mtime makes the kernel re-assemble every layer (assembly, module defaults, mod defaults, scene file, staged overrides), diff the effective values and publish `ConfigurationReloadedEvent` with exactly the changed key paths. Components that subscribe re-read their own keys (keep-last-good on invalid values); components that do not subscribe keep their startup values. World content ([scene]) is re-merged for consistency but its changes are reported as restart-required, never applied live. A reload never re-runs mod discovery: it changes values, not the component graph.

### Shipped defaults

Every module that carries tunables ships a `config.toml` (its shipped defaults; a module without one runs on its code defaults, and each module's README documents which applies) and a mod package may carry one next to its `mod.toml`; the build copies both into the runtime layout (`modules/<name>/config.toml` lands in `config/modules/<name>/config.toml`) and the kernel merges them as the two defaults layers above. Defaults files may only define sections, and never the assembly-owned ones (`[engine]`, `[providers]`, `[permissions]`, `[diagnostics]`, `[trust]`) — a package that could relocate the runtime layout, deny permissions or trust itself would break the authority model — and never `[scene]` (world content has a dedicated channel, see the layer rules above). Any such file fails startup with a precise error. The content package's own `mods/test_scene/config.toml` ships the `renderer.texture_directory` default that points the renderer at the package's texture sets and the `[input.actions]` bindings its content reads; the assembly can override both from `engine.toml`.

### Provenance

Every effective value records the layer and source that provided it. `ConfigurationStore::provenanceReport()` renders the full map - one `<path> <- <layer (source)>` line per effective value - for an embedder's diagnostics (the shipped executable prints it nowhere; the same records power the unknown-key warnings below):

```
configuration provenance (the layer that provided each effective value):
  renderer.texture_directory <- mod defaults 'test_scene'
  scene.camera.fov <- staged override
  scene.objects <- scene file '/opt/engine/runtime/mods/test_scene/scene.toml'
  window.title <- engine configuration '/opt/engine/runtime/config/engine.toml'
  ...
```

The same records power the unknown-key warnings: when startup reports `unknown configuration key 'renderer.exposur' (from 'config/engine.toml')`, the source is the provenance of the offending value.

### Known keys

Every component declares the configuration keys it reads — modules in their manifest (`knownConfigKeys`), mods as `config_keys` in `mod.toml` — and the kernel adds its own assembly keys (`[engine]`, `[providers]`, `[permissions]`, `[diagnostics]`). Startup validation walks the effective table and reports anything nobody declares:

```
warning: unknown configuration key 'renderer.exposur' (from 'config/engine.toml')
warning: unknown configuration key 'windwo.title' (from 'config/engine.toml'; no component declares keys in '[windwo]')
```

Warnings never fail validation — dead keys are noise, not breakage. Keys that only a shipped defaults file provided are exempt: a package owns the keys it ships. Arrays of tables are declared at the array's path (`scene.objects`), since rows are authored wholesale. The per-module key tables in this documentation set are exactly the declared surfaces.

This document covers the GLOBAL and SHARED sections — `[engine]`, `[providers]`, `[permissions]`, `[diagnostics]`, and `[scene]` — because no single module owns them. Each module's own settings are documented by that module, in its `README.md`, next to its code: every module owns the section that matches its capability name.

Sections other than a component's own are read-only shared configuration by convention: the module that owns a section reads it, everyone else treats it as data.

## [engine]

| Key | Type | Default | Meaning |
|---|---|---|---|
| `mod_directory` | string | `"mods"` | directory scanned for mod packages (the first scan root) |
| `extra_mod_directories` | string array | `[]` | additional mod package scan roots, scanned after `mod_directory` in the listed order - an assembly composes its mod set from several install locations (an engine package directory plus a user directory) without copying packages into one tree; a package id that appears in more than one scanned root is reported with both locations and the later package dropped, the same rule one directory already applies to duplicate ids |
| `modules_directory` | string | `"modules"` | directory scanned for module plugin binaries |
| `disabled_mods` | string array | `[]` | mod ids to skip at discovery |
| `max_fps` | integer | `0` | CPU-side frame cap (0 = uncapped; an unconfigured run never reads an extra pacing clock — vsync stays the only pacer) |
| `idle_poll_ms` | integer | `16` | poll interval of the renderer-less event pump (clamped 1..1000) |
| `config_reload_poll_seconds` | float | `0` | live configuration reload poll interval (0 = reload disabled — the frame path pays nothing; when set, the assembly file's mtime is checked at this interval and a change triggers the kernel's reload, see "Live configuration reload") |
| `fixed_update_hz` | integer | `0` | fixed-timestep update rate for the `EngineFixedUpdateEvent` stream the composition root publishes (0 = disabled — the variable `EngineFrameEvent` heartbeat alone is the historical behavior; when set, simulation consumers advance by exactly `1/hz` seconds per dispatch, see "The frame clock" in `architecture.md`) |
| `max_fixed_update_steps` | integer | `5` | catch-up clamp for one iteration's fixed-update burst (a stalled frame yields at most this many dispatches, never a spiral; clamped 1..1000) |
| `quit_after_seconds` | float | `0` | finite run length: the frame loop leaves on its own once the limit is reached and the run shuts down cleanly with exit 0 (0 = run until the window closes, the default and the historical behavior; the headless and CI shape — a smoke run that needs no window and no kill signal) |

No configuration section names components or what they demand, and none needs to. Modules are activated purely by the active mods' capability requirements: a module loads exactly when an active mod requires a capability it provides (and an activated module's own requirements pull its dependencies transitively — the renderer brings `window` and `logging` with it). A capability no mod requires stays dormant: its plugin is never instantiated and its binary is released before any construct phase. A demanded capability with no available provider fails startup with a precise diagnostic naming the consumer and the alternatives. A run with no active mods loads no module at all.

## [providers]

Picks one implementation when several provide the same capability, e.g. `physics = "physics_builtin"`. The key is the capability name - a family such as `physics` when the candidates are its implementation-bound members (`physics.builtin` and friends), or an exact name shared by several providers. Values are component names. Several providers with no selection fails startup, naming the candidates and the key that resolves them.

## [permissions]

| Key | Type | Meaning |
|---|---|---|
| `denied` | array of `[component, permission]` pairs | deny one declared permission of one component |

Modules and mods are both subject to denials; a denied permission fails startup precisely. An acquisition attempt without a grant is blocked at runtime with a `ComponentAccessError` naming the offender and the missing grant. The kernel also prints one structural advisory per mod AND per active module that runs native machine code, requests permissions outside the `capability.*` namespace, or acquires a broad set of capabilities — see `architecture.md`, "Risk advisories and the trust gate"; whether a flagged component may then be USED is the `[trust]` section's business.

## [trust]

| Key | Type | Default | Meaning |
|---|---|---|---|
| `trustMods` | array of strings | `[]` | mod ids whose risk advisories the assembly covers: they are used without the startup question |
| `trustAllMods` | bool | `false` | cover every active mod |
| `trustModules` | array of strings | `[]` | module names whose risk advisories the assembly covers |
| `trustAllModules` | bool | `false` | cover every active module |

The risk advisories always print (trust changes the question, never the visibility); what trust changes is whether the flagged component may be used without the user's agreement. A flagged component NOT covered here requires the user's consent at startup: an interactive run asks once per launch (proceed on yes), a headless run cannot ask and fails with these keys as the instructions. The section is assembly-owned like `[permissions]`: a shipped defaults file cannot define `[trust]` (a package that could trust itself would defeat the gate), while a staged override (`--set trust.trustAllMods=true`) works like any key. The shipped demo trusts its own content package and the advisory-carrying modules of its pipeline; see `architecture.md`, "Risk advisories and the trust gate".

## [diagnostics]

| Key | Type | Default | Meaning |
|---|---|---|---|
| `latency_report` | bool | `false` | record event dispatch latencies and print the per-channel report plus frame-time percentiles at shutdown |
| `frame_history` | integer | `4096` | frame-time percentile ring capacity in frames (0 disables the ring; the percentile report needs `latency_report`) |
| `graph_report` | bool | `false` | print the component graph once after startup: every active component with its capabilities, permissions, script host and start order (also available without starting a backend through `--print-graph`) |

Diagnostics are opt-in: with recording disabled the event bus never reads a clock, so a normal run pays nothing and stays quiet.

## Command line

The shipped executable maps its arguments one-to-one onto hooks the kernel already exposes, so an assembly can be driven per-invocation without editing a file:

```
engine [--config <file>] [--set <key>=<value>]... [--scene <file>] [--mods-dir <dir>]... [--validate] [--print-graph] [--version] [--help]
```

| Flag | Effect |
|---|---|
| `--config <file>` | assembly configuration file (default `config/engine.toml`, relative to the executable) |
| `--set <key>=<value>` | stage one override — the highest layer, applied after every file layer merged; repeatable, TOML value syntax (`--set renderer.exposure=1.4`) |
| `--scene <file>` | replace the world content with the file's `[scene]` section (same rules as `[engine]`'s scene file) |
| `--mods-dir <dir>` | scan `<dir>` for mod packages instead of `[engine].mod_directory`; repeatable — the given directories (in the given order) replace BOTH `mod_directory` and `extra_mod_directories`, so one invocation composes a mod set from several locations without editing the assembly |
| `--validate` | dry run: configuration, manifests, graph resolution, permission check — no backend starts (exit 0/1; CI-suitable) |
| `--print-graph` | dry run like `--validate`, then print the component graph: every active component with its provided/required/optional capabilities, permissions, script host and the resolved start order, plus the event subscription map (which channels have handlers, how many, their priority span) once a run would have one (nothing is constructed; mod development) |
| `--version` / `--help` | print the version / the usage and exit |

A malformed flag or assignment is a usage error (exit 2) before any backend is touched. The staged overrides are the same layer `addCommandLineOverride` has always fed — the executable simply stopped being the only caller that could not reach it.

## Live configuration reload

Reload is opt-in per run and composition-root driven. With `[engine].config_reload_poll_seconds` set (for example `0.5`), the frame loop stats the assembly file at that interval; a changed mtime triggers `EngineKernel::reloadConfiguration`, which:

1. parse-checks every file the re-assembly will read (assembly, module defaults, recorded mod defaults, world source) — a half-edited file fails the reload with a precise diagnostic and the running configuration stays untouched;
2. re-assembles the full layer stack (the staged overrides survive — they are still the highest layer);
3. diffs the effective values leaf by leaf and publishes `ConfigurationReloadedEvent` on the event bus with the changed dotted paths (`renderer.exposure`, `screenshot.format`, ...). Removed keys are in the list too (a subscriber re-reading one falls back to its default).

Subscribers re-read exactly the keys they own and ignore the rest. The raw subscription (`ConfigurationReloadedEvent` through the component context's event bus) stays the full-control surface; the `ConfigurationWatchHub` utility (`kernel/include/engine/kernel/configuration_watch.hpp`) is the per-key sugar: a component instantiates the hub, binds it in construct, and registers one watch per key it owns - the hub subscribes once and fires a callback only when that key's path is in the changed set (a REMOVED key counts as changed; the re-read falls back to the built-in default). The shipped adopters are the renderer (exposure, tonemap mode, bloom threshold/strength — the composite push constants), the screenshot module (format, quality, prefix, output directory — through the watch hub), the audio module (master volume, mute), the logging module (verbosity `level` and `flush` policy — through the watch hub; the file sink is deliberately restart-only) and the app itself (the pacing keys `max_fps` / `idle_poll_ms` / `fixed_update_hz` / `max_fixed_update_steps` / `quit_after_seconds`, also through the watch hub). World content (`[scene]`) is re-merged into the store but reported as restart-required: the scene registry was seeded at start. A reload never re-runs mod discovery — the component graph is frozen for the run; adding or removing a mod remains a restart.

## [scene]

Shared world content, read by the renderer (rasterization) and the lighting technique modules (the exact analytic SDF the GI traces — one geometry truth). Object and light details also live in the technique docs of each lighting module's README (light seeding in the PBR technique's README; how the world is traced in the GI and reflections techniques' READMEs).

| Key | Type | Default | Meaning |
|---|---|---|---|
| `sky_radiance` | float[3] | `[0, 0, 0]` | base sky radiance; zero closes the scene completely |
| `sky.zenith` / `sky.horizon` / `sky.ground` | float[3] | `sky_radiance` | vertical gradient radiance (components left out fall back to the flat `sky_radiance`, so legacy configurations render unchanged) |
| `sky.sun_disk` | bool | `true` | draw the sun disk on the direct view and in reflections (never in the GI transport field) |
| `sky.sun_disk_size_degrees` | float | `0.6` | angular disk diameter; the disk radiance is the sun's irradiance divided by its solid angle, so it adds no energy |
| `camera.position` / `camera.look_at` | float[3] | see source | camera placement (the free-fly STARTING pose) |
| `camera.fov` / `camera.near` / `camera.far` | float | `60` / `0.05` / `500` | vertical field of view and clip planes |
| `camera.free_fly` | bool | `true` | free-fly controls: WASD flies, mouse looks while the cursor is captured (Esc releases, F or left-click re-captures), Space/E rises, Ctrl/Q sinks, Shift boosts, the wheel scales `fly_speed` |
| `camera.fly_speed` / `camera.fly_boost` | float | `4` / `4` | base speed (world units per second) and the Shift boost multiplier (>= 1) |
| `camera.look_sensitivity` | float | `0.0022` | yaw/pitch radians per cursor pixel |
| `sun.direction` / `sun.color` / `sun.intensity` | float[3]/float[3]/float | off | seeds the directional light and the sky's sun disk (`direction` points towards the scene) |
| `objects` | array of tables | `[]` | scene objects (max 256, the GPU primitive buffer's capacity) |

Each `[[scene.objects]]` row: `type` (`box`, `sphere`, or `plane` - authoring sugar for a thin box so rasterized and traced geometry stay the same primitive), `position`, `size` (box half extents / sphere radius), `rotation_y` (degrees), `albedo`, `roughness`, `metallic`, `emissive_strength` (emissive radiance is albedo times this value), `texture` (name of a PBR texture set under `[renderer].texture_directory` as `<name>/` - a folder with `Color.png`, `NormalGL.png`, `Roughness.png` and optional `Metalness.png`; all sets used by one scene must share one resolution), and `uv_scale` (world meters per texture tile, default 1). An object with a texture set takes albedo/roughness/metallic/normal from its maps; the flat values are the fallback. Point lights can be seeded with `[[scene.point_lights]]` rows (`position`, `color`, `intensity`, `radius`).

Where the world itself comes from is a configuration-source question, not a `[scene]` key: the shipped demo world lives in the content mod (`mods/test_scene/scene.toml`, declared through the manifest's `scene` key), and an assembly that defines its own `[scene]` replaces it wholesale (precedence in "The layer stack"). The content package also ships the texture sets and the `renderer.texture_directory` default pointing at them; a scene naming texture sets with no directory configured fails with a precise error instead of rendering flat materials.

The demo scene ships five CC0 texture sets from [ambientCG](https://ambientcg.com): `WoodFloor055`, `Plaster001`, `Bricks070`, `Concrete034`, `Metal049A`.

## Swapping the world

An assembly that wants a different world defines its own `[scene]` section — copy any content-mod-side key (the camera pose, for instance) into it that the swapped world should keep. A composition root can also stage a `[scene]`-only scene file through the kernel, which swaps the section wholesale:

```toml
[scene]
sky_radiance = [0.05, 0.05, 0.08]

[scene.camera]
position = [0.0, 1.5, 3.0]
look_at = [0.0, 0.5, 0.0]

[[scene.objects]]
type = "plane"
position = [0.0, 0.0, 0.0]
size = [3.0, 0.01, 3.0]
albedo = [0.6, 0.6, 0.6]
```
