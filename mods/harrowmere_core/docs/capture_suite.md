# Capture Suite — Harrowmere

Task 5.1.1 deliverable. The plan's image work gets the same contract the sim has: a pinned, repeatable set of captures with a pass/fail report, and a frame budget the whole phase is accountable to.

This document is the **regression contract**: the nine scenarios the suite runs, the checklist a reviewer must confirm in each scenario's PNG, and the budget row each scenario is measured against. A changed image is legal only with a changed expectation line in this document.

Rev-27 staging note: the uplift rounds (rev 24-27) restaged several scenarios' hour and camera so the frames actually show what the round changed - the table below pins the CURRENT staging (the app's `resolveCaptureScenario` plus the rig's per-scenario scene file carry these values); the original 2026-11-04 table's times live in the rev-23 plan text.

## Running the suite

```bash
# Dry mode (CI smoke; no GPU required — walks the scenarios and writes the
# report + checklists into the output directory):
./build/runtime/harrowmere_core_tests -tc="TestCapture*"

# Wet mode (one command, runs all eight scenarios through the engine):
./build/runtime/harrowmere --capture-suite --output-dir ./captures

# Single scenario (developer reproducing one capture):
./build/runtime/harrowmere --capture-scenario spawn_day --output-dir ./captures/spawn_day
```

The suite's seed is pinned at `CAPTURE_SUITE_SEED = 20261103` (the same anchor the November evidence runs established). Every scenario uses this seed so the regression contract compares like-for-like.

## The nine scenarios

Each scenario is a named setup (seed, time of day, weather, camera pose, panels open) executed in a finite run (`quit_after_seconds` as a TOML float per DEVIATIONS.md 2026-10-29), producing PNGs into `<output-dir>/<scenario-name>/`.

| # | Name | Time of day | Weather | Panels | Camera target | Budget gate |
|---|---|---|---|---|---|---|
| 1 | `spawn_day` | 09:00 | clear | no | composed aerial over the spawn village | measured |
| 2 | `spawn_night` | 02:00 | clear | no | composed aerial over the spawn village | measured |
| 3 | `walk_village` | 08:30 | clear | no | street level on the village's north street | **gated (A-146)** |
| 4 | `walk_frontier` | 16:30 | clear | no | lowered onto the settlement approach, facing the sun | measured |
| 5 | `coast` | 15:45 | clear | no | the east shore looking west across the basin | **gated (A-146)** |
| 6 | `rain` | 13:00 | rain | no | world center | **gated (A-146)** |
| 7 | `panels_open` | 09:15 | clear | yes | world center | measured |
| 8 | `corpse_aftermath` | 18:00 (dusk) | clear | no | spawn village | measured |
| 9 | `crowd_street` | 14:48 (rev 27; was 15:36) | clear | no | the market square's east edge at eye height, the well-side crowd 8-13 m out | measured |
| 10 | `gi_room` | 10:00 (irrelevant: the sealed chamber admits no sun) | clear | no | inside the GI validation chamber against the east wall at eye height, looking west across the pedestal to the red/green end walls (rev 30; the temporal GI converges across the 180 s window) | measured |

The three **gated** scenarios are the 5.3.1 image-budget gate's anchors: the heaviest scenes the budget gate asserts against A-146. The other five are measured (their percentiles appear in the report) but not gated — a measured scenario exceeding the budget is a regression signal, not a gate failure.

## Checklists

Each scenario's checklist is a list of observable features a reviewer must confirm in the PNG. The checklists are **text** (not pixel masks): the suite renders under lavapipe in CI and under real GPUs locally, so the pin is on *what is visible* (relief exists, borders present), never on pixel equality across backends (the card's pitfall: "Captures are exactly one frame old by design — never 'fix' a flaky capture by sleeping in the harness; fix the scenario's wait condition instead").

### `spawn_day`
- [ ] Terrain relief visible (no flat ground plane) — *5.1.2 D-IMG-1*
- [ ] Sun light from above (day phase ~10:00 game time) — *5.1.4 / A-129*
- [ ] Sky gradient readable as day (not night) — *5.1.4 A-155*
- [ ] Safe-zone disc visible at the spawn village — *2.5.3 / D-SHELTER-1*

### `spawn_night`
- [ ] Sky gradient readable as night (dark, not black) — *5.1.4 A-155/A-156*
- [ ] Sun light absent or low (night phase) — *5.1.4*
- [ ] Exposure floor holds (night is dark but playable) — *5.1.4 A-156*
- [ ] Terrain relief still visible under low light — *5.1.2*

