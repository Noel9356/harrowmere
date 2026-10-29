#ifndef ENGINE_KERNEL_EVENT_BUS_HPP
#define ENGINE_KERNEL_EVENT_BUS_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine
{

using SubscriptionToken = std::uint64_t;

inline constexpr SubscriptionToken invalidSubscriptionToken = 0;

// The default dispatch priority. Handlers subscribed at this priority - the
// value every historical overload still produces - dispatch in subscription
// order, exactly like the bus always did. Higher numbers dispatch earlier:
// ordering a handler before (or after) another component's reaction to the
// same event is a subscription-time statement, never a re-registration
// dance. Typical uses: drain order at FrameBegin (a capture module that must
// see completions before a consumer of its saved files), and "react last"
// sinks that summarize what the other handlers did.
inline constexpr int defaultEventPriority = 0;

// Communication latency accounting for one channel. The bus is the single
// cross-component communication medium (module-module, mod-module, mod-mod
// all dispatch through it; capability calls are direct virtual calls on a
// pointer acquired once), so these numbers ARE the communication latencies.
// Storage is fixed per channel: two counters plus a bounded ring of recent
// samples - no heap growth, no per-publish allocation.
struct ChannelLatencyStatistics
{
    static constexpr std::size_t sampleCapacity = 128;
    // The per-sample ring stores nanoseconds in 32 bits; larger samples clamp.
    static constexpr std::uint64_t maxRecordableSampleNanoseconds = 0xFFFFFFFFull;

    std::uint64_t deliveries              = 0;
    std::uint64_t totalNanoseconds        = 0;
    std::uint64_t maxNanoseconds          = 0;
    std::uint32_t samples[sampleCapacity] = {};
    std::size_t sampleCount               = 0;
    std::size_t samplePosition            = 0;

    void record(
        std::uint64_t nanoseconds
    )
    {
        ++deliveries;
        totalNanoseconds += nanoseconds;
        maxNanoseconds = std::max(maxNanoseconds, nanoseconds);
        samples[samplePosition]
            = static_cast<std::uint32_t>(std::min<std::uint64_t>(nanoseconds, maxRecordableSampleNanoseconds));
        samplePosition = (samplePosition + 1) % sampleCapacity;
        if (sampleCount < sampleCapacity)
        {
            ++sampleCount;
        }
    }
};

// Prettifies a typeid name into a readable channel name: strips the GCC/Clang
// leading length digits, the MSVC 'struct '/'class ' prefix and the common
// 'engine::' namespace so reports stay stable across compilers.
std::string prettifyChannelName(const std::type_index &eventType);

// Type-erased channel base so the bus can unsubscribe through the RAII guard.
class EventBus;

// A single typed event channel.
//
// Dispatch strategy: handlers are stored as cached function pointer + user data
// pairs inside one contiguous vector. Publishing is therefore a linear scan with
// one indirect call per handler: no std::function, no queue, no allocation once
// the handler set is stable. This is the minimal latency path for same-thread
// communication between modules and mods.
//
// Thread policy: the bus is deliberately single threaded (engine main thread).
// Cross thread delivery is a module concern and may be added later as a
// dedicated delivery module without changing this interface.
template <typename EventType> class EventChannel final
{
public:
    using HandlerFunction = void (*)(void *userData, const EventType &event);

    EventChannel() = default;

    EventChannel(const EventChannel &)            = delete;
    EventChannel &operator=(const EventChannel &) = delete;

    // Subscribe a raw handler: a function taking user data plus the event.
    // Used by free function adapters.
    SubscriptionToken subscribe(
        HandlerFunction handler, void *userData
    )
    {
        return subscribeHandler(handler, userData, defaultEventPriority);
    }

    // Same registration at an explicit dispatch priority: higher numbers
    // dispatch first, equal numbers keep subscription order (the insert is
    // stable). Subscribe-time cost only - the publish loop below is
    // unchanged and stays a linear scan of one contiguous vector.

    SubscriptionToken subscribe(
        HandlerFunction handler, void *userData, int priority
    )
    {
        return subscribeHandler(handler, userData, priority);
    }

    // Subscribe a bound member function. The trampoline is resolved once at
    // registration time; dispatch pays a single indirect call.
    template <typename ListenerClass, void (ListenerClass::*HandlerMethod)(const EventType &)>
    SubscriptionToken subscribe(
        ListenerClass *listener
    )
    {
        return subscribeHandler(
            &EventChannel::dispatchToMember<ListenerClass, HandlerMethod>,
            static_cast<void *>(listener),
            defaultEventPriority
        );
    }

    // The member-function form at an explicit dispatch priority.
    template <typename ListenerClass, void (ListenerClass::*HandlerMethod)(const EventType &)>
    SubscriptionToken subscribe(
        ListenerClass *listener, int priority
    )
    {
        return subscribeHandler(
            &EventChannel::dispatchToMember<ListenerClass, HandlerMethod>, static_cast<void *>(listener), priority
        );
    }

    // Subscribe a free function taking the event by const reference.
    SubscriptionToken subscribe(
        void (*freeFunction)(const EventType &)
    )
    {
        return subscribeHandler(
            &EventChannel::dispatchToFree, reinterpret_cast<void *>(freeFunction), defaultEventPriority
        );
    }

    // The free-function form at an explicit dispatch priority.
    SubscriptionToken subscribe(
        void (*freeFunction)(const EventType &), int priority
    )
    {
        return subscribeHandler(&EventChannel::dispatchToFree, reinterpret_cast<void *>(freeFunction), priority);
    }

    // Remove a handler. Safe to call from inside a handler that is currently
    // being dispatched; the removal is deferred until dispatch completes.
    // Also safe to call on a subscription that was made from inside a
    // dispatch and has not landed yet: the pending insert is dropped instead.
    bool unsubscribe(
        SubscriptionToken token
    )
    {
        if (dispatching)
        {
            for (HandlerEntry &entry : handlers)
            {
                if (entry.token == token && !entry.pendingRemoval)
                {
                    entry.pendingRemoval = true;
                    deferredRemovals.push_back(token);
                    return true;
                }
            }
            // The handler may be a same-dispatch subscription that the
            // outermost guard has not inserted yet: drop it from the pending
            // buffer so it never joins the scan it was subscribed inside of.
            for (std::size_t index = 0; index < pendingSubscriptions.size(); ++index)
            {
                if (pendingSubscriptions[index].token == token)
                {
                    pendingSubscriptions.erase(pendingSubscriptions.begin() + static_cast<std::ptrdiff_t>(index));
                    return true;
                }
            }
            return false;
        }

        // Erase in place: the handler vector is priority-ordered (see
        // subscribeHandler), so a swap-and-pop would silently reorder the
        // remaining entries and break the documented dispatch order after any
        // unsubscribe. Unsubscribe happens on teardown paths, never on the
        // frame path, so the shift costs nothing at runtime.
        for (std::size_t index = 0; index < handlers.size(); ++index)
        {
            if (handlers[index].token == token)
            {
                handlers.erase(handlers.begin() + static_cast<std::ptrdiff_t>(index));
                return true;
            }
        }
        return false;
    }

    // Invoke every live handler synchronously in subscription order. When
    // latency recording is enabled (see EventBus::setLatencyRecordingEnabled)
    // the dispatch duration (handler time only, clock overhead excluded by
    // taking the second timestamp after the loop) is recorded into the
    // channel's latency statistics. When it is disabled the clocks are never
    // read and the bookkeeping collapses to one branch - the measurement
    // costs nothing on the hot path of a normal run.
    void publish(
        const EventType &event
    )
    {
        // NOTE: the scan visits handlers in the order the vector holds, which
        // is priority order (see subscribeHandler's stable insert); the loop
        // itself is untouched by the priority feature.
        const bool measure = recordingFlag != nullptr && *recordingFlag;
        const std::chrono::steady_clock::time_point publishStart
            = measure ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        {
            // RAII scope for the dispatching flag and the deferred-removal
            // processing: a handler that throws (its exception may cross a
            // frame boundary - the app's frame loop catches and continues)
            // must leave the channel exactly as it found it, or the flag
            // would defer every later unsubscribe forever and the
            // pendingRemoval entries would accumulate in the per-frame scan.
            DispatchGuard guard(*this);
            for (std::size_t index = 0; index < handlers.size(); ++index)
            {
                const HandlerEntry &entry = handlers[index];
                // pendingRemoval is a transient state inside one dispatch
                // (an unsubscribe that arrived mid-scan); the scan is the
                // engine's one cross-component fan-out, so the common
                // not-removed branch is marked as such and the check folds
                // into the loop's layout instead of costing a misprediction
                // on every handler of every publish.
                if (!entry.pendingRemoval) [[likely]]
                {
                    entry.invoke(entry.userData, event);
                }
            }
        }
        if (measure)
        {
            statistics.record(elapsedNanoseconds(publishStart));
        }
    }

    // Deferred delivery: queue the event now, publish it at the next
    // flushDeferredEvents() the composition root drives. The queue is a plain
    // vector append - no clock, no allocation once the steady state is
    // reached, and nothing at all until the first use. Main thread only, like
    // every channel operation.
    //
    // What it buys: a handler that is INSIDE a dispatch can defer an event of
    // the same type without re-entering the scan it is running in (a
    // SceneObjectRemovedEvent published from a SceneObjectAddedEvent handler
    // dispatches cleanly after the outer scan completes), and a producer that
    // wants deterministic ordering can batch its notifications into one
    // visible point of the frame. Deferred events publish in FIFO order; an
    // event deferred from inside a flush lands in the NEXT flush, so one
    // flush is always bounded by what was queued before it started.
    void publishDeferred(
        const EventType &event
    )
    {
        deferredEvents.push_back(event);
        if (deferredPendingFlag != nullptr)
        {
            deferredPendingFlag->store(true, std::memory_order_relaxed);
        }
    }

    // Publishes every event queued by publishDeferred, in queue order. The
    // bus drives this through the type-erased holder; the drain buffer keeps
    // its capacity across flushes. Events deferred during the drain land in
    // the next flush (the queue was swapped out, so the running scan cannot
    // see them) - re-entrancy is a non-property by construction.
    void flushDeferred()
    {
        if (deferredEvents.empty())
        {
            return;
        }
        deferredDrain.swap(deferredEvents);
        for (const EventType &event : deferredDrain)
        {
            publish(event);
        }
        deferredDrain.clear();
    }

    // Whether any event is waiting for the next flush. Diagnostics only.
    bool hasDeferredEvents() const
    {
        return !deferredEvents.empty();
    }

    // Number of currently registered handlers (pending removals included -
    // they are transient by contract and the count is a diagnostic, not an
    // invariant). Built on demand for the subscription report; no run time
    // cost.
    std::size_t subscriberCount() const
    {
        return handlers.size();
    }

    // Priority span of the current handler set, for the subscription report.
    // defaultPrioritySpan(): every handler sits at the default priority, so
    // dispatch order is exactly subscription order - the report says so
    // instead of printing a meaningless "0..0".
    bool defaultPrioritySpan() const
    {
        for (const HandlerEntry &entry : handlers)
        {
            if (entry.priority != defaultEventPriority)
            {
                return false;
            }
        }
        return true;
    }

    int lowestPriority() const
    {
        int lowest = defaultEventPriority;
        for (const HandlerEntry &entry : handlers)
        {
            lowest = std::min(lowest, entry.priority);
        }
        return lowest;
    }

    int highestPriority() const
    {
        int highest = defaultEventPriority;
        for (const HandlerEntry &entry : handlers)
        {
            highest = std::max(highest, entry.priority);
        }
        return highest;
    }

    // Wiring for the bus-level recording switch. The flag is owned by the
    // EventBus so one call re-targets every channel.
    void setRecordingFlag(
        const bool *flag
    )
    {
        recordingFlag = flag;
    }

    // Wiring for the bus-level deferred-pending switch: any
    // publishDeferred marks the bus so flushDeferredEvents knows there is
    // work without walking the channels. Same ownership shape as the
    // recording flag (one bool for every channel).
    void setDeferredPendingFlag(
        std::atomic<bool> *flag
    )
    {
        deferredPendingFlag = flag;
    }

    // Communication latency statistics of this channel (module-module,
    // mod-module and mod-mod dispatches all flow through here).
    const ChannelLatencyStatistics &latencyStatistics() const
    {
        return statistics;
    }

private:
    struct HandlerEntry
    {
        void *userData          = nullptr;
        HandlerFunction invoke  = nullptr;
        SubscriptionToken token = invalidSubscriptionToken;
        bool pendingRemoval     = false;
        int priority            = defaultEventPriority;
    };

    // Sets the reentrancy flag for one publish and restores it - with the
    // deferred unsubscribe/subscription passes - afterwards, on the normal
    // return AND on exception unwind. The previous flag value is saved and
    // restored, not cleared: a handler that publishes an event of the SAME
    // type directly nests a second guard inside the first, and only the
    // OUTERMOST guard may flip the channel back to idle and drain the
    // deferred queues. (An unconditional clear would make the outer scan
    // look idle to its own handlers: an unsubscribe from a later outer
    // handler would then erase from the vector mid-scan and silently skip
    // one unrelated handler.) Processing the removals in place (by index,
    // then one clear) keeps the buffers' capacity across publishes: moving
    // them into locals destroyed at scope exit reallocated the buffers on
    // every unsubscribe-during-dispatch after the first.
    class DispatchGuard final
    {
    public:
        explicit DispatchGuard(
            EventChannel &ownerChannel
        )
            : channel(ownerChannel), wasDispatching(ownerChannel.dispatching)
        {
            channel.dispatching = true;
        }

        DispatchGuard(const DispatchGuard &)            = delete;
        DispatchGuard &operator=(const DispatchGuard &) = delete;

        ~DispatchGuard()
        {
            channel.dispatching = wasDispatching;
            if (wasDispatching)
            {
                return; // nested publish: both queues stay for the outer scan
            }
            // Outermost dispatch finished: land the subscriptions requested
            // from inside the scan first (so a removal queued for one of
            // them below can still find it), then process the removals.
            if (!channel.pendingSubscriptions.empty())
            {
                const std::size_t count = channel.pendingSubscriptions.size();
                for (std::size_t index = 0; index < count; ++index)
                {
                    channel.insertHandler(std::move(channel.pendingSubscriptions[index]));
                }
                channel.pendingSubscriptions.clear();
            }
            if (!channel.deferredRemovals.empty())
            {
                const std::size_t count = channel.deferredRemovals.size();
                for (std::size_t index = 0; index < count; ++index)
                {
                    // dispatching is already false: the removal happens
                    // directly, and the tokens' pendingRemoval entries leave
                    // the scan with the erase.
                    channel.unsubscribe(channel.deferredRemovals[index]);
                }
                channel.deferredRemovals.clear();
            }
        }

    private:
        EventChannel &channel;
        bool wasDispatching;
    };

    // Stable insert by descending priority: the handler lands before the
    // first entry whose priority is strictly lower, so equal priorities keep
    // subscription order and higher numbers dispatch first. Subscribe
    // happens during construct, never on the frame path, so the linear
    // scan-and-shift here costs nothing at runtime; publish stays the same
    // contiguous linear scan it always was.
    SubscriptionToken subscribeHandler(
        HandlerFunction handler, void *userData, int priority
    )
    {
        const SubscriptionToken token = nextToken++;
        HandlerEntry entry{
            .userData = userData, .invoke = handler, .token = token, .pendingRemoval = false, .priority = priority
        };
        if (dispatching)
        {
            // Subscribing from inside a dispatch cannot mutate the vector
            // the scan is walking: the priority insert would shift entries
            // over the cursor and re-dispatch handlers that already ran.
            // The insert is deferred to the end of the outermost dispatch
            // (see DispatchGuard); the token is valid immediately, and
            // unsubscribing it before it lands drops the pending entry.
            pendingSubscriptions.push_back(std::move(entry));
            return token;
        }
        insertHandler(std::move(entry));
        return token;
    }

    // Places one handler entry at its priority position. The shared tail of
    // subscribeHandler: called directly when idle, and by the DispatchGuard
    // drain for subscriptions that were deferred out of a dispatch.
    void insertHandler(
        HandlerEntry &&entry
    )
    {
        std::size_t insertPosition = handlers.size();
        for (std::size_t index = 0; index < handlers.size(); ++index)
        {
            if (handlers[index].priority < entry.priority)
            {
                insertPosition = index;
                break;
            }
        }
        handlers.push_back(std::move(entry));
        std::rotate(handlers.begin() + static_cast<std::ptrdiff_t>(insertPosition), handlers.end() - 1, handlers.end());
    }

    template <typename ListenerClass, void (ListenerClass::*HandlerMethod)(const EventType &)>
    static void dispatchToMember(
        void *userData, const EventType &event
    )
    {
        (static_cast<ListenerClass *>(userData)->*HandlerMethod)(event);
    }

    static void dispatchToFree(
        void *userData, const EventType &event
    )
    {
        void (*freeFunction)(const EventType &) = reinterpret_cast<void (*)(const EventType &)>(userData);
        freeFunction(event);
    }

    std::vector<HandlerEntry> handlers;
    // Subscriptions made while a dispatch was walking this channel: they are
    // valid immediately (token-wise) but land in the vector only after the
    // outermost dispatch completes, so a mid-dispatch subscribe can never
    // shift entries over the scan's cursor.
    std::vector<HandlerEntry> pendingSubscriptions;
    std::vector<SubscriptionToken> deferredRemovals;
    ChannelLatencyStatistics statistics;
    const bool *recordingFlag = nullptr;
    // Deferred-delivery state (publishDeferred / flushDeferred). Two vectors,
    // both empty for the whole run unless something defers: the pending queue
    // grows at defer time, the drain buffer exists only to keep its capacity
    // across flushes instead of reallocating per frame.
    std::vector<EventType> deferredEvents;
    std::vector<EventType> deferredDrain;
    std::atomic<bool> *deferredPendingFlag = nullptr;
    bool dispatching                       = false;
    SubscriptionToken nextToken            = 1;

    static std::uint64_t elapsedNanoseconds(
        const std::chrono::steady_clock::time_point &start
    )
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()
        );
    }
};

