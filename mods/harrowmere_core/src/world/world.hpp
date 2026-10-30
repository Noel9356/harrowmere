#ifndef HARROWMERE_CORE_WORLD_WORLD_HPP
#define HARROWMERE_CORE_WORLD_WORLD_HPP

// WorldStore — the static world graph (task 1.4): regions, the territory
// tree, the explicit region -> territory mapping, and THE three doc walks
// (`lawsAt`, `socialClassOf`, jurisdiction) implemented ONCE here (D-JUR-1:
// no system reimplements a parent-chain walk — warrants, reputation
// propagation, exemption lookups and 4.x/6.x all call these).
//
// Doc walks, verbatim semantics (design reference §Laws / §Justice):
//   lawsAt(territory):        t.laws if set; else walk to parent; DEFAULT_LAWS
//                             at the root (parent == 0).
//   socialClassOf(c, start):  check EACH level's titles walking UP from
//                             start; the FIRST (deepest, most local) hit
//                             wins — card 1.4's pitfall: do not "fix" this
//                             into breadth-search; COMMONER when no level
//                             carries a title.
//   isAncestorOrSelf(a, t):   true iff `a` is `t` or an ancestor of `t` —
//                             the jurisdiction walk of `warrantActiveIn`:
//                             a warrant issued for territory X is active in
//                             territory q exactly when the ISSUING
//                             territory falls under (or is) the queried
//                             one, `isAncestorOrSelf(q, X)` — so a village
//                             warrant covers its county, kingdom and
//                             empire, never a sibling village.
//
// Determinism & storage (R7): every map is std::map keyed by the opaque id
// aliases; iteration order is well-defined everywhere the simulation or the
// worldgen hash (1.5 stage 7) can observe it. Cross-references are ids —
// the `laws` pointer is the one deliberate exception (it targets the
// registry-owned LawSet the territory resolves to; see README).
//
// Contracts:
//   * Territory 0 is the NULL territory (the docs' `parent = 0` sentinel and
//     the walks' terminator). territoryAt() returns it for positions with no
//     region (outside the world plane) or no mapping (unclaimed frontier);
//     every walk treats it as the root fallback (DEFAULT_LAWS / COMMONER /
//     not-jurisdiction), so unmapped space behaves as doc-specified unclaimed
//     land without special cases at call sites.
//   * The store is populated once by worldgen (1.5) at new-game and persisted
//     whole (7.1); tests hand-build hierarchies through the same add* surface.
//   * `defaultLaws` (constructor) is the snapshot WorldStore resolves root
//     fallbacks against — DataRegistry::defaultLaws() in production. It must
//     outlive the store and only be re-pointed between sim ticks (the
//     registry swap contract, card 1.3's pitfall).

#include <cstdint>
#include <map>
#include <vector>

#include "core/types.hpp"      // territoryId, regionId, characterId, Position
#include "world/region.hpp"    // Region + the D-WORLD-1 projection
#include "world/territory.hpp" // Territory, CompactRecord, predicates

namespace harrowmere::data
{

struct LawSet; // forward decl: reference/pointer only in this header

} // namespace harrowmere::data

namespace harrowmere::world
{

// The null territory: 0 in every id sense (docs' `parent = 0`). Sentinel
// next to its type; not a game-rule value, so it claims no Appendix A row.
inline constexpr territoryId kNoTerritory = 0;

class WorldStore
{
public:
    // Resolves root fallbacks against `defaultLaws` (production:
    // DataRegistry::defaultLaws() — the DEFAULT_LAWS set the registry's
    // cross-ref pass guarantees exists).
    explicit WorldStore(const data::LawSet &defaultLaws);

    // ---- construction surface (worldgen 1.5 populates; tests hand-build) ----

    // Empty the graph (fresh world / test fixture).
    void clear();

    // Insert or replace a region (whole-struct replace; worldgen owns flags/
    // mana/season initialization, 3.8 advances season/weather in place via
    // region()).
    void addRegion(const Region &region);

