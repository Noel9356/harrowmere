# Bridge

The bridge is the presentation seam between the authoritative simulation
(`harrowmere_core` stores and systems) and the engine's `scene` capability
(runtime object registry). It is a **pure viewer**: it reads world/clock
state and pushes scene objects; it contains no gameplay logic and mutates no
sim state (D-ARCH-3, common.md §4.4 step 13).

## Phase-1 slice (task 7.2P1)

The Phase-1 bridge renders the static world only:

- **One tall thin box per territory center.** Height is by territory type:
  village 5 m, city 10 m, kingdom 15 m, empire 20 m. Tint is by type.
- **One flat tinted disc per safe zone.** Radius matches D-SHELTER-1's
  50 m shelter radius.
- **Sun light** driven by `WorldClock::dayPhase01()`; repushed only on a
  ≥ 1 % phase delta to avoid per-frame registry traffic.
- **Sky tint** driven by the primary weather state, table A-117
  (clear / rain × 0.7 / storm × 0.45 / snow × 1.2).

No characters, corpses, loot, animation, or NPC culling ship here — Phase 2
(card 7.2) extends this bridge to add them. A-118 (`PRESENTATION_RADIUS_M`)
is declared at the seam so the Phase-2 culling path extends rather than
rewrites.

## Files

| File | Role |
| --- | --- |
| `scene_bridge.hpp` | Constants (A-117 / A-118 / A-119), the three bridge-side interfaces (`SceneRegistrySink`, `WorldViewForBridge`, `ClockViewForBridge`), and the `SceneBridge` class. |
| `scene_bridge.cpp` | Per-frame logic: chunked creation, sun hysteresis, sky-tint hysteresis. |
| `README.md` | This file. |

## Interfaces

The bridge depends on three abstract interfaces so it is unit-testable
without the engine or a full `WorldStore` / `WorldClock` graph:

- `SceneRegistrySink` — scene-registry operations (`spawnBox`, `spawnDisc`,
  `setObjectPosition`, `setSunLight`, `setSkyTint`). The production adapter
  wraps the engine's `SceneCapability` (acquired via `ComponentContext` in
  `harrowmere_plugin.cpp`).
- `WorldViewForBridge` — the territories to render (sorted by id, R7) plus
  the primary weather state. The production adapter wraps `WorldStore`
  (task 1.4) and computes territory centers from the region grid (A-40).
- `ClockViewForBridge` — `dayPhase01()`. The production adapter wraps
  `WorldClock` (task 0.3).

Tests provide fakes for all three (see
`mods/harrowmere_core/tests/test_scene_bridge.cpp`).

## Subscription wiring

The bridge does **not** subscribe to `EngineFrameEvent` itself — it exposes
`onFrame()` and the plugin (`harrowmere_plugin.cpp`) subscribes on its
behalf with a priority lower than `harrowmere_ui`'s overlay subscription so
the UI draws on top (common.md §4.0: "higher dispatches first"). This keeps
the bridge decoupled from the engine's `EventBus` type.

## Chunking

Creations are chunked at ≤ `SCENE_CREATE_CHUNK_PER_FRAME` (A-119 = 32) per
frame. The pending queue is built once from `WorldViewForBridge::territories()`
and drained 32 tasks at a time; a world with 120 objects to spawn drains in
4 frames. The queue is territory-id-sorted (R7) and box-before-disc per
territory, so the spawn order is deterministic across runs.

## Constants placement

Per DoD §1.5 every numeric constant belongs in `core/constants.hpp`
(append-only per task 0.2). For the 7.2P1 drop-in deliverable the bridge
constants (A-117, A-118, A-119, plus task-spec-pinned geometry) are kept
local to `scene_bridge.hpp` so this slice does not modify an existing
`constants.hpp`. On integrate, move them into `core/constants.hpp` with
their `// A-nn` Appendix A references intact.

The presentation tints (`TINT_VILLAGE` / `TINT_CITY` / `TINT_KINGDOM` /
`TINT_EMPIRE` / `TINT_SAFE`) are 7.2P1 values not yet in Appendix A; add
them via the deviation protocol (§1.4) or make them data-driven
(`data/bridge_tints.json`) on integrate.

## What does NOT live here

- **Scene handles outside `src/bridge/`.** Per the card's Done-when, no code
  outside `src/bridge/` touches scene handles. The bridge is the only path
  to the scene registry.
- **Tick-step work.** The bridge is frame-coupled, not tick-coupled. Do not
  drive it from a scheduler step; it rides `EngineFrameEvent` only
  (§4.4 step 13).
- **Gameplay logic.** No combat, no economy, no justice, no events. The
  bridge reads; it does not decide.
