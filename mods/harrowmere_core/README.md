# harrowmere_core

Harrowmere's authoritative simulation and ALL base content, packaged as a native mod. This package is the demand root of the whole assembly: its manifest pulls the engine capabilities the game needs (`logging`, `scene`, `input`, `lighting` + `pbr_direct_diffuse` - the 7.2P1 sun seam, required so the kernel's demand closure brings the lighting provider in, plus optional `physics` / `audio` / `animation` / `asset`), it is the run's only world claimant (`scene = "scene.toml"`), and it publishes the game's own capability family, `harrowmere` (member `harrowmere.core`), that the app acquires as a hard requirement and `harrowmere_ui` consumes like any mod.

## Package layout

| Path | Role |
|---|---|
| `mod.toml` | the manifest - identity, demand roots, `provides`, permissions, `config_keys`, the world claim |
| `config.toml` | package defaults: the `[harrowmere]` section (`tick_rate`, `time_scale`, `autosave_game_hours`, `seed`) - config-flavored knobs only; may never define `[engine]`/`[trust]`/`[scene]` |
| `scene.toml` | the initial backdrop world (ground plane, sky, sun, camera - D-PRES-1); the living world is the sim's own state |
| `data/` | all game content as JSON (species, items, laws, events...); filled from task 1.1 on, loaded by the task 0.4 loader |
| `src/harrowmere_plugin.cpp` | the plugin seam: the entry table the kernel resolves (`engine_query_mod_entry_table`), the compiled manifest, the component lifecycle |
| `src/bridge/harrowmere_capability.hpp` | the published capability surface (`SimCapability`) - the ONLY header consumers compile against |
| `src/core/` | the mod's internal core headers: pinned types, enums, and the sim event channels (task 0.7); consumers never compile against them - see `src/core/README.md` |
| `tests/` | `harrowmere_core_tests` (doctest) - harness nucleus landed with task 0.7 (its pinned tests), full harness owned by task 0.6 |
| `docs/DEVIATIONS.md` | the deviation log (common.md §1.4) |

## Data-mod authoring (task 7.5)

A data package is any discovered `mods/<id>/` directory containing a valid `mod.toml` and one or more files under `data/*.json`. `harrowmere_core` loads its own data first, then overlays active packages in ascending manifest-id order. A mod does not need to depend on the core package to contribute data; it must not edit another package's files. The final merged documents pass through the core registry's normal strict schema and cross-reference validation before a campaign starts.

Merge behavior is documented in `src/data/README.md`: objects deep-merge; non-entity arrays replace wholesale; D-MOD-2 entity arrays merge by identity (`key`, except `crime_data` uses `type` per the deviation ledger). To add a species or item, provide the row with its unique stable key and an unused numeric ID. To add a recipe, because `recipes.json` is a replace-array file, include the full resulting recipe array. `mods/example_species_mod/` is a working example package.

Native mods consume the published `harrowmere` capability family and use `SimCapability` read views plus queued `SimCommand` submission. Do not include internal headers from `src/`; the published header `src/bridge/harrowmere_capability.hpp` is the sole compile-time contract.

## The `harrowmere.core` surface

`SimCapability` follows the engine's provider-owns-its-vocabulary rule: views are per-frame snapshots (rebuilt at tick step 13), writes go through `submit(SimCommand)` (applied at tick step 9 or rejected with a logged reason), and no view exposes sim internals. The current snapshot is the live Phase 3/4 contract: world, characters, player, justice, events and UI signals are populated by the bridge fills. Third-party native mods should include only `src/bridge/harrowmere_capability.hpp`; the fixture in `tests/fixtures/native_consumer.cpp` demonstrates a consumer compiled against that published header without reaching into simulation stores. Keep the header platform-free and dependency-free; the purity guard in CMakeLists.txt enforces it mechanically.

## Conventions this package keeps

- Everything game-specific is `harrowmere_*` / `harrowmere.*`; engine modules keep their own names (common.md §3.1, `D-NAME-1`).
- The compiled manifest (in `harrowmere_plugin.cpp`) and `mod.toml` must agree on identity and on `provides` - the kernel verifies both at startup and a drift is a precise error.
- Docs live next to code: every new subsystem directory under `src/` ships its own README (the engine's documentation convention).
