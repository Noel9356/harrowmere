// interactions.cpp — see interactions.hpp for the contract.
//
// The gate is one inclusive f64 compare through region.hpp's D-WORLD-1
// distance helper; the registry is a fixed array (R9's COUNT bound) with a
// std::function per action. Every rejection is a return value (§3.4) and
// logs at TRACE only — routing runs inside tick step 9, the per-tick
// hot path the logging rules fence off (log.hpp rule 2).

#include "player/interactions.hpp"

#include <cassert>
#include <utility>

#include "core/constants.hpp" // INTERACT_RANGE_M (pinned pending Appendix A)
#include "core/log.hpp"       // HARROWMERE_LOG_TRACE — the only permitted per-tick level
#include "world/region.hpp"   // distanceSquaredM — the D-WORLD-1 geometry helper

namespace harrowmere::player
{

InteractionResolver &nullInteractionResolver() noexcept
{
    static NullInteractionResolver instance;
    return instance;
}

void InteractionHandlers::registerHandler(
    InteractIntent::Action action, Handler handler
)
{
    const auto index = static_cast<std::size_t>(action);
    if (index >= handlers_.size())
    {
        // Programmer error (R9: the enum is closed): assert in debug, drop
        // deterministically in release (§3.4 — never undefined behavior).
        assert(false && "registerHandler: action outside the pinned vocabulary");
        return;
    }
    handlers_[index] = std::move(handler);
}

bool InteractionHandlers::hasHandler(
    InteractIntent::Action action
) const noexcept
{
    const auto index = static_cast<std::size_t>(action);
    if (index >= handlers_.size())
        return false;
    return static_cast<bool>(handlers_[index]);
}

bool InteractionHandlers::dispatch(
    const InteractionRequest &request
) const
{
    const auto index = static_cast<std::size_t>(request.action);
    if (index >= handlers_.size() || !static_cast<bool>(handlers_[index]))
        return false;
    handlers_[index](request);
    return true;
}

InteractionHandlers &InteractionHandlers::empty() noexcept
{
    static InteractionHandlers instance;
    return instance;
}

InteractionGate routeInteraction(
    const Position &playerPosition,
    std::uint64_t target,
    InteractIntent::Action action,
    std::uint64_t worldTime,
    const InteractionResolver &resolver,
    const InteractionHandlers &handlers
)
{
    // 1. Target resolution. target 0 is the reserved id (R10) — the null
    //    resolver rejects it like any other unknown id.
    const Position *targetPosition = resolver.positionOf(target);
    if (targetPosition == nullptr)
    {
        HARROWMERE_LOG_TRACE(sim, "interact rejected: unknown target %llu", static_cast<unsigned long long>(target));
        return InteractionGate::UnknownTarget;
    }

    // 2. The proximity gate (card 3.10: "Interactions by proximity + target
    //    type"). Inclusive at INTERACT_RANGE_M — the boundary isSheltered
    //    (3.8) already pins; the compare rides the D-WORLD-1 helper (R5).
    const double rangeSquared = INTERACT_RANGE_M * INTERACT_RANGE_M;
    if (world::distanceSquaredM(playerPosition, *targetPosition) > rangeSquared)
    {
        HARROWMERE_LOG_TRACE(sim, "interact rejected: target %llu out of range", static_cast<unsigned long long>(target));
        return InteractionGate::OutOfRange;
    }

    // 3. Dispatch to the owner handler; an unclaimed action reports the
    //    null default (D-SINK-1) — consumed, deterministic, visible.
    const InteractionRequest request{action, target, *targetPosition, worldTime};
    if (!handlers.dispatch(request))
    {
        HARROWMERE_LOG_TRACE(sim, "interact consumed: no handler registered yet (action %u)", static_cast<unsigned>(action));
        return InteractionGate::NoHandler;
    }
    return InteractionGate::Dispatched;
}

} // namespace harrowmere::player
