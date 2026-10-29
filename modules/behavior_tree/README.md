# behavior_tree module

The engine's AI decision service: trees of stateful composites, decorators and leaves, ticked per frame or per explicit call, with leaf behavior living on the consumer side. The module provides the `BehaviorTreeCapability` (family `behavior_tree`, implementation-bound member `behavior_tree.basic`, published in `behavior_tree_capability.hpp` next to this module) and requires nothing but logging. Before it existed the engine had scripting and timers but no decision structure: an enemy that patrols, chases and attacks would hand-roll the same advance-and-branch loop every other agent rolls, each with its own forgotten reset and its own "stuck in chase forever" bug. This capability is that one structure - the module owns the SHAPE of decision making, consumers own the ACTIONS.

## Capability surface

Two registries and one operation. **The action registry**: `registerAction(name, callback)` / `unregisterAction` / `registeredActionCount` - the callbacks behind `action` and `condition` leaves (the same `std::uint32_t()` signature serves both kinds; the distinction is for the reader). Re-registering a name replaces its callback, and because leaf resolution re-validates against the registry's version stamp on the next evaluation, a live tree picks the new callback up with no invalidation ceremony - one map assignment is the hot-reload story. **Tree construction**: `createTree(description)` validates the whole shape up front (kind/arity rules, a 64-level depth cap, a 1024-node cap, finite waits) and refuses with a precise log; a leaf whose action name is not yet registered is deliberately NOT a refusal - resolution is lazy, so native code and scripts may register actions after tree creation. **The operation**: `tick(handle, deltaSeconds)` returns the root's status; `reset(handle)` clears the runtime memory; `setAutoTick(handle, bool)` opts a tree in or out of the frame tick; `destroyTree`, `activeTreeCount`, `treeName` round out the lifecycle.

## Semantics

The composites are the **stateful** kind: a `sequence` (all children must succeed, in order) and a `selector` (first success wins) resume at the running child instead of re-evaluating from the start, and a composite that completes or aborts resets its whole subtree's memory. Memory-only-while-running is the semantics game AI actually wants, and it is what keeps the built-in `wait` leaf honest under re-runs: a wait that was aborted restarts full, a wait that completed refills before reporting success. The decorators are `invert`, `force_success`, `force_failure` and `repeat(count)` - repeat evaluates its child ONCE per tick and counts completions across ticks (the unambiguous reading once leaves carry time; a multi-evaluation reading would burn the same delta into every evaluation and complete a wait instantly). The built-in leaves beyond wait are `succeed` and `fail`; registry leaves whose name is missing tick to failure, warning ONCE per node (a misnamed leaf must be visible, an auto-ticked tree must not spam the log every frame). A callback returning a value outside the three real statuses degrades to failure - a caller bug must never read as running (a stuck tree) or success (a false positive).

## Frame behavior

Auto-ticked trees advance on the kernel's `EngineFrameEvent` loop heartbeat - the same tick the scheduler, the tweens, physics and the game state walk use (see `docs/architecture.md`, "The frame clock") - in creation order and with the shared frame delta, which is the determinism contract: same trees, same frame, same statuses (the reproducibility discipline the random, noise and fixed-timestep paths keep). The tick source is the loop, not the renderer: a headless run auto-ticks exactly like a rendered one. The module reads no clock of its own - the delta arrives with the event, and tree-free frames short-circuit at the empty check before anything runs. Manual ticks carry their own delta and are the tooling/testing story; a tree ticked manually AND auto-ticked runs twice per frame by definition - `setAutoTick` exists for exactly that choice. `BehaviorTreeFinishedEvent` publishes on the running->done transition, never per finished tick: a permanently succeeding root publishes once and goes quiet until it runs again. The event is news, not a stop signal - auto-tick keeps ticking finished trees, and a one-shot consumer destroys or resets from the event.

## Wiring

| Manifest field | Value |
|---|---|
| provided capabilities | `behavior_tree.basic`, `scripting.lua_bridge.behavior_tree` |
| required capabilities | `logging` |
| required permissions | `capability.logging` |

## Lua bridge

The module ships the `scripting.lua_bridge.behavior_tree` extension (the host discovers it through the multi-provider acquisition of the `scripting.lua_bridge` family), which installs seven functions into every script state's `engine` table. The bridge is DECLARATIVE by design: a Lua table is converted into the same description the native surface builds, in the call, retaining no Lua references - the lifetime discipline every shipped bridge follows (the extension object outlives any single script state). Leaf behavior reaches a script-built tree through the native action registry or the registry-free built-ins:

```lua
-- Native side registered "patrol", "chase", "can_see_player":
local guard = engine.behavior_create_tree("guard", {
    kind = "selector",
    children = {
        { kind = "sequence", children = {
            { kind = "condition", name = "can_see_player" },
            { kind = "action",    name = "chase" },
        }},
        { kind = "action", name = "patrol" },
    },
})

-- Registry-free tree: a beat sheet for a cutscene camera
local beat = engine.behavior_create_tree("beat", {
    kind = "sequence",
    children = {
        { kind = "wait", seconds = 1.5 },
        { kind = "succeed" },
        { kind = "wait", seconds = 0.5 },
    },
})

local status = engine.behavior_tick(guard)            -- or let the frame tick drive it
engine.behavior_status_name(status)                   -- "success" / "failure" / "running"
engine.behavior_set_auto_tick(beat, false)            -- manual ownership
engine.behavior_reset(beat)
engine.behavior_destroy(beat)
```

## Configuration

The module reads its `[behavior_tree]` section during startup. Unknown keys are ignored, and a missing section means the defaults below.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `auto_tick` | bool | `true` | whether newly created trees join the module's frame tick by default; per-tree opt-out through `setAutoTick` |

## Implementation

Standard library only - no third-party code. The description is copied once at `createTree` into a runtime tree of `RuntimeNode`s, each carrying its own memory (child index, remaining repeats, wait remainder, the once-only missing-action flag); a subtree reset is a recursive walk over exactly those fields, which is the whole reset semantics in one function. Leaves resolve their callbacks through a version-stamped cache: every registry mutation (`registerAction`, `unregisterAction`, teardown) bumps a monotonically increasing registry version, a leaf re-runs its unordered_map find the first time it is evaluated after any bump, and between mutations the resolved callback pointer is reused - so a re-registered callback is picked up on the leaf's next evaluation exactly like the old find-per-tick discipline, while the steady state pays one integer compare instead of a string hash per leaf per tick. The auto-tick walk is a flat pass over the creation-ordered vector that calls the shared tick body with the tree reference in hand (the handle-based `tick` finds the tree first, the walk does not re-find); destruction preserves creation order (an order-preserving erase, not a swap-pop), because the walk order is part of the determinism contract.

## Files

- `behavior_tree_capability.hpp` — the published capability surface every consumer includes
- `behavior_tree.hpp` / `behavior_tree.cpp` — the module, the evaluator, the registry and the Lua bridge extension
- `module_plugin.cpp` — the static plugin descriptor (name, factory, provided capabilities)
- `config.toml` — shipped defaults (`[behavior_tree]`)
- `CMakeLists.txt` — target and option wiring (`ENGINE_BUILD_MODULE_BEHAVIOR_TREE`)