// Type-erased surface of one worker handoff queue: the bus's flush point
// calls flush() without knowing the event type, the same discipline the
// channel holders apply to unsubscribe and reporting. Declared OUTSIDE the
// bus so the typed queues can derive from it publicly.
class WorkerQueueBase
{
public:
    virtual ~WorkerQueueBase() = default;

    // Publishes every queued event on the MAIN thread (the bus's flush point
    // or a component's own drain, both single-threaded by contract).
    virtual void flush() = 0;

    // The bus-side shutdown hook: after this returns, the queue accepts no
    // new events and delivers nothing further. The bus calls it on every
    // live queue from releaseChannels, before the channels die - a queue
    // whose owning component still holds a shared_ptr then becomes inert
    // instead of dangling. Default no-op so the seam stays optional for
    // future queue implementations that need no shutdown state.
    virtual void closeQueue()
    {}
};

// A thread-safe handoff queue for ONE event type: worker threads enqueue,
// the main thread's flushDeferredEvents() publishes. This is the second half
// of the deferred-delivery story - the AsyncTaskDispatcher's completion
// callbacks re-enter the single-threaded world one bespoke lambda at a time,
// while a worker queue carries the EVENTS themselves across the boundary: a
// decode worker that finishes twelve textures enqueues twelve
// AssetLoadedEvents and never writes a capturing completion lambda at all.
//
// Contract: enqueue from any thread; flush (directly or through the bus's
// flush point) on the main thread only, because flushing publishes on the
// single-threaded bus. Queued events publish in FIFO order. The mutex is the
// only lock, exactly like the dispatcher's queue mutex. A queue is created
// through EventBus::createWorkerQueue during construct (main thread, when the
// bus's channel map may still grow) and dies with the bus: a component that
// created one keeps the shared_ptr, releases it in its stop phase like any
// subscription, and never enqueues after stop. Enforcement: the bus CLOSES
// every queue when it releases its channels (releaseChannels during stop);
// a closed queue drops new enqueues and drains silently, so a worker thread
// that misses the shutdown window cannot write through a dangling channel
// reference - the queue object (which the component's shared_ptr keeps
// alive) stays safe even after the bus is gone.
template <typename EventType> class WorkerEventQueue final : public WorkerQueueBase
{
public:
    // The queue is owned jointly by the bus (which flushes it) and the creator
    // (which holds the shared_ptr); the closed flag below makes the
    // bus-outlives-queue window safe instead of undefined.
    WorkerEventQueue(
        EventChannel<EventType> &targetChannel, std::atomic<bool> *busDeferredPendingFlag
    )
        : channel(targetChannel), deferredPendingFlag(busDeferredPendingFlag)
    {}

    WorkerEventQueue(const WorkerEventQueue &)            = delete;
    WorkerEventQueue &operator=(const WorkerEventQueue &) = delete;

    // Queues one event for the next main-thread flush. Callable from ANY
    // thread. The pending flag is written inside the lock so a flush that
    // starts after the enqueue always sees the event: the bus-level gate
    // never misses a queued event, at worst a race defers delivery to the
    // next frame's flush (the polling contract the flush point already has).
    // A closed queue (engine stopped) drops the event instead of publishing
    // into a channel that no longer exists.
    void enqueue(
        EventType event
    )
    {
        if (closed.load(std::memory_order_acquire))
        {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (closed.load(std::memory_order_relaxed))
            {
                return; // closed between the fast check and the lock
            }
            pendingEvents.push_back(std::move(event));
            pendingCount.fetch_add(1, std::memory_order_relaxed);
            if (deferredPendingFlag != nullptr)
            {
                deferredPendingFlag->store(true, std::memory_order_relaxed);
            }
        }
    }

    // Publishes every queued event on the MAIN thread. The bus calls this
    // from flushDeferredEvents (through the WorkerQueueBase seam); calling it
    // directly is legal (the same single-thread contract) for a component
    // that wants its own drain point. An idle queue answers one relaxed
    // atomic load and returns without touching the mutex (the counter below
    // is the fast path's only read); a stale zero just defers a just-enqueued
    // event to the next flush, which is the same polling contract
    // AsyncTaskDispatcher::drainCompletedTasks documents for its own gate.
    void flush() override
    {
        if (pendingCount.load(std::memory_order_relaxed) == 0)
        {
            return;
        }
        // Member drain buffer (main thread only - the flush is, by contract):
        // the swap hands the queue's events over without resetting either
        // side's capacity. The local vector this used to declare died at
        // scope exit and took its heap block with it, so a run with active
        // worker traffic re-allocated the queue storage on the next enqueue
        // and freed it here, every flush. The clear() below keeps the
        // buffer's capacity - the same discipline EventChannel::flushDeferred
        // documents for its own drain buffer.
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (pendingEvents.empty())
            {
                return;
            }
            drainBuffer.swap(pendingEvents);
            pendingCount.store(0, std::memory_order_relaxed);
        }
        if (closed.load(std::memory_order_acquire))
        {
            drainBuffer.clear();
            return; // the channel died with the bus; dropping beats dangling
        }
        for (const EventType &event : drainBuffer)
        {
            channel.publish(event);
        }
        drainBuffer.clear();
    }

    // The bus-side shutdown hook (see WorkerQueueBase): after this returns,
    // the queue accepts nothing new and delivers nothing more. Idempotent.
    void closeQueue() override
    {
        closed.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(queueMutex);
        pendingEvents.clear();
        pendingCount.store(0, std::memory_order_relaxed);
    }

