# events/ — the event engine and its organs (tasks 5.1–5.5)

Events are data-driven state machines. A template row in `data/event_templates.json` (Appendix B's schema) declares states, transitions, conditions, entry/exit actions, contribution caps/weights and an optional reward table; the engine (`engine.{hpp,cpp}`) runs the lifecycle headless — the world resolves with zero player input (every state's transitions carry a TimerCondition fallback, or the template declares `maxDurationDays` and the forced exit owns the resolution).

| File | Task | What lives here |
|---|---|---|
| `template.{hpp,cpp}` | 5.1 | the template records, the strict JSON loader, the validator (1.3's cross-ref stage) |
| `engine.{hpp,cpp}` | 5.1 | the doc structs (`Event`, `EventState`), the lifecycle (startEvent/join/leaveEarly/endEvent), scale uniqueness, the depth cap, the tick (step 5), the action interpreter |
| `difficulty.{hpp,cpp}` | 5.2 | the self-balancing difficulty loop — signed delta (R1's founding example), binary/continuous performance, ONE update per state attempt (D-DIFF-1) |
| `contribution.{hpp,cpp}` | 5.3 | validation guards, the contribution math, power snapshots (A-106 cadence), the runtime damage/heal router, the 5.4 instrumentation counter |
| `rewards.{hpp,cpp}` | 5.4 | the single-pass payout (total computed once, remainder to the top contributor), the reward-table routing, A-107's difficulty scaling, the manhunt completion poll |
| `world_effects.{hpp,cpp}` | 5.5 | the step-2 weather-bias slice + the D-FLAGS-1 split documented below |

## Region flags: the persistent half of the split (D-FLAGS-1)

**This is the doc's least obvious rule, so it gets its own section.**

The action vocabulary splits into two worlds:

* **Transient — state-scoped.** Spawned targets, price/weather biases, evacuations, MANHUNT enforcement: all *return to default* when the event ends ("Events eventually return the world to its default state").
  * Spawned targets despawn **at endEvent** (the engine's `despawnSpawnedTargets`, wired into `finishEvent`) — the template's explicit `DespawnTargets` action (B.6's raid) stays available and is idempotent with it. Their deaths generated evidence/hpLost while alive; removal strips nothing retroactively (the amounts were already validated) — only the actors leave.
  * Price/weather biases are timed records; they expire by themselves (`priceBiasFor` answers SCALE again past the until-timestamp; the step-2 slice pins the region weather only inside the window).
* **Persistent — world-scoped.** `SetRegionFlag` writes `Region.flags` and is **NEVER reverted by endEvent**. The doc: "…while flags may remain changed". Only a later event, a war outcome (6.2), or an explicit template action changes the bits. Region flags drive environment flavor (3.8), NPC behavior masks (3.4's intents), event branching (5.1's `FlagCondition`) and settlement/war outcomes (6.2).
* `GarrisonTerritory` (the D-ACTION-1++ extension, DEVIATIONS.md row) is the reconquest exit: the event's NPC participants take the data-named guard role (3.6's `assignRole`) and the territory's lawless flag clears — the shipped `GARRISON_RETURN` template is the doc's NPC-reconquest path out of lawlessness.

An endEvent that clears a region flag is a bug; if a design wants a flag back, the template author writes the second `SetRegionFlag` action.

## Contribution & rewards in one paragraph

Combat damage and heals record through `validateActionAmount` (the doc's full guard set: self-harm, invulnerable targets, template-targets-only, player consent-or-warrant, the heal-economy triple-min, the OBJECTIVE budget) and only the validated amount accumulates. Only template targets pay; healers earn only for hp the targets removed; the power watermark damps overpowered participants (`raw * SCALE / (SCALE + isqrt(snapshot))`); token participation (≤ MIN_CONTRIBUTION) pays nothing. The payout runs at endEvent (before the clears — the doc order), single-pass, excludes smugglers from the total, and hands the flooring remainder to the top contributor. The pool scales with the state difficulty (A-107). Rewards are per-template and optional: a rewardless template pays nothing at any payout point — a no-op, not an error.

## The war battle (6.2, D-WAR-EVT-1)

The shipped `BATTLE` template (regional, `maxDurationDays: 1`, rewardless) declares `special.dualTargetGroups`: the war module's spawn (`IEventSpawner::spawnWarBattle`, the war store's step-8 tick) books the two target groups engine-side (`targetGroups_` — group 0 the attacker side, 1 the defender side), inserts every member into the event's `targetIds` (each side IS the other's targets) and joins them as participants. The war module computes the two member lists — the engine stays faction-blind (D-SINK-1 cuts both ways). One live BATTLE per contested region: the scale-uniqueness gate is the spawn throttle and the template's duration is the cadence. The shape is documented in `src/factions/README.md`; the engine-side book retires with its events at the sweep.
