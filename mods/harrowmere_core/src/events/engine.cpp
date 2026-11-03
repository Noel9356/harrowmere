// engine.cpp — task 5.1's engine implementation (see engine.hpp for the
// pinned lifecycle + the as-built state-exit reading). Determinism: events
// iterate id-ascending (R7), participants stay sorted, transitions evaluate
// in declared order, and all rolls draw the pinned "events" stream (R6).

#include "events/engine.hpp"

#include <algorithm>

#include "character/character.hpp"
#include "character/store.hpp"
#include "core/constants.hpp"        // MAX_EVENT_DEPTH (A-103), WARRANT_DURATION (A-74)
#include "core/constants_phase2.hpp" // YEAR_GAME_SECONDS
#include "core/game_events.hpp"      // EventStarted/EventEnded/EventStateChanged
#include "core/world_context.hpp"    // the stores + the sim-event seam
#include "events/difficulty.hpp"     // 5.2: the per-attempt difficulty hook (D-DIFF-1)
#include "events/rewards.hpp"        // 5.4: the payout point (rewards pay BEFORE endEvent)
#include "economy/economy.hpp"       // goodIdForKey (the PriceBias action's good read)
#include "justice/enforcement.hpp"   // EvacuateTerritory's mechanic (4.6's evacuation)
#include "justice/justice_store.hpp" // 5.4's completion poll reads the active-warrant set
#include "justice/warrants.hpp"      // the complete Warrant record (the poll's map read)
#include "npc/roles.hpp"             // GarrisonTerritory's assignRole (3.6)
#include "npc/state.hpp"
#include "world/region.hpp" // regionCenter (the spawn anchor)
#include "world/spatial_grid.hpp"
#include "world/world.hpp"