private:
    EventChannel<EventType> &channel;
    std::atomic<bool> *deferredPendingFlag = nullptr;
    // Set once by the bus's releaseChannels (which runs while the component
    // shared_ptrs may still hold the queue alive): the flag is what makes a
    // late enqueue or flush from another thread a defined no-op.
    std::atomic<bool> closed{false};
    std::mutex queueMutex;
    std::vector<EventType> pendingEvents;
    // The idle fast path's gate: the queued-but-unflushed count, written only
    // under the mutex (so the mutex stays the source of truth) and read
    // relaxed by flush's empty check - the same shape the completion counter
    // of AsyncTaskDispatcher keeps. A stale zero defers delivery one flush;
    // a stale non-zero costs one lock and an empty swap check, never a wrong
    // delivery.
    std::atomic<std::size_t> pendingCount{0};
    // Main-thread flush scratch, retained across flushes (see flush).
    // Flushing is main-thread only, so it needs no lock of its own.
    std::vector<EventType> drainBuffer;
};

// The event bus owns one channel per event type. Channels are heap anchored so
// references handed out once stay valid for the lifetime of the bus: modules
// cache a channel reference at construction and publish with zero lookups.
//
// Communication latency model: publishing is a linear scan over one contiguous
// handler vector with one indirect call per handler - no queue, no allocation,
// no lock (the bus is single threaded by contract). That is the minimum
// achievable latency for in-process, same-thread delivery; queued or threaded
// alternatives were evaluated and rejected because they trade away exactly the
// latency this engine prioritises (see docs/architecture.md).
//
// Deferred delivery does not soften that model, it runs alongside it:
// publishDeferred is a vector append on the calling thread, and
// flushDeferredEvents turns queued events into ordinary synchronous publishes
// at a point the composition root chooses (once per frame, next to the async
// completion drain). Nothing queued ever jumps a thread boundary on its own,
// and a run that defers nothing pays one atomic load per frame for the gate.
class EventBus final
{
public:
    EventBus() = default;

