// factions.hpp — task 6.1: faction records driving membership, resources and
// relations — the substrate war (Phase 4's 6.2) reads.
//
// The doc's Faction struct is verbatim (design reference §Factions): members
// are a std::set keyed by the opaque characterId (R7), ranks are per-faction
// integers 0..9 ("have ranks" — 0 grunt … 9 leader; NOT SocialClassId — the
// titles live on territories and conflating them corrupts socialClassOf and
// suppression, the card pitfall), sharedResources are goods stock + currency,
// relations are per-pair Relation rows.
//
// The as-built seams this module fills:
//   * IFactionQueries (world_context.hpp) — the real FactionQueries adapter
//     replaces the null default in the plugin's context binding. isAtWar
//     reads the war registry (6.2) and STUBS FALSE until Phase 4 lands it
//     (the card's own Done-when; 3.3's isCriminalAct keeps consuming it).
//   * The IWarRegistry casus-belli stub stays Phase 4 property; nothing here
//     registers a war.
//
// Membership is BIDIRECTIONAL and kept in lockstep (TestFaction_
// MembershipBidirectional): Character.faction is the doc field, Faction.members
// the set, membership_ the store-side index — every mutation goes through one
// store function that writes all sides, never a direct member poke.
//
// NPC membership (the card: "NPCs' factionId from worldgen — kingdom courts,
// free company"): adoptWorldGen walks each living NPC's home territory up the
// legal tree to its kingdom court (1.5's stage-5 seeds carry courtTerritory);
// frontier wanderers (home 0) join the free company. Territory OWNERSHIP is
// not a faction concept — territories are claimed by characters (6.3,
// Phase 4); factions are social + resource structures only.
//
// The monthly dynamics sweep (the card's "monthly sweep") is lazy-gated like
// every other calendar cadence: the first call of a new game-month applies
// the whole month's drift once (deterministic, no per-tick cost).
//
// Persistence shape (§4.5): every state is a keyed store over the opaque ids
// and plain data — serializable in principle; Phase 4's 7.1 wires the format.

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/result.hpp"    // Result<void, E> (0.2's carrier)
#include "core/scheduler.hpp" // IStepSystem (the monthly sweep's registration)
#include "core/types.hpp"     // factionId, characterId, territoryId, goodId
#include "core/world_context.hpp"

namespace harrowmere::world
{
struct FactionSeed; // worldgen.hpp (1.5 stage 5) — the new-game input
}

namespace harrowmere::factions
{

// Relation — design reference verbatim: score on the ±SCALE grid, the two
// standing flags. The worldgen seeds open rivals at -6000 (rivalry) and
// allies at +6000 (alliance); SetRelation re-derives the flags from the
// score (the seeded magnitude ±6000 flips them, constants.hpp).
//
// The one plan-pinned extension (6.2, D-WAR-TREATY-1): `treatyUntil` — the
// mandatory one-week peace treaty's end after a war between the pair (the
// doc's Relation carries three fields; the treaty needs a home; DEVIATIONS.md
// carries the row). declareWar refuses while now < treatyUntil, both
// directions; SetRelation never touches it (a treaty outlives a relation
// score change).
struct Relation
{
    std::int32_t score        = 0; // -SCALE..SCALE
    bool alliance             = false;
    bool rivalry              = false;
    std::uint64_t treatyUntil = 0; // 6.2, D-WAR-TREATY-1: worldTime when the pair may re-declare (0 = no treaty)
};

// Faction — design reference verbatim (the card's Spec block).
struct Faction
{
    factionId id = 0;
    std::string name;
    std::set<characterId> members;                   // characters' factionId points here (R7)
    std::map<characterId, std::uint8_t> ranks;       // 0 grunt … 9 leader
    std::map<goodId, std::uint64_t> sharedResources; // goods stock + currency (goodId 0 = currency)
    std::map<factionId, Relation> relations;         // this faction's standing toward the others
    territoryId capital = 0;
    bool playerFounded  = false; // SP: at most one (D-SP-1)
};

// The rule-level failures of the faction interactions (§3.4: return values,
// never exceptions).
enum class FactionError
{
    UnknownFaction,
    UnknownCharacter,
    NotLeader,            // management is the rank-9 seat's alone (6.1)
    AlreadyMember,        // the subject already stands in a faction
    PlayerFactionExists,  // D-SP-1's singleton (at most one playerFounded)
    InsufficientCurrency, // the A-108 founding cost
    NoTerritoryPresence,  // the founding needs a territory presence
    NotMember,            // rank/relation management on an outsider
    BadRank               // ranks are the closed 0..9 band
};

using FactionResult = Result<void, FactionError>;

class FactionStore
{
public:
    FactionStore() = default;

    // New-game wiring: create one Faction per seed (1.5's stage-5 rivals +
    // alliance open both directions at the seeded scores), then assign NPC
    // membership by home territory (kingdom court walk; frontier -> the free
    // company) and the deterministic rank ladder (leader 9 = the highest
    // social class, ties by lower id). Called once at plugin start, after
    // the world and the population exist.
    void adoptWorldGen(const std::vector<world::FactionSeed> &seeds, WorldContext &ctx);

    // ---- reads ----------------------------------------------------------------
    const std::map<factionId, Faction> &all() const
    {
        return factions_;
    }
    const Faction *find(factionId id) const;
    Faction *find(factionId id);
    std::size_t size() const
    {
        return factions_.size();
    }

    // The membership index (lockstep with Faction.members + Character.faction).
    factionId factionOfCharacter(characterId who) const;
    bool isMember(
        characterId who, factionId faction
    ) const
    {
        return factionOfCharacter(who) == faction && faction != 0;
    }

