#ifndef ENGINE_KERNEL_MAIN_THREAD_SCHEDULER_HPP
#define ENGINE_KERNEL_MAIN_THREAD_SCHEDULER_HPP

// Time-based callbacks on the engine's single main thread, as an opt-in kernel
// utility - the timing twin of what AsyncTaskDispatcher is for blocking work.
//
// The engine's threading contract (docs/architecture.md) keeps the event bus
// and the frame loop on one thread; that contract is exactly why timed work
// needs ONE well-defined shape instead of every component rolling its own:
//
//   scheduleAfter(seconds, task)    one task, first considered after the delay
//   scheduleEvery(seconds, task)    a recurring task, one beat per interval
//   tick()                          the main thread (frame path) runs what is due
//
// A task is a plain std::function callback that runs inside tick(), on the
// main thread, at frame granularity - the same world a frame event handler
// lives in, so a task may touch anything a frame handler may (re-read
// configuration, publish events, mutate scene state). The clock is
// steady_clock, read ONCE per tick with work pending: a run with no scheduled
// tasks (the default everywhere) pays one empty-vector check per frame and no
// clock read at all.
//
// What it buys over the hand-rolled pattern (subscribe to the EngineFrameEvent
// heartbeat, read a clock every frame, compare against a member deadline - the
// shape the profiler, the scripting module's timers and any content tween
// would otherwise each reimplement): the deadline arithmetic, the catch-up policy
// and the cancellation bookkeeping live here once, and a component that wants
// periodic work states an interval instead of subscribing to frame events it
// does not otherwise need. It also runs in renderer-less loops: the
// composition root ticks the scheduler every frame iteration, whether or not
// a renderer publishes frame events.
//
// Cancellation is RAII like EventSubscription: the ScheduledTask handle a
// schedule call returns cancels its task when it dies (or when cancel() is
// called), so a module owning one as a member can never leave a task pointing
// at its own destructed state. A recurring task reschedules itself only while
// its handle is alive and un-cancelled.
//
// Ordering policy, deliberately simple: tasks are kept sorted by next
// deadline, due tasks run in deadline order (earliest first), and a task
// scheduled from inside a running task is first considered at the NEXT tick -
// tick() is never re-entered, so a callback cannot recurse the scheduler by
// scheduling a zero-delay task. The recurring catch-up policy: after a task
// runs, its next deadline advances in whole intervals past NOW (a stall that
// swallowed three beats yields one beat, not a burst of three), so the frame
// path never pays for time a hung frame already spent.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace engine
{

class MainThreadScheduler;

// RAII ownership of one scheduled task, in the same discipline as
// EventSubscription: releasing (or destroying) the handle cancels the task -
// a one-shot that has not run yet is removed, a recurring task stops
// rescheduling itself. A default-constructed handle owns nothing; the handle
// of a refused schedule call (a non-positive recurring interval) is also
// empty. The handle must not outlive the scheduler it came from: the kernel
// declares the scheduler so it is destroyed after every component record.
class ScheduledTask final
{
public:
    ScheduledTask() = default;

    ScheduledTask(
        MainThreadScheduler *ownerScheduler, std::shared_ptr<bool> taskArmed
    )
        : owner(ownerScheduler), armed(std::move(taskArmed))
    {}

    ~ScheduledTask()
    {
        cancel();
    }

    ScheduledTask(const ScheduledTask &)            = delete;
    ScheduledTask &operator=(const ScheduledTask &) = delete;

    ScheduledTask(
        ScheduledTask &&other
    ) noexcept
        : owner(other.owner), armed(std::move(other.armed))
    {
        other.owner = nullptr;
    }

    ScheduledTask &operator=(
        ScheduledTask &&other
    ) noexcept
    {
        if (this != &other)
        {
            cancel();
            owner       = other.owner;
            armed       = std::move(other.armed);
            other.owner = nullptr;
        }
        return *this;
    }

    // Cancels the task this handle owns and empties the handle. Safe to call
    // from inside the task's own callback (the entry is disarmed first, so a
    // recurring task never reschedules) and on an empty handle (a no-op).
    void cancel();

    // Whether this handle still owns a live task. A task that already ran to
    // completion (a one-shot, or a recurring task between beats) counts as
    // live until its handle is released - the ownership, not the urgency, is
    // what the handle tracks.
    bool valid() const
    {
        return armed != nullptr;
    }

private:
    MainThreadScheduler *owner = nullptr;
    // The disarm flag shared with the scheduler's entry: the flag is the one
    // piece of state both sides may touch, which is what makes cancellation
    // safe while tick() is running (see ScheduledTask::cancel and
    // MainThreadScheduler::tick).
    std::shared_ptr<bool> armed;
};

// The scheduler itself. One instance lives in the kernel (reached by
// components through ComponentContext::scheduler(), by the composition root
// through EngineKernel::scheduler()) and is ticked once per frame iteration
// by the app's frame loop, next to the bus's deferred-event flush.
//
// Thread policy: everything except the documented "schedule/cancel/tick on
// the main thread" contract does not exist - there is no lock, matching the
// bus and the frame loop. Main-thread-only is what keeps the utility free of
// the synchronization a cross-thread timer service would have to pay.
class MainThreadScheduler final
{
public:
    MainThreadScheduler() = default;

    MainThreadScheduler(const MainThreadScheduler &)            = delete;
    MainThreadScheduler &operator=(const MainThreadScheduler &) = delete;

    ~MainThreadScheduler()
    {
        clear();
    }

    // One task, considered starting after delaySeconds (clamped to >= 0; a
    // zero delay runs at the next tick). The returned handle owns the task.
    [[nodiscard]] ScheduledTask scheduleAfter(double delaySeconds, std::function<void()> task);

    // One recurring task, one beat every intervalSeconds. A non-positive
    // interval is refused (it would spin the frame path): the returned handle
    // is empty, the task is not scheduled, and the refusal is a programming
    // error to fix at the call site - not a state the scheduler keeps.
    [[nodiscard]] ScheduledTask scheduleEvery(double intervalSeconds, std::function<void()> task);

    // Runs every task whose deadline has passed, in deadline order, and
    // reschedules the recurring ones. Reads the clock once, and only when at
    // least one task exists; a run with no tasks pays the empty check. Not
    // reentrant: a task that calls tick() directly is ignored (the flag makes
    // the recursion a no-op instead of corrupting the run list).
    void tick();

    // Drops every task, armed or not. The kernel calls this at the top of
    // stop() so no scheduled callback can fire during teardown; handles held
    // by components disarm their shared flags and become inert.
    void clear();

    // Number of tasks currently held (armed ones; disarmed entries are purged
    // lazily by the next tick). A diagnostic, nothing more.
    std::size_t taskCount() const;

private:
    friend class ScheduledTask;

    struct TaskEntry
    {
        // Sorted key: earliest first. tick() collects the due prefix in one
        // pass because the vector is kept sorted by insert (the same
        // subscribe-time discipline the event bus applies to priorities).
        std::chrono::steady_clock::time_point nextDue;
        // 0 = one-shot; > 0 = recurring beat in seconds.
        double intervalSeconds = 0.0;
        std::function<void()> callback;
        // Shared with the owning ScheduledTask handle: *armed == false means
        // cancelled - a running tick re-checks it before invoking and before
        // rescheduling, so a cancel during a callback takes effect without
        // touching the vector the tick is walking.
        std::shared_ptr<bool> armed;
    };

    void insertTask(TaskEntry entry);

    // The shared entry point of both schedule forms: delay places the first
    // deadline, interval selects one-shot (0) or recurring (> 0).
    ScheduledTask scheduleTaskEntry(double delaySeconds, double intervalSeconds, std::function<void()> task);

    // Removes the entry whose armed flag matches, if it is still held (the
    // entry may be mid-run in tick's local list, in which case the flag is
    // already false and only the entry's slot matters). Called from
    // ScheduledTask::cancel on the main thread.
    void removeTask(const std::shared_ptr<bool> &taskArmed);

    // Converts a double-seconds span into a steady_clock duration. The cast
    // truncates, so a positive-but-sub-tick span (e.g. 5e-8 s on a clock with
    // a coarser period) would come back as ZERO and every catch-up loop that
    // adds it would spin forever; the one-tick floor makes the smallest
    // representable interval "every clock tick", which is the closest
    // meaningful reading of what the caller asked for. Non-finite and
    // overflow-huge inputs also land on this floor instead of undefined
    // durations.
    static std::chrono::steady_clock::duration secondsAsDuration(
        double seconds
    )
    {
        std::chrono::steady_clock::duration converted{};
        if (seconds > 0.0 && std::isfinite(seconds))
        {
            converted = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(seconds)
            );
        }
        if (converted <= std::chrono::steady_clock::duration::zero())
        {
            converted = std::chrono::steady_clock::duration(1);
        }
        return converted;
    }

    std::vector<TaskEntry> tasks;
    // tick()'s due-prefix scratch, retained across ticks (see tick).
    std::vector<TaskEntry> dueScratch;
    bool ticking = false;
};