    EventBus(const EventBus &)            = delete;
    EventBus &operator=(const EventBus &) = delete;

    // Communication latency recording is a diagnostic, not a default: a normal
    // run publishes thousands of events per second and its users do not want a
    // report on shutdown. Disabled (the default) the bus never reads a clock
    // for measurement; enabled it records into the per-channel bounded rings
    // that latencyReport() prints. The app enables it from [diagnostics]
    // latency_report configuration key.
    void setLatencyRecordingEnabled(
        bool enabled
    )
    {
        latencyRecording = enabled;
    }

    [[nodiscard]] bool latencyRecordingEnabled() const
    {
        return latencyRecording;
    }

    // Stable reference to the channel for the given event type. The reference
    // remains valid for the lifetime of the bus; cache it in the caller.
    template <typename EventType> EventChannel<EventType> &channel()
    {
        const std::type_index eventType = typeid(EventType);
        auto found                      = channels.find(eventType);
        if (found == channels.end())
        {
            found = channels.emplace(eventType, std::make_unique<ChannelHolder<EventType>>()).first;
            ChannelHolder<EventType> *holder = static_cast<ChannelHolder<EventType> *>(found->second.get());
            holder->channel.setRecordingFlag(&latencyRecording);
            holder->channel.setDeferredPendingFlag(&deferredPending);
            // Creation order IS the deferred flush order: deterministic
            // across runs of the same assembly (construct order is), unlike
            // the unordered_map's iteration order, which must never be
            // observable through delivery semantics.
            channelOrder.push_back(holder);
        }
        return static_cast<ChannelHolder<EventType> *>(found->second.get())->channel;
    }

