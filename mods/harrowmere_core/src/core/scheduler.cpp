// core/scheduler.cpp — Task 3.1 implementation (§4.4 order is load-bearing; see D-TICK-1).
#include "core/scheduler.hpp"

#include <cassert>

#include "core/world_context.hpp"

namespace harrowmere
{

void Scheduler::registerSystem(
    TickStep step, IStepSystem *system
)
{
    assert(system != nullptr && "registerSystem: null system");
    for (const IStepSystem *s : steps_[static_cast<std::size_t>(step)])
    {
        assert(s != system && "registerSystem: system registered twice (debug contract, 3.1)");
        (void)s; // NDEBUG (release) drops the assert; keep the variable used (DoD: -Werror)
    }
    steps_[static_cast<std::size_t>(step)].push_back(system);
}

void Scheduler::runTick(
    WorldContext &ctx
)
{
    ++tickCount_;

    // The queue promotion runs FIRST: work enqueued during tick N (or between
    // ticks) joins this tick's step queues here, so "the next tick's step 5"
    // means the runTick call that immediately follows the enqueue (the §4.4
    // reentrancy rule's contract — 4.6's manhunt spawn lands on the next
    // tick, never two ticks out). Work enqueued DURING this tick lands in
    // nextPending_ and runs in the same step of tick N+1, as pinned.
    for (std::size_t s = 0; s < static_cast<std::size_t>(TickStep::COUNT); ++s)
    {
        if (nextPending_[s].empty())
            continue;
        for (TaskFn &fn : nextPending_[s])
            pending_[s].push_back(std::move(fn));
        nextPending_[s].clear();
    }

    // Step 0 — Clocks. The clock system advances worldTime (and season roll, 3.8)
    // and refreshes ctx.now; everything downstream reads the fresh value.
    runStep(TickStep::Clocks, ctx);

    // Game-time timers due at or before the fresh worldTime fire inside their target
    // step's slot of THIS tick (steps 1..13 are still ahead of us here).
    drainDueTimers(ctx);

    for (std::uint8_t s = 1; s < static_cast<std::uint8_t>(TickStep::COUNT); ++s)
    {
        runStep(static_cast<TickStep>(s), ctx);
    }
}

void Scheduler::runStep(
    TickStep step, WorldContext &ctx
)
{
    for (IStepSystem *system : steps_[static_cast<std::size_t>(step)])
        system->run(ctx);
    if (!pending_[static_cast<std::size_t>(step)].empty())
    {
        std::vector<TaskFn> tasks = std::move(pending_[static_cast<std::size_t>(step)]);
        pending_[static_cast<std::size_t>(step)].clear();
        for (TaskFn &fn : tasks)
            fn(ctx);
    }
}

void Scheduler::enqueueForNextTick(
    TickStep step, TaskFn fn
)
{
    nextPending_[static_cast<std::size_t>(step)].push_back(std::move(fn));
}

void Scheduler::scheduleAt(
    std::uint64_t dueWorldTime, TickStep step, TaskFn fn
)
{
    timers_[dueWorldTime].push_back(TimerEntry{step, timerSeq_++, std::move(fn)});
}

void Scheduler::drainDueTimers(
    WorldContext &ctx
)
{
    if (timers_.empty())
        return;
    const std::uint64_t now = ctx.now;
    // Map order = due-time order; within one due time, schedule order (seq) holds (R7).
    for (auto it = timers_.begin(); it != timers_.end() && it->first <= now; it = timers_.erase(it))
    {
        for (TimerEntry &entry : it->second)
        {
            if (entry.step == TickStep::Clocks)
            {
                // Step 0 already ran this tick; its timers wait for the next one.
                nextPending_[static_cast<std::size_t>(entry.step)].push_back(std::move(entry.fn));
            } else
            {
                // Joins the target step's CURRENT-tick pending list, after that
                // step's registered systems.
                pending_[static_cast<std::size_t>(entry.step)].push_back(std::move(entry.fn));
            }
        }
    }
}

int SchedulerMath::countTicks(
    double elapsedSeconds, double &accumulatorSeconds, int tickHz, int maxFixedUpdateSteps
)
{
    const double fixedDelta = 1.0 / static_cast<double>(tickHz);
    accumulatorSeconds += elapsedSeconds;
    int ticks = 0;
    while (accumulatorSeconds >= fixedDelta && ticks < maxFixedUpdateSteps)
    {
        accumulatorSeconds -= fixedDelta;
        ++ticks;
    }
    return ticks;
}

std::uint64_t SchedulerMath::gameSecondsPerTick(
    int timeScale, int tickHz, std::uint64_t &carryNumerator
)
{
    carryNumerator += static_cast<std::uint64_t>(timeScale);
    const std::uint64_t seconds = carryNumerator / static_cast<std::uint64_t>(tickHz);
    carryNumerator %= static_cast<std::uint64_t>(tickHz);
    return seconds;
}

} // namespace harrowmere
