// tests/test_scheduler.cpp — Task 3.1 pinned tests.
// TestScheduler_StepOrderPinned, TestScheduler_EnqueueRunsNextTickNotNow,
// TestScheduler_AccumulatorCatchesUp, TestScheduler_TimeAdvancedMatchesTickMath.
//
// D-TICK-1: tests drive ticks, not wall time — runTick is called directly; the
// accumulator mirror (SchedulerMath) exists precisely so the ENGINE-side math stays
// testable without a frame loop.
#include <doctest.h> // the single implement define lives in main.cpp (task 0.6)

#include <algorithm>
#include <vector>

#include "core/world_context.hpp"
#include "test_phase2_fixture.hpp"

namespace
{

// phase2_test already carries `using namespace harrowmere;` (fixture header):
// a second using-directive here makes the qualified name `harrowmere::`
// ambiguous for the TU (transitive lookup reaches the namespace by two paths).
using namespace phase2_test;

// TickStep is enum class (3.1's spec); the unary-plus index idiom gets one
// explicit conversion helper for the whole file.
constexpr std::uint8_t operator+(
    TickStep step
)
{
    return static_cast<std::uint8_t>(step);
}

struct ProbeSystem final : public IStepSystem
{
    std::vector<TickStep> *log;
    TickStep step;
    int *runs;
    ProbeSystem(
        std::vector<TickStep> *l, TickStep s, int *r
    )
        : log(l), step(s), runs(r)
    {}
    void run(
        WorldContext &
    ) override
    {
        ++*runs;
        log->push_back(step);
    }
};

// Enqueues a probe task into step 9 on its first run (the §4.4 reentrancy escape
// hatch: work created mid-tick joins the SAME step of the NEXT tick). The task's
// counter lives in a static so TaskFn captures nothing but the WorldContext.
struct EnqueuingSystem final : public IStepSystem
{
    bool fired = false;
    void run(
        WorldContext &ctx
    ) override
    {
        if (fired)
            return;
        fired = true;
        ctx.scheduler->enqueueForNextTick(TickStep::Player, [](WorldContext &) { ++taskRuns(); });
    }
    static int &taskRuns()
    {
        static int runs = 0;
        return runs;
    }
};

} // namespace

TEST_CASE(
    "TestScheduler_StepOrderPinned"
)
{
    Fixture f;
    std::vector<TickStep> log;
    int runs[+TickStep::COUNT] = {};

    ProbeSystem p0{&log, TickStep::Clocks, &runs[+TickStep::Clocks]};
    ProbeSystem p1{&log, TickStep::Sweeps, &runs[+TickStep::Sweeps]};
    ProbeSystem p3{&log, TickStep::Lod, &runs[+TickStep::Lod]};
    ProbeSystem p4a{&log, TickStep::Npcs, &runs[+TickStep::Npcs]};
    ProbeSystem p4b{&log, TickStep::Npcs, &runs[+TickStep::Npcs]};
    ProbeSystem p7{&log, TickStep::Economy, &runs[+TickStep::Economy]};
    ProbeSystem p9{&log, TickStep::Player, &runs[+TickStep::Player]};
    ProbeSystem p13{&log, TickStep::Publish, &runs[+TickStep::Publish]};

    f.scheduler.registerSystem(TickStep::Clocks, &p0);
    f.scheduler.registerSystem(TickStep::Sweeps, &p1);
    f.scheduler.registerSystem(TickStep::Lod, &p3);
    f.scheduler.registerSystem(TickStep::Npcs, &p4a); // registration order within a step
    f.scheduler.registerSystem(TickStep::Npcs, &p4b); // is execution order (pinned)
    f.scheduler.registerSystem(TickStep::Economy, &p7);
    f.scheduler.registerSystem(TickStep::Player, &p9);
    f.scheduler.registerSystem(TickStep::Publish, &p13);

    CHECK(f.scheduler.systemCount(TickStep::Npcs) == 2);
    CHECK(f.scheduler.systemCount(TickStep::Weather) == 0); // empty steps still exist

    f.scheduler.runTick(f.ctx);

    REQUIRE(log.size() == 8);
    for (std::size_t i = 1; i < log.size(); ++i)
    {
        CHECK(
            static_cast<std::uint8_t>(log[i - 1]) <= static_cast<std::uint8_t>(log[i])
        ); // steps never run out of order
    }
    // Within step 4: a before b (registration order).
    const auto firstNpcs = std::find(log.begin(), log.end(), TickStep::Npcs);
    REQUIRE(firstNpcs != log.end());
    CHECK(*firstNpcs == TickStep::Npcs);
    CHECK(runs[static_cast<std::size_t>(TickStep::Clocks)] == 1);
    CHECK(runs[static_cast<std::size_t>(TickStep::Publish)] == 1);
}

