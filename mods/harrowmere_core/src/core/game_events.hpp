#ifndef HARROWMERE_CORE_CORE_GAME_EVENTS_HPP
#define HARROWMERE_CORE_CORE_GAME_EVENTS_HPP

// Sim event channels (task 0.7): the typed events the game's systems publish
// to notify other systems and the UI without direct coupling. One struct per
// event, one engine channel per struct type (kernel event_bus.hpp keys
// channels by type). The engine's EventChannel is the ONLY fan-out medium;
// publishing is synchronous on the caller's stack.
//
// The rules pinned with the card (phase-1-walk.md, task 0.7):
//
//   1. Events are FACTS THAT ALREADY HAPPENED. Publish after the mutation is
//      complete, never as a request to perform one. Every handler must be
//      able to read the post-state - the docs' systems assume it (the §4.6
//      worked trace: a manhunt handler sees the culprit already inserted into
//      its target set). Publishing before the mutation lets a handler observe
//      stale state; the struct names below are past-tense facts on purpose.
//
//   2. A handler must not publish to the same channel re-entrantly. Work a
//      handler causes in a later step is queued through the scheduler's
//      pending list (enqueueForNextTick, §4.4 reentrancy rule); same-channel
//      deferral may use the engine's publishDeferred, which delivers at the
//      composition root's next flushDeferredEvents().
//
//   3. Subscriptions are RAII (engine::EventSubscription). A stopped
//      component's guard destructor unsubscribes, so a stopped component
//      never receives a dangling dispatch - engine guarantee, keep using it.
//      An unsubscribe that lands during a dispatch is safe: the kernel
//      defers the structural removal (the handler vector is not mutated
//      mid-scan) while the logical removal is immediate (the removed handler
//      is skipped for the rest of that scan) - both halves pinned by
//      TestEvents_UnsubscribeDuringDispatchDeferred.
//
// The struct set is pinned by task 0.7 - extend it only through a
// DEVIATIONS.md row (§1.4). Later tasks publish and consume ONLY these types
// (review gate); cross-system calls inside harrowmere_core that do not fit an
// event go through internal interfaces, not through a new channel.
//
// Owner map (tick steps per common.md §4.4; card ids are global):
//   CharacterDied / CrimeRecorded / WarrantIssued / WarrantResolved - justice
//     systems (4.1-4.7, tick step 6; combat deaths apply at step 10).
//   EventStarted / EventEnded / EventStateChanged - the event engine
//     (5.1-5.5, tick step 5).
//   TerritoryChanged - whichever system mutates a territory's observable
//     facets (laws, owner chain, safe flag, war: 6.1-6.3).
//   PriceChanged - the economy (3.7, tick step 7).
//   AppearanceChanged - the appearance path (1.1's CooldownType::APPEARANCE).
//   PlayerSignal - UI-visible signals queued for the local player (§4.2:
//     rejections surface through these, never through exceptions; the
//     capability-side queue is SimCapability::signals, rebuilt at step 13).

#include <cstdint>
#include <string>

#include "enums.hpp"
#include "types.hpp"

namespace harrowmere
{

// ---- justice ----------------------------------------------------------------

// A character died: the death is already applied (combat applies deaths
// immediately at step 10; aftermath - XP transfer, corpse/loot - follows at
// step 11). killer == 0 means the environment (the docs' convention for a
// non-combat death).
struct CharacterDied
{
    characterId victim;
    characterId killer; // 0 = environment
};

// A crime was recorded: the evidence object was created atomically at crime
// commit time (recordCrime, 4.1) - the territory snapshot (`where`) was taken
// at the commit position, the species-exemption hard gate already ran (an
// exempted act records no evidence and publishes nothing), and the first
// witness reports are already inside the evidence. Nothing about this is
// visible to the player yet (design rule: nothing is visible at the act).
struct CrimeRecorded
{
    std::uint64_t evidenceId;
    characterId culprit;
    CrimeType type;
    territoryId where;
};

// A warrant was issued from accumulated evidence (issueWarrant, 4.2): the
// warrant is live, `lethal` was copied from the crime data at issuance, and
// enforcement AI may already be responding.
struct WarrantIssued
{
    std::uint64_t warrantId;
    characterId culprit;
    territoryId where;
    bool lethal;
};

// A warrant left the active set with a terminal verdict: conviction (4.2),
// expiry (step 1 sweeps, 4.2), payment (4.2), or surrender (4.6, surrender to
// any guard of the issuing territory).
struct WarrantResolved
{
    std::uint64_t warrantId;
    enum class How
    {
        Convicted,
        Expired,
        Paid,
        Surrendered
    } how;
};

// ---- events -----------------------------------------------------------------

// An event instance started (spawnEvent, 5.1): the instance is live in the
// event store. eventId is world-unique, templateId points at the event
// template it instantiated, region is where it plays out.
struct EventStarted
{
    std::uint64_t eventId;
    std::uint64_t templateId;
    regionId region;
};

// An event instance ended: it is already removed from the active set and its
// world effects are applied; `success` is the state machine's terminal
// verdict (5.1).
struct EventEnded
{
    std::uint64_t eventId;
    bool success;
};

// An event instance moved between states of its template (5.1); oldState and
// newState are the template's state ids. Late observers diff through this
// instead of holding event-internal state.
struct EventStateChanged
{
    std::uint64_t eventId;
    std::uint64_t oldState;
    std::uint64_t newState;
};

// ---- world ------------------------------------------------------------------

// A territory's observable facets changed: the effective law set inputs, its
// owner in the tree, its safe flag, or a war state touching it (6.1-6.3).
// The event carries the fact, not the payload - consumers re-query the world
// stores (lawsAt, the territory tree, safe-zone state).
struct TerritoryChanged
{
    territoryId id;
    enum class What
    {
        Laws,
        Owner,
        Safety,
        War
    } what;
};

// A region price was recomputed (3.7, tick step 7): newPrice is the
// recomputed unit price now in effect for the good in that region.
struct PriceChanged
{
    regionId region;
    std::uint32_t goodId;
    std::uint32_t newPrice;
};

// ---- characters -------------------------------------------------------------

// A character's appearance changed (the appearance path behind
// CooldownType::APPEARANCE, 1.1). The event carries the fact, not the look -
// observers (UI, witness perception 3.9) re-read the character.
struct AppearanceChanged
{
    characterId who;
};

// A UI-visible signal for the local player (design reference; §4.2 rule 2:
// command rejections surface back through these, never through exceptions).
// The sim queues it into its signals view (SimCapability::signals, rebuilt at
// step 13); the event is the cross-system notification that a new signal
// arrived.
struct PlayerSignal
{
    enum class Kind
    {
        Investigated,
        Wanted,
        Poster,
        Greeting
    } kind;
    std::string text;
};

} // namespace harrowmere

#endif // HARROWMERE_CORE_CORE_GAME_EVENTS_HPP
