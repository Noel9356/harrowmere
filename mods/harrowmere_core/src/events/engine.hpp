#ifndef HARROWMERE_CORE_EVENTS_ENGINE_HPP
#define HARROWMERE_CORE_EVENTS_ENGINE_HPP

// engine.hpp — task 5.1's engine half: events as data-driven state machines
// with the doc's exact lifecycle, participant bookkeeping, the nesting cap,
// scale uniqueness, and the world-resolves-without-players guarantee.
//
// The doc structs (design reference verbatim — field-for-field, the Done-when):
//   Event       { templateId, regionId (=0 global), depth, currentState,
//                 participants (SORTED, R7), actionAmounts, hpLost,
//                 targetIds, excludedFromRewards, startedAt, lastTransitionAt }
//   EventState  { stateId, difficulty, attempts, successRate, powerSnapshots }
// Not doc state rides engine-side maps (the caseLastTick pattern): the
// leave-early marks, the per-event spawned targets (DespawnTargets'), the
// granted reward tiers (5.4's read) and the ended-event retention.
//
// Lifecycle (design reference verbatim, with the as-built readings noted):
//   startEvent  — state initialized from the template (all four EventState
//                 fields; participants/actionAmounts/hpLost start empty),
//                 entry actions run, EventStarted published AFTER.
//   join        — participants push_sorted_unique; the character's eventId
//                 = the event id.
//   leaveEarly  — the id STAYS in participants (end-of-event rewards still
//                 pay for work done) and the eventId is NOT cleared (the
//                 actor is still "in an event" for shareEvent checks until
//                 the event ends — the card pitfall, test-pinned); the
//                 leave mark refuses later contribution (5.3's
//                 validateActionAmount reads hasLeftEarly).
//   endEvent    — actionAmounts/hpLost/powerSnapshots cleared; EVERY
//                 participant's eventId cleared (leavers included); EventEnded
//                 published. Rewards pay BEFORE endEvent (5.4's order).
//
// State-exit semantics (the as-built reading the shipped templates author
// against): exitActions run when a state EXITS — on a fired transition (old
// state's exits run before the onFire action, then the new state's entry
// actions run and EventStateChanged publishes) and on the maxDurationDays
// forced exit. An EndEvent action fired from an entry/onFire position does
// NOT re-run the current state's exits (ending is not exiting; the
// EndEvent action is idempotent). The transition sequence skips its
// remaining steps the moment the event ends.
//
// Rules enforced here, each with a test: at most ONE global event active;
// regional events coexist iff regionId differs; SpawnNestedEvent sets
// depth = parent.depth + 1 and refuses beyond MAX_EVENT_DEPTH (A-103 —
// depth 2 LEVELS: a child spawns, a grandchild does not); transitions
// evaluate per tick (step 5) in the state's DECLARED order;
// maxDurationDays forces the exit.
//
// The engine implements the IEventSpawner + IEventExclusionSink seams
// (world_context.hpp): 4.6's manhunt spawn enqueues into step 5 and 4.7's
// smuggling rule reads target-side membership / flags reward exclusions —
// neither includes events/ internals (D-SINK-1).

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "core/scheduler.hpp"     // IStepSystem (the step-5 tick)
#include "core/types.hpp"         // characterId, regionId, IdGenerator
#include "core/world_context.hpp" // the IEventSpawner + IEventExclusionSink seams (the base classes)
#include "events/template.hpp"    // EventTemplateTable + the vocabularies

namespace harrowmere
{
struct EventStarted;
struct WorldContext;
struct ManhuntSeed; // world_context.hpp — complete through the include above
} // namespace harrowmere

