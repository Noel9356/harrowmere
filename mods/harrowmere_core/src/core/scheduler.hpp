// core/scheduler.hpp — Task 3.1: the pinned 14-step tick pipeline (§4.4).
//
// D-TICK-1 / D-ARCH-3: timing belongs to the ENGINE. The Scheduler is
// harrowmere_core's ONE subscriber to EngineFixedUpdateEvent — one dispatch is
// exactly one runTick (wired in harrowmere_plugin.cpp). The game writes no
// accumulator; the engine owns the catch-up clamp ([engine].max_fixed_update_steps).
//
// Order within a step is registration order (pinned per system docs). Steps that
// must cause work in a later step enqueue via enqueueForNextTick — never re-enter.
// Game-time timers (task 3.6's scheduleAt) are tick-driven: they are drained right
// after step 0 each tick and execute inside their target step's slot for THIS tick.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace harrowmere
{

struct WorldContext; // core/world_context.hpp (kept out to avoid a cycle)

enum class TickStep : std::uint8_t
{
    Clocks = 0,
    Sweeps,
    Weather,
    Lod,
    Npcs,
    Events,
    Justice,
    Economy,
    Macro,
    Player,
    Combat,
    Aftermath,
    PowerFlush,
    Publish,
    COUNT
};
static_assert(static_cast<std::uint8_t>(TickStep::COUNT) == 14, "the pinned pipeline has exactly 14 steps (§4.4)");

class IStepSystem
{
public:
    virtual ~IStepSystem()              = default;
    virtual void run(WorldContext &ctx) = 0;
};

using TaskFn = std::function<void(WorldContext &)>;

class Scheduler
{
public:
    // Registration order within a step is execution order; startup-only.
    void registerSystem(TickStep step, IStepSystem *system);

    // Executes steps 0..13 in order: per step, registered systems then this tick's
    // queued tasks. Timing is the engine's; headless tests call runTick directly.
    void runTick(WorldContext &ctx);

    // The reentrancy escape hatch (§4.4): work enqueued during tick N runs in the
    // same step of tick N+1 (e.g. justice spawning a MANHUNT enqueues into step 5).
    void enqueueForNextTick(TickStep step, TaskFn fn);

    // Game-time timers (task 3.6's `scheduleAt(ctx.now + REPLACEMENT_DELAY, ...)`).
    // Fired on the first tick whose fresh worldTime >= due; the task runs inside the
    // given step's slot of that tick (deterministic: due-time order, then schedule order).
    void scheduleAt(std::uint64_t dueWorldTime, TickStep step, TaskFn fn);

    std::uint64_t tickCount() const
    {
        return tickCount_;
    }
    std::size_t systemCount(
        TickStep step
    ) const
    {
        return steps_[static_cast<std::uint8_t>(step)].size();
    }

private:
    void drainDueTimers(WorldContext &ctx);
    void runStep(TickStep step, WorldContext &ctx);

    std::array<std::vector<IStepSystem *>, static_cast<std::uint8_t>(TickStep::COUNT)> steps_;
    std::array<std::vector<TaskFn>, static_cast<std::uint8_t>(TickStep::COUNT)> pending_;     // runs this tick
    std::array<std::vector<TaskFn>, static_cast<std::uint8_t>(TickStep::COUNT)> nextPending_; // enqueued -> next tick
    struct TimerEntry
    {
        TickStep step;
        std::uint64_t seq;
        TaskFn fn;
    };
    std::map<std::uint64_t, std::vector<TimerEntry>> timers_; // due worldTime -> entries (R7 order)
    std::uint64_t timerSeq_  = 0;
    std::uint64_t tickCount_ = 0;
};

// Test-facing mirror of the ENGINE's own accumulator loop (D-TICK-1: the engine owns
// the real one; this mirror exists so the pinned scheduler tests can verify the
// catch-up math and the tick-math without a frame loop). NOT used by the runtime.
struct SchedulerMath
{
    // Classic accumulator: returns how many ticks fire for `elapsedSeconds` given
    // tickHz and the engine's burst clamp. 4 frames of 25 ms at 10 Hz -> exactly 1 tick.
    static int countTicks(double elapsedSeconds, double &accumulatorSeconds, int tickHz, int maxFixedUpdateSteps);

    // Tick math with integer remainder carry (§4.4): each tick advances worldTime by
    // TIME_SCALE/tickHz game-seconds, carrying the remainder (R5: integers only).
    static std::uint64_t gameSecondsPerTick(int timeScale, int tickHz, std::uint64_t &carryNumerator);
};

} // namespace harrowmere
