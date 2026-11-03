#ifndef HARROWMERE_CORE_FACTIONS_CLAIMS_HPP
#define HARROWMERE_CORE_FACTIONS_CLAIMS_HPP

// claims.hpp — task 6.3: player claims over territories with the doc's
// protection/abandonment rules and custom-law installation.
//
// Claiming (doc): unclaimed frontier territory (parent == 0 && children
// empty && !laws — the isUnclaimed predicate, D-JUR-1's one implementation)
// or lawless territory (4.6): the claim interaction stands the claimant IN
// the territory (territoryAt(position) == t — the nearest-center reading the
// runtime adapter resolves); cost CLAIM_COST (A-113 = 20000 currency);
// requires no active war involving it (the war store's contested-region read
// — a declared war contesting the territory's region, notice included).
// Claiming sets owner = playerId on the Territory — the D-TERR-OWNER-1
// extension (the doc's struct has no owner; DEVIATIONS.md carries the row,
// 7.1 persists the field with the territory) — and allows installing custom
// Laws: the player authors a law set constrained to the LawSet schema (1.2);
// installing writes territory.laws = customSet and lawsAt returns it (the doc
// walk, untouched — D-JUR-1). Claiming a village inside a kingdom requires
// the kingdom's tolerance (the relation check below) or war (the war arm is
// 6.2's outcome transfer, never the claim interaction).
//
// Protections (doc verbatim): "Player-owned territories are indestructible
// while offline, except during active war" — destruction attempts (arson
// events, siege outcomes) refuse while !warEffectivelyActive (6.2); during
// war they may be destroyed/transferred (6.2's outcome). The SP reading
// (D-SP-WAR-1): the defender's player-owned protections are suspended from
// noticeEndsAt while the war is active — the gate below asks the war store's
// effectively-active contested read. Safe-zone rule intact: a claimed
// territory that was safe keeps its ward reports but the player owner may not
// remove safe (safe zones are spawn points, worldgen-owned — pinned).
//
// Abandonment (doc verbatim): "flagged abandoned after 14 real-world days
// with no owner login and no active war involvement; abandoned territories
// can be claimed or dismantled. One owner session of at least 30 validated
// active minutes resets the timer; AFK time never counts". THE TWO-CLOCKS
// PITFALL (the card's top misinterpretation risk): the 14 days are REAL
// (D-TIME-1's real-clock rule), the war-active check is GAME time. The
// presence ledger accumulates real seconds only while !afk (D-AFK-1); one
// stint of >= ACTIVITY_MINUTES (A-114 = 30 real minutes) resets the timer;
// ABANDON_DAYS (A-115 = 14 real days) with no qualifying session flags
// abandoned — unless a war effectively involves the territory (the war
// involvement pauses the flagging, the doc's "no active war involvement").
// Abandoned territories claim at half cost (CLAIM_COST / 2) or dismantle:
// the territory reverts to unclaimed (owner 0), custom laws discarded, the
// archive preserved (archives are world memory — burning destroys them, not
// ownership changes, 4.2's rule).
//
// Session shape (the deterministic reading, DEVIATIONS.md row): presence
// stamps land per tick from the caller (the player's stamps ride the plugin's
// step-9 slice with the controller's AFK verdict; the sweep stamps living NPC
// owners — an NPC owner is always present while alive and never AFK). A stamp
// gap longer than the AFK timeout (A-58 — a lapse that long IS a logout)
// restarts the stint from zero; a stint reaching A-114 fires the qualifying
// reset. Only PLAYERS claim (the doc's claims are player claims; NPC courts
// hold territories through titles and the 6.2 outcome, never through this
// interaction).
//
// Persistence shape (§4.5): every state is a keyed store over the opaque ids
// and plain data; the custom law sets are CLAIM-STORE-OWNED (stable
// addresses — the registry-swap contract's std::map guarantee) so the
// territory's laws pointer stays valid; 7.1 serializes rows verbatim.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/result.hpp"    // Result<void, E> (0.2's carrier)
#include "core/scheduler.hpp" // IStepSystem (the step-1 sweep)
#include "core/types.hpp"     // characterId, territoryId
#include "core/world_context.hpp"
#include "data/laws.hpp" // LawSet (the schema the custom sets constrain to)

namespace harrowmere::factions
{

// The rule-level failures of the claim interactions (§3.4).
enum class ClaimError
{
    UnknownTerritory,     // the id names nothing in the world store
    UnknownCharacter,     // the actor does not exist
    NotPlayer,            // only players claim/dismantle (the doc's player claims)
    NotStandingThere,     // the interaction needs the actor in the territory
    NotClaimable,         // neither unclaimed, nor lawless, nor abandoned
    AlreadyOwned,         // a live owner holds the territory (re-claim goes through abandon/war)
    AtWar,                // an active war involves the territory (the declaration gate)
    NoTolerance,          // the kingdom's tolerance check failed (the relation read)
    InsufficientCurrency, // the A-113 purse gate (or the half-cost abandoned gate)
    NotOwner,             // installing laws / acting on a claim one does not hold
    NotAbandoned,         // dismantling needs the abandoned flag
    BadLawSet             // the authored set violates the LawSet schema's basic shape
};
using ClaimResult = Result<void, ClaimError>;

// One claim's ledger row (the owner field lives ON the Territory —
// D-TERR-OWNER-1; this row carries the real-time abandonment state, §4.5's
// serializable plain data).
struct ClaimState
{
    characterId owner                    = 0;
    std::uint64_t stintRealSeconds       = 0; // the current presence stint (validated = non-AFK only)
    std::uint64_t lastQualifyingRealTime = 0; // the last >=A-114 validated stint's end (the abandonment timer)
    std::uint64_t lastStampRealTime      = 0; // the last presence stamp (the stint-gap read; 0 = never stamped)
    std::string lawKey;                       // the installed custom set's key (empty = none installed)
    bool abandoned = false;
};

class ClaimStore
{
public:
    ClaimStore() = default;

