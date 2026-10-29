#ifndef ENGINE_KERNEL_ASYNC_TASK_DISPATCHER_HPP
#define ENGINE_KERNEL_ASYNC_TASK_DISPATCHER_HPP

// Off-main-thread work with main-thread completion, as an opt-in utility.
//
// The engine's threading contract is deliberate and documented
// (docs/architecture.md): the event bus is
// single threaded, the frame loop is serialized, and nothing shares GPU
// resources across threads. That contract is exactly why this utility exists
// as a KERNEL utility rather than a bus feature: modules occasionally need
// blocking work OFF the frame path (filesystem polling, file IO, decode
// steps), and the safe shape for that is always the same -
//
//   post(work)                 work runs on the worker thread
//   postWithCompletion(work,   work runs on the worker; completion runs on
//     completion)              the MAIN thread at the next drain() call
//   drainCompletedTasks()      the main thread (frame path) drains
//
// - so results re-enter the single-threaded world at a well-defined point
// and no lock but the dispatcher's own queue mutex ever exists. The bus API
// is untouched (the architecture doc reserved this: "a future delivery
// module can add queues without touching the channel API").
//
// One dispatcher, a worker count sized by defaultWorkerCount()
// (parallel_for.hpp): the machine's hardware thread count, clamped to a
// bounded range. That makes the dispatcher PARALLEL BY DEFAULT - a module
// that calls start() with no arguments gets a pool that uses the cores the
// machine offers, which is the right default for the IO-shaped and CPU-
// shaped batch work modules post (image decode bursts, file IO, encode
// steps). With several workers, tasks still run to completion and
// completions still re-enter the main thread through drainCompletedTasks(),
// but dispatch ORDER between tasks is no longer guaranteed (two workers
// race the same deque) - a module whose posted tasks are ordered (a poll
// cycle that re-arms itself, a queue with FIFO semantics) passes an
// explicit 1 to start() and keeps the historical single-worker behavior,
// which stays exactly one argument away. The fine-grained per-frame
// fan-out alternative is parallelFor() (parallel_for.hpp): transient
// threads for one coarse batch, fork-join, no dispatcher lifecycle.

#include "engine/kernel/parallel_for.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace engine
{

class AsyncTaskDispatcher final
{
public:
    AsyncTaskDispatcher() = default;

    AsyncTaskDispatcher(const AsyncTaskDispatcher &)            = delete;
    AsyncTaskDispatcher &operator=(const AsyncTaskDispatcher &) = delete;

    ~AsyncTaskDispatcher()
    {
        stop();
    }

    // Starts the worker thread(s). Calling start twice without stop is a
    // contract breach (one dispatcher); start after stop is legal (a module
    // that re-opens its watcher). workerCount defaults to
    // defaultWorkerCount() - the machine's hardware thread count clamped to
    // a bounded range (see parallel_for.hpp) - so an unconfigured start()
    // parallelizes across the cores the machine offers. Values above one
    // start that many identical workers on the same queue (clamped to
    // [1, 32]); work must then be independent, since dispatch order between
    // tasks is undefined. An explicitly ordered user (a self re-arming poll
    // cycle, a FIFO work queue) passes 1 and keeps the historical
    // single-worker behavior.
    void start(
        std::size_t workerCount = defaultWorkerCount()
    )
    {
        if (running_.load())
        {
            return;
        }
        // The clamp keeps a runaway configuration from spawning a thread
        // per core times ten: the pool is for IO-shaped parallelism, not a
        // general compute farm.
        const std::size_t clampedWorkerCount = std::clamp<std::size_t>(workerCount, 1, 32);
        running_.store(true);
        workers_.reserve(clampedWorkerCount);
        for (std::size_t index = 0; index < clampedWorkerCount; ++index)
        {
            workers_.emplace_back(&AsyncTaskDispatcher::workerLoop, this);
        }
    }

    // Stops the workers: pending work still runs (the workers finish their
    // queue before noticing the shutdown flag), completions posted but not
    // drained are DROPPED (the main thread chose to stop draining; keeping
    // them would resurrect stale state at an unpredictable later point).
    // Safe to call twice.
    void stop()
    {
        if (!running_.exchange(false))
        {
            return;
        }
        // No queue clear here: the contract above lets the workers finish
        // what was already posted, and the loop's predicate
        // (!running_ && workQueue_.empty()) keeps them alive until they have.
        wakeCondition_.notify_all();
        for (std::thread &worker : workers_)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
        workers_.clear();
    }

    // Posts one unit of work to the worker thread. Work must not touch the
    // bus, GPU resources, or anything the frame loop owns - it should
    // compute and return (captured) values.
    //
    // The wake is gated on the sleeping-worker count (see sleepingWorkers_):
    // when every worker is mid-task it will re-check the queue before it
    // sleeps, so the notify - a futex syscall on the posting thread - buys
    // nothing and is skipped. The count is read under the same mutex the
    // push holds, which is what makes the gate race-free (the worker
    // increments under the lock before waiting).
    void post(
        std::function<void()> work
    )
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            workQueue_.push_back(std::move(work));
            if (sleepingWorkers_.load(std::memory_order_relaxed) > 0) [[unlikely]]
            {
                // At least one worker sits in the wait below: the notify is
                // the wake. Zero sleepers means every worker is mid-task and
                // will re-check the queue under the lock before it can sleep,
                // so the notify - a futex syscall on the posting thread -
                // buys nothing and is skipped.
                wakeCondition_.notify_one();
            }
        }
    }

    // Posts work whose RESULT re-enters the main thread: after the work
    // lambda returns on the worker, the completion lambda is queued to run
    // during the next drainCompletedTasks() on the main thread.
    void postWithCompletion(
        std::function<void()> work, std::function<void()> completion
    )
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            workQueue_.push_back([this, work = std::move(work), completion = std::move(completion)]() {
                // A throwing work item must not kill the process: the
                // call runs on a worker thread, so an escaping exception
                // would std::terminate the engine. The completion is
                // skipped on that path (its author's contract is
                // "completion runs with the result"; there is no
                // result), the error is reported on stderr, and the
                // worker stays alive for the next task.
                try
                {
                    work();
                } catch (const std::exception &error)
                {
                    std::fprintf(stderr, "async dispatcher: task failed: %s\n", error.what());
                    return;
                } catch (...)
                {
                    std::fprintf(stderr, "async dispatcher: task failed with an unknown error\n");
                    return;
                }
                std::lock_guard<std::mutex> completionLock(mutex_);
                completionQueue_.push_back(std::move(completion));
                pendingCompletions_.fetch_add(1, std::memory_order_relaxed);
            });
            // Same sleeping-worker gate post() applies (see there): the wake
            // only matters when a worker is actually parked on the predicate.
            if (sleepingWorkers_.load(std::memory_order_relaxed) > 0) [[unlikely]]
            {
                wakeCondition_.notify_one();
            }
        }
    }

    // Drains the completion queue on the main thread. Call from the frame
    // path (an EngineFrameEvent handler is the conventional point). Calling
    // from a non-main thread is a contract breach - completions may publish
    // events, and the bus is single threaded.
    //
    // The common case is an empty queue: a relaxed atomic check answers it
    // without touching the mutex (the frame path drains every frame, the
    // workers complete work only occasionally). The counter is only ever
    // written under the mutex, so the mutex stays the source of truth; a
    // stale zero just defers a just-posted completion to the next drain,
    // which is the polling contract anyway.
    void drainCompletedTasks()
    {
        if (pendingCompletions_.load(std::memory_order_relaxed) == 0)
        {
            return;
        }
        // Member drain buffer (main thread only, like the drain itself): the
        // swap hands the queue's events over WITHOUT resetting either side's
        // capacity - the local variable this loop used to declare died at
        // scope exit and took its heap block with it, so every drain under
        // active completion traffic re-allocated the queue storage on the
        // next post and freed it here. The clear() below keeps the buffer's
        // capacity, so a steady frame path stops touching the heap after its
        // high-water mark - the same discipline EventChannel::flushDeferred
        // applies to the deferred-delivery drain.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            drainBuffer_.swap(completionQueue_);
            pendingCompletions_.store(0, std::memory_order_relaxed);
        }
        for (std::function<void()> &completion : drainBuffer_)
        {
            completion();
        }
        drainBuffer_.clear();
    }

    std::size_t pendingWorkCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return workQueue_.size();
    }

