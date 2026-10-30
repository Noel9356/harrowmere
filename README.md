# Harrowmere

*The worked land around the still water.* A persistent, simulation-heavy
sandbox RPG built on a modular C++20 engine — single-player, data-driven,
moddable. This repository is `Noel9356/engine` specialized in place: the
engine's kernel stays stock, everything else became the game.

## Where to start (new programmers)

Read, in order: `common.md` §1.6 (the reading order), then your phase plan
(e.g. `phase-1-walk.md`) — "Playable when", "You inherit", "Not your job"
first, then the cards. The plan (common.md + your phase file) and the public
engine repository are the only two sources you need. Deviations go to
`mods/harrowmere_core/docs/DEVIATIONS.md`.

## Build & run

Requirements: CMake 3.24+, a C++20 compiler, a Vulkan loader, and
`glslangValidator` (see `docs/building.md` — the engine's build docs still
apply verbatim).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/runtime/harrowmere                 # windowed
./build/runtime/harrowmere --validate      # CI dry run (no backend)
./build/runtime/harrowmere --set engine.quit_after_seconds=1.0   # headless smoke, exit 0
```

The finite-run override is a TOML FLOAT (`1.0`, never `1`): toml++ reads no
integer node for this key, so a staged `1` parses as unset and a finite run
never quits (DEVIATIONS.md 2026-10-29). Exit codes: 0 clean, 1 failed
startup/validate, 2 usage error, 3 graphics backend validation errors.
Configuration paths resolve relative to the executable directory.

## The assembly

The game is an app that carries mods — the engine's own demo assembly
rewritten in place, packaged by the engine's own pipeline (nothing
replicated):

```
harrowmere/                      # this repo
├── kernel/                      # THE ONLY STOCK ZONE — never edited; upstream-syncable
├── app/                         # the composition root: main.cpp, harrowmere_app.{hpp,cpp} (target `harrowmere`)
├── config/harrowmere.toml       # the assembly config: [engine] pacing, [trust], permission policy
├── mods/
│   ├── harrowmere_core/         # native mod: authoritative simulation + ALL base content
│   │                            #   requires logging/scene/input (+ optional physics/audio/animation/asset)
│   │                            #   provides harrowmere.core; claims the world (scene.toml); owns data/
│   └── harrowmere_ui/           # native mod: ImGui surface (requires harrowmere, input, renderer.vulkan.overlay;
│                                #   provides ui.harrowmere) — the 7.3P1 debug overlay (F1) + the 7.4P1 input layer
├── modules/                     # engine modules — TRIMMED to 15 (see below)
├── third_party/                 # engine-fetched deps
└── docs/, cmake/                # the engine's docs & cmake helpers
```

Runtime layout after a build: `build/runtime/{harrowmere, modules/, config/,
mods/<id>/…, shaders/}` — relocatable; `module_plugins.cache` next to the exe
is derived state, deleting it is always safe.

## The stock-zone rule

`git diff <pinned upstream tag> -- kernel/` stays empty for the life of the
project — that is what keeps engine syncs cheap. Everything else
(`app/`, `mods/`, `config/`, `cmake/`, `docs/`, the root `CMakeLists.txt`) is
game code and may change freely. If the kernel seems to need a change, work
around it on our side.

## The module trim

The engine shipped 22 modules; the game deleted the ones no card consumes
**and no kept module builds against**, outright (a present-but-disabled
module would still ship its defaults into the runtime):

- **Deleted (3):** `behavior_tree`, `localization_toml`,
  `radiance_cascades` — no card consumes them and nothing includes their
  headers.
- **Kept (19)** — the 15 the plan's trim table keeps, plus four restored
  under §3.6's build-edge rule ("if a kept module's build turns out to
  reference a deleted one, restore that one module — the trim table yields to
  a real build edge"):
  `logging_stdout`, `window_glfw`, `input_glfw`, `scene_registry`,
  `renderer_vulkan`, `scheduler_frame`, `settings_toml`, `screenshot_png`,
  `hot_reload_watcher`, `profiler`, `physics_builtin`, `audio_miniaudio`,
  `animation_tween`, `asset_gltf`, `pbr`,
  **`bvh`** (physics_builtin includes its capability header),
  **`lua_scripting`** (its `lua_bridge_extension.hpp` is the tree-wide
  extension contract — `scene_registry`, which the game hard-requires,
  includes it),
  **`ui_imgui`** (input_glfw and renderer_vulkan include its capability
  header),
  **`persistence_toml`** (ui_imgui includes its capability header).

  The restored four are inert at runtime: no active component demands `bvh`,
  `scripting`, `ui` or `persistence` capabilities, so they never join the
  run — `--print-graph` still shows exactly the planned seven components, and
  the boot test asserts `ui_imgui`'s absence from the resolved graph. The
  full evidence trail is `mods/harrowmere_core/docs/DEVIATIONS.md`.

If an upstream sync ever reintroduces a demo mod with its own `scene.toml`,
delete it — ours must be the only scene claimant (the engine's one-world
rule).

## Configuration keys

Layer stack (decreasing precedence): staged overrides (`--set`) >
`config/harrowmere.toml` (this assembly) > scene file > module defaults >
mod package defaults > code defaults. Tables deep-merge; arrays and
`[[scene.objects]]` rows replace wholesale. A defaults file may never define
`[engine]`, `[providers]`, `[permissions]`, `[diagnostics]`, `[trust]` or
`[scene]`.

- **Assembly-owned** (`config/harrowmere.toml`): `[engine] fixed_update_hz`
  (10 — the fixed-update lane the sim rides, `D-TICK-1`),
  `max_fixed_update_steps` (5), and `[trust] trustMods` (both native mods are
  risk-flagged by definition; without a standing decision here every headless
  run dies at the trust gate).
- **Package defaults** (`mods/harrowmere_core/config.toml`, `[harrowmere]`
  section, declared via `config_keys` in the manifest): `tick_rate = 10`,
  `time_scale = 60` (D-TIME-1: 1 real second = 1 game minute),
  `autosave_game_hours = 6`, `seed = 0` (0 = chosen at first launch, then
  persisted by the save system, task 7.1).
- **Rule values** (species, items, laws, events...) are JSON data under
  `mods/harrowmere_core/data/` — TOML is configuration, JSON is content
  (`D-DATA-1`).

## Tests

- `scripts/ci.sh` — the one-command gate (task 0.6): configure (Release,
  warnings-as-errors) → build → ctest → `--validate`, non-zero exit on any
  failure. ctest suites: `harrowmere_core_tests` (the card-pinned Phase-1
  battery), `harrowmere_ui_tests` (7.4P1 input) and
  `harrowmere_ui_debug_panel_tests` (7.3P1) — each is directory-local:
  `ctest --test-dir build/mods/<id>/tests --output-on-failure`.
- `scripts/bootstrap_tests.sh` — the task 0.1 acceptance (the pinned
  bootstrap tests): headless boot (exit 0, startup log line, one world
  claimant, no `ui_imgui` in the graph) and the windowed loop check
  (skipped headless — it needs a display).
- Worldgen determinism (the exit checklist): two runs with the same seed log
  the same FNV-1a summary hash at INFO (`world generated: ... hash=...`); a
  different seed differs.
