#include "scheduler.hpp"

#include "logging_capability.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace engine
{

SchedulerModule::SchedulerModule()
{
    componentManifest.name = "scheduler";
    componentManifest.kind = ComponentKind::Module;
    // The implementation-bound member of the "scheduler" family: the member
    // names the tick source (the frame event), which is the only axis a
    // competing implementation would differ on - a wall-clock-async variant
    // would declare its own member and the [providers] configuration picks
    // between them.
    componentManifest.providedCapabilities = {"scheduler.frame"};
    componentManifest.requiredCapabilities = {"logging"};
    componentManifest.requiredPermissions  = {"capability.logging"};
}

void SchedulerModule::construct(
    ComponentContext &context
)
{
    // The one cross-component touch of the construct phase: the log sink.
    // The requirement is declared in the manifest and in the static plugin
    // descriptor, so the grant ("capability.logging") was derived from the
    // requirement and the provider is ordered before this module.
    logger = &context.acquireCapability<LoggerCapability>();

    // The tick subscription is not optional for this module: timers are what
    // the capability IS, so the module always listens for frames. The tick
    // source is the kernel's EngineFrameEvent heartbeat (the composition
    // root's loop clock), not the renderer's FrameBeginEvent - the module
    // needs no renderer capability include and ticks in renderer-less runs
    // too (headless servers, batch simulations), at the cost of one
    // subscription record, exactly like the hot_reload watcher's drain.
    EventBus &bus         = context.eventBus();
    frameTickChannel      = &bus.channel<EngineFrameEvent>();
    frameTickSubscription = EventSubscription(
        &bus,
        typeid(EngineFrameEvent),
        frameTickChannel->subscribe<SchedulerModule, &SchedulerModule::forwardFrameTick>(this)
    );
}

void SchedulerModule::start()
{
    if (logger != nullptr)
    {
        logger->logMessage(LogLevel::Information, "scheduler module started");
    }
}

void SchedulerModule::stop()
{
    frameTickSubscription.release();
    timers.clear();
    logger = nullptr;
}

void *SchedulerModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    if (capabilityName == "scheduler.frame")
    {
        return static_cast<SchedulerCapability *>(this);
    }
    return nullptr;
}

bool SchedulerModule::durationIsValid(
    double seconds, bool repeating
) const
{
    // Repeating timers need a strictly positive period (an interval of 0
    // would fire every frame forever, which the caller can already spell as
    // a scheduleRepeating with the frame rate it wants); one-shot delays of
    // exactly 0 are the documented "next frame" spelling and are valid.
    if (repeating)
    {
        return std::isfinite(seconds) && seconds > 0.0;
    }
    return std::isfinite(seconds) && seconds >= 0.0;
}

SchedulerModule::TimerHandle SchedulerModule::scheduleTimer(
    double seconds, bool repeating, std::function<void(TimerHandle)> callback
)
{
    if (callback == nullptr)
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning, "scheduler module: refused a timer without a callback (handle stays invalid)"
            );
        }
        return invalidTimer;
    }
    if (!durationIsValid(seconds, repeating))
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning,
                repeating ? "scheduler module: refused a repeating timer with a non-positive or non-finite interval"
                          : "scheduler module: refused a one-shot timer with a negative or non-finite delay"
            );
        }
        return invalidTimer;
    }

    const TimerHandle handle = nextHandle;
    nextHandle += 1;

    TimerEntry entry;
    entry.handle    = handle;
    entry.interval  = seconds;
    entry.remaining = seconds;
    entry.repeating = repeating;
    entry.callback  = std::move(callback);
    timers.push_back(std::move(entry));
    return handle;
}

SchedulerModule::TimerHandle SchedulerModule::scheduleOnce(
    double delaySeconds, std::function<void(TimerHandle)> callback
)
{
    return scheduleTimer(delaySeconds, false, std::move(callback));
}

SchedulerModule::TimerHandle SchedulerModule::scheduleRepeating(
    double intervalSeconds, std::function<void(TimerHandle)> callback
)
{
    return scheduleTimer(intervalSeconds, true, std::move(callback));
}

bool SchedulerModule::cancel(
    TimerHandle handle
)
{
    if (handle == invalidTimer)
    {
        return false;
    }
    for (std::size_t index = 0; index < timers.size(); ++index)
    {
        if (timers[index].handle == handle)
        {
            timers.erase(timers.begin() + static_cast<std::ptrdiff_t>(index));
            return true;
        }
    }
    return false;
}

std::uint32_t SchedulerModule::activeTimerCount() const
{
    return static_cast<std::uint32_t>(timers.size());
}

void SchedulerModule::forwardFrameTick(
    const EngineFrameEvent &event
)
{
    // The empty check first, every frame: a run whose timers are all done
    // (or that never scheduled one) pays one comparison and no clock read -
    // the heartbeat's own delta makes a self-measured clock unnecessary, so
    // there is nothing left to seed, refresh or carry across idle frames.
    if (timers.empty())
    {
        return;
    }
    // The composition root measured this iteration's delta once (the frame
    // clock every simulation module shares); the first frame of a run
    // carries delta 0, which advances no timer - the same "first tick has
    // no elapsed time" contract the self-measured clock seeded with.
    const double elapsedSeconds = event.deltaSeconds;

    // Collect what is due THIS tick first, then fire: a callback that
    // schedules a zero-delay timer sees it on the next frame, never inside
    // its own invocation - scheduling stays non-reentrant by construction,
    // and the fire loop below iterates a snapshot instead of the mutating
    // vector. The scratch is a member (clear() preserves capacity), so a
    // beat with due timers stops allocating after its high-water mark.
    dueFireScratch.clear();
    for (TimerEntry &entry : timers)
    {
        entry.remaining -= elapsedSeconds;
        if (entry.remaining <= 0.0)
        {
            dueFireScratch.push_back(entry.handle);
        }
    }
    if (dueFireScratch.empty())
    {
        return;
    }

    for (const TimerHandle handle : dueFireScratch)
    {
        // The entry may have been cancelled by an earlier callback of this
        // very tick - the per-handle lookup is the guard, not a precomputed
        // iterator that erase would have invalidated.
        auto found = std::find_if(timers.begin(), timers.end(), [handle](const TimerEntry &entry) {
            return entry.handle == handle;
        });
        if (found == timers.end())
        {
            continue;
        }

        // The callback is moved out before the entry changes state: a
        // callback that cancels ITSELF (the common repeating pattern) must
        // not execute through a reference into a vector entry that the
        // cancel call erases mid-invocation. A one-shot's callback MOVES
        // rather than copies - the entry is erased below regardless, so the
        // moved-out husk is dead state by the time the fire runs, and a
        // capturing lambda fires without the heap round trip a std::function
        // copy pays per beat. A repeating timer still copies: its entry must
        // keep the callback for the next beat, and restoring a moved-out one
        // after the fire would write through a `found` that a reentrant
        // schedule inside the callback may have invalidated.
        if (found->repeating)
        {
            // At most one fire per frame: the missed periods are dropped and
            // the next fire lands one full interval after this one, so a
            // stall (a breakpoint, a window drag) never bursts into a
            // catch-up spiral. A period shorter than the frame time simply
            // fires every frame - the honest floor of a frame-paced clock.
            std::function<void(TimerHandle)> callback = found->callback;
            found->remaining                          = found->interval;
            callback(handle);
        } else
        {
            std::function<void(TimerHandle)> callback = std::move(found->callback);
            timers.erase(found);
            callback(handle);
        }
    }
}

} // namespace engine
