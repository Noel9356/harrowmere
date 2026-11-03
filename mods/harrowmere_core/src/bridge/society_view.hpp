// society_view.hpp — task 7.3b: the society half of the view builder (the
// tested free functions; rebuildViews is their thin caller — the 2.8
// player_view.hpp pattern).
//
// The fog-of-war is STRUCTURAL here: buildJusticeView reads the live stores
// and fills exactly the published whitelist (reputation per territory, the
// notoriety record, the live warrants against the player with their
// accused-side facts, jail status) — never a case score, never an evidence
// list, never a report weight. TestSocietyView_NoHiddenEvidenceExposed (the
// core-side twin of the UI suite's TestUi_NoHiddenEvidenceExposed) pins the
// whitelist against the LIVE builder: a wanted player's screen shows nothing
// the design does not permit.
//
//   buildTerritoryTree — the Map panel's territory tree (id-ascending, R7;
//     safe/lawless facets + the law-set key of the lawsAt walk; claims ride
//     the claimedByMe facet, 6.3 fills it).
//   buildJusticeView — the whitelist fill (decay-before-read on every read;
//     the warrants' purge-on-read is the doc's mutating read).
//   buildEventsView — the active events with MY contribution and my
//     estimated share (5.3's math; the panel displays, never re-derives) and
//     the join/leave affordances the core marked.

#pragma once

#include <cstdint>
#include <vector>

#include "core/types.hpp"            // characterId
#include "core/world_context.hpp"    // WorldContext (the fill reads through it)
#include "harrowmere_capability.hpp" // the published views (the wire shapes)

namespace harrowmere::events
{
class EventEngine;         // events/engine.hpp — pointer only here
struct EventTemplateTable; // events/template.hpp — pointer only here
} // namespace harrowmere::events

namespace harrowmere::world
{
class WorldStore; // world/world.hpp — pointer only here
}

namespace harrowmere::bridge
{

// The whole territory tree, id-ascending (R7), each node with the facets the
// Map panel renders: the effective law set's key (the lawsAt walk — D-JUR-1's
// one walk), the safe-zone and lawless flags, and the claim facet (6.3's
// fill; false until then).
std::vector<views::TerritorySummary> buildTerritoryTree(const world::WorldStore &world);

// The JusticeView whitelist, filled from the live justice state. All reads
// decay/purge BEFORE they read (the 4.4/4.2 lazy rules — the view never
// shows a stale warrant or a decayed-away grudge). `player` must exist in
// the store (the builder asserts; the plugin only calls it with the real
// player id).
views::JusticeView buildJusticeView(WorldContext &ctx, characterId player);

// The EventsView: every ACTIVE event (id-ascending), the player's
// contribution + estimated reward share (5.3's functions — the core derives,
// the panel displays), the join affordance (not a participant and not the
// event's target — a hunted player cannot join their own manhunt), the leave
// affordance (a participant; leaveEarly keeps the reward share for work
// done). Requires the engine + its template table bound.
views::EventsView
buildEventsView(const events::EventEngine &engine, const events::EventTemplateTable &templates, characterId player);

} // namespace harrowmere::bridge