    // `a`'s standing toward `b` (nullptr when either faction or the row is
    // absent — no row is the honest neutral, never a synthesized one).
    const Relation *relationBetween(factionId a, factionId b) const;

    // isAtWar — reads the war registry (6.2): a pair is AT WAR exactly while
    // an active war between them has passed its notice end (the shared
    // warEffectivelyActive gate — the card pitfall forbids a second
    // implementation). Null registry in the context reads false (D-SINK-1).
    bool isAtWar(factionId a, factionId b, WorldContext &ctx) const;

    // ---- the player faction (6.1; D-SP-1's singleton) -------------------------

    // Found: the A-108 cost from the founder's purse + a territory presence;
    // the founder becomes the rank-9 leader; at most one playerFounded
    // faction exists. The id continues the worldgen sequence (R10).
    FactionResult foundPlayerFaction(characterId founder, const std::string &name, WorldContext &ctx);

    // Dissolve: leader-only. Shared resources distribute to the members by
    // rank share (weight = rank + 1 — a grunt keeps a token share; the
    // flooring remainder goes to the highest weight, ties to the lower id,
    // R7), then the faction and every membership row disappear.
    FactionResult dissolveFaction(characterId leader, WorldContext &ctx);

    // Invite: leader-only. The candidate (living sapient adult, no faction)
    // answers with the acceptance roll — affinity toward the inviter moves
    // it, a weaker candidate discounts (constants.hpp's pinned magnitudes);
    // the roll draws the pinned "npc_life" stream (R6).
    FactionResult inviteMember(characterId inviter, characterId candidate, WorldContext &ctx);

    // Rank assignment: leader-only, members only, the closed 0..9 band.
    FactionResult setMemberRank(characterId leader, characterId member, std::uint8_t rank, WorldContext &ctx);

    // SetRelation: the score lands clamped on the ±SCALE grid, both
    // directions symmetric; the standing flags re-derive from the score at
    // the seeded magnitude (±6000: alliance up top, rivalry at the bottom).
    FactionResult setRelation(factionId a, factionId b, std::int32_t score, WorldContext &ctx);

    // StampTreaty — 6.2's endWar stamps the mandatory peace treaty's end on
    // the pair's relation rows (both directions, the mirror-row rule; an
    // absent row gains a neutral one). SetRelation never touches the treaty
    // field (a treaty outlives a relation score change).
    void stampTreaty(factionId a, factionId b, std::uint64_t treatyUntil);

    // ---- the monthly dynamics sweep --------------------------------------------

    // Lazy-gated: the first call in a new game-month applies the month once.
    // Per pair (id-ascending, R7): rivalry pairs harden or soften by the
    // leaders' mean reputation (4.4's read — reviled leaders harden); every
    // other non-alliance pair drifts toward neutral. Trade volume and war
    // memory are the two dormant arms (no v1 driver; the interfaces carry
    // them — tradeVolume_ records and the war registry is Phase 4's).
    void monthlyDrift(WorldContext &ctx);

    // ---- save seam (task 7.1) ---------------------------------------------------
    //
    // The store's whole book as one plain-data snapshot (the factions, the
    // lockstep membership index, the dormant trade volume, the lazy month
    // gate and the id counter). persist/ serializes it; the load path hands
    // it back through restore(). A loaded FactionStore does NOT re-run
    // adoptWorldGen (the court walk is a new-game action; the save carries
    // the adopted result).
    struct Snapshot
    {
        std::map<factionId, Faction> factions;
        std::map<characterId, factionId> membership;
        std::map<std::uint64_t, std::uint64_t> tradeVolume;
        std::uint64_t lastDriftMonth = 0;
        factionId nextFactionId      = 1;
    };

    [[nodiscard]] Snapshot snapshot() const;
    void restore(const Snapshot &saved); // replaces the store book wholesale

private:
    // The one membership write (all three sides: the set, the index, the
    // Character.faction field). Callers validated the gates.
    void attach(characterId who, factionId faction, std::uint8_t rank, WorldContext &ctx);
    void detach(characterId who, WorldContext &ctx);

    std::map<factionId, Faction> factions_;              // R7: id-ascending
    std::map<characterId, factionId> membership_;        // the lockstep index (R7)
    std::map<std::uint64_t, std::uint64_t> tradeVolume_; // packed pair -> volume (dormant in v1)
    std::uint64_t lastDriftMonth_ = 0;                   // the lazy monthly gate (0 = never drifted)
    factionId nextFactionId_      = 1;                   // continues the worldgen sequence (R10)
};

// ---- the runtime adapters ------------------------------------------------------

// The real IFactionQueries (the plugin swaps the Phase-2 null default in).
// isAtWar reads the live war registry through the bound context's warStore
// (6.2's real registry answers; see war.hpp).
class FactionQueries final : public IFactionQueries
{
public:
    void bind(
        const FactionStore *store, WorldContext *ctx
    )
    {
        store_ = store;
        ctx_   = ctx;
    }

    bool isAtWar(factionId a, factionId b) const override;

private:
    const FactionStore *store_ = nullptr;
    WorldContext *ctx_         = nullptr;
};

// The monthly dynamics sweep (step 1's faction slice, after the justice
// backstop): the store's lazy month gate makes extra calls no-ops.
class FactionMonthlySystem final : public IStepSystem
{
public:
    FactionStore *store = nullptr;

    void run(
        WorldContext &ctx
    ) override
    {
        if (store != nullptr)
        {
            store->monthlyDrift(ctx);
        }
    }
};

} // namespace harrowmere::factions
