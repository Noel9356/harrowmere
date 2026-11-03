# Harrowmere

*The worked land around the still water.* A persistent, simulation-heavy sandbox RPG built on a modular C++20 engine — single-player, data-driven, moddable. This repository is `Noel9356/engine` specialized in place: the engine's kernel stays stock, everything else became the game.

## Where to start (new programmers)

Read, in order: `common.md` §1.6 (the reading order), then your phase plan (e.g. `phase-1-walk.md`) — "Playable when", "You inherit", "Not your job" first, then the cards. The plan (common.md + your phase file) and the public engine repository are the only two sources you need. Deviations go to `mods/harrowmere_core/docs/DEVIATIONS.md`.

## Build & run

Requirements: CMake 3.24+, a C++20 compiler, a Vulkan loader, and `glslangValidator` (see `docs/building.md` — the engine's build docs still apply verbatim).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/runtime/harrowmere                 # windowed
./build/runtime/harrowmere --validate      # CI dry run (no backend)
./build/runtime/harrowmere --set engine.quit_after_seconds=1.0   # headless smoke, exit 0
```

The finite-run override is a TOML FLOAT (`1.0`, never `1`): toml++ reads no integer node for this key, so a staged `1` parses as unset and a finite run never quits (DEVIATIONS.md 2026-10-29). Exit codes: 0 clean, 1 failed startup/validate, 2 usage error, 3 graphics backend validation errors. Configuration paths resolve relative to the executable directory.

## Phase 4 verification

```bash
scripts/ci.sh                 # configure, build, CTest, bootstrap, balance and A40 acceptance
scripts/acceptance.sh --check-only
build/runtime/harrowmere_soak --list-scenarios
```

Set `HARROWMERE_FULL_NIGHTLY=1` when running `scripts/ci.sh` to include the three-game-year headless soak. The 40-case player-chair manual and sign-off sheet are in `docs/acceptance_walkthrough.md`; automated checks do not fill those two human sign-offs.

## The assembly

The game is an app that carries mods — the engine's own demo assembly rewritten in place, packaged by the engine's own pipeline (nothing replicated):

```
harrowmere/                      # this repo
├── kernel/                      # THE ONLY STOCK ZONE — never edited; upstream-syncable
├── app/                         # the composition root: main.cpp, harrowmere_app.{hpp,cpp} (target `harrowmere`)
├── config/harrowmere.toml       # the assembly config: [engine] pacing, [trust], permission policy
├── mods/
│   ├── harrowmere_core/         # native mod: authoritative simulation + ALL base content
│   │                            #   requires logging/scene/input + lighting/pbr_direct_diffuse
│   │                            #   (+ optional physics/audio/animation/asset)
│   │                            #   provides harrowmere.core; claims the world (scene.toml); owns data/
│   ├── harrowmere_ui/           # native mod: ImGui surface, simulation panels, persistent Options (F2), settings reload
│   └── example_species_mod/    # sample native data package: adds mossling species + recipe through D-MOD-2
├── modules/                     # engine modules — TRIMMED to 15 (see below)
├── third_party/                 # engine-fetched deps
└── docs/, cmake/                # the engine's docs & cmake helpers
```

Runtime layout after a build: `build/runtime/{harrowmere, modules/, config/, mods/<id>/…, shaders/}` — relocatable; `module_plugins.cache` next to the exe is derived state, deleting it is always safe.

## The stock-zone rule

The phase handout (`common.md` revision 16) records the upstream engine HEAD as `94108c0` on 2026-10-29. This is a commit reference, not a verified tag; the supplied ZIP has no Git metadata, so confirm the exact upstream tag before an engine sync. Once pinned, `git diff <upstream-tag> -- kernel/` must stay empty for the life of the project — that is what keeps engine syncs cheap. Everything else (`app/`, `mods/`, `config/`, `cmake/`, `docs/`, the root `CMakeLists.txt`) is game code and may change freely. If the kernel seems to need a change, work around it on our side.

## The module trim

The engine shipped 22 modules; the game deleted the ones no card consumes **and no kept module builds against**, outright (a present-but-disabled module would still ship its defaults into the runtime):

- **Deleted (3):** `behavior_tree`, `localization_toml`, `radiance_cascades` — no card consumes them and nothing includes their headers.
- **Kept (19)** — the 15 the plan's trim table keeps, plus four restored under §3.6's build-edge rule ("if a kept module's build turns out to reference a deleted one, restore that one module — the trim table yields to a real build edge"): `logging_stdout`, `window_glfw`, `input_glfw`, `scene_registry`, `renderer_vulkan`, `scheduler_frame`, `settings_toml`, `screenshot_png`, `hot_reload_watcher`, `profiler`, `physics_builtin`, `audio_miniaudio`, `animation_tween`, `asset_gltf`, `pbr`, **`bvh`** (physics_builtin includes its capability header), **`lua_scripting`** (its `lua_bridge_extension.hpp` is the tree-wide extension contract — `scene_registry`, which the game hard-requires, includes it), **`ui_imgui`** (input_glfw and renderer_vulkan include its capability header), **`persistence_toml`** (ui_imgui includes its capability header).

The restored four are inert at runtime: no active component demands `bvh`, `scripting`, `ui` or `persistence` capabilities, so they never join the run — `--print-graph` still shows exactly the planned seven components, and the boot test asserts `ui_imgui`'s absence from the resolved graph. The full evidence trail is `mods/harrowmere_core/docs/DEVIATIONS.md`.

If an upstream sync ever reintroduces a demo mod with its own `scene.toml`, delete it — ours must be the only scene claimant (the engine's one-world rule).

## Configuration keys

Layer stack (decreasing precedence): staged overrides (`--set`) > `config/harrowmere.toml` (this assembly) > scene file > module defaults > mod package defaults > code defaults. Tables deep-merge; arrays and `[[scene.objects]]` rows replace wholesale. A defaults file may never define `[engine]`, `[providers]`, `[permissions]`, `[diagnostics]`, `[trust]` or `[scene]`.

- **Assembly-owned** (`config/harrowmere.toml`): `[engine] fixed_update_hz` (10 — the fixed-update lane the sim rides, `D-TICK-1`), `max_fixed_update_steps` (5), and `[trust] trustMods` + `trustModules` (the three native mods are risk-flagged by definition, and the `settings`/`hot_reload` modules the options panel activates draw their own advisories; without a standing decision here every headless run dies at the trust gate).
- **Package defaults** (`mods/harrowmere_core/config.toml`, `[harrowmere]` section, declared via `config_keys` in the manifest): `tick_rate = 10`, `time_scale = 60` (D-TIME-1: 1 real second = 1 game minute), `autosave_game_hours = 6`, `seed = 0` (0 = chosen at first launch, then persisted by the save system, task 7.1).
- **Rule values** (species, items, laws, events...) are JSON data under `mods/harrowmere_core/data/` — TOML is configuration, JSON is content (`D-DATA-1`).

## Tests and Phase 4 acceptance

- `scripts/ci.sh` is the one-command automated gate: configure (Release), build (warnings-as-errors), core/UI CTest, bootstrap smoke, graph sanity, one-year balance instrumentation, and the task 8.5 acceptance preflight. `.github/workflows/ci.yml` runs it on pushes and pull requests and enables the three-game-year soak on its daily schedule. Set `HARROWMERE_FULL_NIGHTLY=1` to request that soak locally.
- `scripts/acceptance.sh --check-only` checks the 40-scenario walkthrough's structure without requiring a build; `scripts/acceptance.sh --ci` also validates the runtime and checks the resolved component graph. Neither mode claims that the two required human sign-offs have been performed.
- `docs/acceptance_walkthrough.md` is the player-chair 8.5 runbook; its engineer and independent-tester sign-off records must be filled by actual executions. `docs/phase-4-handover.md` records the supplied-tree status, measurements, outstanding items, and Appendix E re-entry review.
- Core CTest includes the determinism (8.1), save/load and fuzz (8.2), soak smoke (8.3), balance (8.4), and the war/claims regressions; UI CTest covers the UI panel and input mapper suites.
- Worldgen determinism (the exit checklist): two runs with the same seed log the same FNV-1a summary hash at INFO (`world generated: ... hash=...`); a different seed differs.