TEST_CASE(
    "TestScheduler_EnqueueRunsNextTickNotNow"
)
{
    Fixture f;
    std::vector<TickStep> log;
    int step9SystemRuns = 0;

    ProbeSystem p9{&log, TickStep::Player, &step9SystemRuns};
    EnqueuingSystem enqueuer;

    f.scheduler.registerSystem(TickStep::Events, &enqueuer);
    f.scheduler.registerSystem(TickStep::Player, &p9);
    EnqueuingSystem::taskRuns() = 0;

    f.scheduler.runTick(f.ctx); // tick 1: the enqueue happens at step 5...
    // ...but the task must NOT run later in the SAME tick, even though step 9 follows.
    CHECK(EnqueuingSystem::taskRuns() == 0);

    f.scheduler.runTick(f.ctx); // tick 2: the task runs inside step 9.
    CHECK(EnqueuingSystem::taskRuns() == 1);
    // And exactly once — the swap consumed it.
    f.scheduler.runTick(f.ctx);
    CHECK(EnqueuingSystem::taskRuns() == 1);
}

TEST_CASE(
    "TestScheduler_AccumulatorCatchesUp"
)
{
    // 4 frames of 25 ms at 10 Hz -> exactly 1 tick (the card's pinned example).
    double acc = 0.0;
    int ticks  = 0;
    for (int frame = 0; frame < 4; ++frame)
    {
        ticks += SchedulerMath::countTicks(0.025, acc, 10, 5);
    }
    CHECK(ticks == 1);
    CHECK(acc < 1e-9); // the exact 0.1 s fired, nothing carried

    // Catch-up burst is clamped by [engine].max_fixed_update_steps = 5: a stalled
    // second yields at most 5 ticks, never a spiral (3.1 pitfalls).
    acc = 0.0;
    CHECK(SchedulerMath::countTicks(1.2, acc, 10, 5) == 5);
}

TEST_CASE(
    "TestScheduler_TimeAdvancedMatchesTickMath"
)
{
    // TIME_SCALE 60 at 10 Hz: exactly 6 game-seconds per tick (D-TICK-1).
    std::uint64_t carry = 0;
    std::uint64_t total = 0;
    for (int i = 0; i < 10; ++i)
        total += SchedulerMath::gameSecondsPerTick(60, 10, carry);
    CHECK(total == 60);
    CHECK(carry == 0);

    // TIME_SCALE 61: the remainder carries — 10 ticks advance exactly 61 game-seconds.
    carry              = 0;
    total              = 0;
    std::uint64_t last = 0;
    for (int i = 0; i < 10; ++i)
    {
        last = SchedulerMath::gameSecondsPerTick(61, 10, carry);
        total += last;
    }
    CHECK(total == 61);
    CHECK(carry == 0);
    CHECK(last == 7); // the carry lands on the last tick of the block

    // And through the scheduler itself: a clock system advancing ctx.now by the tick
    // math makes 10 ticks == 60 game-seconds at the pinned 60/10 config.
    Fixture f;
    struct ClockSystem final : public IStepSystem
    {
        std::uint64_t carry = 0;
        void run(
            WorldContext &ctx
        ) override
        {
            ctx.now += SchedulerMath::gameSecondsPerTick(60, 10, carry);
        }
    } clockSystem;
    f.scheduler.registerSystem(TickStep::Clocks, &clockSystem);
    for (int i = 0; i < 10; ++i)
        f.scheduler.runTick(f.ctx);
    CHECK(f.ctx.now == 60);
    CHECK(f.scheduler.tickCount() == 10);
}