inline ScheduledTask MainThreadScheduler::scheduleAfter(
    double delaySeconds, std::function<void()> task
)
{
    return scheduleTaskEntry(delaySeconds, 0.0, std::move(task));
}

inline ScheduledTask MainThreadScheduler::scheduleEvery(
    double intervalSeconds, std::function<void()> task
)
{
    if (intervalSeconds <= 0.0)
    {
        // Refused, not clamped: a "recurring" task with no interval is a
        // spin, and silently clamping it to some epsilon would hide the
        // mistake behind surprising behavior. The empty handle says so.
        return ScheduledTask{};
    }
    return scheduleTaskEntry(intervalSeconds, intervalSeconds, std::move(task));
}

inline void MainThreadScheduler::tick()
{
    if (ticking || tasks.empty())
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();

    // Collect the due prefix: entries are sorted by deadline, so the run is
    // one scan that stops at the first future entry. Disarmed entries inside
    // the prefix are dropped here instead of dispatched.
    std::size_t dueCount = 0;
    for (std::size_t index = 0; index < tasks.size(); ++index)
    {
        if (tasks[index].nextDue > now)
        {
            break;
        }
        ++dueCount;
    }
    if (dueCount == 0)
    {
        return;
    }
    // Member drain scratch, retained across ticks: the local vector this
    // used to declare re-allocated (and freed) a heap block on every firing
    // tick - a run with recurring tasks pays it per beat. clear() below
    // keeps the capacity, so the frame path stops touching the heap after
    // its high-water mark, like the bus's deferred drain.
    dueScratch.clear();
    dueScratch.reserve(dueCount);
    for (std::size_t index = 0; index < dueCount; ++index)
    {
        if (*tasks[index].armed)
        {
            dueScratch.push_back(std::move(tasks[index]));
        }
    }
    tasks.erase(tasks.begin(), tasks.begin() + static_cast<std::ptrdiff_t>(dueCount));

    ticking = true;
    for (TaskEntry &entry : dueScratch)
    {
        if (!*entry.armed)
        {
            continue; // cancelled between collection and this beat
        }
        entry.callback();
        if (entry.intervalSeconds > 0.0 && *entry.armed)
        {
            // Catch-up policy: advance in whole intervals past now, so a
            // stalled frame period yields one beat on recovery instead of a
            // burst of every beat the stall swallowed. The advance is one
            // closed-form jump, not an add-one-interval loop: a sub-tick
            // interval (secondsAsDuration floors those to one tick) plus a
            // long stall (debugger pause, laptop sleep) would otherwise
            // iterate billions of times on the frame thread. The miss count
            // comes from dividing the TICK COUNTS (duration/duration yields
            // a plain number, not a duration), and the huge-miss cap keeps
            // the multiplication itself from overflowing for absurd
            // combinations; landing on now + interval is the same "one beat
            // on recovery" policy.
            TaskEntry recurring    = std::move(entry);
            const auto interval    = secondsAsDuration(recurring.intervalSeconds);
            const auto intervalCnt = interval.count();
            if (recurring.nextDue + interval > now)
            {
                recurring.nextDue += interval;
            } else
            {
                const auto missedCount = (now - recurring.nextDue).count() / intervalCnt;
                if (missedCount > 1000000000)
                {
                    recurring.nextDue = now + interval;
                } else
                {
                    recurring.nextDue += interval * static_cast<std::intmax_t>(missedCount + 1);
                }
            }
            insertTask(std::move(recurring));
        }
    }
    // Drop the moved-from husks and keep the buffer's capacity for the next
    // beat. This runs after ticking = false: a callback that scheduled work
    // or cancelled tasks interacted with the live `tasks` vector, never with
    // this scratch.
    dueScratch.clear();
    ticking = false;
}

