# scheduler_frame module

The timer service for native components: one-shot and repeating callbacks, advanced and fired on the frame tick (the kernel's `EngineFrameEvent` loop heartbeat - see `docs/architecture.md`, "The frame clock"). The module provides the `SchedulerCapability` (family `scheduler`, implementation-bound member `scheduler.frame`, published in `scheduler_capability.hpp` next to this module) and requires nothing but logging. Before it existed the C++ side of the tree had no timer at all: script mods have the scripting host's own surface (`engine.create_timer`, self-contained in the lua_scripting module), but modules and native mods that needed a delayed or periodic call had to measure frame deltas by hand - every one of them re-implementing the same advance-and-fire loop, each with its own stall and cancellation semantics. This capability is that one loop, owned once.

## Capability surface

Four methods on handles that are never reused: `scheduleOnce(delaySeconds, callback)` fires one callback once; `scheduleRepeating(intervalSeconds, callback)` fires until cancelled; `cancel(handle)` returns whether the timer was still active; `activeTimerCount()` is the current size. The callback signature is `void(TimerHandle)` - a repeating timer receives its own handle, which is everything a self-cancelling "run for N seconds" pattern needs. The callback is a `std::function` on purpose: invocations are rare events (an expiry, not a per-frame poll), so the allocation happens once at schedule time and never dominates the fire path.

Refusals are precise and non-throwing: a negative or non-finite delay, a non-positive repeating interval, or an empty callback return `invalidTimer` and log one warning naming the reason. Like every capability call in the engine, scheduling never throws onto the frame path.

## The tick contract

The module subscribes `EngineFrameEvent` (the composition root's loop heartbeat, declared in the kernel's `component_interface.hpp`) and owns the whole advance: the handler advances every timer by the event's `deltaSeconds` - the one delta the composition root measured for the iteration, the same tick source the scripting host's timers and every other simulation module keep - and fires what is due, on the main thread, inside the frame tick. The consequences are deliberate:

- **The tick source is the loop, so a headless run ticks.** Timers are frame-paced, which is the honest clock for gameplay pacing: a stalled frame (a breakpoint, a window drag) genuinely pauses them. A paused or minimized window keeps pumping the loop, so timers keep running there; a run whose composition root stops iterating the loop stops them - the same contract the Lua timers' "fire between frames" keeps, so a mod's script timers and its native timers stall together.
- **Scheduling is non-reentrant.** A callback scheduled with delay 0 fires on the NEXT frame, never inside the schedule call and never inside the same tick's fire loop - the loop iterates a snapshot of due handles, so a callback that schedules work cannot re-enter its own dispatch.
- **A repeating timer fires at most once per frame.** The missed periods after a stall (a breakpoint, a window drag) are dropped and the next fire lands one full interval after the last one - a stall never bursts into a catch-up spiral, and a period shorter than the frame time honestly degrades to "every frame".
- **Cancellation is safe from inside callbacks.** The due list is re-checked per handle: a callback cancelling itself (or another timer due in the same tick) makes the not-yet-dispatched callback skip.

## Wiring

| Manifest field | Value |
|---|---|
| provided capabilities | `scheduler.frame` |
| required capabilities | `logging` |
| required permissions | `capability.logging` |

The renderer is deliberately NOT a requirement and no longer even an include: the tick source is the kernel's loop heartbeat, which every module sees through `component_interface.hpp`. A headless run (no window, no renderer) ticks exactly like a rendered one - timers, headless servers and batch simulations included; a run whose composition root never publishes the heartbeat costs the module one subscription record and nothing else.

## Configuration

None. The module reads no keys: every tunable is an argument of the schedule calls, and a timer's pacing belongs to the code that schedules it. The scripting host's timer surface (`engine.create_timer` / `engine.cancel_timer`) remains the tool for script mods - it isolates per mod and contains callback errors; this capability is the same service for consumers that hold plain C++ callables and no script state. Neither knows about the other, and a run can use both without ambiguity.

## Implementation

Standard library only, no third-party code. Timers live in one order-preserving vector (erasure is rare, iteration is deterministic), handles come from a monotonic counter, and the tick advances on the shared frame delta - the same event the animation module's tweens advance on, so a scheduled callback and a tween started at the same moment stay in step. The module reads no clock of its own; a run with zero active timers short-circuits at the handler's empty check and pays one comparison for the whole frame.

## Files

- `scheduler_capability.hpp` — the published capability surface every consumer includes
- `scheduler.hpp` / `scheduler.cpp` — the module, the timer store and the frame-event tick
- `module_plugin.cpp` — the static plugin descriptor (name, factory, provided capabilities)
- `CMakeLists.txt` — target and option wiring (`ENGINE_BUILD_MODULE_SCHEDULER_FRAME`)