namespace harrowmere::events
{

namespace
{

constexpr const char *kEventRngStream = "events";

void publish(
    WorldContext &ctx, const EventStarted &ev
)
{
    if (ctx.simEvents != nullptr)
    {
        ctx.simEvents->onEventStarted(ev);
    }
}

void publish(
    WorldContext &ctx, const EventEnded &ev
)
{
    if (ctx.simEvents != nullptr)
    {
        ctx.simEvents->onEventEnded(ev);
    }
}

void publish(
    WorldContext &ctx, const EventStateChanged &ev
)
{
    if (ctx.simEvents != nullptr)
    {
        ctx.simEvents->onEventStateChanged(ev);
    }
}

} // namespace

// ---- the seams -----------------------------------------------------------------

bool EventEngine::spawnEvent(
    const char *templateKey, const ManhuntSeed &seed
)
{
    // The IEventSpawner seam arrives context-free; the engine reads the
    // world through the bound context (the JusticeSystem binding pattern).
    if (ctx_ == nullptr)
    {
        return false;
    }
    return startEventByKey(templateKey, seed.region, 0, &seed, *ctx_) != 0;
}

bool EventEngine::spawnWarBattle(
    const WarBattleSeed &seed
)
{
    // 6.2's battle spawn (D-WAR-EVT-1). The BATTLE template declares
    // special.dualTargetGroups; the war module hands over the two member
    // lists (faction-blind engine, D-SINK-1). One live BATTLE per contested
    // region — the scale-uniqueness gate inside startEventByKey is the
    // throttle, the template's maxDurationDays the cadence.
    if (ctx_ == nullptr)
    {
        return false;
    }
    const std::uint64_t id = startEventByKey("BATTLE", seed.region, 0, nullptr, *ctx_);
    if (id == 0)
    {
        return false; // unknown template or a battle already stands there
    }
    Event *ev = findMutable(id);
    if (ev == nullptr)
    {
        return false; // the entry actions ended it (a data-authored instant exit)
    }
    std::array<std::set<characterId>, 2> &groups = targetGroups_[id];
    for (const characterId member : seed.attackerMembers)
    {
        groups[0].insert(member);     // R7: the seed arrives id-ascending
        ev->targetIds.insert(member); // each side is the other's targets
        join(member, id, *ctx_);
    }
    for (const characterId member : seed.defenderMembers)
    {
        groups[1].insert(member);
        ev->targetIds.insert(member);
        join(member, id, *ctx_);
    }
    return true;
}

const std::set<characterId> *EventEngine::targetGroup(
    std::uint64_t eventId, std::uint8_t group
) const
{
    if (group >= 2)
    {
        return nullptr;
    }
    const auto it = targetGroups_.find(eventId);
    return it == targetGroups_.end() ? nullptr : &it->second[group];
}

bool EventEngine::isTargetSideParticipant(
    characterId culprit, std::uint64_t eventId
)
{
    const Event *ev = find(eventId);
    return ev != nullptr && ev->targetIds.count(culprit) != 0;
}

void EventEngine::excludeFromRewards(
    characterId who, std::uint64_t eventId
)
{
    Event *ev = findMutable(eventId);
    if (ev != nullptr)
    {
        ev->excludedFromRewards.insert(who);
    }
}

// ---- the lifecycle ----------------------------------------------------------------

std::uint64_t EventEngine::startEventByKey(
    const std::string &templateKey, regionId region, std::uint8_t depth, const ManhuntSeed *seed, WorldContext &ctx
)
{
    if (templates_ == nullptr)
    {
        return 0;
    }
    const EventTemplate *tpl = templates_->byKey(templateKey);
    if (tpl == nullptr)
    {
        return 0;
    }

    // Scale uniqueness (the doc rule): at most ONE global event active;
    // regional events coexist iff regionId differs — same-region refuse
    // ("may coexist if they do not overlap"). A NESTED spawn (depth > 0)
    // is the doc's own "events can spawn smaller events" — the child shares
    // its parent's region by design and never trips the overlap refusal;
    // the one-global rule stays absolute at every depth.
    for (const auto &[id, active] : active_)
    {
        const EventTemplate *activeTpl = templates_->byId(active.templateId);
        const bool activeGlobal        = activeTpl != nullptr && activeTpl->global;
        if (tpl->global && activeGlobal)
        {
            return 0; // one global at a time
        }
        if (depth == 0 && !tpl->global && !activeGlobal && active.region == region)
        {
            return 0; // same-region refuse (independent regional events overlap)
        }
    }

    // The event goes live FIRST (the entry actions may end it — the store
    // move must be reachable from finishEvent).
    const std::uint64_t id = ids_.nextEvent();
    Event &ev              = active_[id];
    ev.templateId          = tpl->id;
    ev.region              = tpl->global ? 0 : region; // 0 = global-scale (the doc rule)
    ev.depth               = depth;
    ev.startedAt           = ctx.now;
    ev.lastTransitionAt    = ctx.now;

    startEvent(id, ev, *tpl, ctx);

    // The warrant seed's target bookkeeping (5.4's targetFromWarrant; the
    // §4.6 worked trace: "the player is inserted into targetIds"). The entry
    // actions may have ended the event — re-look-up. The seed rides the
    // engine-side book (5.4's completion poll reads it every tick).
    if (seed != nullptr)
    {
        seeds_[id] = *seed;
        if (tpl->specialTargetFromWarrant && seed->culprit != 0)
        {
            Event *live = findMutable(id);
            if (live != nullptr)
            {
                live->targetIds.insert(seed->culprit);
                if (tpl->specialCulpritEarnsNothing)
                {
                    live->excludedFromRewards.insert(seed->culprit);
                }
                // 6.5 consumer 2 — the manhunt's difficulty normalizes
                // toward the culprit (guards always join, volunteers by
                // score proximity) so the hunt resolves at plausible odds
                // rather than a fixed mob size. The null sink keeps the
                // template difficulty (D-SINK-1).
                if (ctx.matchmaking != nullptr)
                {
                    live->currentState.difficulty
                        = ctx.matchmaking->normalizedManhuntDifficulty(seed->culprit, tpl->initialDifficulty);
                }
            }
        }
    }
    return id;
}

void EventEngine::startEvent(
    std::uint64_t eventId, Event &ev, const EventTemplate &tpl, WorldContext &ctx
)
{
    // startEvent, design reference verbatim: all four EventState fields from
    // the template (the doc comment pins them); participants/actionAmounts/
    // hpLost start empty (they do — the Event was value-initialized).
    const TemplateState *initial = tpl.state(tpl.initialStateId);
    ev.currentState              = EventState{tpl.initialStateId, tpl.initialDifficulty, 0, tpl.initialSuccessRate, {}};
    if (initial != nullptr)
    {
        runActions(eventId, ev, initial->entryActions, ctx);
    }
    publish(ctx, EventStarted{eventId, ev.templateId, ev.region});
}

void EventEngine::join(
    characterId who, std::uint64_t eventId, WorldContext &ctx
)
{
    Event *ev = findMutable(eventId);
    if (ev == nullptr || ctx.characters == nullptr || !ctx.characters->exists(who))
    {
        return;
    }
    const bool firstParticipant = ev->participants.empty();
    const auto it               = std::lower_bound(ev->participants.begin(), ev->participants.end(), who);
    if (it == ev->participants.end() || *it != who)
    {
        ev->participants.insert(it, who); // sorted-unique (R7)
    }
    ctx.characters->get(who).eventId = eventId;

    // 6.5 consumer 3 — the all-NPC difficulty calibration. The doc pins the
    // read "at startEvent, if all current participants are NPCs", but the
    // doc's own startEvent comment starts the participants EMPTY; the first
    // moment an all-NPC set can exist is the first join (the as-built
    // adaptation, DEVIATIONS row). It runs exactly once: an NPC first joiner
    // seeds the difficulty through the matchmaking sink; a player first
    // joiner makes "all current participants are NPCs" permanently false.
    // The sink reads the scores itself (the engine never touches the power
    // pipeline); the null default keeps the template difficulty (D-SINK-1).
    if (firstParticipant && ctx.matchmaking != nullptr && !ctx.characters->isPlayer(who))
    {
        ev->currentState.difficulty
            = ctx.matchmaking->calibratedEventDifficulty(ev->participants, ev->currentState.difficulty);
    }
}

void EventEngine::leaveEarly(
    characterId who, std::uint64_t eventId, WorldContext &ctx
)
{
    Event *ev = findMutable(eventId);
    if (ev == nullptr || ctx.characters == nullptr)
    {
        return;
    }
    // The doc rule: the id STAYS in participants (end-of-event rewards still
    // pay for work done) and the eventId is NOT cleared (the actor is still
    // "in an event" until the event ends — the card pitfall, test-pinned).
    // Only the leave mark lands; 5.3's validateActionAmount refuses later
    // contributions through hasLeftEarly.
    leftEarly_[eventId].insert(who);
    (void)ctx;
}

bool EventEngine::hasLeftEarly(
    std::uint64_t eventId, characterId who
) const
{
    const auto it = leftEarly_.find(eventId);
    return it != leftEarly_.end() && it->second.count(who) != 0;
}

void EventEngine::finishEvent(
    std::uint64_t eventId, bool success, WorldContext &ctx
)
{
    const auto it = active_.find(eventId);
    if (it == active_.end())
    {
        return; // already finished (the EndEvent action is idempotent)
    }
    Event &ev = it->second;
    // 5.4's payout point: rewards pay BEFORE endEvent's clears (the doc
    // order; the payout reads the contribution books + the difficulty-scaled
    // pool). A rewardless template pays nothing (a no-op, not an error).
    if (templates_ != nullptr)
    {
        if (const EventTemplate *tpl = templates_->byId(ev.templateId))
        {
            payEventRewards(ev, *tpl, ctx);
        }
    }
    ev.actionAmounts.clear();
    ev.hpLost.clear();
    ev.currentState.powerSnapshots.clear();
    for (const characterId a : ev.participants)
    {
        if (ctx.characters != nullptr && ctx.characters->exists(a))
        {
            ctx.characters->get(a).eventId = 0; // EVERY participant, leavers included
        }
    }
    // 5.5's transient split: the event's spawned targets despawn at
    // endEvent (the D-FLAGS-1 reading); the contributions stay (they were
    // already validated — only the actors leave).
    despawnSpawnedTargets(eventId, ev, ctx);
    publish(ctx, EventEnded{eventId, success});
    ended_.emplace(eventId, std::move(ev));
    active_.erase(it);
    leftEarly_.erase(eventId);
    seeds_.erase(eventId);
}

// ---- the tick -----------------------------------------------------------------

void EventEngine::tick(
    WorldContext &ctx
)
{
    if (templates_ == nullptr)
    {
        return;
    }
    // Deterministic walk: event-id ascending (R7). An event may finish
    // mid-tick (its own transitions) — re-look-up per event.
    std::vector<std::uint64_t> ids;
    ids.reserve(active_.size());
    for (const auto &[id, ev] : active_)
    {
        ids.push_back(id);
    }
    for (const std::uint64_t id : ids)
    {
        const auto it = active_.find(id);
        if (it == active_.end())
        {
            continue; // ended during this tick (a nested event's cascade)
        }
        const EventTemplate *tpl = templates_->byId(it->second.templateId);
        if (tpl == nullptr)
        {
            continue;
        }
        // 5.4's completion poll: a manhunt whose seed warrant left the
        // justice store's active set (convicted / paid / surrendered /
        // expired — the justice side's WarrantResolved facts) ends at the
        // same tick ("any manhunt ends"); the state's exits give their
        // verdict (the forceExit path — the template's EndEvent exits end
        // it successfully).
        if (tpl->specialTargetFromWarrant)
        {
            const auto seedIt = seeds_.find(id);
            if (seedIt != seeds_.end() && seedIt->second.warrantId != 0 && ctx.justiceStore != nullptr
                && ctx.justiceStore->warrants.find(seedIt->second.warrantId) == ctx.justiceStore->warrants.end())
            {
                forceExit(id, it->second, *tpl, ctx);
                if (active_.find(id) == active_.end())
                {
                    continue; // the exits ended it
                }
            }
        }
        // The state's own transitions evaluate FIRST (the template's timer
        // resolves the event on its own terms — bandit_raid's TimerCondition
        // carries the author's success verdict at the same instant the
        // backstop would); maxDurationDays is the backstop for the states
        // whose transitions never fire (the doc's "events eventually return
        // the world to its default state").
        evaluateTransitions(id, it->second, *tpl, ctx);
        if (active_.find(id) == active_.end())
        {
            continue; // the transitions resolved it
        }
        if (tpl->maxDurationDays > 0
            && ctx.now - it->second.startedAt
                   >= static_cast<std::uint64_t>(tpl->maxDurationDays) * core::DAY_LENGTH_SECONDS)
        {
            forceExit(id, it->second, *tpl, ctx);
        }
    }
}

void EventEngine::sweepEnded(
    WorldContext &
)
{
    ended_.clear();
    spawnedTargets_.clear();
    grantedTiers_.clear();
    seeds_.clear();
    targetGroups_.clear(); // 6.2's D-WAR-EVT-1 book retires with its events
}

// ---- the internals ---------------------------------------------------------------

void EventEngine::forceExit(
    std::uint64_t eventId, Event &ev, const EventTemplate &tpl, WorldContext &ctx
)
{
    // 5.2's cadence: the forced exit concludes the state's attempt with the
    // failure verdict (binary for objective-less states; the objective
    // amounts read continuous — D-DIFF-1).
    noteStateAttempt(ev, tpl, /*success=*/false);
    // The forced exit gives the current state's exit actions their say (the
    // author's cleanup + verdict — MANHUNT's exits end it successfully); an
    // event still standing after them resolves as failed.
    const TemplateState *state = tpl.state(ev.currentState.stateId);
    if (state != nullptr)
    {
        runActions(eventId, ev, state->exitActions, ctx);
    }
    finishEvent(eventId, false, ctx); // a no-op when the exits already finished it
}

void EventEngine::evaluateTransitions(
    std::uint64_t eventId, Event &ev, const EventTemplate &tpl, WorldContext &ctx
)
{
    const TemplateState *state = tpl.state(ev.currentState.stateId);
    if (state == nullptr)
    {
        return;
    }
    // Transitions evaluate in DECLARED order; the first whose conditions
    // fire holds (determinism — the card's rule). Conditions are ANY-of: B.6
    // pairs ActionThreshold and TimerCondition on one transition and its own
    // note says the Timer ALONE "guarantees the no-player resolution path"
    // — conjunction would break that guarantee. One transition per tick.
    for (const Transition &t : state->transitions)
    {
        bool any = false;
        for (const Condition &c : t.conditions)
        {
            if (conditionHolds(ev, c, ctx))
            {
                any = true;
                break;
            }
        }
        if (!any)
        {
            continue;
        }
        // Fire: the 5.2 difficulty update (the attempt concludes when the
        // transition fires — D-DIFF-1's one update per attempt; the success
        // verdict is the declared transition having fired), then the old
        // state's exit actions, then the onFire action, then the state move
        // + the new state's entry actions + the publication. The sequence
        // stops the moment the event ends.
        noteStateAttempt(ev, tpl, /*success=*/true);
        runActions(eventId, ev, state->exitActions, ctx);
        Event *live = findMutable(eventId);
        if (live == nullptr)
        {
            return; // the exits ended the event (an exitActions EndEvent)
        }
        if (t.hasOnFire)
        {
            runAction(eventId, *live, t.onFire, ctx);
            live = findMutable(eventId);
            if (live == nullptr)
            {
                return; // the onFire EndEvent
            }
        }
        live->currentState.stateId = t.to;
        live->lastTransitionAt     = ctx.now;
        const TemplateState *next  = tpl.state(t.to);
        if (next != nullptr)
        {
            runActions(eventId, *live, next->entryActions, ctx);
        }
        publish(ctx, EventStateChanged{eventId, state->stateId, t.to});
        return;
    }
}

bool EventEngine::conditionHolds(
    const Event &ev, const Condition &c, WorldContext &ctx
) const
{
    switch (c.kind)
    {
    case ConditionKind::Random:
        return ctx.rng != nullptr && ctx.rng->pct(kEventRngStream) < c.pct;
    case ConditionKind::Flag: {
        if (ctx.worldStore == nullptr || ctx.data == nullptr)
        {
            return false;
        }
        const world::Region *region = ctx.worldStore->region(ev.region);
        if (region == nullptr)
        {
            return false;
        }
        const auto &flagTable = ctx.data->territoryTemplates().regionFlags();
        const auto bit        = flagTable.find(c.regionFlag);
        if (bit == flagTable.end())
        {
            return false; // undeclared flag: never holds (deterministic)
        }
        const bool isSet = (region->flags & bit->second) != 0;
        return isSet == c.set;
    }
    case ConditionKind::ActionThreshold: {
        std::uint64_t total    = 0;
        const std::size_t slot = static_cast<std::size_t>(c.actionType);
        for (const characterId p : ev.participants)
        {
            const auto it = ev.actionAmounts.find(p);
            if (it != ev.actionAmounts.end())
            {
                total += it->second[slot];
            }
        }
        return total >= c.total;
    }
    case ConditionKind::EventState: {
        // Another live event's state (by template key).
        for (const auto &[id, other] : active_)
        {
            if (other.templateId == ev.templateId)
            {
                continue;
            }
            const EventTemplate *otherTpl = templates_ != nullptr ? templates_->byId(other.templateId) : nullptr;
            if (otherTpl != nullptr && otherTpl->key == c.templateKey && other.currentState.stateId == c.stateId)
            {
                return true;
            }
        }
        return false;
    }
    case ConditionKind::Timer:
        return ctx.now >= ev.lastTransitionAt + static_cast<std::uint64_t>(c.days) * core::DAY_LENGTH_SECONDS;
    case ConditionKind::MinParticipants:
        return ev.participants.size() >= c.n;
    case ConditionKind::TargetDead: {
        if (ev.targetIds.empty())
        {
            return false;
        }
        for (const characterId target : ev.targetIds)
        {
            if (ctx.characters != nullptr && ctx.characters->exists(target))
            {
                return false; // still alive (dead characters leave the store, 3.3)
            }
        }
        return true;
    }
    case ConditionKind::TargetCaptured: {
        if (ev.targetIds.empty() || ctx.justice == nullptr)
        {
            return false;
        }
        for (const characterId target : ev.targetIds)
        {
            if (ctx.justice->isArrested(target))
            {
                return true;
            }
        }
        return false;
    }
    }
    return false;
}

void EventEngine::runActions(
    std::uint64_t eventId, Event &ev, const std::vector<Action> &actions, WorldContext &ctx
)
{
    for (const Action &action : actions)
    {
        runAction(eventId, ev, action, ctx);
    }
}

void EventEngine::runAction(
    std::uint64_t eventId, Event &ev, const Action &action, WorldContext &ctx
)
{
    switch (action.kind)
    {
    case ActionKind::SpawnTargets: {
        assert(
            ctx.characters != nullptr && ctx.npcs != nullptr && ctx.data != nullptr && ctx.worldStore != nullptr
            && ctx.grid != nullptr && "SpawnTargets: unbound context"
        );
        if (ctx.characters == nullptr || ctx.npcs == nullptr || ctx.data == nullptr || ctx.worldStore == nullptr
            || ctx.grid == nullptr)
        {
            return;
        }
        const data::SpeciesData *species = ctx.data->speciesByKey(action.speciesKey);
        if (species == nullptr)
        {
            return; // unknown species row: the action resolves to nothing (deterministic)
        }
        const territoryId home = ctx.worldStore->territoryOfRegion(ev.region);
        const Position at      = world::regionCenter(ev.region);
        for (std::uint32_t i = 0; i < action.count; ++i)
        {
            NpcSeed seed;
            seed.species         = species->id;
            seed.homeTerritory   = home;
            seed.homeRegion      = ev.region;
            seed.roleKey         = action.roleKey;
            seed.ageYears        = ADULT_YEARS; // an adult target (A-44; the child gate never bites)
            seed.givenName       = action.roleKey.empty() ? "target" : action.roleKey;
            const characterId id = ctx.characters->createNpc(seed);
            if (id == 0)
            {
                continue;
            }
            NpcState &npc      = ctx.npcs->create(id);
            npc.name           = seed.givenName;
            npc.birthWorldTime = ctx.now - static_cast<std::uint64_t>(seed.ageYears) * YEAR_GAME_SECONDS;
            npc.roleKey        = seed.roleKey;
            npc.homeTerritory  = home;
            npc.currentRegion  = ev.region;
            npc.tier           = FAR;
            Character &c       = ctx.characters->get(id);
            c.body.position    = at;
            ctx.grid->insert(id, at);
            ev.targetIds.insert(id);
            spawnedTargets_[eventId].push_back(id);
        }
        break;
    }
    case ActionKind::DespawnTargets:
        despawnSpawnedTargets(eventId, ev, ctx);
        break;
    case ActionKind::SetRegionFlag: {
        assert(ctx.worldStore != nullptr && ctx.data != nullptr && "SetRegionFlag: unbound context");
        if (ctx.worldStore == nullptr || ctx.data == nullptr)
        {
            return;
        }
        world::Region *region = ctx.worldStore->region(ev.region);
        if (region == nullptr)
        {
            return;
        }
        const auto &flagTable = ctx.data->territoryTemplates().regionFlags();
        const auto bit        = flagTable.find(action.flag);
        if (bit == flagTable.end())
        {
            return; // undeclared flag: a no-op (the data declares the bits)
        }
        if (action.set)
        {
            region->flags |= bit->second;
        } else
        {
            region->flags &= ~bit->second;
        }
        break;
    }
    case ActionKind::PriceBias: {
        if (ctx.economy == nullptr)
        {
            return;
        }
        const goodId good = ctx.economy->goodIdForKey(action.good.c_str());
        if (good == 0)
        {
            return; // unknown good: a no-op (deterministic)
        }
        priceBias_[ev.region][good]
            = {action.pct, ctx.now + static_cast<std::uint64_t>(action.days) * core::DAY_LENGTH_SECONDS};
        break;
    }
    case ActionKind::WeatherBias:
        weatherBias_[ev.region]
            = {action.state, ctx.now + static_cast<std::uint64_t>(action.days) * core::DAY_LENGTH_SECONDS};
        break;
    case ActionKind::SpawnNestedEvent: {
        // depth = parent.depth + 1, refused beyond MAX_EVENT_DEPTH (A-103:
        // an event may spawn a child — depth 1; no grandchildren — the
        // depth-2 spawn refuses).
        const std::uint16_t childDepth = static_cast<std::uint16_t>(ev.depth) + 1;
        if (childDepth >= MAX_EVENT_DEPTH)
        {
            break;
        }
        startEventByKey(action.templateKey, ev.region, static_cast<std::uint8_t>(childDepth), nullptr, ctx);
        break;
    }
    case ActionKind::GrantRewardTier:
        grantedTiers_[eventId].push_back(action.tier); // 5.4's payout reads these
        break;
    case ActionKind::GarrisonTerritory: {
        // 5.5's reconquest exit (the D-ACTION-1++ row): the event's NPC
        // participants take the data-named guard role (3.6's assignRole)
        // in the event's territory and the lawless flag clears — the doc's
        // NPC-reconquest path out of lawlessness. Participants iterate in
        // sorted order (R7); players and missing characters skip.
        if (ctx.worldStore == nullptr || ctx.npcs == nullptr || ctx.characters == nullptr || ctx.rng == nullptr)
        {
            return;
        }
        const territoryId terr = ctx.worldStore->territoryOfRegion(ev.region);
        if (terr == 0)
        {
            return;
        }
        for (const characterId p : ev.participants)
        {
            if (!ctx.characters->exists(p) || ctx.characters->isPlayer(p) || !ctx.npcs->contains(p))
            {
                continue;
            }
            assignRole(p, action.roleKey, ctx); // 3.6's path (integrity draws the named "npc_life" stream, R6)
        }
        ctx.worldStore->clearLawless(terr);
        break;
    }
    case ActionKind::EvacuateTerritory: {
        if (ctx.worldStore == nullptr)
        {
            return;
        }
        const territoryId terr = ctx.worldStore->territoryOfRegion(ev.region);
        if (terr != 0)
        {
            // The manhunt's evacuation window rides the warrant duration
            // (A-74) — the enforcement mechanic, the engine's trigger.
            justice::evacuateTerritoryCivilians(terr, ctx.now + WARRANT_DURATION_GAME_S, ctx);
        }
        break;
    }
    case ActionKind::EndEvent:
        finishEvent(eventId, action.success, ctx);
        break;
    }
}

const Event *EventEngine::find(
    std::uint64_t eventId
) const
{
    const auto active = active_.find(eventId);
    if (active != active_.end())
    {
        return &active->second;
    }
    const auto done = ended_.find(eventId);
    return done == ended_.end() ? nullptr : &done->second;
}

Event *EventEngine::findMutable(
    std::uint64_t eventId
)
{
    // Mutable access is an ACTIVE-event surface: an ended event is retained
    // read-only (the observers' record — TestEvent_EndEventClearsMapsAndEventIds
    // pins findMutable == nullptr after the end).
    const auto active = active_.find(eventId);
    return active == active_.end() ? nullptr : &active->second;
}

const Event *EventEngine::findActiveByKey(
    const std::string &templateKey
) const
{
    if (templates_ == nullptr)
    {
        return nullptr;
    }
    for (const auto &[id, ev] : active_)
    {
        const EventTemplate *tpl = templates_->byId(ev.templateId);
        if (tpl != nullptr && tpl->key == templateKey)
        {
            return &ev;
        }
    }
    return nullptr;
}

const std::vector<std::uint32_t> &EventEngine::grantedTiers(
    std::uint64_t eventId
) const
{
    static const std::vector<std::uint32_t> kEmpty;
    const auto it = grantedTiers_.find(eventId);
    return it == grantedTiers_.end() ? kEmpty : it->second;
}

std::uint64_t EventEngine::priceBiasFor(
    regionId region, goodId good, std::uint64_t now
) const
{
    const auto regionIt = priceBias_.find(region);
    if (regionIt == priceBias_.end())
    {
        return SCALE;
    }
    const auto goodIt = regionIt->second.find(good);
    if (goodIt == regionIt->second.end() || goodIt->second.second <= now)
    {
        return SCALE; // no bias, or expired
    }
    return goodIt->second.first;
}

const std::string *EventEngine::weatherBiasFor(
    regionId region, std::uint64_t now
) const
{
    const auto it = weatherBias_.find(region);
    if (it == weatherBias_.end() || it->second.second <= now)
    {
        return nullptr;
    }
    return &it->second.first;
}

std::vector<regionId> EventEngine::activeWeatherBiasRegions(
    std::uint64_t now
) const
{
    std::vector<regionId> out;
    for (const auto &[region, bias] : weatherBias_) // region-ascending (R7)
    {
        if (bias.second > now)
        {
            out.push_back(region);
        }
    }
    return out;
}

const ManhuntSeed *EventEngine::seedOf(
    std::uint64_t eventId
) const
{
    const auto it = seeds_.find(eventId);
    return it == seeds_.end() ? nullptr : &it->second;
}

void EventEngine::despawnSpawnedTargets(
    std::uint64_t eventId, Event &ev, WorldContext &ctx
)
{
    assert(
        ctx.characters != nullptr && ctx.npcs != nullptr && ctx.grid != nullptr && "DespawnTargets: unbound context"
    );
    if (ctx.characters == nullptr || ctx.npcs == nullptr || ctx.grid == nullptr)
    {
        return;
    }
    const auto spawned = spawnedTargets_.find(eventId);
    if (spawned == spawnedTargets_.end())
    {
        return; // nothing spawned (or the template's explicit action already despawned)
    }
    for (const characterId id : spawned->second)
    {
        ctx.grid->remove(id);
        ctx.npcs->erase(id);
        ctx.characters->remove(id);
        ev.targetIds.erase(id);
    }
    spawnedTargets_.erase(spawned);
}

// ---- save seam (task 7.1) -------------------------------------------------------

EventEngine::Snapshot EventEngine::snapshot() const
{
    Snapshot out;
    out.active         = active_;
    out.ended          = ended_;
    out.leftEarly      = leftEarly_;
    out.spawnedTargets = spawnedTargets_;
    out.grantedTiers   = grantedTiers_;
    out.seeds          = seeds_;
    out.targetGroups   = targetGroups_;
    out.priceBias      = priceBias_;
    out.weatherBias    = weatherBias_;
    out.idCounters     = ids_.state();
    return out;
}

void EventEngine::restore(
    const Snapshot &saved
)
{
    active_         = saved.active;
    ended_          = saved.ended;
    leftEarly_      = saved.leftEarly;
    spawnedTargets_ = saved.spawnedTargets;
    grantedTiers_   = saved.grantedTiers;
    seeds_          = saved.seeds;
    targetGroups_   = saved.targetGroups;
    priceBias_      = saved.priceBias;
    weatherBias_    = saved.weatherBias;
    ids_.loadState(saved.idCounters); // never rewinds a live generator (R10)
}

} // namespace harrowmere::events