inline void MainThreadScheduler::clear()
{
    // Disarm before dropping: a clear() that runs while tick() is executing
    // (a task that shuts the engine down does exactly that) must not leave
    // the tick's local list running. Tick re-checks the armed flag before
    // every callback and before every reschedule, so flagging the entries
    // here stops both - and the recurring reschedule sees the disarmed flag
    // instead of inserting itself into the freshly emptied vector.
    for (TaskEntry &entry : tasks)
    {
        if (entry.armed)
        {
            *entry.armed = false;
        }
    }
    tasks.clear();
}

inline std::size_t MainThreadScheduler::taskCount() const
{
    return tasks.size();
}

inline void MainThreadScheduler::insertTask(
    TaskEntry entry
)
{
    std::size_t insertPosition = tasks.size();
    for (std::size_t index = 0; index < tasks.size(); ++index)
    {
        if (tasks[index].nextDue > entry.nextDue)
        {
            insertPosition = index;
            break;
        }
    }
    tasks.insert(tasks.begin() + static_cast<std::ptrdiff_t>(insertPosition), std::move(entry));
}

inline void MainThreadScheduler::removeTask(
    const std::shared_ptr<bool> &taskArmed
)
{
    for (std::size_t index = 0; index < tasks.size(); ++index)
    {
        if (tasks[index].armed == taskArmed)
        {
            tasks.erase(tasks.begin() + static_cast<std::ptrdiff_t>(index));
            return;
        }
    }
    // Not held: the entry is mid-run (tick owns it in its local list, and the
    // flag is already false, so it will not run or reschedule) or already
    // finished. Nothing to remove.
}

inline void ScheduledTask::cancel()
{
    if (owner == nullptr || armed == nullptr)
    {
        return;
    }
    *armed = false; // disarms first: a mid-tick entry sees this before running
    owner->removeTask(armed);
    owner = nullptr;
    armed.reset();
}

// The shared entry point of both schedule forms: delay places the first
// deadline, interval selects one-shot (0) or recurring (> 0). The armed flag
// is captured before the entry moves into the vector - it is the identity the
// returned handle and the scheduler's entry share for cancellation.
inline ScheduledTask MainThreadScheduler::scheduleTaskEntry(
    double delaySeconds, double intervalSeconds, std::function<void()> task
)
{
    TaskEntry entry;
    entry.nextDue                   = std::chrono::steady_clock::now() + secondsAsDuration(std::max(delaySeconds, 0.0));
    entry.intervalSeconds           = intervalSeconds;
    entry.callback                  = std::move(task);
    entry.armed                     = std::make_shared<bool>(true);
    std::shared_ptr<bool> taskArmed = entry.armed;
    insertTask(std::move(entry));
    return ScheduledTask(this, std::move(taskArmed));
}

} // namespace engine

#endif // ENGINE_KERNEL_MAIN_THREAD_SCHEDULER_HPP
