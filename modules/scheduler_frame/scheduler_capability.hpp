#ifndef ENGINE_MODULES_SCHEDULER_SCHEDULER_CAPABILITY_HPP
#define ENGINE_MODULES_SCHEDULER_SCHEDULER_CAPABILITY_HPP

// The published capability surface of the scheduler module. Every consumer -
// other modules, native mods, the app - includes THIS header, from the
// module that provides the capability: the provider owns and publishes its
// own capability vocabulary, so there is no central contract directory and
// no mirrored copy that could drift. Like every published capability header
// this stays implementation-, platform- and vendor-agnostic; the per-module
// purity guard in the module's CMakeLists enforces that mechanically.
// See mods/README.md, section "Portability".

#include <cstdint>
#include <functional>

namespace engine
{

// Capability "scheduler": delayed and repeating callbacks for native
// components.
//
// The engine had no timer service on the C++ side: script mods have the
// scripting host's own timer surface (engine.create_timer, self-contained in
// the scripting module), but modules and native mods that needed a delayed
// or periodic call had to measure frame deltas by hand - every one of them
// re-implementing the same advance-and-fire loop, each with its own stall
// and cancellation semantics. This capability is that one loop, owned once:
// a consumer schedules a callback, the provider advances and fires it on
// the frame event, and the bus's single-thread contract holds without any
// consumer doing work.
//
// Callbacks run on the MAIN thread, inside the provider's FrameBeginEvent
// handler - the same dispatch point the net module's pump() and the
// scripting host's timers use. A run with no renderer never ticks: timers
// are frame-paced, which is the honest clock for gameplay pacing (a paused
// or minimized window genuinely pauses them). The std::function callback is
// deliberate: invocations are rare events (a timer expiry, not a per-frame
// poll), so the allocation happens once at schedule time and never on the
// fire path's order of magnitude.
class SchedulerCapability
{
public:
    // The implementation-agnostic family name of this capability. Consumers
    // acquire through it; the module's plugin descriptor carries the
    // implementation-bound member name ("scheduler.frame").
    static constexpr const char *capabilityName = "scheduler";

    // Timers are addressed by handle; handles are never reused.
    using TimerHandle = std::uint64_t;

    static constexpr TimerHandle invalidTimer = 0;

    virtual ~SchedulerCapability() = default;

    // Schedules one callback to fire ONCE, delaySeconds from the next frame
    // tick (0.0 = the next frame - a scheduled callback never fires inside
    // the schedule call, so scheduling is non-reentrant by construction).
    // Returns invalidTimer for a negative or non-finite delay - the
    // provider logs the precise reason.
    virtual TimerHandle scheduleOnce(double delaySeconds, std::function<void(TimerHandle)> callback) = 0;

    // Schedules one callback to fire every intervalSeconds until cancelled.
    // A repeating timer fires AT MOST ONCE PER FRAME: a long stall (a
    // breakpoint, a window drag) never spirals into a burst of catch-up
    // invocations - the missed periods are dropped and the next fire lands
    // one full interval after the last one. Returns invalidTimer for an
    // interval that is not strictly positive.
    virtual TimerHandle scheduleRepeating(double intervalSeconds, std::function<void(TimerHandle)> callback) = 0;

    // Cancels one timer. Returns false for unknown handles (including
    // already-fired one-shots). Safe to call from inside a callback: a
    // timer cancelling itself, or another timer due in the same tick, is a
    // normal pattern - the cancelled callback, if it was due in this very
    // tick and not yet dispatched, is skipped.
    virtual bool cancel(TimerHandle handle) = 0;

    // How many timers are currently active (one-shots still pending count;
    // fired one-shots stop counting the moment they fire).
    virtual std::uint32_t activeTimerCount() const = 0;
};

} // namespace engine

#endif // ENGINE_MODULES_SCHEDULER_SCHEDULER_CAPABILITY_HPP
