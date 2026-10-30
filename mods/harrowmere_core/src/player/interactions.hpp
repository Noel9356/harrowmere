#ifndef HARROWMERE_CORE_PLAYER_INTERACTIONS_HPP
#define HARROWMERE_CORE_PLAYER_INTERACTIONS_HPP

// interactions.hpp — the context-interaction semantics of card 3.10: the
// range gate ("Interactions by proximity + target type"), the handler
// registry, and the routing one Interact{target, action} intent takes at
// tick step 9 (§4.4 names step 9 "Player").
//
// ---- What this file owns vs. what its owners still owe ----------------------
//
// The card's action vocabulary is pinned in the published capability header
// (InteractIntent::Action — the input mod compiles against nothing else).
// The per-action HANDLERS belong to their owner tasks and do not exist yet:
//
//   talk         -> trade via 3.7 (vendor) / title-claim dialog via 6.3
//                   (noble) — the target-TYPE split reads the character
//                   store's roles, which land in Phase 2
//   loot         -> 3.3 (corpses)
//   pick_up/drop -> 2.4 (inventory)
//   report_crime + bribe -> 4.1 (the player-report / bribery flows)
//   surrender    -> 4.3 (to any guard of the issuing territory)
//
// Until an owner lands, its action routes through the D-SINK-1 pattern
// ("cross-layer calls go through sink interfaces with null defaults until
// owners land"): the gate runs, an unregistered action dispatches to the
// null default (NoHandler), and the owner replaces the null by registering
// a handler — the vocabulary, the gate and the routing never move.
//
// ---- The two seams (frozen signatures) ---------------------------------------
//
//   * InteractionResolver — maps an interaction target id to its world
//     position. The null default answers "unknown" for every id; Phase 2's
//     character store binds the real one (positions live with the
//     characters; the spatial grid (1.6) answers the radius queries around
//     them). Single-target position lookup — not a neighbour scan — so it
//     does not route through SpatialGrid; the grid remains the only
//     RADIUS-query path (card 1.6's Done-when).
//
//   * InteractionHandlers — action -> handler table. A handler receives one
//     plain InteractionRequest (action, target, resolved position, the
//     worldTime of the consuming tick) and does its owner's work; owners
//     that must cause work in a LATER step enqueue via the scheduler
//     (§4.4 reentrancy rule) — never publish re-entrantly (0.7 rule 2).
//
// Determinism (R7): routing order is the queue's FIFO order (the intent
// order the input layer produced); the registry is a fixed array indexed by
// the action enum (R9's COUNT sentinel) — no unordered container anywhere,
// nothing here iterates over a hash map.
//
// Error handling (§3.4): an unknown target or an out-of-range target is a
// RULE-level rejection — a return value (InteractionGate), never an
// exception, and never a crash. The rejection surfaces to the player as a
// PlayerSignal when the runtime wiring lands (§4.2 rule 2; Phase 1's
// headless tests read the gate outcome directly).
//
// R5: the range comparison is f64 on the x/z plane through region.hpp's
// D-WORLD-1 distance helper (distanceSquaredM) — the same seam every other
// geometric compare uses; the boundary is INCLUSIVE (d <= range), the
// convention isSheltered (3.8) already pins.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "core/types.hpp"            // Position (task 0.2)
#include "harrowmere_capability.hpp" // InteractIntent — the published vocabulary

namespace harrowmere::player
{

// ---- the resolver seam --------------------------------------------------------

// Maps an interaction target id to its world position. Returns nullptr when
// the id names no interactable (unknown, stale, or despawned) — the gate
// then rejects with InteractionGate::UnknownTarget. A pure query: must not
// mutate sim state (the routing runs inside tick step 9).
class InteractionResolver
{
public:
    virtual ~InteractionResolver() = default;

