# harrowmere_core tests

The unit-test target `harrowmere_core_tests` (doctest). The framework and target name are pinned by task 0.6 (the full harness: test-count gate, `scripts/ci.sh`); task 0.7 landed the harness nucleus - the doctest entry point, the first pinned test files, and ctest registration - so its card's tests exist and run.

## Layout

| File | Role |
|---|---|
| `main.cpp` | the doctest entry point (`DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`); test files never define it |
| `test_events.cpp` | task 0.7's two pinned tests over the sim event channels |
| `test_seasons.cpp` | task 3.8's two pinned tests (`TestSeasons_CycleLengthAndOrder`, `TestWeather_MarkovStaysInTable`) + the cadence/determinism rules, the year fast-forward, and the frozen hook signatures (see `src/world/README.md`, "Seasons & weather") |
| `test_status.cpp` | task 2.1's six pinned tests (derived statuses, the consent pair/arena channels, cooldown slots) + the IN_WAR seam and the no-stored-flag negative |
| `test_species.cpp` | task 2.2's four pinned tests (multipliers, the elf 4x row, non-sapient beasts, level-extended lifespan) + the bias-core read |
| `test_progression.cpp` | task 2.3's eleven pinned tests (cost/cap tables, effectiveness, purchases, D-REFUND-1 respec, the D-LORE-1 mana rule, the XP floor) + the skill-requirement gates |
| `CMakeLists.txt` | the target: links the kernel (event bus headers + lib), compiles `../src/core/` headers, registers with ctest |

The target links the kernel and compiles the mod's headers directly - tests run without the plugin scanner, exactly as the harness card requires ("link objects, not the mod runtime"). The doctest single header is vendored at `third_party/doctest/` (v2.4.12, the version task 0.6 pins).

## Running

```sh
ctest --test-dir build/mods/harrowmere_core/tests --output-on-failure
# or directly:
./build/runtime/harrowmere_core_tests
```

ctest discovery is directory-local until task 0.6 lifts `enable_testing()` to the tree root (its `ci.sh` runs ctest from the configure root).

## The pinned tests (task 0.7)

- `TestEvents_PublishAfterMutationOrdering` - the publish-after-mutation rule: a handler dispatched by the engine observes the post-state (the culprit already inside the manhunt target set) and the event carries the post-mutation values; also pins that dispatch is synchronous on the caller's stack.
- `TestEvents_UnsubscribeDuringDispatchDeferred` - pins the kernel's exact contract for an `EventSubscription` released during a dispatch (verified against `kernel/include/engine/kernel/event_bus.hpp`): the structural removal is deferred (an already-scanned handler keeps its in-flight delivery) while the logical removal is immediate (a handler released before the scan reaches it is not invoked - no dangling dispatch), both silent from the next publish on, and a second `release()` is a no-op.

New test files follow the pinned naming `Test<System>_<Behavior>_<Expected>` (common.md §3.5) and are added to the target's source list; `main.cpp` stays untouched.

## Phase-5 slice (tasks 5.1.1 / 5.1.2)

| File | Role |
| --- | --- |
| `test_terrain.cpp` | task 5.1.2's five pinned tests: `TestTerrain_SameSeedSameHeights`, `TestTerrain_DifferentSeedDifferentHeights`, `TestTerrain_HeightContinuousAtRegionEdges`, `TestTerrain_HeightAtPureFunction`, `TestTerrain_WorldgenHashUnchanged`. The worldgen-hash test asserts the terrain build does NOT change 1.6.1's FNV-1a summary hash. |
| `test_capture_scenarios.cpp` | task 5.1.1's three pinned tests: `TestCapture_ScenarioListComplete`, `TestCapture_RunProducesPngsPerScenario`, `TestCapture_BudgetReportParses`. The dry-mode smoke writes the checklist.md and report.json artifacts into a temp directory and asserts their shape; the parser test exercises the budget-report roundtrip. |
| `capture_scenarios.hpp` / `capture_scenarios.cpp` | task 5.1.1's implementation: the pinned eight-scenario list (spawn_day, spawn_night, walk_village, walk_frontier, coast, rain, panels_open, corpse_aftermath), the per-scenario setup + checklist + budget row, the budget-report serializer/parser (JSON shape, stable across versions), the run orchestrator (dry-mode walks the scenarios; wet-mode shells out to the engine executable per scenario). |

| `test_character_visuals.cpp` | task 5.1.3's five pinned tests: `TestCharVis_SilhouettePerSpecies`, `TestCharVis_YawFollowsVelocity`, `TestCharVis_BobPhaseFromDistance`, `TestCharVis_CorpseFlattenGrayUnchanged`, `TestCharVis_NoSimStateTouched`. The tests exercise the pure-data derivation directly (no GPU, no window). |
| `test_atmosphere.cpp` | task 5.1.4's six pinned tests: `TestAtmo_SunDirectionTracksDayPhase`, `TestAtmo_SkyAnchorsInterpolateInOrder`, `TestAtmo_RainRaisesFogDensity`, `TestAtmo_NightWithinExposureFloorCeiling`, `TestAtmo_WeatherStateUnmodified`, `TestAtmo_SeasonTintsApply`. |

| `test_scatter.cpp` | task 5.2.1's six pinned tests: `TestScatter_SameSeedSamePlacements`, `TestScatter_DensityFollowsClimateTable`, `TestScatter_NoPropsInWaterOrOnSteepSlope`, `TestScatter_SettlementClusterMatchesTemplateTier`, `TestScatter_InstanceCapRespectedPerView`, `TestScatter_SaveLoadReproducesPlacements`. |
| `test_borders.cpp` | task 5.2.2's six pinned tests: `TestBorders_RibbonProjectsOnTerrain`, `TestBorders_OwnerColorFromFactionPalette`, `TestBorders_TerritoryChangedRecolorsNextFrame`, `TestBorders_SafeZoneStyleDistinct`, `TestBorders_CoastWaterOnlyWhereClimateSays`, `TestBorders_NoSimWrites`. |
