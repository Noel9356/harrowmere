#ifndef ENGINE_MODULES_SCHEDULER_HPP
#define ENGINE_MODULES_SCHEDULER_HPP

#include "logging_capability.hpp"
#include "scheduler_capability.hpp"
#include "engine/kernel/component_interface.hpp" // EngineFrameEvent (the tick
                                                 // source: declared in the
                                                 // kernel's component-facing
                                                 // header, so the module needs
                                                 // no renderer capability
                                                 // include and ticks in
                                                 // renderer-less runs too)
#include "engine/kernel/event_bus.hpp"

#include <functional>
#include <string>
#include <vector>

namespace engine
{

// Module "scheduler": frame-driven timers for native components (family
// member "scheduler.frame").
//
// Shape: the module subscribes EngineFrameEvent (the composition root's loop
// heartbeat) in construct; the handler advances every active timer by the
// event's deltaSeconds - the frame delta the composition root measured once
// for the whole iteration (docs/architecture.md, "The frame clock": every
// simulation module follows the same heartbeat, so every module sees the
// same delta) - and fires what is due, on the main thread, inside the frame
// event, exactly the dispatch point the net module's pump() and the
// scripting host's timers keep. The bus stays single threaded; a frame with
// no active timers costs one empty-vector check and no clock read at all,
// and the module never reads a clock of its own: the heartbeat carries the
// time.
//
// Division of labor with the scripting host's timer surface: script mods
// use engine.create_timer (self-contained in the lua_scripting module, with
// per-mod isolation and error containment); this capability is the same
// service for C++ consumers - modules and native mods - whose callbacks are
// plain std::function objects with no script state to protect. Neither
// knows about the other, and a run can use both without ambiguity.
class SchedulerModule final : public ModuleInterface, public SchedulerCapability
{
public:
    SchedulerModule();

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;

    // SchedulerCapability.
    TimerHandle scheduleOnce(double delaySeconds, std::function<void(TimerHandle)> callback) override;
    TimerHandle scheduleRepeating(double intervalSeconds, std::function<void(TimerHandle)> callback) override;
    bool cancel(TimerHandle handle) override;
    std::uint32_t activeTimerCount() const override;

private:
    struct TimerEntry
    {
        TimerHandle handle = invalidTimer;
        double interval    = 0.0;
        double remaining   = 0.0;
        bool repeating     = false;
        std::function<void(TimerHandle)> callback;
    };

    // Validates one requested duration against the tick contract (finite,
    // and positive for repeating timers); logs the precise reason and
    // returns false when the request is refused.
    bool durationIsValid(double seconds, bool repeating) const;

    TimerHandle scheduleTimer(double seconds, bool repeating, std::function<void(TimerHandle)> callback);

    void forwardFrameTick(const EngineFrameEvent &event);

    ComponentManifest componentManifest;
    LoggerCapability *logger = nullptr;
    EventSubscription frameTickSubscription;
    EventChannel<EngineFrameEvent> *frameTickChannel = nullptr;
    // The active timers, in scheduling order. Erasure is rare (cancel, and
    // one-shot expiry), so a plain vector with order-preserving erase keeps
    // the tick's iteration deterministic - two runs with the same schedule
    // fire the same callbacks in the same order.
    std::vector<TimerEntry> timers;
    TimerHandle nextHandle = 1;
    // The due-fire collection buffer of one tick, retained across ticks
    // (the kernel scheduler's dueScratch discipline): clearing preserves
    // capacity, so a frame that fires timers stops touching the heap after
    // its high-water mark instead of allocating a fresh vector every beat.
    std::vector<TimerHandle> dueFireScratch;
};

} // namespace engine

#endif // ENGINE_MODULES_SCHEDULER_HPP