    // Insert or replace a territory. Wires the tree: the territory detaches
    // from a previous parent's children and attaches to `parent`'s (when the
    // parent exists; a root's parent is 0 = none). Inserting id 0 is a
    // programmer error (the null territory): assert in debug, deterministic
    // no-op in release.
    void addTerritory(const Territory &territory);

    // The explicit generation mapping (card 1.4: stored once at generation —
    // runtime lookups are one map hit, never a geometric recompute). A
    // territory may cover several regions; a region belongs to EXACTLY ONE
    // territory (re-mapping moves it).
    void setRegionTerritory(regionId region, territoryId territory);

    // ---- reads ---------------------------------------------------------------

    // The doc walks (D-JUR-1: implemented once, here). See the header doc.
    territoryId territoryAt(const Position &p) const;
    const data::LawSet &lawsAt(territoryId t) const;
    SocialClassId socialClassOf(characterId c, territoryId start) const;
    bool isSafeTerritory(territoryId t) const noexcept;
    bool isInSafeZone(const Position &p) const noexcept;
    bool isAncestorOrSelf(territoryId ancestor, territoryId t) const;

    // ---- parent/child traversal helpers (used by 4.x/6.x) ---------------------

    // Parent id, or 0 for the root / an unknown territory.
    territoryId parentOf(territoryId t) const noexcept;

    // The children of `t` (empty for a leaf or an unknown territory).
    const std::vector<territoryId> &childrenOf(territoryId t) const noexcept;

    // The jurisdiction chain of `t`: t, parent, ..., root (empty only for the
    // null territory). Reputation propagation (4.4) and warrant walks (4.2)
    // consume this instead of re-walking.
    std::vector<territoryId> chainToRoot(territoryId t) const;

    // ---- structural reads -------------------------------------------------------

    bool hasTerritory(
        territoryId t
    ) const noexcept
    {
        return territories_.find(t) != territories_.end();
    }
    bool hasRegion(
        regionId r
    ) const noexcept
    {
        return regions_.find(r) != regions_.end();
    }

    // The territory node, or nullptr when absent (callers null-check; the
    // walks below never require callers to hold nodes).
    const Territory *territory(territoryId t) const noexcept;
    Territory *territory(territoryId t) noexcept;

    // The region cell, or nullptr when absent.
    const Region *region(regionId r) const noexcept;
    Region *region(regionId r) noexcept;

    // The territory a region was mapped to at generation (kNoTerritory when
    // the region is unknown or unclaimed frontier).
    territoryId territoryOfRegion(regionId r) const noexcept;

    const std::map<territoryId, Territory> &territories() const noexcept
    {
        return territories_;
    }
    const std::map<regionId, Region> &regions() const noexcept
    {
        return regions_;
    }
    const std::map<regionId, territoryId> &regionTerritories() const noexcept
    {
        return regionTerritory_;
    }

    std::size_t territoryCount() const noexcept
    {
        return territories_.size();
    }
    std::size_t regionCount() const noexcept
    {
        return regions_.size();
    }
    std::size_t mappedRegionCount() const noexcept
    {
        return regionTerritory_.size();
    }

private:
    // The walks advance one parent hop per step; a chain can never be longer
    // than the tree is deep, so territories_.size() + 1 bounds every walk.
    // (A malformed cycle is a programmer error: assert in debug, stop the
    // walk at the bound in release — deterministic either way, R7.)
    std::size_t walkBound() const noexcept
    {
        return territories_.size() + 1;
    }

    const data::LawSet *defaultLaws_; // the root fallback (never null)
    std::map<territoryId, Territory> territories_;
    std::map<regionId, Region> regions_;
    std::map<regionId, territoryId> regionTerritory_; // the generation mapping
};

} // namespace harrowmere::world

#endif // HARROWMERE_CORE_WORLD_WORLD_HPP