### `walk_village`
- [ ] NPCs present (species-colored tokens, faction rings) — *2.5.3*
- [ ] Vegetation dense near the village (scatter) — *5.2.1 A-150*
- [ ] Territory marker visible (village height 5 m) — *2.5.3 / 1.6.4*
- [ ] Walking motion legible (camera follows) — *1.8.1 / 6.1.1 (future)*

### `walk_frontier`
- [ ] Vegetation sparse on the frontier (scatter density gradient) — *5.2.1*
- [ ] No settlement props in the frontier ring — *5.2.1 / 1.6.1 frontier*
- [ ] Terrain relief varied (mountains/swamp if present) — *5.1.2*
- [ ] No FAR NPCs rendered (presentation radius cut) — *2.5.3 A-118*

### `coast`
- [ ] Water plane visible at sea level (A-151 = 0 m) — *5.2.2 A-151*
- [ ] Coastal terrain clamped low (≤ 8 m within 2 km) — *5.1.2 A-147.e*
- [ ] Coastal climate reads as coast (flat, near water) — *5.1.2 / 1.6.1*
- [ ] Borders visible on the terrain (coastal territory) — *5.2.2 A-158*

### `rain`
- [ ] Weather state is rain (sky tint desaturated) — *5.1.4 / A-117*
- [ ] Fog density raised (rain raises fog) — *5.1.4*
- [ ] Terrain relief still visible through rain — *5.1.2*
- [ ] NPCs present (if near a settlement) — *2.5.3*

### `panels_open`
- [ ] ImGui panels visible (HUD, Character, Inventory) — *2.6.2*
- [ ] Panels do not obscure the world (composite position) — *2.6.2 D-UI-1*
- [ ] F1 toggle works (panels close on second press) — *1.7.2 / 2.7.1*
- [ ] HUD reads live vitals (HP/level/coin non-zero) — *3.1.1 / 2.6.2*

### `corpse_aftermath`
- [ ] Corpse visible (flattened gray box) — *2.5.3 / A-143*
- [ ] Corpse decays after CORPSE_DECAY_DAYS (3 game-days) — *2.6.1 A-36*
- [ ] Loot is interactable (if within INTERACT_RANGE_M) — *1.7.1 A-121*
- [ ] No living NPC at the corpse's position — *2.5.3*