    virtual const Position *positionOf(
        std::uint64_t target
    ) const = 0;
};

// The D-SINK-1 null default: every target unknown. Phase 1 runs on this
// (no character store exists — "No NPC exists in the world state beyond the
// population plan"); Phase 2's store-backed resolver replaces it at the
// step-9 registration, never inside a tick.
class NullInteractionResolver final : public InteractionResolver
{
public:
    const Position *positionOf(
        std::uint64_t
    ) const override
    {
        return nullptr;
    }
};

// The shared null instance (initialization-on-first-use; the default
// argument of applyIntents and routeInteraction).
InteractionResolver &nullInteractionResolver() noexcept;

// ---- the handler registry -----------------------------------------------------

// One routed interaction: the plain-data payload an owner handler consumes.
// Owners extend their own stores internally; the request stays this shape
// (add fields only with the owner that needs them — source-compatible).
struct InteractionRequest
{
    InteractIntent::Action action = InteractIntent::Action::TALK;
    std::uint64_t target          = 0; // the characterId the intent named
    Position targetPosition{};         // the position the gate passed on (x/z; y cosmetic)
    std::uint64_t worldTime       = 0; // the consuming tick's game time (WorldClock.worldTime)
};

// The action -> handler table (fixed array over InteractIntent::Action,
// R9: the enum ends in COUNT exactly for this bound). An action without a
// registered owner dispatches to the null default: the call is a no-op and
// the gate reports NoHandler — visible, deterministic, never a crash.
class InteractionHandlers
{
public:
    using Handler = std::function<void(const InteractionRequest &)>;

    // Register (or replace) the handler of `action`. Owner tasks call this
    // once at construction — between ticks, never mid-tick (the registry is
    // read inside step 9).
    void registerHandler(
        InteractIntent::Action action, Handler handler
    );

    // True when an owner has claimed the action (the registration probe a
    // Phase-2 owner uses to assert its claim landed).
    bool hasHandler(
        InteractIntent::Action action
    ) const noexcept;

    // Run the handler registered for request.action. Returns false (and
    // does nothing) when no handler is registered — the D-SINK-1 null
    // default. A registered null std::function counts as unregistered.
    bool dispatch(
        const InteractionRequest &request
    ) const;

    // The shared empty instance (applyIntents' default argument): every
    // action unregistered — the exact Phase-1 shape.
    static InteractionHandlers &empty() noexcept;

private:
    std::array<Handler, static_cast<std::size_t>(InteractIntent::Action::COUNT)> handlers_{};
};

// ---- the gate + routing -------------------------------------------------------

// The outcome of routing one Interact intent (the rule-level rejections are
// return values, §3.4; each carries its logged reason):
//
//   Dispatched    — the target is known, within range, and its owner
//                   handler ran.
//   NoHandler     — the target is known and within range but no owner has
//                   registered for the action yet (the D-SINK-1 null
//                   default; the action is consumed — the queue never
//                   re-delivers it).
//   OutOfRange    — the target is known but beyond INTERACT_RANGE_M.
//   UnknownTarget — the resolver named no position for the target id.
enum class InteractionGate
{
    Dispatched,
    NoHandler,
    OutOfRange,
    UnknownTarget
};

// Route one Interact{target, action} intent: resolve the target, gate on
// proximity (INCLUSIVE at INTERACT_RANGE_M — d <= range, x/z plane), then
// dispatch to the registered owner handler. The player position comes from
// the sim-side transform (PlayerController.position); worldTime stamps the
// request with the consuming tick's game time. No rejections are thrown,
// logged only at TRACE (the per-tick hot-path rule, §3.4/log.hpp rule 2).
InteractionGate routeInteraction(
    const Position &playerPosition,
    std::uint64_t target,
    InteractIntent::Action action,
    std::uint64_t worldTime,
    const InteractionResolver &resolver,
    const InteractionHandlers &handlers
);

} // namespace harrowmere::player

#endif // HARROWMERE_CORE_PLAYER_INTERACTIONS_HPP