    // ---- the claim interaction ---------------------------------------------------

    // claim — the doc's claiming rules: claimable-class gate, the no-active-
    // war gate, the kingdom tolerance (a village inside a kingdom), the A-113
    // cost (half for an abandoned territory), the D-TERR-OWNER-1 write, the
    // lawless clearing (4.6's exit: "the player claim 6.3 calls it").
    ClaimResult claim(territoryId t, characterId claimant, WorldContext &ctx);

    // installLaws — the custom-law installation: the owner authors a LawSet
    // (schema-constrained); installing writes territory.laws = the store's
    // copy (stable address) and lawsAt returns it. Territory.safe is
    // WORLDGEN-OWNED and never touched by this path (the spawn-point pin).
    ClaimResult installLaws(territoryId t, characterId by, const data::LawSet &set, WorldContext &ctx);

    // The installed custom set on `t` (nullptr = none — the territory
    // inherits or carries a registry set).
    const data::LawSet *lawsInstalledOn(territoryId t) const;

    // dismantleAbandoned — the abandoned territory reverts to unclaimed:
    // owner 0, custom laws discarded, the ARCHIVE PRESERVED (world memory —
    // burning destroys archives, ownership changes never do).
    ClaimResult dismantleAbandoned(territoryId t, characterId by, WorldContext &ctx);

    // ---- the presence ledger (the abandonment machinery) ---------------------------

    // notePresence — one stamp: real seconds accrue only while !afk
    // (D-AFK-1); a gap longer than the AFK timeout (A-58) restarts the stint
    // (the player left); a stint reaching A-114 fires the qualifying reset.
    // The plugin stamps the player's claims at step 9 (the controller's AFK
    // verdict is fresh there); the sweep stamps living NPC owners.
    void notePresence(territoryId t, characterId owner, bool afk, std::uint64_t realTimeSeconds);

    // tick — the step-1 sweep: NPC-owner presence stamps, then the lazy
    // abandonment flagging (14 REAL days, no qualifying session, no active
    // war involvement — the war check is GAME time; D-TIME-1's two-clocks
    // rule cited at the call site).
    void tick(WorldContext &ctx);

    // ---- 6.2's claim rewiring --------------------------------------------------------

    // noteOwnershipTransfer — the war outcome's transfer: the ledger row
    // follows the new owner with a fresh timer (armed at the transfer's real
    // time); the installed laws stay (the claim transfers with its law set).
    // The Territory.owner write itself is the war store's (the doc's
    // territorial-control arm).
    void noteOwnershipTransfer(territoryId t, characterId newOwner, std::uint64_t realTimeSeconds);

    // ---- reads ------------------------------------------------------------------------

    // isProtectedFromDestruction — the doc's protection sentence, the gate
    // every destruction path (arson events, siege outcomes) asks: a
    // PLAYER-owned territory refuses destruction while no effectively-active
    // war contests it (D-SP-WAR-1: the suspension starts at noticeEndsAt).
    // Unowned and NPC-held ground answers false (the protection class is
    // player-owned).
    bool isProtectedFromDestruction(territoryId t, WorldContext &ctx) const;

    bool isAbandoned(territoryId t) const;

    characterId ownerOf(territoryId t, WorldContext &ctx) const;

    // The territories `who` owns (id-ascending, R7) — the plugin's step-9
    // presence stamping walks exactly this list.
    std::vector<territoryId> claimsOwnedBy(characterId who) const;

    const std::map<territoryId, ClaimState> &all() const
    {
        return claims_;
    }

    // ---- save seam (task 7.1) ---------------------------------------------------

    // A custom law set by key (nullptr = none) — the load path's law-pointer
    // resolution reads this after restore() has re-inserted the sets (stable
    // addresses, the registry-swap contract).
    const data::LawSet *customLawByKey(const std::string &key) const;

    // The ledger + the custom sets as one plain-data snapshot. The
    // Territory.laws pointers are NOT part of it: they are rebuilt by the
    // load path (claims first, then the world's law keys resolved through
    // customLawByKey / the registry).
    struct Snapshot
    {
        std::map<territoryId, ClaimState> claims;
        std::map<std::string, data::LawSet> customLaws;
    };

    [[nodiscard]] Snapshot snapshot() const;
    void restore(const Snapshot &saved); // replaces the ledger + sets wholesale

private:
    // The kingdom court faction whose capital is an ancestor of `t` (or of
    // the territory itself), lowest faction id first (R7). 0 = no kingdom
    // above.
    factionId kingdomFactionAbove(territoryId t, WorldContext &ctx) const;

    std::map<territoryId, ClaimState> claims_;       // R7: territory-id ascending
    std::map<std::string, data::LawSet> customLaws_; // the store-owned sets (stable addresses)
};

// Step 1's claim slice (§4.4's sweeps row, after the faction drift): the
// abandonment sweep + the NPC-owner presence stamps.
class ClaimSweepSystem final : public IStepSystem
{
public:
    ClaimStore *store = nullptr;

    void run(
        WorldContext &ctx
    ) override
    {
        if (store != nullptr)
        {
            store->tick(ctx);
        }
    }
};

} // namespace harrowmere::factions

#endif // HARROWMERE_CORE_FACTIONS_CLAIMS_HPP