private:
    void workerLoop()
    {
        while (true)
        {
            std::function<void()> work;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // The sleeping count is maintained under the mutex (the same
                // one the post paths hold), which is what makes their
                // skip-notify gate race-free: a post that reads zero cannot
                // race a worker into sleep, because the worker counts itself
                // as sleeping BEFORE the wait releases the lock, and clears
                // the count after the wake reacquires it.
                sleepingWorkers_.fetch_add(1, std::memory_order_relaxed);
                wakeCondition_.wait(lock, [this]() { return !running_.load() || !workQueue_.empty(); });
                sleepingWorkers_.fetch_sub(1, std::memory_order_relaxed);
                if (!running_.load() && workQueue_.empty())
                {
                    return;
                }
                if (workQueue_.empty())
                {
                    continue; // stale wakeup (stop() raced a drain)
                }
                work = std::move(workQueue_.front());
                workQueue_.pop_front();
            }
            // Bare post() work gets the same exception containment as the
            // completion form above: a throwing task reports and moves on,
            // the worker survives for the next item in the queue.
            try
            {
                work();
            } catch (const std::exception &error)
            {
                std::fprintf(stderr, "async dispatcher: task failed: %s\n", error.what());
            } catch (...)
            {
                std::fprintf(stderr, "async dispatcher: task failed with an unknown error\n");
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable wakeCondition_;
    std::deque<std::function<void()>> workQueue_;
    std::vector<std::function<void()>> completionQueue_;
    // Main-thread drain scratch, retained across drains (see
    // drainCompletedTasks). Never touched off the main thread, so it needs
    // no lock of its own.
    std::vector<std::function<void()>> drainBuffer_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
    // Number of completions queued but not yet drained; written under the
    // mutex, read relaxed by drainCompletedTasks' empty fast path.
    std::atomic<std::size_t> pendingCompletions_{0};
    // Workers parked in the condition wait right now. Written only under the
    // mutex (the wait's lock and the wake's reacquire bracket the count), so
    // the post paths read it under the same mutex with a plain relaxed load -
    // the atomic is for the toolchain, the mutex is the ordering.
    std::atomic<std::size_t> sleepingWorkers_{0};
};

} // namespace engine

#endif // ENGINE_KERNEL_ASYNC_TASK_DISPATCHER_HPP