namespace harrowmere::events
{

// The doc's EventState, verbatim.
struct EventState
{
    std::uint64_t stateId     = 0;
    std::uint32_t difficulty  = 0;
    std::uint32_t attempts    = 0;
    std::uint32_t successRate = 0;
    std::map<characterId, std::uint64_t> powerSnapshots; // 5.3 fills (5.1 starts empty)
};

// The doc's Event, verbatim.
struct Event
{
    std::uint64_t templateId = 0;
    regionId region          = 0; // the doc's `regionId regionId` field (the member drops the
                                  // type-name clash -Wchanges-meaning rejects); 0 = global-scale (the doc rule)
    std::uint8_t depth       = 0;
    EventState currentState;
    std::vector<characterId> participants; // SORTED (R7) — reward math reads it in order
    std::map<characterId, std::array<std::uint32_t, static_cast<std::size_t>(ActionType::COUNT)>> actionAmounts;
    std::map<characterId, std::uint32_t> hpLost;
    std::set<characterId> targetIds;           // declared by the template (+ manhunt insert)
    std::set<characterId> excludedFromRewards; // 4.7's hook (5.3 respects it)
    std::uint64_t startedAt        = 0;
    std::uint64_t lastTransitionAt = 0;
};

class EventEngine final : public IEventSpawner, public IEventExclusionSink, public IEventEffectsSink
{
public:
    EventEngine()                               = default;
    EventEngine(const EventEngine &)            = delete;
    EventEngine &operator=(const EventEngine &) = delete;

    // The template table is the DataRegistry's committed event-templates
    // stage (tests insert rows directly). The context binding serves the
    // IEventSpawner seam (the manhunt spawn arrives context-free — the
    // engine reads the world through the bound pointer, the JusticeSystem
    // binding pattern). Bind both once, before any tick.
    void bind(
        const EventTemplateTable *templates
    )
    {
        templates_ = templates;
    }
    void bindContext(
        WorldContext *ctx
    )
    {
        ctx_ = ctx;
    }

    // The bound table, read-only (the 8.3 contribution walk's reader-gate
    // looks the template up through the SAME binding every other engine
    // consumer uses — production binds the registry's stage, the rigs their
    // own tables — one lookup path, never a second source of truth).
    [[nodiscard]] const EventTemplateTable *templates() const noexcept
    {
        return templates_;
    }

    // ---- the seams (the plugin binds this engine into the context) ----

    // IEventSpawner: the keyed start with a warrant seed — the MANHUNT
    // shape. When the template declares special.targetFromWarrant the
    // culprit enters targetIds; culpritEarnsNothing adds them to
    // excludedFromRewards. Refused when the template is unknown, the scale
    // uniqueness bites, or the depth cap trips.
    bool spawnEvent(const char *templateKey, const ManhuntSeed &seed) override;

    // IEventSpawner: 6.2's battle spawn (D-WAR-EVT-1) — one regional BATTLE
    // event whose two target groups are the seed's member lists (the war
    // module computes them; the engine stays faction-blind). Every member
    // joins as a participant and a target of the other side. Refused when
    // the template is unknown or a BATTLE already stands in the region (the
    // scale-uniqueness gate is the spawn throttle).
    bool spawnWarBattle(const WarBattleSeed &seed) override;

    // IEventExclusionSink: 4.7's smuggling rule.
    bool isTargetSideParticipant(characterId culprit, std::uint64_t eventId) override;
    void excludeFromRewards(characterId who, std::uint64_t eventId) override;

    // ---- the lifecycle ---------------------------------------------------------

    // startEventByKey — the generic start: builds the Event from the
    // template, enforces scale uniqueness (one global; same-region regional
    // refused), runs startEvent (state init + entry actions + EventStarted).
    // 0 = refused. `seed` may be null (world events; bandit_raid).
    std::uint64_t startEventByKey(
        const std::string &templateKey, regionId region, std::uint8_t depth, const ManhuntSeed *seed, WorldContext &ctx
    );

    // join — the doc's verbatim: sorted-unique insert + the character's
    // eventId stamped. Unknown event or missing character = no-op.
    void join(characterId who, std::uint64_t eventId, WorldContext &ctx);

    // leaveEarly — the doc's rule (see the header doc): the participant row
    // STAYS, the eventId stays, the leave mark refuses later contribution.
    void leaveEarly(characterId who, std::uint64_t eventId, WorldContext &ctx);

    // hasLeftEarly — 5.3's validateActionAmount input (the leave mark).
    bool hasLeftEarly(std::uint64_t eventId, characterId who) const;