    // Convenience publish with a lookup per call. Hot paths should cache the
    // channel reference and publish through it directly.
    template <typename EventType>
    void publish(
        const EventType &event
    )
    {
        channel<EventType>().publish(event);
    }

    // Convenience deferral with a lookup per call: same rule as publish - a
    // hot deferrer caches the channel and calls publishDeferred on it.
    template <typename EventType>
    void publishDeferred(
        const EventType &event
    )
    {
        channel<EventType>().publishDeferred(event);
    }

    // The deferred-delivery flush point: publishes everything queued by
    // publishDeferred (channels in creation order, FIFO within a channel) and
    // everything enqueued by worker queues (registration order, FIFO within a
    // queue). The composition root drives this once per frame; the kernel
    // drives one final flush at the top of stop() so an event deferred during
    // the last frame is still delivered while every component is alive.
    //
    // Cost contract: one relaxed atomic load when nothing is pending, and
    // nothing else - the channel walk happens only after a defer actually
    // marked the gate. Events deferred during this flush (from inside a
    // handler) re-mark the gate and land in the NEXT flush, so one call is
    // always bounded by the backlog that existed when it started.
    void flushDeferredEvents()
    {
        if (!deferredPending.load(std::memory_order_relaxed))
        {
            return;
        }
        deferredPending.store(false, std::memory_order_relaxed);
        for (ChannelHolderBase *channelHolder : channelOrder)
        {
            channelHolder->flushDeferredChannel();
        }
        for (const std::shared_ptr<WorkerQueueBase> &workerQueue : workerQueues)
        {
            workerQueue->flush();
        }
    }

