#ifndef HARROWMERE_CORE_COMBAT_CONSENT_HPP
#define HARROWMERE_CORE_COMBAT_CONSENT_HPP

// consent.hpp — the two consent channels the whole justice system depends on
// (task 2.7): duels and arenas, with the doc's broadcast rule so a lawful
// duel generates zero assault evidence.
//
// Design rule (design reference §Death notes): "Duels and arenas broadcast
// consent to nearby NPCs; otherwise every legal duel generates assault
// evidence." Card 2.7 pins the mechanics:
//
//   * Duel — requestDuel -> target accepts -> BOTH duelOpponent fields set
//     (2.1's consentsToPvp reads the victim's own field; 3.2 never checks
//     anything else). Acceptance BROADCASTS DuelStarted to every character
//     from idsWithinRadius(midpoint, CONSENT_BROADCAST_M) (A-26 = 50 m) —
//     the witness registry (3.9) stores the consent window per observer.
//     PINNED ORDER (the card's pitfall): broadcast -> enable damage. The
//     observers are gathered and delivered BEFORE the consent fields flip,
//     or the first blow records evidence.
//   * End — yield command, death, DUEL_TIMEOUT_S (A-27 = 10 game-min) of no
//     mutual damage, or range separation > 100 m (DUEL_MAX_SEPARATION_M,
//     constants.hpp pending promotion) -> both fields cleared, end
//     broadcast.
//   * Arena — territories declare arena zones (territory_templates.json:
//     arena flag + arenaRadiusM; one per CITY in v1). Entering sets
//     arenaId = zoneId (the same value for all occupants — the doc's
//     shared-id consent; the zone's territory id), leaving clears it.
//     consentsToPvp returns true exactly for these two channels — never
//     reimplemented here (2.1's Done-when keeps ONE consent check).
//
// NPC acceptance policy ("6.5's score proximity + disposition") is 6.5's:
// acceptance arrives explicitly (player confirm / the future NPC policy),
// this file only keeps the request bookkeeping honest. NPC-vs-NPC scheduled
// bouts are the card's marked *stretch* — dropped for v1, unlike the
// zone-consent rule (§5).
//
// Seams (D-SINK-1): IDuelBroadcast is the witness-registry consumer seam —
// null = nobody records the consent windows, the duel still works (the
// fields carry the consent). Wiring points, when their owners land: 3.9
// implements IDuelBroadcast; the step-9 command handler maps
// RequestDuel/YieldDuel onto requestDuel/yieldDuel; 3.2's applyDamage calls
// noteDuelDamage; the death handler (3.3) calls endDuelOnDeath; the movement
// application (step 9's transform + 3.4/3.5's NPC movement) calls
// refreshArenaPresence. None of those owners change this file's signatures.

#include <cstdint>
#include <map>
#include <vector>

#include "character/character.hpp" // Character (the doc struct)
#include "core/constants.hpp"      // A-26/A-27, DUEL_MAX_SEPARATION_M
#include "core/result.hpp"         // Result<void, E>
#include "core/types.hpp"          // characterId, territoryId, Position

namespace harrowmere::character
{
class CharacterStore; // character/store.hpp (task 2.0)
} // namespace harrowmere::character

namespace harrowmere::data
{
class DataRegistry; // registry.hpp (task 1.3) — territory templates
} // namespace harrowmere::data

namespace harrowmere::world
{
class SpatialGrid; // world/spatial_grid.hpp (task 1.6) — the radius gather
class WorldStore;  // world/world.hpp (task 1.4) — the territory/region graph
} // namespace harrowmere::world

namespace harrowmere::combat
{

// ---- duel bookkeeping ------------------------------------------------------------

// A pending request: challenger asked target; acceptance is the target's
// call. A request has NO expiry (no Appendix A row pins one — none
// invented); it dies by acceptance, by a death of either party
// (endDuelOnDeath), or by being replaced.
struct PendingDuel
{
    characterId challenger    = 0;
    characterId target        = 0;
    std::uint64_t requestedAt = 0;
};

// An active duel: both sides' consent fields point at each other (a < b by
// the record key). lastDamage is the worldTime of the most recent mutual
// damage — 3.2's applyDamage reports through noteDuelDamage; the timeout
// sweep reads it (A-27).
struct ActiveDuel
{
    characterId a            = 0; // the lower id (the record key)
    characterId b            = 0; // the higher id
    std::uint64_t startedAt  = 0;
    std::uint64_t lastDamage = 0;
};

// ConsentRegistry — the duel bookkeeping store (§4.5: stores keyed by the
// opaque id aliases; std::map everywhere, R7). Arena consent needs no
// registry: the doc's shared-id design lives entirely on
// Character.arenaId.
class ConsentRegistry
{
public:
    // ---- pending requests ----

