// world.cpp — WorldStore implementation (task 1.4).
//
// The three doc walks live here, each a single iterative translation of its
// design-reference recursion (same visit order, no call-stack growth). All
// containers are std::map keyed by opaque ids (R7); every walk is bounded by
// the tree size so a malformed hierarchy degrades deterministically instead
// of hanging.

#include "world/world.hpp"

#include <cassert>

#include "data/laws.hpp" // the LawSet definition (root fallback + territory.laws)

namespace harrowmere::world
{

WorldStore::WorldStore(
    const data::LawSet &defaultLaws
)
    : defaultLaws_(&defaultLaws)
{}

// ---- construction surface -----------------------------------------------------

void WorldStore::clear()
{
    territories_.clear();
    regions_.clear();
    regionTerritory_.clear();
}

void WorldStore::addRegion(
    const Region &region
)
{
    regions_[region.id] = region;
}

void WorldStore::addTerritory(
    const Territory &territory
)
{
    if (territory.id == kNoTerritory)
    {
        // The null territory is the walks' terminator, never a node.
        assert(false && "WorldStore::addTerritory: id 0 is the null territory");
        return;
    }

    // Detach from the previous parent's children on replace.
    const auto existing = territories_.find(territory.id);
    if (existing != territories_.end() && existing->second.parent != 0)
    {
        const auto oldParent = territories_.find(existing->second.parent);
        if (oldParent != territories_.end())
        {
            auto &siblings = oldParent->second.children;
            for (auto it = siblings.begin(); it != siblings.end(); ++it)
            {
                if (*it == territory.id)
                {
                    siblings.erase(it);
                    break;
                }
            }
        }
    }

    territories_[territory.id] = territory;

    // Attach to the new parent's children (a root has parent 0 = none).
    if (territory.parent != 0)
    {
        const auto parent = territories_.find(territory.parent);
        if (parent != territories_.end())
        {
            auto &siblings     = parent->second.children;
            const bool present = [&] {
                for (const territoryId child : siblings)
                    if (child == territory.id)
                        return true;
                return false;
            }();
            if (!present)
                siblings.push_back(territory.id);
        }
    }
}

void WorldStore::setRegionTerritory(
    regionId region, territoryId territory
)
{
    if (territory == kNoTerritory)
    {
        // Un-mapping (frontier stays unclaimed): the mapping entry goes.
        regionTerritory_.erase(region);
        return;
    }
    regionTerritory_[region] = territory;
}

// ---- the doc walks (D-JUR-1) ----------------------------------------------------

territoryId WorldStore::territoryAt(
    const Position &p
) const
{
    // Region grid -> territory covering point (card 1.4): one map hit on the
    // generation mapping, no geometric recompute. No region (outside the
    // plane) or no mapping (unclaimed frontier) -> the null territory, whose
    // walk semantics (DEFAULT_LAWS / COMMONER / no jurisdiction) are exactly
    // the doc's unclaimed-land behavior.
    const regionId r = regionIdAt(p);
    if (r == kNoRegion)
        return kNoTerritory;
    const auto it = regionTerritory_.find(r);
    return it == regionTerritory_.end() ? kNoTerritory : it->second;
}

const data::LawSet &WorldStore::lawsAt(
    territoryId t
) const
{
    // Design reference §Laws:
    //   lawsAt(territory):
    //       if territory.laws != nullptr: return territory.laws
    //       if territory.parent == 0:     return DEFAULT_LAWS
    //       return lawsAt(territories[territory.parent])
    // Iteratively: walk up while the id is live; the first explicit law set
    // wins; falling off the chain (root, unknown id) resolves the DEFAULT.
    const std::size_t maxSteps = walkBound();
    std::size_t steps          = 0;
    for (territoryId u = t; u != kNoTerritory; u = parentOf(u))
    {
        assert(steps < maxSteps && "WorldStore::lawsAt: parent chain exceeds the tree size (cycle?)");
        if (steps++ >= maxSteps)
            break;
        const auto it = territories_.find(u);
        if (it == territories_.end())
            break; // broken/orphaned chain: resolve the default (deterministic)
        if (it->second.laws != nullptr)
            return *it->second.laws;
    }
    return *defaultLaws_;
}

SocialClassId WorldStore::socialClassOf(
    characterId c, territoryId start
) const
{
    // Design reference §Laws, verbatim walk (card 1.4's pitfall pins the
    // direction): start at the GIVEN territory, walk UP, and return the
    // FIRST level carrying a title for `c` — the deepest (most local) title;
    // COMMONER when the whole chain is title-less. Do not "fix" this into a
    // breadth-search that would return the outermost hit instead.
    const std::size_t maxSteps = walkBound();
    std::size_t steps          = 0;
    for (territoryId t = start; t != kNoTerritory; t = parentOf(t))
    {
        assert(steps < maxSteps && "WorldStore::socialClassOf: parent chain exceeds the tree size (cycle?)");
        if (steps++ >= maxSteps)
            break;
        const auto it = territories_.find(t);
        if (it == territories_.end())
            break;
        const auto title = it->second.titles.find(c);
        if (title != it->second.titles.end())
            return title->second;
    }
    return SocialClassId::COMMONER;
}

bool WorldStore::isSafeTerritory(
    territoryId t
) const noexcept
{
    // Design reference §Safe Zones: territories are safe when safe = true.
    // The null / unknown territory is simply not safe (find, never .at() —
    // unmapped positions must resolve, not throw).
    const auto it = territories_.find(t);
    return it != territories_.end() && it->second.safe;
}

bool WorldStore::isInSafeZone(
    const Position &p
) const noexcept
{
    // Design reference §Safe Zones, verbatim:
    //   isInSafeZone(position): return isSafeTerritory(territoryAt(position))
    return isSafeTerritory(territoryAt(p));
}

bool WorldStore::isAncestorOrSelf(
    territoryId ancestor, territoryId t
) const
{
    // Design reference §Justice, `warrantActiveIn` (D-JUR-1): the warrant is
    // active in the queried territory exactly when the ISSUING territory
    // sits on the query's chain:
    //
    //     warrantActiveIn(warrant, q)  ==  isAncestorOrSelf(q, warrant.territoryId)
    //
    // The loop below is the doc's walk verbatim under that call shape:
    // start at `t` (the issuing territory), walk UP, and answer whether
    // `ancestor` (the queried territory) is met — i.e. `ancestor` is `t` or
    // an ancestor of `t`. A village warrant covers the village and its
    // county/kingdom/empire — never a sibling village (the sibling is not
    // an ancestor of the issuer). Self counts (isAncestorOrSELF).
    const std::size_t maxSteps = walkBound();
    std::size_t steps          = 0;
    for (territoryId u = t; u != kNoTerritory; u = parentOf(u))
    {
        assert(steps < maxSteps && "WorldStore::isAncestorOrSelf: parent chain exceeds the tree size (cycle?)");
        if (steps++ >= maxSteps)
            break;
        if (u == ancestor)
            return true;
    }
    return false;
}

// ---- parent/child traversal helpers ---------------------------------------------

territoryId WorldStore::parentOf(
    territoryId t
) const noexcept
{
    const auto it = territories_.find(t);
    return it == territories_.end() ? kNoTerritory : it->second.parent;
}

const std::vector<territoryId> &WorldStore::childrenOf(
    territoryId t
) const noexcept
{
    // A leaf and an unknown territory both read as childless (the docs'
    // "nullptr" children); the shared empty vector keeps the reference valid.
    static const std::vector<territoryId> kEmpty;
    const auto it = territories_.find(t);
    return it == territories_.end() ? kEmpty : it->second.children;
}

std::vector<territoryId> WorldStore::chainToRoot(
    territoryId t
) const
{
    std::vector<territoryId> chain;
    const std::size_t maxSteps = walkBound();
    std::size_t steps          = 0;
    for (territoryId u = t; u != kNoTerritory; u = parentOf(u))
    {
        assert(steps < maxSteps && "WorldStore::chainToRoot: parent chain exceeds the tree size (cycle?)");
        if (steps++ >= maxSteps)
            break;
        chain.push_back(u);
    }
    return chain;
}

// ---- structural reads -------------------------------------------------------------

const Territory *WorldStore::territory(
    territoryId t
) const noexcept
{
    const auto it = territories_.find(t);
    return it == territories_.end() ? nullptr : &it->second;
}

Territory *WorldStore::territory(
    territoryId t
) noexcept
{
    const auto it = territories_.find(t);
    return it == territories_.end() ? nullptr : &it->second;
}

const Region *WorldStore::region(
    regionId r
) const noexcept
{
    const auto it = regions_.find(r);
    return it == regions_.end() ? nullptr : &it->second;
}

Region *WorldStore::region(
    regionId r
) noexcept
{
    const auto it = regions_.find(r);
    return it == regions_.end() ? nullptr : &it->second;
}

territoryId WorldStore::territoryOfRegion(
    regionId r
) const noexcept
{
    const auto it = regionTerritory_.find(r);
    return it == regionTerritory_.end() ? kNoTerritory : it->second;
}

} // namespace harrowmere::world
