// consent.cpp — duels & arenas (task 2.7). See consent.hpp for the pinned
// semantics; this file is the bookkeeping plus the pinned
// broadcast -> enable-damage order.

#include "combat/consent.hpp"

#include <cassert>

#include "character/store.hpp"          // CharacterStore (the field writes)
#include "data/registry.hpp"            // DataRegistry::territoryTemplate(s)
#include "data/territory_templates.hpp" // TerritoryTypeTemplate (arena flag + radius)
#include "world/region.hpp"             // regionCenter, distanceSquaredM (D-WORLD-1)
#include "world/spatial_grid.hpp"       // idsWithinRadius (A-26's gather)
#include "world/world.hpp"              // WorldStore (territory graph)

namespace harrowmere::combat
{

// ---- ConsentRegistry ----------------------------------------------------------------

void ConsentRegistry::request(
    characterId challenger, characterId target, std::uint64_t now
)
{
    pending_[target][challenger] = now; // replace-or-insert, deterministic either way
}

void ConsentRegistry::cancelRequestsInvolving(
    characterId id
)
{
    // As the target: the whole row goes.
    pending_.erase(id);
    // As the challenger: every target's row drops the id.
    for (auto it = pending_.begin(); it != pending_.end();)
    {
        it->second.erase(id);
        if (it->second.empty())
            it = pending_.erase(it);
        else
            ++it;
    }
}

void ConsentRegistry::cancelRequest(
    characterId challenger, characterId target
)
{
    const auto row = pending_.find(target);
    if (row == pending_.end())
        return;
    row->second.erase(challenger);
    if (row->second.empty())
        pending_.erase(row);
}

bool ConsentRegistry::hasRequest(
    characterId challenger, characterId target
) const
{
    const auto row = pending_.find(target);
    return row != pending_.end() && row->second.count(challenger) > 0;
}

std::size_t ConsentRegistry::countPending_() const noexcept
{
    std::size_t n = 0;
    for (const auto &[target, challengers] : pending_)
        n += challengers.size();
    return n;
}

const ActiveDuel *ConsentRegistry::duelOf(
    characterId id
) const
{
    // Either side finds the duel; the record sits under the lower id.
    const auto it = duels_.find(id);
    if (it != duels_.end())
        return &it->second;
    for (const auto &[key, duel] : duels_)
    {
        (void)key;
        if (duel.b == id)
            return &duel;
    }
    return nullptr;
}

void ConsentRegistry::noteDuelDamage(
    characterId id, std::uint64_t now
)
{
    // Mutating through the lower-id key; the higher side resolves by scan
    // (duels are a handful at any time; the card pins no index requirement).
    const auto it = duels_.find(id);
    if (it != duels_.end())
    {
        it->second.lastDamage = now;
        return;
    }
    for (auto &[key, duel] : duels_)
    {
        (void)key;
        if (duel.b == id)
        {
            duel.lastDamage = now;
            return;
        }
    }
}

void ConsentRegistry::insertDuel(
    characterId a, characterId b, std::uint64_t now
)
{
    const characterId low  = a < b ? a : b;
    const characterId high = a < b ? b : a;
    const auto inserted    = duels_.emplace(low, ActiveDuel{low, high, now, now});
    assert(inserted.second && "insertDuel: a duel already stands under this key (programmer error)");
    if (!inserted.second)
        inserted.first->second = ActiveDuel{low, high, now, now}; // deterministic release recovery
}

bool ConsentRegistry::removeDuel(
    characterId id, ActiveDuel &removed
)
{
    const auto it = duels_.find(id);
    if (it != duels_.end())
    {
        removed = it->second;
        duels_.erase(it);
        return true;
    }
    for (auto it2 = duels_.begin(); it2 != duels_.end(); ++it2)
    {
        if (it2->second.b == id)
        {
            removed = it2->second;
            duels_.erase(it2);
            return true;
        }
    }
    return false;
}

// ---- internal: the shared end path ---------------------------------------------------

namespace
{

// The midpoint of two characters' positions (y cosmetic — 0).
Position midpointOf(
    const Character &a, const Character &b
)
{
    return Position{(a.body.position.x + b.body.position.x) / 2.0, 0.0, (a.body.position.z + b.body.position.z) / 2.0};
}

// End one duel: clear both consent fields, drop the record, broadcast the
// close (observers re-gathered at the CURRENT midpoint). The broadcast
// happens after the fields clear (0.7's post-state rule — a handler must
// read the truth: the duel is over).
void endDuelImpl(
    const ActiveDuel &duel,
    ConsentRegistry &duels,
    character::CharacterStore &characters,
    const world::SpatialGrid &grid,
    IDuelBroadcast *broadcast,
    std::uint64_t now
)
{
    ActiveDuel removed{};
    if (!duels.removeDuel(duel.a, removed))
        return; // already gone (a double end is harmless, deterministic)

    if (characters.exists(duel.a))
        characters.get(duel.a).duelOpponent = 0;
    if (characters.exists(duel.b))
        characters.get(duel.b).duelOpponent = 0;

    if (broadcast != nullptr)
    {
        // Re-gather the people standing at the midpoint NOW — the ones still
        // watching are the ones the close reaches. A character record that
        // already left the store (the death path may have removed it)
        // gathers nothing: an empty observer list closes no window that a
        // dead witness could have held open.
        std::vector<characterId> observers;
        if (characters.exists(duel.a) && characters.exists(duel.b))
            observers
                = grid.idsWithinRadius(midpointOf(characters.get(duel.a), characters.get(duel.b)), CONSENT_BROADCAST_M);
        broadcast->duelEnded(duel.a, duel.b, now, observers);
    }
}

} // namespace

// ---- the duel flow -------------------------------------------------------------------

DuelResult requestDuel(
    characterId challenger,
    characterId target,
    ConsentRegistry &duels,
    const character::CharacterStore &characters,
    std::uint64_t now
)
{
    if (!characters.exists(challenger) || !characters.exists(target))
        return DuelResult::err(DuelError::UnknownCharacter);
    if (challenger == target)
        return DuelResult::err(DuelError::SelfDuel);

    // Either side already standing in a duel cannot open a second front.
    if (duels.duelOf(challenger) != nullptr || duels.duelOf(target) != nullptr)
        return DuelResult::err(DuelError::AlreadyDueling);

    duels.request(challenger, target, now);
    return DuelResult::ok();
}

DuelResult acceptDuel(
    characterId target,
    characterId challenger,
    ConsentRegistry &duels,
    character::CharacterStore &characters,
    const world::SpatialGrid &grid,
    IDuelBroadcast *broadcast,
    std::uint64_t now
)
{
    if (!characters.exists(challenger) || !characters.exists(target))
        return DuelResult::err(DuelError::UnknownCharacter);
    if (!duels.hasRequest(challenger, target))
        return DuelResult::err(DuelError::NotRequested);
    if (duels.duelOf(challenger) != nullptr || duels.duelOf(target) != nullptr)
        return DuelResult::err(DuelError::AlreadyDueling);

    // PINNED ORDER — broadcast BEFORE the consent fields flip (the card's
    // pitfall: "Broadcast must reach witnesses before the first blow lands
    // (order: broadcast -> enable damage) or the first hit records
    // evidence"). The observer set is the A-26 radius gather over the
    // midpoint; the callback above can rely on duelOpponent still reading 0.
    if (broadcast != nullptr)
    {
        const std::vector<characterId> observers
            = grid.idsWithinRadius(midpointOf(characters.get(challenger), characters.get(target)), CONSENT_BROADCAST_M);
        broadcast->duelStarted(challenger, target, now, observers);
    }

    // Enable damage: BOTH fields set (2.1's consentsToPvp reads the
    // victim-side field, so each side must name the other).
    duels.cancelRequest(challenger, target);
    characters.get(challenger).duelOpponent = target;
    characters.get(target).duelOpponent     = challenger;
    duels.insertDuel(challenger, target, now);
    return DuelResult::ok();
}

DuelResult yieldDuel(
    characterId yielder,
    ConsentRegistry &duels,
    character::CharacterStore &characters,
    const world::SpatialGrid &grid,
    IDuelBroadcast *broadcast,
    std::uint64_t now
)
{
    const ActiveDuel *duel = duels.duelOf(yielder);
    if (duel == nullptr)
        return DuelResult::err(DuelError::NotDueling);

    endDuelImpl(*duel, duels, characters, grid, broadcast, now);
    return DuelResult::ok();
}

void endDuelOnDeath(
    characterId victim,
    ConsentRegistry &duels,
    character::CharacterStore &characters,
    const world::SpatialGrid &grid,
    IDuelBroadcast *broadcast,
    std::uint64_t now
)
{
    // The dead cannot duel and cannot accept: close the active duel (if
    // any) and every pending request touching the victim.
    duels.cancelRequestsInvolving(victim);
    if (const ActiveDuel *duel = duels.duelOf(victim); duel != nullptr)
        endDuelImpl(*duel, duels, characters, grid, broadcast, now);
}

void sweepDuels(
    ConsentRegistry &duels,
    character::CharacterStore &characters,
    const world::SpatialGrid &grid,
    IDuelBroadcast *broadcast,
    std::uint64_t now
)
{
    // Collect the doomed keys first (mutating mid-iteration is how sweeps
    // corrupt); visit in id-ascending order (R7). Timeout and separation
    // are independent ends — either closes the duel.
    std::vector<ActiveDuel> doomed;
    for (const auto &[key, duel] : duels.duels())
    {
        (void)key;
        // A future lastDamage stamp (a clock anomaly) is NOT idle time —
        // the guarded subtraction reads no underflow (the same rule 2.1's
        // combat-window test pins for lastCombatTime).
        const bool idle = now >= duel.lastDamage && now - duel.lastDamage >= DUEL_TIMEOUT_S;
        const bool apart
            = !characters.exists(duel.a) || !characters.exists(duel.b)
           || world::distanceSquaredM(characters.get(duel.a).body.position, characters.get(duel.b).body.position)
                  > DUEL_MAX_SEPARATION_M * DUEL_MAX_SEPARATION_M;
        if (idle || apart)
            doomed.push_back(duel);
    }
    for (const ActiveDuel &duel : doomed)
        endDuelImpl(duel, duels, characters, grid, broadcast, now);
}

// ---- arenas ---------------------------------------------------------------------------

void ArenaTable::build(
    const data::DataRegistry &data, const world::WorldStore &world
)
{
    zones_.clear();
    for (const auto &[id, territory] : world.territories())
    {
        if (territory.type != TerritoryType::CITY)
            continue; // one arena per CITY in v1 (card 2.7)
        if (territory.region == world::kNoRegion)
            continue; // no anchor, no zone (cannot happen for generated cities)

        // Array-indexed template read (task 1.3): debug-asserts a committed
        // registry, so build() runs after the load stages — the same
        // precondition every template reader carries.
        const data::TerritoryTypeTemplate &tpl = data.territoryTemplate(TerritoryType::CITY);
        if (!tpl.arena || tpl.arenaRadiusM == 0)
            continue; // the data says this city type has no arena

        ArenaZone zone;
        zone.territory = id; // the shared-consent id every occupant will carry
        zone.center    = world::regionCenter(territory.region);
        zone.radiusM   = static_cast<double>(tpl.arenaRadiusM);
        zones_.emplace(id, zone);
    }
}

const ArenaZone *ArenaTable::zoneAt(
    const Position &p
) const
{
    for (const auto &[id, zone] : zones_)
    {
        (void)id;
        // Planar x/z (R5); the squared compare avoids the sqrt (region.hpp's
        // distance primitive's documented form).
        if (world::distanceSquaredM(p, zone.center) <= zone.radiusM * zone.radiusM)
            return &zone;
    }
    return nullptr;
}

const ArenaZone *ArenaTable::zoneOfTerritory(
    territoryId territory
) const
{
    const auto it = zones_.find(territory);
    return it != zones_.end() ? &it->second : nullptr;
}

void refreshArenaPresence(
    Character &c, const ArenaTable &arenas
)
{
    // Entering sets the zone's id; leaving clears to 0 ("0 = not in an
    // arena", the doc field default). Unconditional write both ways: the
    // field is always exactly the truth of the current position.
    if (const ArenaZone *zone = arenas.zoneAt(c.body.position); zone != nullptr)
        c.arenaId = zone->territory;
    else
        c.arenaId = 0;
}

} // namespace harrowmere::combat
