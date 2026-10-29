# harrowmere_core

Harrowmere's authoritative simulation and ALL base content, packaged as a
native mod. This package is the demand root of the whole assembly: its
manifest pulls the engine capabilities the game needs (`logging`, `scene`,
`input`, plus optional `physics` / `audio` / `animation` / `asset`), it is the
run's only world claimant (`scene = "scene.toml"`), and it publishes the
game's own capability family, `harrowmere` (member `harrowmere.core`), that
the app acquires as a hard requirement and `harrowmere_ui` consumes like any
mod.

## Package layout

| Path | Role |
|---|---|
| `mod.toml` | the manifest - identity, demand roots, `provides`, permissions, `config_keys`, the world claim |
| `config.toml` | package defaults: the `[harrowmere]` section (`tick_rate`, `time_scale`, `autosave_game_hours`, `seed`) - config-flavored knobs only; may never define `[engine]`/`[trust]`/`[scene]` |
| `scene.toml` | the initial backdrop world (ground plane, sky, sun, camera - D-PRES-1); the living world is the sim's own state |
| `data/` | all game content as JSON (species, items, laws, events...); filled from task 1.1 on, loaded by the task 0.4 loader |
| `src/harrowmere_plugin.cpp` | the plugin seam: the entry table the kernel resolves (`engine_query_mod_entry_table`), the compiled manifest, the component lifecycle |
| `src/bridge/harrowmere_capability.hpp` | the published capability surface (`SimCapability`) - the ONLY header consumers compile against |
| `tests/` | `harrowmere_core_tests` (doctest) - wired by task 0.6 |
| `docs/DEVIATIONS.md` | the deviation log (common.md §1.4) |

## The `harrowmere.core` surface

`SimCapability` follows the engine's provider-owns-its-vocabulary rule: views
are per-frame snapshots (rebuilt at tick step 13), writes go through
`submit(SimCommand)` (applied at tick step 9 or rejected with a logged
reason), and no view ever exposes sim internals. v0.1 ships the surface as
inert stubs on purpose - the header's shape is the contract later phases
fill. Keep the header platform-free and dependency-free; the purity guard in
CMakeLists.txt enforces it mechanically.

## Conventions this package keeps

- Everything game-specific is `harrowmere_*` / `harrowmere.*`; engine modules
  keep their own names (common.md §3.1, `D-NAME-1`).
- The compiled manifest (in `harrowmere_plugin.cpp`) and `mod.toml` must agree
  on identity and on `provides` - the kernel verifies both at startup and a
  drift is a precise error.
- Docs live next to code: every new subsystem directory under `src/` ships its
  own README (the engine's documentation convention).
