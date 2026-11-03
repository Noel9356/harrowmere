// perception.cpp — the sightings store + the live perceive seam (task 3.9).
// Every rule is card-verbatim; see perception.hpp for the pinned semantics.

#include "npc/perception.hpp"

#include "character/appearance.hpp" // AppearanceHistory (2.6 — the version source)
#include "data/registry.hpp"        // DataRegistry (row reads for qualification callers)

namespace harrowmere::npc
{

// ---- the sightings store -------------------------------------------------------

void PerceptionStore::recordSighting(
    characterId observer, const Sighting &sighting, bool culpritMasked
)
{
    // The MASKED gate (the card's sighting rule): a masked culprit leaves no
    // memory — no new record, no refresh of an existing one.
    if (culpritMasked || sighting.culpritId == 0)
    {
        return; // id 0 is reserved (R10) and never a culprit
    }
    sightings_[observer][sighting.culpritId] = sighting; // upsert: the latest identification
}

const Sighting *PerceptionStore::sightingOf(
    characterId observer, characterId culpritId
) const
{
    const auto observerIt = sightings_.find(observer);
    if (observerIt == sightings_.end())
    {
        return nullptr;
    }
    const auto culpritIt = observerIt->second.find(culpritId);
    return culpritIt == observerIt->second.end() ? nullptr : &culpritIt->second;
}

bool PerceptionStore::knowsIdentity(
    characterId observer,
    const NpcState &observerState,
    characterId culpritId,
    std::uint64_t culpritVersionNow,
    std::uint64_t now
) const
{
    // (a) Acquaintance: affinity >= A-54 — matches by id regardless of the
    // culprit's appearance version and never lapses ("disguises defeat
    // strangers, never acquaintances"). A NEGATIVE affinity is an enemy, not
    // an acquaintance — only the positive threshold knows.
    const auto affinityIt = observerState.affinity.find(culpritId);
    if (affinityIt != observerState.affinity.end() && affinityIt->second >= ACQUAINTANCE_AFFINITY)
    {
        return true;
    }

    // (b) Sighting memory: recorded, appearance-current, not lapsed.
    const Sighting *sighting = sightingOf(observer, culpritId);
    if (sighting == nullptr)
    {
        return false;
    }
    if (sighting->appearanceVersion != culpritVersionNow)
    {
        return false; // the sighting is keyed to a superseded look — the disguise reset
    }
    if (now < sighting->worldTime)
    {
        return true; // a re-record landing "in the past" cannot un-know; the stamp governs forward
    }
    return now - sighting->worldTime <= STRANGER_MEMORY_S; // lapse at strictly-more than A-56
}

const std::map<characterId, Sighting> &PerceptionStore::sightingsOf(
    characterId observer
) const
{
    static const std::map<characterId, Sighting> kEmpty{};
    const auto it = sightings_.find(observer);
    return it == sightings_.end() ? kEmpty : it->second;
}

// ---- the live seam ----------------------------------------------------------------

void PerceptionSink::bind(
    const world::SpatialGrid *grid,
    const character::CharacterStore *characters,
    const NpcStore *npcs,
    const data::DataRegistry *data
)
{
    grid_       = grid;
    characters_ = characters;
    npcs_       = npcs;
    data_       = data;
}

void PerceptionSink::bindAppearanceHistory(
    const character::AppearanceHistory *history
)
{
    history_ = history;
}

std::vector<characterId> PerceptionSink::perceiveWithin(
    const Position &position, double radiusMeters
)
{
    std::vector<characterId> out;
    if (grid_ == nullptr || characters_ == nullptr)
    {
        return out; // unbound seam: the empty answer (D-SINK-1's reading)
    }

    // The grid's radius query is THE one spatial path (1.6's Done-when) and
    // its output is already id-ascending (R7). The spatial half filters to
    // characters that can still perceive (alive — the dead leave the store
    // at 3.3, and a 0-HP record cannot witness); the social half
    // (sapience/age/knowledge) is the caller's qualify step.
    for (characterId id : grid_->idsWithinRadius(position, radiusMeters))
    {
        if (!characters_->exists(id))
        {
            continue;
        }
        if (characters_->get(id).body.currentHp == 0)
        {
            continue;
        }
        out.push_back(id);
    }
    return out;
}

std::uint64_t PerceptionSink::culpritAppearanceVersionNow(
    characterId culprit
) const
{
    // 2.6's change count IS the version (one applied change = one bump); no
    // bound history reads 0 — one permanent look, sightings never stale.
    return history_ == nullptr ? 0 : static_cast<std::uint64_t>(history_->changeCount(culprit));
}

bool PerceptionSink::knowsIdentity(
    characterId observer, const NpcState &observerState, characterId culprit
) const
{
    return store_.knowsIdentity(observer, observerState, culprit, culpritAppearanceVersionNow(culprit), now_);
}

bool PerceptionSink::knowsIdentityNoAffinity(
    characterId observer, characterId culprit
) const
{
    // The player observer: no NpcState, no affinity map (D-STORE-1) — the
    // sighting half only, against the player's own sighting rows.
    const Sighting *sighting = store_.sightingOf(observer, culprit);
    if (sighting == nullptr)
    {
        return false;
    }
    if (sighting->appearanceVersion != culpritAppearanceVersionNow(culprit))
    {
        return false;
    }
    if (now_ < sighting->worldTime)
    {
        return true;
    }
    return now_ - sighting->worldTime <= STRANGER_MEMORY_S;
}

// ---- the seam's identity half (4.1) --------------------------------------------

bool PerceptionSink::knowsIdentity(
    characterId observer, characterId culprit
)
{
    // NPC observers run the full rule (affinity acquaintance + sighting
    // memory); the player — no NpcState exists for the SP player (D-STORE-1)
    // — keeps the sighting-only answer. An unbound store or an observer with
    // no record answers false (the honest zero).
    if (npcs_ != nullptr && npcs_->contains(observer))
    {
        return knowsIdentity(observer, npcs_->get(observer), culprit);
    }
    return knowsIdentityNoAffinity(observer, culprit);
}

std::uint64_t PerceptionSink::lastSightingOf(
    characterId who
)
{
    std::uint64_t freshest = 0;
    for (const auto &[observer, sightings] : store_.sightingsAll())
    {
        const auto it = sightings.find(who);
        if (it != sightings.end() && it->second.worldTime > freshest)
        {
            freshest = it->second.worldTime;
        }
    }
    return freshest;
}

} // namespace harrowmere::npc