    // Creates one worker handoff queue for the event type (see
    // WorkerEventQueue). Main thread only - the call touches the channel map,
    // which grows during construct and is frozen afterwards. The returned
    // shared_ptr keeps the queue object alive; the bus holds its own reference
    // and flushes the queue at every flushDeferredEvents until the kernel
    // releases the queues during stop. A component that created a queue
    // releases its shared_ptr in its stop phase; enqueueing after stop is a
    // contract breach like publishing through a cached channel after stop.
    template <typename EventType> [[nodiscard]] std::shared_ptr<WorkerEventQueue<EventType>> createWorkerQueue()
    {
        auto queue = std::make_shared<WorkerEventQueue<EventType>>(channel<EventType>(), &deferredPending);
        workerQueues.push_back(queue);
        return queue;
    }

    // Unsubscribe without knowing the channel type. Used by EventSubscription.
    void unsubscribeThrough(
        std::type_index eventType, SubscriptionToken token
    )
    {
        const auto found = channels.find(eventType);
        if (found != channels.end())
        {
            found->second->unsubscribe(token);
        }
    }

    // Destroys every channel now, at an explicitly chosen point. A channel's
    // concrete destructor is code inside the plugin binary that first created
    // it (the channel<Event> template instantiates in the consumer that
    // called it), so the channels must die while every provider library is
    // still loaded: stop() releases the component records - and with them the
    // plugin libraries - right after draining, and the implicit bus teardown
    // at kernel destruction then works on an already empty map instead of
    // calling into unloaded code. The worker queues die in the same breath:
    // every queue references a channel, and a queue that outlived its channel
    // would hold a dangling reference for its next flush.
    void releaseChannels()
    {
        // Close the worker queues FIRST: their flush walks into the channels
        // about to be destroyed, and a component-held shared_ptr can keep a
        // queue alive past this point. Closing makes every later enqueue and
        // flush a defined no-op (see WorkerEventQueue).
        for (const std::shared_ptr<WorkerQueueBase> &workerQueue : workerQueues)
        {
            workerQueue->closeQueue();
        }
        channels.clear();
        channelOrder.clear();
        workerQueues.clear();
        deferredPending.store(false, std::memory_order_relaxed);
    }

