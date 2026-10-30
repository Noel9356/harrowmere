#ifndef HARROWMERE_CORE_WORLD_TERRITORY_HPP
#define HARROWMERE_CORE_WORLD_TERRITORY_HPP

// Territory — the LEGAL hierarchy node of the world model (task 1.4; design
// reference §Territories). Fields are the doc struct VERBATIM (card 1.4)
// plus the two plan-pinned extensions the card lists: `region` (the spatial
// anchor into the region grid) and `lawless` (4.6's garrison-less
// unconquered state). `owner` is NOT here — D-TERR-OWNER-1 adds it in task
// 6.3 (do not build ahead).
//
// Storage rules that keep the doc semantics load-bearing:
//   * `children` empty (not null) == leaf — the docs' "nullptr" maps to an
//     empty vector; every walk treats empty as the terminator.
//   * `laws` null -> inherit via lawsAt (design reference's recursive walk);
//     the pointer targets a DataRegistry-owned LawSet and stays valid for
//     the registry snapshot's lifetime (swap only between sim ticks — card
//     1.3's pitfall contract, restated in src/world/README.md).
//   * `titles` keyed by characterId (the docs' map<characterId,
//     SocialClassId>); std::map for deterministic iteration (R7).
//   * Sovereign vs unclaimed (design rule, pinned on card 1.4):
//       parent == 0 && !children.empty()                -> sovereign
//       parent == 0 && children.empty() && laws == null -> unclaimed
//     implemented ONCE in isSovereign()/isUnclaimed() below — D-JUR-1's
//     "no system reimplements a walk" applies to these predicates too.
//
// Worldgen (1.5) never produces unclaimed territories inside settled space
// (only frontier regions stay unmapped); tests hand-build hierarchies
// freely.

#include <cstdint>
#include <map>
#include <vector>

#include "core/enums.hpp"   // TerritoryType, SocialClassId
#include "core/types.hpp"   // characterId, territoryId, regionId
#include "world/region.hpp" // kNoRegion (the spatial-anchor sentinel)

namespace harrowmere::data
{

struct LawSet; // the registry-owned law set (data/laws.hpp); pointer-only here

} // namespace harrowmere::data

namespace harrowmere::world
{

// CompactRecord — one archived case row in a territory's `archive` (design
// reference §Territories: "archive holds compact case records; burning it
// destroys them"). Task 4.1's `compactRecord(evidence)` produces the rows;
// task 4.2's burned-archive mechanic destroys them — both write THROUGH the
// vector on Territory, never around it.
//
// The design reference defines no field layout for the record itself, so v1
// carries the identification minimum any case row needs (which case, when,
// what crime, who) and 4.1 finalizes the layout — DEVIATIONS.md carries the
// row. Keep it a trivially-copyable POD: archive rows are the save format's
// per-territory payload (task 7.1).
struct CompactRecord
{
    std::uint64_t evidenceId = 0; // the archived case (IdGenerator "evidence" space)
    std::uint64_t worldTime  = 0; // game seconds when the case was archived
    std::uint32_t crimeType  = 0; // CrimeType ordinal at archive time (closed list, 1.1)
    characterId culprit      = 0; // 0 = unknown identity (stranger reports)
};

// Territory — design reference §Territories, fields verbatim (card 1.4).
struct Territory
{
    TerritoryType type = TerritoryType::VILLAGE;
    territoryId id     = 0;
    territoryId parent = 0;             // 0 = none (root/sovereign level)
    std::vector<territoryId> children;  // empty == leaf
    const data::LawSet *laws = nullptr; // null -> inherit via lawsAt
    std::map<characterId, SocialClassId> titles;
    bool safe = false;                  // safe zones are spawn points + beginner territory
    std::vector<CompactRecord> archive; // burned archives destroy these (4.2 hook)
    regionId region = kNoRegion;        // spatial anchor for the region grid
    bool lawless    = false;            // 4.6: garrison-less unconquered territory
};

// Sovereign: the root of a governed tree (empire-level claimant).
bool isSovereign(const Territory &territory) noexcept;

// Unclaimed: no parent, no children, no laws — worldgen only ever leaves
// these OUTSIDE settled space (frontier), never inside it.
bool isUnclaimed(const Territory &territory) noexcept;

} // namespace harrowmere::world

#endif // HARROWMERE_CORE_WORLD_TERRITORY_HPP
