# src/player — the sim-side of the player (task 3.10)

The player-control spine: **intents in, sim state out**. The input layer
(7.4, the UI mod) produces intents through the published capability header's
`SimCommand` envelope (`Move{dir}`, `Attack{target}`, `Interact{target,
action}`, `Emote{act}` — the vocabulary lives there because it is the only
header the input mod compiles against); this directory consumes them on the
fixed-update lane and owns the sim-side player state.

| File | Role |
|---|---|
| `controller.hpp/.cpp` | `PlayerController` (the sim-side transform, the AFK state), `PlayerActions` (the action log: reqActions counters + the observable suspicion acts), `IntentQueue` (the FIFO drain side of §4.2's write path), and **`applyIntents(...)`** — THE step-9 slice. |
| `interactions.hpp/.cpp` | The `Interact{target, action}` semantics: `InteractionResolver` (target id → position; null default, D-SINK-1), `InteractionHandlers` (the action → handler registry), `routeInteraction` (the proximity gate + dispatch), `InteractionGate` (the rejection vocabulary). |

## The one entry point (and its Phase-2 migration)

`applyIntents(player, queue, clock, dt, resolver, handlers)` is the card's
`applyIntents(dt)`: drain the queue FIFO, then ONE kinematic integration for
the slice's winning motion, then the AFK verdict. There is no Scheduler in
Phase 1 — the headless tests drive the function directly (the card's
Done-when) — and when 3.1's Scheduler lands (Phase 2) it registers this
function into step 9 of the 14-step table (§4.4 names step 9 "Player") as a
**re-registration, not a rewrite**, binding the character-store-backed
resolver and the owner-registered handler table. The Phase-1 tests stay
green unchanged.

The clocks are read-only here: they advance at step 0 (0.3's `WorldClock`;
3.8's Phase-1 slice composes that advance). A pinned test fails if
`applyIntents` ever advances them — a double-advance would drift every
composed Phase-1 slice.

## The slices' semantics

- **Move is motion state, not a displacement instruction.** The input layer
  polls keys per frame and re-submits the camera-relative direction every
  frame (7.4P1), so a 10 Hz tick receives bursts. The drain takes the
  LATEST Move in the slice (latest wins) and integrates exactly once:
  `speed × dt` at A-57's `MOVE_SPEED_MPS` (4 m/s; sprint ×2, free in
  Phase 1 — the rest-need cost lands with 2.1's player mirror). Frame-rate
  independent, and the reading that keeps D-AFK-1 coherent (intents flow
  continuously while playing, so "no intents for 120 real seconds" means
  the player really went idle). The zero direction is the pinned no-motion
  fallback (R2) — never a NaN.
- **Attack drains deterministically.** Its application is step 10's
  `applyDamage` (the §4.6 worked trace: step 9 consumes, step 10 applies);
  the consumer lands with 3.2/3.3 (Phase 2). Phase 1 leaves the state
  untouched — the drop is pinned by a test so Phase 2's arrival is
  conscious.
- **Interact routes through the gate.** Resolve the target
  (`InteractionResolver`), gate on proximity (INCLUSIVE at
  `INTERACT_RANGE_M`, x/z plane, through region.hpp's D-WORLD-1
  `distanceSquaredM`), dispatch to the registered owner handler.
  Unregistered actions consume as `NoHandler` (the D-SINK-1 null default).
  The per-action owners: talk → trade 3.7 / title dialog 6.3, loot → 3.3,
  pick_up/drop → 2.4, report_crime/bribe → 4.1, surrender → 4.3. The
  target-TYPE split (vendor vs noble for `talk`) reads roles that land in
  Phase 2 — the owner handlers do it, not the gate.
- **Emote toggles an observable act.** CROUCH / LIGHT_TORCH (UNLIT's
  counter-act) / MASK (the MASKED act), edge-triggered (one intent = one
  flip; the input path owns de-duplication — 7.4P1's double-fire pitfall).
  The mask's item-ownership check activates with 2.4's inventory; the
  Phase-1 toggle is free but the act is logged (card 3.10 verbatim; the
  `cloth_mask` item with `enablesAct: "MASKED"` ships in items.json since
  1.1).
- **AFK (D-AFK-1, A-58).** ANY drained intent — even one the rules will
  reject — refreshes `lastIntentRealTime = clock.realTime` and clears
  `afk`; after the drain, `afk = (realTime − lastIntentRealTime ≥ 120)`.
  REAL seconds (one of D-TIME-1's three real-time rules), u64, monotonic —
  no underflow (R1), never the wall clock (R8). Consumers: 4.5 (suspicion
  exemption) and 6.3 (activity validation, "AFK time never counts").
  Session-scoped: `afk` and `lastIntentRealTime` are deliberately NOT save
  data (7.1 persists the transform, the act toggles, the counters).

## The action log (`PlayerActions`)

Counters mirroring `NpcState.actionCounters` (task 2.0's NPC side): kills
by sapience class, crafts, purchases — the `reqActions` inputs of 2.3 and
quest objectives (6.4). The `recordKill/recordCraft/recordPurchase`
helpers are the single write path their owner systems call when they land
(kills 3.3, crafts 2.4, purchases 3.7).

The act surface lives on the same log: `exposes(SuspicionAct)` and
`appendActiveActs` (enum order, R7). The three player-toggled acts are
CROUCH, UNLIT (true while NO torch burns — the absence of light), MASKED;
TRESPASS / BLOODSTAINED / FLEE read false here — their owners set them
(4.5, 4.1, 3.2). Whether an exposed act is SUSPICIOUS is 4.5's read
(NIGHT + no-darkvision masks gate it there); this log carries the honest
state only.

## Deliberate seams (do not build past them)

- `InteractionResolver`: the Phase-2 character store binds the real
  position lookup at the step-9 registration. Single-target lookup, not a
  neighbour scan — the spatial grid (1.6) stays the only radius-query path.
- `InteractionHandlers`: owners register between ticks, never mid-tick.
  Handlers that cause work in a later step enqueue via the scheduler
  (§4.4 reentrancy rule); rejections surface as `PlayerSignal`s when the
  runtime wiring lands (§4.2 rule 2).
- `submit()` in the capability header stays the v0.1 stub until the runtime
  bootstrap wires the plugin-owned queue (the same line 3.8's
  DEVIATIONS.md row records); `IntentQueue` is the landfall it will push
  into.
- New intent variants (consent-gated intents, 2.7) extend the published
  `SimCommand` envelope — a new `Kind` value + payload field — without
  breaking this drain.

Constants: A-57 (`MOVE_SPEED_MPS`, sprint ×2), A-58
(`AFK_TIMEOUT_REAL_S`), and the card-unpinned `INTERACT_RANGE_M` (3 m,
pinned pending Appendix A promotion — DEVIATIONS.md) all live in
`core/constants.hpp`.

Tests: `tests/test_player_control.cpp` — the card's five pinned tests
(`TestMove_IntentsApplyOnFixedUpdate`, `TestInteract_RangeGated`,
`TestAfk_TimeoutAndClear`, `TestCrouchMask_ExposeSuspicionActs`,
`TestActionLog_CountersIncrement`) plus the structural pins (FIFO order,
read-only clocks, the Attack drain, the standalone defaults).
