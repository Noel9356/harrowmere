// society_view.cpp — task 7.3b: the society view fills (the header carries
// the fog-of-war contract; this file is the implementation the whitelist test
// exercises against the live stores).

#include "bridge/society_view.hpp"

#include <algorithm>

#include "core/enums.hpp" // TerritoryType (the closed list)
#include "core/world_context.hpp"
#include "events/contribution.hpp" // getContribution / getRewardShare (5.3's reads)
#include "events/engine.hpp"       // the EventEngine's active set
#include "events/template.hpp"     // the template table (keys + contribution weights)
#include "justice/conviction.hpp"  // isArrested (the lazy jail read)
#include "justice/reputation.hpp"  // getNotoriety / getReputationIn / decayIfDue (4.4)
#include "justice/warrants.hpp"    // purgeExpiredWarrants (the mutating read)
#include "world/world.hpp"         // WorldStore (the tree + the lawsAt walk)

namespace harrowmere::bridge
{

namespace
{

// The closed-list mirror (core/enums.hpp's SCREAMING spellings -> the
// published display names; the same mapping the plugin's current-territory
// fill pins). A default arm keeps the closed list's future extensions
// compiling to the least-surprising member.
views::TerritoryType publishTerritoryType(
    TerritoryType type
)
{
    switch (type)
    {
    case TerritoryType::VILLAGE:
        return views::TerritoryType::Village;
    case TerritoryType::CITY:
        return views::TerritoryType::City;
    case TerritoryType::KINGDOM:
        return views::TerritoryType::Kingdom;
    case TerritoryType::EMPIRE:
        return views::TerritoryType::Empire;
    case TerritoryType::COUNT:
        break;
    }
    return views::TerritoryType::Village;
}

} // namespace

std::vector<views::TerritorySummary> buildTerritoryTree(
    const world::WorldStore &world
)
{
    // The store's map is id-ascending (R7) — the tree ships in that order.
    std::vector<views::TerritorySummary> out;
    out.reserve(world.territories().size());
    for (const auto &[id, territory] : world.territories())
    {
        views::TerritorySummary node;
        node.id          = territory.id;
        node.parent      = territory.parent;
        node.type        = publishTerritoryType(territory.type);
        node.safe        = territory.safe;
        node.lawless     = territory.lawless;
        node.lawSetKey   = world.lawsAt(id).key; // the lawsAt walk (D-JUR-1)
        node.claimedByMe = false;                // 6.3's fill (Phase 4) — the honest zero until then
        out.push_back(std::move(node));
    }
    return out;
}

views::JusticeView buildJusticeView(
    WorldContext &ctx, characterId player
)
{
    views::JusticeView view{};
    if (ctx.justiceStore == nullptr)
        return view; // no justice state: the zero view (never a made-up field)

    justice::JusticeStore &store = *ctx.justiceStore;

    // The whitelist, decay-before-read (4.4's lazy rule — every read decays
    // first, the view never shows a stale grudge or a decayed-away record).
    view.notoriety
        = static_cast<std::uint64_t>(std::max<std::int64_t>(0, justice::getNotoriety(store, player, ctx.now)));

    // The per-territory walk flattened (the rows are the territories the
    // reputation walk wrote — "per territory I have stood in"; the map's
    // id-ascending order is the view's order, R7).
    const auto repIt = store.reputations.find(player);
    if (repIt != store.reputations.end())
    {
        justice::decayIfDue(repIt->second, ctx.now);
        // The walk flattened ORIGIN-FIRST (the panel's own note: "origin
        // first, root last (4.4's propagation order)") — the map iterates
        // id-ascending (root side first), so the rows reverse into the
        // propagation order. Deterministic (R7) either way; the reversal
        // matches the Justice panel's shipped wording.
        for (const auto &[territory, value] : repIt->second.perTerritory)
        {
            view.perTerritory.push_back({territory, value});
        }
        std::reverse(view.perTerritory.begin(), view.perTerritory.end());
        if (ctx.world != nullptr)
        {
            const Character &self  = ctx.characters->get(player);
            const territoryId here = ctx.world->territoryAt(self.body.position);
            view.reputation        = justice::getReputationIn(store, player, here, ctx.now);
        }
    }

    // The live warrants against the player (the accused-side view: exactly
    // what 4.6's signals + poster text may say — territory, bounty, expiry,
    // lethality). The purge-on-read IS the doc's mutating read (4.2): the
    // lazy sweep runs before the copy so an expired warrant never shows.
    if (ctx.world != nullptr)
    {
        justice::purgeExpiredWarrants(ctx);
    }
    const auto wantedIt = store.activeWarrantsByCulprit.find(player);
    if (wantedIt != store.activeWarrantsByCulprit.end())
    {
        view.activeWarrantsAgainstYou = static_cast<std::uint32_t>(wantedIt->second.size());
        for (const std::uint64_t warrantId : wantedIt->second) // set order: id-ascending (R7)
        {
            const auto wIt = store.warrants.find(warrantId);
            if (wIt == store.warrants.end())
                continue; // the two maps' lockstep invariant keeps this unreachable
            const justice::Warrant &warrant = wIt->second;
            view.warrants.push_back({warrant.territory, warrant.bounty, warrant.expiresTime, warrant.lethal});
        }
    }

    // The jail status (4.3's lazy read; the jailUntil entry is the sentence's
    // end worldTime exactly when isArrested answers true).
    view.isJailed = justice::isArrested(store, player, ctx.now);
    if (view.isJailed)
    {
        const auto jailIt = store.jailUntil.find(player);
        view.jailUntil    = jailIt != store.jailUntil.end() ? jailIt->second : 0;
    }
    return view;
}

views::EventsView buildEventsView(
    const events::EventEngine &engine, const events::EventTemplateTable &templates, characterId player
)
{
    views::EventsView view;
    for (const auto &[eventId, ev] : engine.active()) // id-ascending (R7)
    {
        const events::EventTemplate *tpl = templates.byId(ev.templateId);
        if (tpl == nullptr)
            continue; // the template table's lockstep invariant; a stray id shows nothing

        views::EventSummary row;
        row.eventId     = eventId;
        row.templateKey = tpl->key;
        row.stateId     = ev.currentState.stateId;
        row.difficulty  = ev.currentState.difficulty;

        const auto participantIt = std::lower_bound(ev.participants.begin(), ev.participants.end(), player);
        row.amParticipant        = participantIt != ev.participants.end() && *participantIt == player;

        // The contribution math is the core's (5.3's normalized, capped,
        // weighted amounts; the share is the getRewardShare estimate) — the
        // panel displays, never re-derives. The share rides the SCALE grid:
        // permille = share / 10 (SCALE 10000 -> 1000 permille).
        row.myContribution  = events::getContribution(ev, player, *tpl);
        row.mySharePermille = static_cast<std::uint32_t>(events::getRewardShare(ev, player, *tpl) / 10);

        // The join affordance: not a participant and not the event's target
        // (a hunted player cannot join their own manhunt — the fog-of-war
        // read the player already has: the hunt's poster says so).
        row.joinable = !row.amParticipant && ev.targetIds.count(player) == 0;

        view.active.push_back(std::move(row));
    }
    return view;
}

} // namespace harrowmere::bridge