    // Human readable subscription map: one line per channel that has at least
    // one handler - the channel name, the handler count and the priority span
    // (or a note that every handler runs at the default priority in
    // subscription order). Built on demand for the component graph report
    // (nothing caches it); a dry run with no subscriptions produces an empty
    // string and prints nothing. This is the mod developer's answer to "who is
    // listening, and how heavy is the scan I am about to join".
    std::string subscriptionReport() const
    {
        std::string report;
        std::vector<const ChannelHolderBase *> ordered;
        ordered.reserve(channels.size());
        for (const auto &entry : channels)
        {
            if (entry.second->subscriberCount() > 0)
            {
                ordered.push_back(entry.second.get());
            }
        }
        if (ordered.empty())
        {
            return report;
        }
        // Deterministic order: by channel name, like the latency report.
        std::sort(ordered.begin(), ordered.end(), [](const ChannelHolderBase *left, const ChannelHolderBase *right) {
            return left->reportName() < right->reportName();
        });
        report += "event subscriptions (" + std::to_string(ordered.size()) + " channel(s) with handlers):\n";
        for (const ChannelHolderBase *channelHolder : ordered)
        {
            channelHolder->appendSubscriptionLine(report);
        }
        return report;
    }

    // Human readable communication latency report: one line per active
    // channel with delivery count, mean and the p50/p95/p99/max of the most
    // recent dispatches. Percentiles come from the bounded per-channel sample
    // ring, so they are estimates over recent history, not lifetime exacts.
    std::string latencyReport() const
    {
        std::string report;
        std::vector<const ChannelHolderBase *> ordered;
        ordered.reserve(channels.size());
        for (const auto &entry : channels)
        {
            ordered.push_back(entry.second.get());
        }
        // Deterministic order: by channel name.
        std::sort(ordered.begin(), ordered.end(), [](const ChannelHolderBase *left, const ChannelHolderBase *right) {
            return left->reportName() < right->reportName();
        });
        for (const ChannelHolderBase *channel : ordered)
        {
            channel->appendReport(report);
        }
        return report;
    }

private:
    struct ChannelHolderBase
    {
        virtual ~ChannelHolderBase()                         = default;
        virtual bool unsubscribe(SubscriptionToken token)    = 0;
        virtual const std::string &reportName() const        = 0;
        virtual void appendReport(std::string &report) const = 0;
        // Publishes the channel's deferred backlog (no-op when empty).
        virtual void flushDeferredChannel() = 0;
        // Handler count for the subscription report.
        virtual std::size_t subscriberCount() const = 0;
        // One "  <name>: <count> handler(s) ..." line; called only for
        // channels with at least one handler.
        virtual void appendSubscriptionLine(std::string &report) const = 0;
    };