    // finishEvent — the EndEvent action's engine: clears the contribution
    // maps, clears every participant's eventId, publishes EventEnded, and
    // moves the event to the ended set. No exit actions run here (they run
    // at the state-exit sites — see the header doc). The doc's endEvent is
    // idempotent: an event already finished resolves as a no-op.
    void finishEvent(std::uint64_t eventId, bool success, WorldContext &ctx);

    // ---- the tick ---------------------------------------------------------------

    // tick — step 5 (§4.4 "event engine tick"): per active event (id
    // ascending), the maxDuration forced exit first, then the state's
    // transitions in DECLARED order (first fire wins).
    void tick(WorldContext &ctx);

    // sweepEnded — step 1's event cleanup (§4.4): ended events leave the
    // store (the EventEnded consumers have had the whole retention window).
    void sweepEnded(WorldContext &ctx);

    // ---- reads (the tests' + 5.2-5.5's surface) ---------------------------------

    const Event *find(std::uint64_t eventId) const; // active OR ended
    Event *findMutable(std::uint64_t eventId);      // 5.3's contribution writes
    const std::map<std::uint64_t, Event> &active() const
    {
        return active_;
    }
    const std::map<std::uint64_t, Event> &ended() const
    {
        return ended_;
    }
    std::size_t activeCount() const
    {
        return active_.size();
    }

    // Is a live event of this template key running (the EventStateCondition's
    // other-event read + the uniqueness tests' helper)?
    const Event *findActiveByKey(const std::string &templateKey) const;

    // 5.4's read: the reward tiers the template's GrantRewardTier actions
    // granted (order of grant).
    const std::vector<std::uint32_t> &grantedTiers(std::uint64_t eventId) const;

    // 3.7/3.8's hook reads (the PriceBias/WeatherBias world effects; SCALE =
    // no bias). The consumers join with 5.5's wiring — the records are live
    // from 5.1 (the actions are interpreted, not dropped).
    std::uint64_t priceBiasFor(regionId region, goodId good, std::uint64_t now) const override;
    const std::string *weatherBiasFor(regionId region, std::uint64_t now) const override;

    // 5.5's step-2 slice read: the regions holding an UNEXPIRED weather bias
    // at `now`, region-ascending (R7) — the WorldEffectsTickSystem pins each
    // region's weather kind for the window.
    std::vector<regionId> activeWeatherBiasRegions(std::uint64_t now) const;

    // 5.4's manhunt-completion poll: the warrant seed an event spawned from
    // (the IEventSpawner seam's ManhuntSeed). When the seed's warrant has
    // left the justice store's active set (convicted / paid / surrendered /
    // expired — the justice side's WarrantResolved facts), the hunt ends at
    // the same tick: "any manhunt ends". The event publishes no duplicate
    // WarrantResolved — the fact's owner already did (0.7).
    const ManhuntSeed *seedOf(std::uint64_t eventId) const;

    // 6.2's D-WAR-EVT-1 read: the two target groups of a war battle (group 0
    // = the seed's attacker side, 1 = the defender side), or nullptr when the
    // event hosts no dual-group book. The war tests + the README document
    // the shape.
    const std::set<characterId> *targetGroup(std::uint64_t eventId, std::uint8_t group) const;

    // ---- save seam (task 7.1) ---------------------------------------------------
    //
    // The engine's whole book as one plain-data snapshot: the live + ended
    // events, the per-event side maps (the 5.1/5.3/5.4/6.2 books — the card
    // pins spawned targets and biases as world state), the 5.5's price/weather
    // bias records and the "event" id counter. persist/ serializes it; the
    // load path hands it back through restore().
    struct Snapshot
    {
        std::map<std::uint64_t, Event> active;
        std::map<std::uint64_t, Event> ended;
        std::map<std::uint64_t, std::set<characterId>> leftEarly;
        std::map<std::uint64_t, std::vector<characterId>> spawnedTargets;
        std::map<std::uint64_t, std::vector<std::uint32_t>> grantedTiers;
        std::map<std::uint64_t, ManhuntSeed> seeds;
        std::map<std::uint64_t, std::array<std::set<characterId>, 2>> targetGroups;
        std::map<regionId, std::map<goodId, std::pair<std::uint32_t, std::uint64_t>>> priceBias;
        std::map<regionId, std::pair<std::string, std::uint64_t>> weatherBias;
        std::map<std::string, std::uint64_t> idCounters; // the "event" id space (IdGenerator state)
    };