    // Record/replace a request. Replacing is deterministic: target ->
    // challenger is the key, so the same pair refreshes its stamp and a
    // second challenger's request coexists until the target picks one.
    void request(characterId challenger, characterId target, std::uint64_t now);

    // Drop every request touching `id` (the death path and the acceptance
    // path both clean through here).
    void cancelRequestsInvolving(characterId id);

    void cancelRequest(characterId challenger, characterId target);

    [[nodiscard]] std::size_t pendingCount() const noexcept
    {
        return countPending_();
    }
    [[nodiscard]] bool hasRequest(characterId challenger, characterId target) const;

    // ---- active duels ----

    // The duel `id` stands in (either side), or nullptr.
    [[nodiscard]] const ActiveDuel *duelOf(characterId id) const;

    // 3.2's report: mutual damage landed at `now` (either side calls).
    // A character not in a duel is a no-op — the combat layer reports every
    // damage event; only duelists update anything.
    void noteDuelDamage(characterId id, std::uint64_t now);

    // Insert a started duel (acceptDuel's write path; a dueling-overlap is
    // refused by the callers' gates — a duplicate insert is a programmer
    // error, asserted in debug).
    void insertDuel(characterId a, characterId b, std::uint64_t now);

    // Remove the duel `id` stands in (the end paths' write; returns the
    // removed record, or false when there was none).
    [[nodiscard]] bool removeDuel(characterId id, ActiveDuel &removed);

    [[nodiscard]] std::size_t activeCount() const noexcept
    {
        return duels_.size();
    }

    // Id-ascending view (R7) — sweepDuels iterates a copy of it.
    const std::map<characterId, ActiveDuel> &duels() const noexcept
    {
        return duels_;
    }

    // ---- save seam (task 7.1): the pending requests + live duels are sim
    // state (a save mid-duel resumes it); persist/ serializes both maps.
    struct Snapshot
    {
        std::map<characterId, std::map<characterId, std::uint64_t>> pending;
        std::map<characterId, ActiveDuel> duels;
    };
    [[nodiscard]] Snapshot snapshot() const
    {
        return Snapshot{pending_, duels_};
    }
    void restore(
        const Snapshot &saved
    )
    {
        pending_ = saved.pending;
        duels_   = saved.duels;
    }

private:
    [[nodiscard]] std::size_t countPending_() const noexcept;

    // target -> challenger -> requestedAt (R7: nested std::map).
    std::map<characterId, std::map<characterId, std::uint64_t>> pending_;
    std::map<characterId, ActiveDuel> duels_; // keyed by the LOWER id (R7)
};

// ---- the broadcast seam -------------------------------------------------------------

// The consent-broadcast consumer (3.9's witness registry implements it; null
// = nobody records the windows, D-SINK-1). Observers arrive in id-ascending
// order (the spatial grid's sorted query, R7). `at` is the worldTime both
// sides of the window need.
struct IDuelBroadcast
{
    virtual ~IDuelBroadcast() = default;

    // The duel is READY TO FIGHT but its consent fields are NOT yet set —
    // the pinned order (broadcast -> enable damage) makes this callback the
    // one place that can observe the pre-consent world.
    virtual void duelStarted(characterId a, characterId b, std::uint64_t at, const std::vector<characterId> &observers)
        = 0;