    template <typename EventType> struct ChannelHolder final : ChannelHolderBase
    {
        EventChannel<EventType> channel;
        std::string name = prettifyChannelName(typeid(EventType));

        bool unsubscribe(
            SubscriptionToken token
        ) override
        {
            return channel.unsubscribe(token);
        }

        const std::string &reportName() const override
        {
            return name;
        }

        void appendReport(
            std::string &report
        ) const override
        {
            const ChannelLatencyStatistics &stats = channel.latencyStatistics();
            if (stats.deliveries == 0)
            {
                return;
            }
            std::vector<std::uint32_t> recent(stats.samples, stats.samples + stats.sampleCount);
            std::sort(recent.begin(), recent.end());
            const auto percentile = [&recent](std::size_t fraction) {
                const std::size_t index = std::min(recent.size() - 1, recent.size() * fraction / 100);
                return recent[index];
            };
            const std::uint64_t mean = stats.totalNanoseconds / stats.deliveries;
            report += "  " + name + ": " + std::to_string(stats.deliveries) + " dispatches, mean "
                    + std::to_string(mean) + " ns, p50 " + std::to_string(percentile(50)) + " ns, p95 "
                    + std::to_string(percentile(95)) + " ns, p99 " + std::to_string(percentile(99)) + " ns, max "
                    + std::to_string(stats.maxNanoseconds) + " ns\n";
        }

        void flushDeferredChannel() override
        {
            channel.flushDeferred();
        }

        std::size_t subscriberCount() const override
        {
            return channel.subscriberCount();
        }

        void appendSubscriptionLine(
            std::string &report
        ) const override
        {
            report += "  " + name + ": " + std::to_string(channel.subscriberCount()) + " handler(s)";
            if (channel.defaultPrioritySpan())
            {
                report += " (subscription order)\n";
            } else
            {
                report += " (priorities " + std::to_string(channel.lowestPriority()) + ".."
                        + std::to_string(channel.highestPriority()) + ")\n";
            }
        }
    };

    std::unordered_map<std::type_index, std::unique_ptr<ChannelHolderBase>> channels;
    // Channels in creation order: the deferred flush walks this, not the map,
    // so which channel's backlog publishes first is a property of the
    // assembly (construct order), never of hash bucket layout.
    std::vector<ChannelHolderBase *> channelOrder;
    // Worker handoff queues created by components, flushed after the channels
    // at the same flush point. Empty for a run that created none - the flush
    // loop above iterates nothing.
    std::vector<std::shared_ptr<WorkerQueueBase>> workerQueues;
    // The deferred-delivery gate: set by publishDeferred (main thread) and by
    // WorkerEventQueue::enqueue (any thread, under the queue lock), read
    // relaxed by flushDeferredEvents. Atomic only because workers touch it.
    std::atomic<bool> deferredPending{false};
    bool latencyRecording = false;
};

// RAII guard for one subscription. Releasing the guard unsubscribes the handler,
// which guarantees that a stopped module or an unloaded mod can never observe
// events through a dangling handler.
class EventSubscription final
{
public:
    EventSubscription() = default;

    EventSubscription(
        EventBus *ownerBus, std::type_index eventType, SubscriptionToken token
    )
        : ownerBus(ownerBus), eventType(eventType), token(token)
    {}

    ~EventSubscription()
    {
        release();
    }

    EventSubscription(const EventSubscription &)            = delete;
    EventSubscription &operator=(const EventSubscription &) = delete;

    EventSubscription(
        EventSubscription &&other
    ) noexcept
        : ownerBus(other.ownerBus), eventType(other.eventType), token(other.token)
    {
        other.ownerBus = nullptr;
        other.token    = invalidSubscriptionToken;
    }

    EventSubscription &operator=(
        EventSubscription &&other
    ) noexcept
    {
        if (this != &other)
        {
            release();
            ownerBus       = other.ownerBus;
            eventType      = other.eventType;
            token          = other.token;
            other.ownerBus = nullptr;
            other.token    = invalidSubscriptionToken;
        }
        return *this;
    }

    void release()
    {
        if (ownerBus != nullptr && token != invalidSubscriptionToken)
        {
            ownerBus->unsubscribeThrough(std::type_index(eventType), token);
        }
        ownerBus = nullptr;
        token    = invalidSubscriptionToken;
    }

private:
    EventBus *ownerBus        = nullptr;
    std::type_index eventType = typeid(void);
    SubscriptionToken token   = invalidSubscriptionToken;
};

} // namespace engine

#endif // ENGINE_KERNEL_EVENT_BUS_HPP