    [[nodiscard]] Snapshot snapshot() const;
    void restore(const Snapshot &saved); // replaces the engine book wholesale

    // The "event" id counter's next-to-issue value (the save's ids.event key;
    // the cheap read the hasher uses instead of a full snapshot copy).
    [[nodiscard]] std::uint64_t peekEventId() const noexcept
    {
        return ids_.peek("event");
    }

private:
    // startEvent — the doc's verbatim body: state init from the template,
    // empty participant books, entry actions, EventStarted (after). The
    // event is already LIVE in the store under `eventId` (the entry actions
    // may end it — the store move must be reachable).
    void startEvent(std::uint64_t eventId, Event &ev, const EventTemplate &tpl, WorldContext &ctx);

    // The id accompanies every helper: the doc Event carries no id field
    // (the store's key is the id), and the engine-side side maps key by it.
    void runActions(std::uint64_t eventId, Event &ev, const std::vector<Action> &actions, WorldContext &ctx);
    void runAction(std::uint64_t eventId, Event &ev, const Action &action, WorldContext &ctx);
    void evaluateTransitions(std::uint64_t eventId, Event &ev, const EventTemplate &tpl, WorldContext &ctx);
    bool conditionHolds(const Event &ev, const Condition &c, WorldContext &ctx) const;
    void forceExit(std::uint64_t eventId, Event &ev, const EventTemplate &tpl, WorldContext &ctx);

    // 5.5's transient-targets split: the event's spawned targets despawn
    // (the D-FLAGS-1 reading — "removed at endEvent"); the template's
    // explicit DespawnTargets action shares this helper (idempotent). The
    // contribution books are NOT retroactively stripped (the amounts were
    // already validated) — only the actors leave.
    void despawnSpawnedTargets(std::uint64_t eventId, Event &ev, WorldContext &ctx);

    const EventTemplateTable *templates_ = nullptr;
    WorldContext *ctx_                   = nullptr;                    // the IEventSpawner seam's context
    std::map<std::uint64_t, Event> active_;                            // R7: event-id ascending
    std::map<std::uint64_t, Event> ended_;                             // retained until sweepEnded
    std::map<std::uint64_t, std::set<characterId>> leftEarly_;         // per event
    std::map<std::uint64_t, std::vector<characterId>> spawnedTargets_; // per event (despawn)
    std::map<std::uint64_t, std::vector<std::uint32_t>> grantedTiers_; // 5.4's seam
    std::map<std::uint64_t, ManhuntSeed> seeds_;                       // 5.4's completion poll (the warrant seed)
    std::map<std::uint64_t, std::array<std::set<characterId>, 2>> targetGroups_; // 6.2's D-WAR-EVT-1 book (world state;
                                                                                 // 7.1 serializes it with the books)
    std::map<regionId, std::map<goodId, std::pair<std::uint32_t, std::uint64_t>>>
        priceBias_;                                                         // region -> good -> (pct, until)
    std::map<regionId, std::pair<std::string, std::uint64_t>> weatherBias_; // region -> (state, until)
    IdGenerator ids_;                                                       // the "event" id space (R10)
};

// Step 5 (Events): the engine's tick (§4.4 — the event engine owns the step).
class EventTickSystem final : public IStepSystem
{
public:
    EventEngine *engine = nullptr;
    void run(
        WorldContext &ctx
    ) override
    {
        if (engine != nullptr)
        {
            engine->tick(ctx);
        }
    }
};

// Step 1's event slice: the ended-event cleanup (§4.4 names event cleanup
// on the Sweeps row, 5.1).
class EventSweepSystem final : public IStepSystem
{
public:
    EventEngine *engine = nullptr;
    void run(
        WorldContext &ctx
    ) override
    {
        if (engine != nullptr)
        {
            engine->sweepEnded(ctx);
        }
    }
};

} // namespace harrowmere::events

#endif // HARROWMERE_CORE_EVENTS_ENGINE_HPP