    // The consent window closes: fields already cleared when this fires.
    virtual void duelEnded(characterId a, characterId b, std::uint64_t at, const std::vector<characterId> &observers)
        = 0;
};

// ---- the duel flow ------------------------------------------------------------------

enum class DuelError
{
    UnknownCharacter, // challenger or target has no character record
    SelfDuel,         // challenging yourself
    AlreadyDueling,   // either side stands in an active duel already
    NotRequested,     // acceptance without a matching pending request
    NotDueling        // yield from someone not in a duel
};

using DuelResult = Result<void, DuelError>;

// requestDuel — record the challenge. Gates: both characters exist, not
// self, neither side already in an active duel. NPC acceptance policy is
// 6.5's (not wired here); the request simply waits.
DuelResult requestDuel(
    characterId challenger,
    characterId target,
    ConsentRegistry &duels,
    const character::CharacterStore &characters,
    std::uint64_t now
);

// acceptDuel — the target takes the challenge:
//   1. gates (request exists; both exist; neither already dueling),
//   2. gather observers: idsWithinRadius(midpoint, CONSENT_BROADCAST_M)
//      (A-26) — BEFORE the fields flip (the pinned order, tested),
//   3. deliver duelStarted on the broadcast seam (null = skipped),
//   4. set BOTH duelOpponent fields + insert the duel record.
// The order matters because the fields ARE the damage enable: from step 4
// on, 3.2's applyDamage sees consent and records nothing.
DuelResult acceptDuel(
    characterId target,
    characterId challenger,
    ConsentRegistry &duels,
    character::CharacterStore &characters,
    const world::SpatialGrid &grid,
    IDuelBroadcast *broadcast,
    std::uint64_t now
);

// yieldDuel — the yield command: `yielder` concedes. Both fields clear, the
// record drops, duelEnded broadcasts (observers re-gathered at the midpoint
// — the people standing there now are the ones still watching).
DuelResult yieldDuel(
    characterId yielder,
    ConsentRegistry &duels,
    character::CharacterStore &characters,
    const world::SpatialGrid &grid,
    IDuelBroadcast *broadcast,
    std::uint64_t now
);

// endDuelOnDeath — the death path (3.3 calls it): a duel ends when either
// side dies, and every pending request touching the dead character dies with
// them (no posthumous acceptances). The killer is NOT this function's
// concern — the death handlers route XP/justice; this only closes consent.
void endDuelOnDeath(
    characterId victim,
    ConsentRegistry &duels,
    character::CharacterStore &characters,
    const world::SpatialGrid &grid,
    IDuelBroadcast *broadcast,
    std::uint64_t now
);

// sweepDuels — the periodic consent sweep (the tick's step 1 Sweeps, once
// 3.1 wires it; tests call it directly). Two end conditions per duel:
//   * timeout: now - lastDamage >= DUEL_TIMEOUT_S (A-27) — idle duels
//     cannot hold consent open indefinitely;
//   * separation: the duelists' positions sit more than
//     DUEL_MAX_SEPARATION_M apart (planar x/z, R5) — a duel is a duel
//     because both stand in it.
// Duels are visited in id-ascending order; every end clears both fields and
// broadcasts once.
void sweepDuels(
    ConsentRegistry &duels,
    character::CharacterStore &characters,
    const world::SpatialGrid &grid,
    IDuelBroadcast *broadcast,
    std::uint64_t now
);

// ---- arenas -------------------------------------------------------------------------

// One arena zone: the shared-consent circle of one CITY territory (one per
// CITY in v1, card 2.7). The zone id IS the territory id — every occupant
// carries it in Character.arenaId, and 2.1's consentsToPvp reads the shared
// non-zero value as consent.
struct ArenaZone
{
    territoryId territory = 0; // the zone id (R10: opaque; here it IS the territory's)
    Position center{};
    double radiusM = 0.0;
};

// ArenaTable — the zones derived once from data + world: for every CITY-type
// territory whose template declares arena (and whose region anchor exists),
// a zone centered at the territory's region center (regionCenter — the same
// rule 7.2P1's markers use; a template is a blueprint, not a world
// position, so the radius rides the template and the center rides the
// generated territory. DEVIATIONS.md carries the note) with the template's
// arenaRadiusM.
class ArenaTable
{
public:
    ArenaTable() = default;

    // Build from the registry's territory templates + the world graph.
    // Deterministic: territories iterate id-ascending (R7).
    void build(const data::DataRegistry &data, const world::WorldStore &world);

    // The first zone (lowest territory id) containing `p`, or nullptr.
    // Zones may touch in principle; the id order makes the answer
    // deterministic if they ever do.
    [[nodiscard]] const ArenaZone *zoneAt(const Position &p) const;

    [[nodiscard]] const ArenaZone *zoneOfTerritory(territoryId territory) const;

    std::size_t size() const noexcept
    {
        return zones_.size();
    }

private:
    std::map<territoryId, ArenaZone> zones_; // keyed by territory id (R7)
};

// refreshArenaPresence — entering sets arenaId = zoneId, leaving clears it
// (0 = not in an arena, the doc's field default). Called from the movement
// application paths (the player transform after step 9's Move, NPC movement
// in 3.4/3.5) once their wiring lands; tests call it directly. A character
// standing in no zone always leaves with arenaId 0 — the clear is
// unconditional, never a no-op.
void refreshArenaPresence(Character &c, const ArenaTable &arenas);

} // namespace harrowmere::combat

#endif // HARROWMERE_CORE_COMBAT_CONSENT_HPP
