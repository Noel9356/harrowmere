#ifndef ENGINE_KERNEL_PARALLEL_FOR_HPP
#define ENGINE_KERNEL_PARALLEL_FOR_HPP

// Fork-join parallel iteration, as a kernel utility.
//
// The engine's threading contract is deliberate and documented
// (docs/architecture.md): the event bus is single threaded, the frame loop
// is serialized, and nothing shares GPU resources across threads. That
// contract governs COMMUNICATION; it does not forbid parallel COMPUTE. CPU-
// heavy batch work with independent items (texture decode bursts, image
// encoding, bulk data preparation) runs well on several cores as long as
// the results re-enter the single-threaded world at a well-defined point.
// This header is the smallest safe shape for that:
//
//   parallelFor(itemCount, [](std::size_t begin, std::size_t end) {
//       // process items [begin, end) - each item independent of the others
//   });
//
// - the range is split into contiguous chunks, the chunks run on the
// calling thread AND on a small set of transient worker threads, and the
// call BLOCKS until every chunk has finished (fork-join: after the call
// returns, all work is done and only the calling thread remains). A body
// that throws has its exception captured and rethrown on the calling
// thread after the join, so a failed batch is a precise error at the call
// site, never a std::terminate on a worker.
//
// The threads are transient: spawned per call and joined before it returns.
// That keeps the utility stateless (no pool lifecycle, no static threads
// racing process teardown) at the price of a thread spawn per call - which
// is why the utility is for COARSE-GRAINED work: a chunk should represent
// roughly a millisecond or more of CPU time (one texture, one image, one
// file). Fine-grained per-frame fan-out belongs on an owned
// AsyncTaskDispatcher instead (see async_dispatcher.hpp), whose workers are
// started once and reused every frame.
//
// Sizing: defaultWorkerCount() is the engine's one answer to "how many
// workers for CPU-bound parallel work" - the hardware thread count clamped
// to a bounded range, so a many-core machine parallelizes usefully without
// oversubscribing and a single-core machine (or an unknown count, where
// hardware_concurrency() reports 0) simply runs serially. Callers that own
// a persistent dispatcher start it with the same helper, so the whole
// engine sizes its parallelism one way.

#include <algorithm>
#include <cstddef>
#include <exception>
#include <functional>
#include <thread>
#include <vector>

namespace engine
{

// The engine-wide default worker count for CPU-bound parallel work: the
// hardware thread count, clamped to [1, 16]. The clamp bounds
// oversubscription on very large machines (decode and encode work is
// memory-bandwidth bound long before 16 workers) and the floor answers an
// unknown count (hardware_concurrency() may report 0) with the serial
// path's exact semantics.
inline std::size_t defaultWorkerCount()
{
    const unsigned int hardware = std::thread::hardware_concurrency();
    return std::clamp<std::size_t>(hardware == 0 ? 1 : static_cast<std::size_t>(hardware), 1, 16);
}

// Runs itemCount units of independent work across the calling thread and
// transient workers: the range [0, itemCount) is split into one contiguous
// chunk per worker (chunkBody receives [begin, end) half-open), every chunk
// runs to completion, and the call blocks until all of them have finished.
//
// Contract:
// - Item independence: chunk i must not read or write what chunk j writes.
//   The body owns its synchronization only when it knows better.
// - The body must not touch the event bus, GPU resources, or anything the
//   frame loop owns - it should compute and (captured) write results. This
//   is the same contract every worker thread obeys (docs/architecture.md).
// - The body must not call parallelFor (a nested fork-join can deadlock:
//   the inner call waits for workers the outer call is holding) and must
//   not destroy the objects its chunk body captures.
// - itemCount == 0 is a no-op; a count that fits one chunk runs entirely on
//   the calling thread with zero threads spawned - a parallelFor on small
//   batches is correct, just not faster.
//
// Exceptions: a throwing chunk is captured (exception_ptr) and the FIRST
// captured exception is rethrown on the calling thread after the join.
// Every chunk still runs to completion - a batch fails as a whole, at a
// well-defined point, with no threads left behind.
//
// The body is a template parameter, not a std::function: the fork-join runs
// each chunk through the caller's callable directly, so the compiler inlines
// the chunk body into the worker entry (no double indirection, no closure
// copy) - this utility sits under the renderer's startup texture decode,
// where the body is where all the time goes, and the erased form below stays
// for callers that hold the body as a std::function already.

// The shared fork-join mechanics over an arbitrary body reference. Internal:
// both public entry points below funnel here.
template <typename Body> inline void parallelForDispatch(
    std::size_t itemCount, Body &chunkBody
)
{
    if (itemCount == 0)
    {
        return;
    }
    const std::size_t workerCount = std::min(defaultWorkerCount(), itemCount);
    if (workerCount <= 1)
    {
        chunkBody(0, itemCount);
        return;
    }

    // Contiguous split: worker w owns [w*chunkSize, min((w+1)*chunkSize,
    // itemCount)). The remainder lands in the LAST chunk, so chunk sizes
    // differ by at most one item and every item is covered exactly once.
    const std::size_t chunkSize = itemCount / workerCount;
    const std::size_t remainder = itemCount % workerCount;

    // One exception slot per worker; the first capture wins the rethrow.
    std::vector<std::exception_ptr> failures(workerCount, nullptr);
    std::vector<std::thread> workers;
    workers.reserve(workerCount - 1);

    const auto runChunk = [&chunkBody, &failures](std::size_t workerIndex, std::size_t begin, std::size_t end) {
        try
        {
            chunkBody(begin, end);
        } catch (...)
        {
            failures[workerIndex] = std::current_exception();
        }
    };

    // The calling thread takes worker 0's chunk; workers 1..n-1 run theirs.
    for (std::size_t workerIndex = 1; workerIndex < workerCount; ++workerIndex)
    {
        const std::size_t begin = workerIndex * chunkSize + std::min(workerIndex, remainder);
        const std::size_t end   = begin + chunkSize + (workerIndex < remainder ? 1 : 0);
        workers.emplace_back([&runChunk, workerIndex, begin, end]() { runChunk(workerIndex, begin, end); });
    }
    runChunk(0, 0, chunkSize + (remainder > 0 ? 1 : 0));

    for (std::thread &worker : workers)
    {
        worker.join();
    }
    for (std::exception_ptr &failure : failures)
    {
        if (failure != nullptr)
        {
            std::rethrow_exception(failure);
        }
    }
}

// The primary form: the body is any callable taking (begin, end) - a lambda
// at every call site today. Direct call, fully inlinable.
template <typename Body> inline void parallelFor(
    std::size_t itemCount, Body &&chunkBody
)
{
    parallelForDispatch(itemCount, chunkBody);
}

// The type-erased form for a body already held as a std::function (or any
// callable stored in one): identical semantics, one indirection per chunk.
// Kept so existing call sites that pass an lvalue std::function keep
// compiling - overload resolution prefers this exact match over the
// template above, and a lambda or a move-in callable prefers the template.
inline void parallelFor(
    std::size_t itemCount, const std::function<void(std::size_t, std::size_t)> &chunkBody
)
{
    if (chunkBody == nullptr)
    {
        return; // the historical guard: an empty std::function is a no-op batch
    }
    parallelForDispatch(itemCount, chunkBody);
}

} // namespace engine

#endif // ENGINE_KERNEL_PARALLEL_FOR_HPP
