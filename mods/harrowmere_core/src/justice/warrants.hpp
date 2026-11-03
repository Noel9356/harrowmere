#ifndef HARROWMERE_CORE_JUSTICE_WARRANTS_HPP
#define HARROWMERE_CORE_JUSTICE_WARRANTS_HPP

// warrants.hpp — task 4.2's warrant half: issuance, the two doc-verbatim
// maps (they live on the JusticeStore), the LAZY expiry purge ("hasWarrantIn
// purges first — reads are mutating; do not make them const"), and the
// jurisdiction walk (D-JUR-1: `warrantActiveIn` IS 1.4's isAncestorOrSelf).
//
// The doc invariants pinned here:
//   * The store is EXACTLY the two maps: `warrants: map<u64, Warrant>` +
//     `activeWarrantsByCulprit: map<characterId, set<u64>>` (§4.5). Every
//     mutation deletes from BOTH (issue/convict/expire/pay — the invariant
//     test pins all four paths).
//   * `bountyFor` is D-BOUNTY-1 verbatim: min(BOUNTY_CAP (A-73), severity x 5
//     currency per point).
//   * `lethalForce` is copied from the crime data at issuance (MURDER true,
//     everything else false — the pinned v1 data).
//   * `expiresTime = now + WARRANT_DURATION` (A-74, 7 game-days); expiry is a
//     PAUSE (the case downgrades to INVESTIGATING), never an eraser.
//   * A warrant issued for territory X is active in territory q exactly when
//     isAncestorOrSelf(q, X): a village warrant covers its county, kingdom
//     and empire — never a sibling village.
//
// The mutating reads take the WorldContext (the purge may resolve warrants
// and the resolution publishes WarrantResolved through the sim-event seam —
// the same shape damage.cpp's warrantCovers already uses).

#include <cstdint>
#include <vector>

#include "core/game_events.hpp" // WarrantResolved (the resolved verdict names its path)
#include "core/types.hpp"       // characterId, territoryId
#include "justice/justice_store.hpp"

namespace harrowmere
{
struct WorldContext;
} // namespace harrowmere

namespace harrowmere::justice
{
struct Evidence; // evidence.hpp — the complete type; pointer/reference only here
} // namespace harrowmere::justice

namespace harrowmere::world
{
class WorldStore; // world.hpp (1.4) — the jurisdiction walk; pointer only
} // namespace harrowmere::world

namespace harrowmere::justice
{

// The Warrant record: the doc's fields (bounty/lethal/expires/territory/
// culprit) plus the evidence link (convict() and surrender resolve the case
// through it) and the `paid` flag (4.7's pay-once rule — the flag is on the
// WARRANT, not the claimant).
struct Warrant
{
    std::uint64_t id          = 0;
    std::uint64_t evidenceId  = 0;
    characterId culpritId     = 0;
    territoryId territory     = 0;     // the doc's `territoryId territoryId` (member drops the
                                       // type-name clash); the ISSUING territory (jurisdiction anchor)
    bool lethal               = false; // copied from crime data at issuance
    std::uint64_t bounty      = 0;     // D-BOUNTY-1
    std::uint64_t expiresTime = 0;     // now + A-74
    bool paid                 = false; // 4.7: capture/kill claims pay once per warrant
};

// bountyFor — D-BOUNTY-1 verbatim: min(BOUNTY_CAP, severity * 5 currency per
// point) over the evidence's crime severity (crime_data.json).
std::uint64_t bountyFor(const Evidence &e, WorldContext &ctx);

// issueWarrant — design reference verbatim: draws the warrant id, copies the
// crime data's lethalForce, stamps the bounty + expiry, inserts into BOTH
// maps, flips the case to WANTED, publishes WarrantIssued.
void issueWarrant(Evidence &e, WorldContext &ctx);

// purgeExpiredWarrants — the doc-mandated lazy purge: erase every warrant
// whose expiresTime <= now from BOTH maps (publishing WarrantResolved{
// Expired} once per deleted warrant — deletion is permanent, so the event
// fires exactly once). The reads below call this FIRST.
void purgeExpiredWarrants(WorldContext &ctx);

// hasWarrantIn — the mutating read (purge first — the doc's rule). True iff
// the suspect holds an active warrant issued by a territory that `territory`
// covers (isAncestorOrSelf(territory, issuing) through 1.4's walk).
bool hasWarrantIn(WorldContext &ctx, characterId suspect, territoryId territory);

// hasLethalWarrantIn — the same walk over lethal warrants only (3.3's
// isCriminalAct exonerates a lethal-warrant kill through this).
bool hasLethalWarrantIn(WorldContext &ctx, characterId suspect, territoryId territory);

// warrantActiveIn — the doc's predicate, verbatim: the warrant covers
// `queried` exactly when isAncestorOrSelf(queried, warrant.territoryId).
bool warrantActiveIn(const Warrant &warrant, const world::WorldStore &world, territoryId queried);

// The live (unexpired) warrants of one culprit, warrant-id ascending (R7) —
// surrender (4.3) and the enforcement AI (4.6) iterate these. Purges first.
std::vector<Warrant> activeWarrantsOf(WorldContext &ctx, characterId culprit);

// resolveWarrantsForEvidence — the terminal arms' shared cleanup (the doc
// invariant's engine): delete every warrant of `culprit` whose evidence is
// `evidenceId` from BOTH maps, publishing WarrantResolved{how} per warrant.
// convict() passes Convicted (or Surrendered through the surrender path);
// nothing else resolves a warrant by evidence.
void resolveWarrantsForEvidence(
    std::uint64_t evidenceId, characterId culprit, WarrantResolved::How how, WorldContext &ctx
);

// payWarrant — the bounty's terminal arm (4.7's claim routes here): marks
// `paid`, deletes from BOTH maps, publishes WarrantResolved{Paid}. A missing
// id is a no-op (payment deletes the warrant — a second claim finds nothing,
// which is the pay-once rule's store-level form).
void payWarrant(std::uint64_t warrantId, WorldContext &ctx);

} // namespace harrowmere::justice

#endif // HARROWMERE_CORE_JUSTICE_WARRANTS_HPP
