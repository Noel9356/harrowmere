# src/core — the mod's core headers

The shared vocabulary of the simulation: doc-pinned types, the closed enum
lists, and the sim event channels. Everything here is internal to
harrowmere_core - consumers never compile against it (§4.2: the published
surface is `src/bridge/harrowmere_capability.hpp` and nothing else); the unit
tests include these headers directly.

| File | Owner | Content |
|---|---|---|
| `types.hpp` | task 0.2 | the verbatim id aliases and `Position` from design reference §Conventions (R10) |
| `enums.hpp` | task 1.1 | the closed enum lists (R9: every list ends in `COUNT`) |
| `game_events.hpp` | task 0.7 | the typed sim event channels - one struct per event, one engine channel per struct type |
| `constants.hpp` | task 0.2 | the central constants registry (A-nn references) - lands with 0.2 |
| `math.hpp/.cpp` | task 0.2 | SCALE helpers, integer sqrt, wide-product division, XP clamping - lands with 0.2 |
| `clock.hpp/.cpp`, `rng.hpp/.cpp` | task 0.3 | WorldClock and SeedRng - land with 0.3 |
| `log.hpp` | task 0.5 | the `HARROWMERE_LOG_*` wrapper over the logging capability - lands with 0.5 |

Provenance note: tasks 0.2/0.3/0.4/0.5 run in parallel with 0.7, and 0.7's
event structs reference the id aliases and `CrimeType`, so `types.hpp` and the
`CrimeType` slice of `enums.hpp` landed with task 0.7 - verbatim from the
owning cards' pinned blocks (see `docs/DEVIATIONS.md`). The files are
append-only until their owning tasks complete.

## The sim event rules (task 0.7)

The engine's `EventChannel` is the only fan-out medium; publishing is
synchronous on the caller's stack. Three rules, pinned in the header and
enforced by review:

1. **Events are facts that already happened.** Publish after the mutation is
   complete - a handler must be able to read the post-state (§4.6's worked
   trace: a manhunt handler sees the culprit already inside its target set).
2. **No re-entrant publish** to the channel you are dispatching in; queue
   later work through the scheduler's pending list (`enqueueForNextTick`,
   §4.4) or the engine's `publishDeferred`.
3. **Subscriptions are RAII** (`engine::EventSubscription`): a stopped
   component never receives a dangling dispatch. An unsubscribe that lands
   during a dispatch is safe - the kernel defers the structural removal and
   skips the removed handler for the rest of that scan (both halves pinned by
   `TestEvents_UnsubscribeDuringDispatchDeferred`).

The struct set is pinned by task 0.7; extending it requires a DEVIATIONS.md
row (§1.4). Later tasks publish and consume only those structs.
