# Bridge

The bridge is the presentation seam between the authoritative simulation (`harrowmere_core` stores and systems) and the engine's `scene` capability (runtime object registry). It is a **pure viewer**: it reads world/clock state and pushes scene objects; it contains no gameplay logic and mutates no sim state (D-ARCH-3, common.md §4.4 step 13).

## Phase-1 slice (task 7.2P1)

The Phase-1 bridge renders the static world only:

- **One tall thin box per territory center.** Height is by territory type: village 5 m, city 10 m, kingdom 15 m, empire 20 m. Tint is by type.
- **One flat tinted disc per safe zone.** Radius matches D-SHELTER-1's 50 m shelter radius.
- **Sun light** driven by `WorldClock::dayPhase01()`; repushed only on a ≥ 1 % phase delta to avoid per-frame registry traffic.
- **Sky tint** driven by the primary weather state, table A-117 (clear / rain × 0.7 / storm × 0.45 / snow × 1.2).

No characters, corpses, loot, animation, or NPC culling ship here — Phase 2 (card 7.2) extends this bridge to add them. A-118 (`PRESENTATION_RADIUS_M`) is declared at the seam so the Phase-2 culling path extends rather than rewrites.

## Files

| File | Role |
| --- | --- |
| `scene_bridge.hpp` | Constants (A-117 / A-118 / A-119), the three bridge-side interfaces (`SceneRegistrySink`, `WorldViewForBridge`, `ClockViewForBridge`), and the `SceneBridge` class. |
| `scene_bridge.cpp` | Per-frame logic: chunked creation, sun hysteresis, sky-tint hysteresis. |
| `harrowmere_capability.hpp` | The published `SimCapability` surface (§4.2): the read views (the fog-of-war whitelist lives in `JusticeView`'s shape), the pinned `SimCommand` vocabulary, the presentation pacing. The ONLY header any consumer mod compiles against. |
| `player_view.hpp` | Task 2.8's tested `buildPlayerView` free function (the step-13 PlayerView fill; `rebuildViews` is the thin caller). |
| `society_view.hpp` | Task 7.3b's tested society fills: `buildTerritoryTree` (the Map panel's tree), `buildJusticeView` (the whitelist fill — never a case score or a report weight), `buildEventsView` (contribution/share/join reads). The live whitelist test (`TestSocietyView_NoHiddenEvidenceExposed`) exercises these against the real stores. |
| `store_adapters.hpp` | The 7.2 runtime `CharactersView` over the stores (the World inspector's present set). |
| `README.md` | This file. |

## Interfaces

The bridge depends on three abstract interfaces so it is unit-testable without the engine or a full `WorldStore` / `WorldClock` graph:

- `SceneRegistrySink` — scene-registry operations (`spawnBox`, `spawnDisc`, `setObjectPosition`, `setSunLight`, `setSkyTint`). The production adapter wraps the engine's `SceneCapability` (acquired via `ComponentContext` in `harrowmere_plugin.cpp`).
- `WorldViewForBridge` — the territories to render (sorted by id, R7) plus the primary weather state. The production adapter wraps `WorldStore` (task 1.4) and computes territory centers from the region grid (A-40).
- `ClockViewForBridge` — `dayPhase01()`. The production adapter wraps `WorldClock` (task 0.3).

Tests provide fakes for all three (see `mods/harrowmere_core/tests/test_scene_bridge.cpp`).

## Subscription wiring

The bridge does **not** subscribe to `EngineFrameEvent` itself — it exposes `onFrame()` and the plugin (`harrowmere_plugin.cpp`) subscribes on its behalf with a priority lower than `harrowmere_ui`'s overlay subscription so the UI draws on top (common.md §4.0: "higher dispatches first"). This keeps the bridge decoupled from the engine's `EventBus` type.

## Chunking

Creations are chunked at ≤ `SCENE_CREATE_CHUNK_PER_FRAME` (A-119 = 32) per frame. The pending queue is built once from `WorldViewForBridge::territories()` and drained 32 tasks at a time; a world with 120 objects to spawn drains in 4 frames. The queue is territory-id-sorted (R7) and box-before-disc per territory, so the spawn order is deterministic across runs.

## Constants placement

Per DoD §1.5 every numeric constant belongs in `core/constants.hpp` (append-only per task 0.2). For the 7.2P1 drop-in deliverable the bridge constants (A-117, A-118, A-119, plus task-spec-pinned geometry) are kept local to `scene_bridge.hpp` so this slice does not modify an existing `constants.hpp`. On integrate, move them into `core/constants.hpp` with their `// A-nn` Appendix A references intact.

The presentation tints (`TINT_VILLAGE` / `TINT_CITY` / `TINT_KINGDOM` / `TINT_EMPIRE` / `TINT_SAFE`) are 7.2P1 values not yet in Appendix A; add them via the deviation protocol (§1.4) or make them data-driven (`data/bridge_tints.json`) on integrate.

## What does NOT live here

- **Scene handles outside `src/bridge/`.** Per the card's Done-when, no code outside `src/bridge/` touches scene handles. The bridge is the only path to the scene registry.
- **Tick-step work.** The bridge is frame-coupled, not tick-coupled. Do not drive it from a scheduler step; it rides `EngineFrameEvent` only (§4.4 step 13).
- **Gameplay logic.** No combat, no economy, no justice, no events. The bridge reads; it does not decide.

## Phase-5 slice (task 5.1.2 — terrain relief)

The flat ground plane becomes land. `bridge/terrain.{hpp,cpp}` adds a deterministic heightfield the renderer draws and everything else reads through one function — with the simulation untouched.

**D-IMG-1 (common.md Appendix A.14): HEIGHT IS PRESENTATION.** Sim positions remain planar (x, z); the tick never samples height. `TerrainField::heightAt(x, z)` is the ONE height door — the camera (6.1.1), scatter placement (5.2.1), water clipping (5.2.2) and the bridge mesh publish all read through it.

### Files

| File | Role |
| --- | --- |
| `terrain.hpp` | The `TerrainField` class: `heightAt` (the ONE height door), `meshGrid` (the A-148 512×512 backdrop mesh the bridge publishes), test-friendly structural accessors (`climateBaselineAt`, `climateNoiseAmplitudeAt`). |
| `terrain.cpp` | The splitmix64 hash over `(seed, cellX, cellZ)` (NOT the worldgen RNG stream — D-IMG-1's pitfall), the A-147.a-e octave sum, the climate baseline/amplitude bilinear interp, the A-147.e coastal-clamp BFS LUT, the A-148 mesh grid. |

### Constants

A-146 (render frame budget), A-147.a-e (terrain noise field), A-148 (grid resolution), A-149 (region-edge gradient cap) live in `core/constants_phase5.hpp` (the Phase-5 registry block, append-only). Per-climate baseline and amplitude values are presentation-only (the card did not pin per-climate numbers); they live in `terrain.cpp`'s `climateBaselineM` / `climateNoiseAmplitudeM` and follow the same promotion pattern as `scene_bridge.hpp`'s presentation tints.

### What does NOT live here

- **The bridge mesh publish** — the bridge's `scene_bridge.cpp` already publishes a flat ground plane through `scene.toml`'s claim. The bridge-side swap to `TerrainField::meshGrid()` is a one-line bridge edit on integrate (the card's spec: "scene.toml's ground-plane claim is replaced by the bridge's terrain claim, not deleted").
- **Water** (5.2.2), **camera floor** (6.1.1), **scatter placement** (5.2.1) — owner cards consume `heightAt`; they do not edit this module.

### Tests

`tests/test_terrain.cpp` — the five pinned tests: `TestTerrain_SameSeedSameHeights`, `TestTerrain_DifferentSeedDifferentHeights`, `TestTerrain_HeightContinuousAtRegionEdges`, `TestTerrain_HeightAtPureFunction`, `TestTerrain_WorldgenHashUnchanged`. The worldgen-hash test asserts the terrain build does NOT change 1.6.1's FNV-1a summary hash (the terrain reads `region.climate` already in the hash and adds no fields to `Region`).

## Phase-5 slice (task 5.1.3 — characters beyond the token)

The species-colored token grows a body: per-species silhouette primitives (A-153), a yaw from movement direction, and a two-pose walk bob (A-154). The faction ring stays the political truth; corpse visuals keep the as-built flatten-and-gray rule.

**Files:** `character_visuals.{hpp,cpp}` — `CharacterVisual` (pure-data: height/girth scale, yaw, bob offset, corpse flag) + `computeCharacterVisual(input)` (pure: no sim state touched). The bridge composes the visual AFTER reading the sim's position, never before writing it (the card's pitfall: the walk bob must not modulate the collision/position the sim sees).

**Constants:** A-153 (nine species silhouette rows in `SPECIES_SILHOUETTES`), A-154 (`WALK_BOB_HZ = 2.2`, `WALK_BOB_AMPLITUDE_M = 0.08`) — both in `core/constants_phase5.hpp`.

**Tests:** `tests/test_character_visuals.cpp` — the five pinned tests (`SilhouettePerSpecies`, `YawFollowsVelocity`, `BobPhaseFromDistance`, `CorpseFlattenGrayUnchanged`, `NoSimStateTouched`).

## Phase-5 slice (task 5.1.4 — sky, sun & weather on screen)

The sky stops being flat: time of day, season and weather — all already simulated — become what the player sees, through the renderer's existing sun/sky/fog path. **D-IMG-3: no new engine module, no kernel edit.**

**Files:** `atmosphere.{hpp,cpp}` — `AtmosphereSample` (pure-data: sun direction/intensity, sky tint, fog density/tint, exposure target, bloom strength) + `sampleAtmosphere(input)` (pure: no sim state touched). The bridge reads the sample and pushes it to the scene sink (`setSunLight`, `setSkyTint`) and the renderer's fog/exposure/bloom seams. The renderer's bloom pass (already in the tree) is enabled for the sun disc and torch-range lights only (A-157 = 0.35).

**Constants:** A-155 (`SKY_ANCHORS` — dawn/noon/dusk/night Linear-RGB + `SEASON_TINTS`), A-156 (`EXPOSURE_NIGHT_FLOOR = 0.08`, `EXPOSURE_DAY_CEILING = 1.0`), A-157 (`BLOOM_STRENGTH_SUN = 0.35`, `BLOOM_STRENGTH_TORCH = 0.35`) — all in `core/constants_phase5.hpp`.

**Tests:** `tests/test_atmosphere.cpp` — the six pinned tests (`SunDirectionTracksDayPhase`, `SkyAnchorsInterpolateInOrder`, `RainRaisesFogDensity`, `NightWithinExposureFloorCeiling`, `WeatherStateUnmodified`, `SeasonTintsApply`).

**Determinism:** every output is a pure function of `(dayPhase01, weather, season)`. No wall clock (the card's pitfall: "Interpolating between anchors needs the PHASE, not wall time — importing real time here breaks determinism reviews and the D-TIME-1 two-clock rule"). No RNG stream.

## Phase-5 slice (task 5.2.1 — scatter: vegetation, rock & settlement props)

The land is inhabited from the outside: trees, rocks, shrubs and settlement buildings stand where the world says they should — dense near villages, sparse on the frontier — all of it deterministic, all of it data. **D-IMG-2: scatter is data + seed.**

**Files:** `scatter.{hpp,cpp}` — `ScatterField` (build-once, read-many: given seed + climate grid + terrain + tables, produces the deterministic placement list) + `loadScatterTables()` (loads `data/scatter_tables.json` through the strict JSON door) + the splitmix64 hash over `(seed, cellX, cellZ)` (NOT the worldgen RNG stream). Data: `data/scatter_tables.json` (Appendix B.9 shape: props + per-climate tables + per-tier settlement footprints).

**Constants:** A-150 (`SETTLEMENT_FOOTPRINT_VILLAGE/TOWN/CITY` — 8-14/15-30/31-60 buildings), A-151 (`SEA_LEVEL_M = 0`), A-152 (`SCATTER_INSTANCES_PER_VIEW = 12,000`) — all in `core/constants_phase5.hpp`.

**Tests:** `tests/test_scatter.cpp` — the six pinned tests (`SameSeedSamePlacements`, `DensityFollowsClimateTable`, `NoPropsInWaterOrOnSteepSlope`, `SettlementClusterMatchesTemplateTier`, `InstanceCapRespectedPerView`, `SaveLoadReproducesPlacements`).

## Phase-5 slice (task 5.2.2 — water, coasts & borders on the land)

The map's political and geographic truth is visible ON the terrain: water where the climate data says coast, territory borders drawn on the land, safe zones styled, and every border redrawn when ownership changes.

**Files:** `borders.{hpp,cpp}` — `BordersField` (the per-frame border state: water plane + territory ribbons) + `BorderRibbon` (one territory's border: polygon, owner, safe flag, resolved tint) + `WaterPlane` (the single sea-level plane, A-151) + `resolveTint(owner, safe)` (faction palette / neutral / safe tint) + `recolorOnTerritoryChanged(id, newOwner)` (event-driven recolor, no polling) + `regionBoxPolygon(center, halfExtent)` (rectangular polygon around a region center).

**Constants:** A-151 (`SEA_LEVEL_M = 0`), A-158 (`BORDER_NEUTRAL_TINT`, `BORDER_SAFE_TINT`, `BORDER_SAFE_DOUBLE_RIBBON_SPACING_M`), A-159 (`BORDER_LIFT_M = 0.35`) — all in `core/constants_phase5.hpp`.

**Tests:** `tests/test_borders.cpp` — the six pinned tests (`RibbonProjectsOnTerrain`, `OwnerColorFromFactionPalette`, `TerritoryChangedRecolorsNextFrame`, `SafeZoneStyleDistinct`, `CoastWaterOnlyWhereClimateSays`, `NoSimWrites`).