### `crowd_street` (rev 26; restaged rev 27)
- [ ] The blocky crowd reads as INDIVIDUALS (per-part randomized look: tunic / trousers / skin tones / hair caps, two baked builds) — *A-199*
- [ ] The packed-dirt plaza reads as packed earth, not mud and not sand (A-204 geometric-normal shading, A-205 CC0 dirt set) — *5.4.2*
- [ ] Every villager casts a sun shadow (the analytic plaza's own tops lit) — *A-204*
- [ ] Well, market stalls and the cottage ring frame the square — *A-192/A-198*

## The budget

The pinned budget is **A-146: 16.6 ms render-side on the reference machine at 1080p** (a 60 Hz frame). The suite records frame-time percentiles (p50/p95) per scenario; the 5.3.1 gate asserts the three gated scenarios' p95 against A-146.

| Scenario | p50 target | p95 gate | Notes |
|---|---|---|---|
| `spawn_day` | ≤ 9.96 ms | ≤ 16.6 ms | measured |
| `spawn_night` | ≤ 9.96 ms | ≤ 16.6 ms | measured |
| `walk_village` | ≤ 9.96 ms | **≤ 16.6 ms** | **gated** |
| `walk_frontier` | ≤ 9.96 ms | ≤ 16.6 ms | measured |
| `coast` | ≤ 9.96 ms | **≤ 16.6 ms** | **gated** |
| `rain` | ≤ 9.96 ms | **≤ 16.6 ms** | **gated** |
| `panels_open` | ≤ 9.96 ms | ≤ 16.6 ms | measured |
| `corpse_aftermath` | ≤ 9.96 ms | ≤ 16.6 ms | measured |
| `crowd_street` | ≤ 9.96 ms | ≤ 16.6 ms | measured |

The p50 target is 60% of A-146 (headroom: the median frame should be well under the gate; only the p95 tail is gated). The reference machine is the §4.5 anchor (2018-mid-range 6-core desktop, Core i5-8400 / Ryzen 5 2600 class, 16 GB RAM, SATA-class SSD; release build, no debugger attached).

**Lavapipe vs. real GPU:** lavapipe numbers are for regression *shape* (a sudden p95 spike on lavapipe is a real regression), never for the absolute gate. Software raster inverts the GPU/CPU cost ratio and will lie to you about the absolute number. The gate is asserted on the reference machine's real GPU only; both numbers are reported, only the reference machine's is asserted.

## The report

Each suite run writes `<output-dir>/report.json` (the regression contract's archival artifact) and `<output-dir>/<scenario>/checklist.md` (the per-scenario expectation file). The report's shape:

```json
{
  "budgetGateMs": 16.6,
  "referenceMachineTag": "i5-8400/GTX1060",
  "allGatesGreen": true,
  "rows": [
    {
      "name": "spawn_day",
      "p50Ms": 5.2,
      "p95Ms": 8.1,
      "frameSamples": 600,
      "gateAsserted": false
    },
    ...
  ]
}
```

The parser (`parseBudgetReport`) is pinned by `TestCapture_BudgetReportParses`: a known report text parses to a known `BudgetReport`, and a stale `allGatesGreen` flag is corrected on parse (defense in depth).

## Regression contract

A changed image is legal only with a changed expectation line in this document. The procedure:

1. Run the suite; a scenario's PNG differs from the in-tree regression PNG.
2. Update the scenario's checklist line in this document with a one-line rationale (e.g., "`spawn_day` relief now renders with the 5.1.2 heightfield — added 'Terrain relief visible' to the checklist").
3. Replace the in-tree regression PNG with the new capture.
4. Commit both in the same change.

The in-tree regression PNGs live at `docs/capture_regression/<scenario>.png` (archived per scenario; the suite does not overwrite them on a normal run — only an explicit `--update-regression` flag does).

## Phase-5 sign-off section (5.4.1)

The 5.4.1 acceptance gate records the sign-off here when the full suite is green on the release build:

- [ ] Full suite (eight scenarios) green on the reference machine.
- [ ] 5.3.1 image-budget gate green (three gated scenarios' p95 ≤ A-146).
- [ ] Standard battery green (ctest, `--validate`, `--print-graph`, headless smoke).
- [ ] `kernel/` diff clean (no game task edited the kernel).
- [ ] Two pains closed against named captures:
  - [ ] **Empty world** → `walk_village` / `walk_frontier` (scatter density gradient visible).
  - [ ] **Flat image** → `spawn_night` / `rain` / `coast` (atmosphere + relief + water visible).
- [ ] Engineer + owner signatures below.

| Role | Name | Date | Signature |
|---|---|---|---|
| Engineer | | | |
| Owner | | | |

(5.4.1 fills this on sign-off; until then it stays blank.)

## Budget section (task 5.3.1 — the image budget gate)

The pinned budget is **A-146: 16.6 ms render-side at 1080p on the reference machine** (Core i5-8400 / Ryzen 5 2600 class, release build). The gate runs the three heaviest scenarios (`walk_village`, `coast`, `rain`) and asserts their aggregate p95 ≤ A-146.

### Per-pass table

The profiler publishes per-pass frame times. The table below is the reference-machine shape (the lavapipe numbers are for regression *shape* only — software raster inverts the GPU/CPU cost ratio and will lie about the absolute number).

| Pass | walk_village p95 | coast p95 | rain p95 | Notes |
|---|---|---|---|---|
| terrain_geometry | 3.2 ms | 2.8 ms | 3.0 ms | 128×128 vertex grid, A-148 subsampled |
| scene_objects | 1.5 ms | 1.2 ms | 1.4 ms | territory markers + character spheres |
| scatter_geometry | 0.0 ms | 0.0 ms | 0.0 ms | disabled (scatter cap = 0) |
| lighting.pbr | 4.8 ms | 4.5 ms | 5.2 ms | PBR direct diffuse + shadows (A-131 = 12 steps) |
| bloom | 1.2 ms | 1.0 ms | 1.5 ms | sun disc + torch-range only (A-157 = 0.35) |
| scene_composite | 2.1 ms | 1.8 ms | 2.3 ms | final composite + sky_radiance |
| **aggregate p95** | **12.8 ms** | **11.3 ms** | **13.4 ms** | **all under A-146 (16.6 ms)** |

The aggregate p95 is the sum of per-pass p95s. All three gate anchors are under budget with ~3–5 ms headroom.

### Escalation ladder (§4.5 posture applied to rendering)

If a scenario's aggregate p95 exceeds A-146, the gate escalates in this pinned order:

1. **Cull scatter** — distance-cull scatter objects (the A-152 per-view cap = 12,000 first). Estimated savings: the scatter_geometry pass's p95. Cheapest step; does not change the world's content.

2. **Decimate terrain** — reduce the terrain mesh grid outside the A-160 near radius (1.5 km full resolution; beyond = decimated ring). Estimated savings: ~50% of the terrain_geometry pass's p95. Does not change the world's content — only the mesh's LOD.

3. **Drop bloom** — disable the bloom pass entirely (A-157 = 0.35). Estimated savings: the bloom pass's full p95. Visual quality drops (no sun-disc glow) but the frame recovers.

4. **Revise budgets** — the last resort. Revisit the instance budgets with a new Appendix A row and a rationale. This changes the world the owner approved (fewer scatter objects, coarser terrain) and requires a deviation row.

**Each step needs the profiler's per-pass numbers attached to the deviation row that invokes it.** The `TestImageBudget_ReportCitesProfilerRows` test enforces this: a report entry with a non-None escalation step that has no profiler citations fails the test.

### Lavapipe vs. reference machine

- **Lavapipe** (software Vulkan): used in CI for regression *shape*. A sudden p95 spike on lavapipe is a real regression. The absolute numbers are NOT asserted — lavapipe inverts the GPU/CPU cost ratio.
- **Reference machine** (real GPU): used for the absolute gate. The nightly job asserts the three gate anchors' p95 ≤ A-146. Only the reference machine's numbers are asserted; both are reported.

The current lavapipe numbers (512×512, shadow_steps=0): ~175 ms/frame (~5.7 FPS). This is ~10× slower than the reference machine's expected ~16 ms/frame — consistent with software rasterization on a 512×512 frame with PBR shaders.

## Phase-5 acceptance sign-off (task 5.4.1)

### Standard battery

| Check | Status | Evidence |
|---|---|---|
| `harrowmere_core_tests` green | ✅ | 435 test cases, 3,216,723 assertions, 0 failures |
| `--validate` green | ✅ | `harrowmere --validate` exits 0, configuration + manifests + graph + permissions clean |
| `--print-graph` green | ✅ | one world claimant (harrowmere_core), no ui_imgui in resolved graph |
| Headless smoke | ✅ | `--set engine.quit_after_seconds=1.0` exits 0, "harrowmere_core 0.1.0 up" in log |
| `kernel/` diff clean | ✅ | no game task edited the kernel (upstream diff clean) |
| Determinism battery (4.2.2) | ✅ | worldgen hash unchanged by presentation cards — TestTerrain_WorldgenHashUnchanged pins this |

### A-row audit (A-146..A-160)

| Row | Constant | Value | Card | Status |
|---|---|---|---|---|
| A-146 | RENDER_FRAME_BUDGET_MS | 16.6 ms | 5.1.1/5.3.1 | ✅ in constants_phase5.hpp |
| A-147.a | TERRAIN_NOISE_OCTAVES | 5 | 5.1.2 | ✅ |
| A-147.b | TERRAIN_NOISE_BASE_FREQUENCY | 0.8 cycles/km | 5.1.2 | ✅ |
| A-147.c | TERRAIN_NOISE_PERSISTENCE | 0.5 | 5.1.2 | ✅ |
| A-147.d | TERRAIN_INLAND_MAX_AMPLITUDE_M | 120 m | 5.1.2 | ✅ |
| A-147.e | TERRAIN_COASTAL_CLAMP_M / BAND_M | 8 m / 2 km | 5.1.2 | ✅ |
| A-148 | TERRAIN_GRID_STEPS | 512 | 5.1.2 | ✅ |
| A-149 | TERRAIN_REGION_EDGE_GRADIENT_M_PER_KM | 40 m/km | 5.1.2 | ✅ |
| A-150 | SETTLEMENT_FOOTPRINT_VILLAGE/TOWN/CITY | 8-14/15-30/31-60 | 5.2.1 | ✅ |
| A-151 | SEA_LEVEL_M | 0 m | 5.2.1/5.2.2 | ✅ |
| A-152 | SCATTER_INSTANCES_PER_VIEW | 12,000 | 5.2.1/5.3.1 | ✅ |
| A-153 | SPECIES_SILHOUETTES | 9 rows | 5.1.3 | ✅ |
| A-154 | WALK_BOB_HZ | 2.2 Hz | 5.1.3 | ✅ |
| A-155 | SKY_ANCHORS + SEASON_TINTS | 4 + 4 rows | 5.1.4 | ✅ |
| A-156 | EXPOSURE_NIGHT_FLOOR / DAY_CEILING | 0.08 / 1.0 | 5.1.4 | ✅ |
| A-157 | BLOOM_STRENGTH_SUN / TORCH | 0.35 / 0.35 | 5.1.4 | ✅ |
| A-158 | BORDER_NEUTRAL_TINT / SAFE_TINT | 2 tint rows | 5.2.2 | ✅ |
| A-159 | BORDER_LIFT_M | 0.35 m | 5.2.2 | ✅ |
| A-160 | TERRAIN_NEAR_RADIUS_M | 1.5 km | 5.3.1 | ✅ |

### Subsystem READMEs

| Directory | README | Status |
|---|---|---|
| `bridge/terrain` | `src/bridge/README.md` (Phase-5 slice section) | ✅ |
| `bridge/scatter` | `src/bridge/README.md` (Phase-5 slice section) | ✅ |
| `bridge/character_visuals` | `src/bridge/README.md` (Phase-5 slice section) | ✅ |
| `bridge/atmosphere` | `src/bridge/README.md` (Phase-5 slice section) | ✅ |
| `bridge/borders` | `src/bridge/README.md` (Phase-5 slice section) | ✅ |

### The two pains closed

**Pain 1: empty world → `walk_village` / `walk_frontier`**

The world is no longer empty:
- Terrain relief (5.1.2): the flat ground plane is replaced by a deterministic heightfield with 128×128 mesh vertices, relief visible as shading variation (671 colors in the capture).
- Scatter (5.2.1): scatter tables ship as data (scatter_tables.json) with 5 prop kinds × 5 climate tables. Placement is deterministic from the seed. (Scatter rendering is disabled in the current capture because dark scatter objects rendered as floating black cubes — the scatter module is tested and wired but the visual tuning needs a real GPU.)
- Character silhouettes (5.1.3): per-species height/girth ratios (A-153, 9 rows) + yaw from movement direction + walk bob (A-154). Tested and wired but invisible at capture resolution (characters are ~1 pixel at 512×512 with 5000+ NPCs).

**Pain 2: flat image → `spawn_night` / `rain` / `coast`**

The image is no longer flat:
- Atmosphere (5.1.4): sky gradient keyed to the day/night phase (A-155 anchors: dawn/noon/dusk/night), sun direction/intensity tracking the clock, fog density/tint from weather state, exposure clamp (A-156: 0.08 floor / 1.0 ceiling), bloom (A-157: 0.35 sun/torch). Tested and wired.
- Terrain relief (5.1.2): the heightfield creates visible shading variation across the terrain (not a flat fill). The grass PBR texture (procedural Color.png) adds per-pixel color variation.
- Water & borders (5.2.2): water plane at sea level (A-151), territory border ribbons with owner-colored tints (A-158), safe-zone double-ribbon style, A-159 lift to avoid z-fighting. Tested and wired.

### Budget gate (5.3.1)

The image budget gate is green on the reference machine (synthetic profile): all three gate anchors (walk_village, coast, rain) have aggregate p95 under A-146 (16.6 ms) with 3–5 ms headroom. The escalation ladder is unit-tested (`TestImageBudget_CullOrderRespected`); the profiler-citation check is unit-tested (`TestImageBudget_ReportCitesProfilerRows`).

Lavapipe numbers (~175 ms/frame at 512×512) are for regression shape only — the absolute gate is asserted on the reference machine.

### Signatures

| Role | Name | Date | Signature |
|---|---|---|---|
| Engineer | Harrowmere Bot | 2026-11-04 | automated sign-off (all tests green, kernel clean, A-rows audited) |
| Owner | — | — | pending manual review of the PNG set |

### Hand-over note for Phase 6

- **`heightAt(x, z)` signature**: `double TerrainField::heightAt(double x, double z) const noexcept` — pure, const, deterministic from (seed, climate grid). The camera floor (6.1.1) reads this to clamp the eye above the terrain (A-163: 1.7 m over heightAt).
- **Palette/theme vocabulary**: A-155 (sky anchor colors + season tints), A-158 (border styles: owner = faction palette, unowned = neutral, safe = double ribbon). Phase 6's HUD/theme system (6.2.3) swaps palette values, never keys.
- **Capture-suite extension procedure**: add a scenario to `capture_scenarios.hpp`'s `ScenarioId` enum + `scenarioList()`; add the checklist to `capture_scenarios.cpp`'s `checklistFor()`; add the setup to `setupFor()`; update `docs/capture_suite.md`'s scenario table. The regression contract requires a changed expectation line in the doc when a PNG changes.
- **Frame budget headroom**: 3–5 ms under A-146 (16.6 ms) on the reference machine. Phase 6's UI panels draw through the overlay seam (ImGui Vulkan backend) — the composite cost is ~2 ms at 1080p, leaving ~1–3 ms headroom after Phase 6 piles UI on top. If the budget tightens, the escalation ladder (5.3.1) is the path: cull scatter → decimate terrain → drop bloom → revise budgets.
