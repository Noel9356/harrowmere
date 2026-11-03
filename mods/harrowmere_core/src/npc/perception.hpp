#ifndef HARROWMERE_CORE_NPC_PERCEPTION_HPP
#define HARROWMERE_CORE_NPC_PERCEPTION_HPP

// perception.hpp — task 3.9's perception half: sighting memories (identity
// knowledge) and the live implementation of the perceive→report seam
// (combat/justice_sink.hpp's IPerceptionSink, stubbed empty in Phase 2).
//
// What lands here, per the card:
//
//   * SIGHTINGS — the store `{culpritId, appearanceVersion, worldTime}` per
//     (observer, culprit) pair. A sighting is recorded when the observer sees
//     the culprit at <= IDENTIFY_RANGE_M (A-55, 15 m) and the culprit is not
//     MASKED (the suspicion act active) at sighting time — a masked stranger
//     leaves no memory (pinned by TestKnows_MaskedStrangerNo). The
//     appearance version is 2.6's change count at sighting time (the
//     AppearanceHistory version helpers): a sighting keyed to an older
//     version no longer matches the current look — "disguises defeat
//     strangers, never acquaintances".
//
//   * IDENTITY KNOWLEDGE — `w.knows(culpritId)`: true iff (a) acquaintance:
//     the observer's affinity toward the culprit >= ACQUAINTANCE_AFFINITY
//     (A-54 = 2000) — matches by id regardless of any appearance change and
//     never lapses; or (b) sighting memory: a recorded sighting of the
//     culprit whose version still matches the culprit's current look and
//     whose last (re)sighting stamp is within STRANGER_MEMORY_S (A-56, 10
//     game-min). Stranger memory persists while the observer remains within
//     sight of the culprit (NEAR-tier co-presence — the driver keeps
//     re-recording, which refreshes the stamp) and lapses once they have
//     lost sight for the window.
//
//   * THE SEAM — PerceptionSink implements IPerceptionSink over 1.6's
//     spatial grid: perceiveWithin(position, radius) answers the spatial
//     half ("who is there to perceive") with the ALIVE characters in radius,
//     ids ascending (R7). Qualification (canTestify/integrity/affinity — the
//     social half) stays with 3.9's relationships API so 4.1's pinned loop
//     (`ctx.grid.idsWithinRadius` + `qualify(w)`) reads both halves through
//     this task, never a justice-side reimplementation.
//
// Persistence shape (§4.5): the store is plain data in R7 containers keyed by
// opaque ids — serializable in principle; Phase 4's 7.1 wires the save format
// without re-plumbing this store.

#include <cstdint>
#include <map>
#include <vector>

#include "character/character.hpp" // CharacterStore lives in character/store.hpp; Character here
#include "character/store.hpp"     // CharacterStore (the alive/existence reads)
#include "combat/justice_sink.hpp" // IPerceptionSink (the seam this implements)
#include "core/constants.hpp"      // A-54/A-55/A-56
#include "core/types.hpp"          // characterId, Position
#include "npc/state.hpp"           // NpcState (the affinity half of knows())
#include "npc/relationships.hpp"   // Witness (the qualification product)
#include "world/spatial_grid.hpp"  // 1.6's index — the one spatial-query path

namespace harrowmere::data
{
class DataRegistry; // registry.hpp (task 1.3)
} // namespace harrowmere::data

namespace harrowmere::character
{
class AppearanceHistory; // appearance.hpp (2.6) — the version source
} // namespace harrowmere::character

namespace harrowmere::npc
{

// One recorded sighting (card 3.9's store shape, persistence-shaped §4.5).
struct Sighting
{
    characterId culpritId           = 0;
    std::uint64_t appearanceVersion = 0; // the culprit's look version at sighting time (2.6)
    std::uint64_t worldTime         = 0; // the last (re)sighting stamp — the stranger-memory clock
};

class PerceptionStore
{
public:
    // Record/refresh the observer's sighting of one culprit. A MASKED
    // culprit records NOTHING — not a new memory, and not a refresh of an
    // existing one (the observer cannot re-identify a face they cannot see;
    // the previous unmasked sighting stands and its clock keeps running).
    // `worldTime` is the sighting stamp (R8: the sim clock, never the wall
    // clock); co-presence re-records keep it fresh, which is exactly the
    // card's "persists while the observer remains within sight".
    void recordSighting(characterId observer, const Sighting &sighting, bool culpritMasked);

    // The observer's sighting of `culpritId`, or nullptr (a read view — the
    // store owns the storage; callers copy, never hold across mutations).
    const Sighting *sightingOf(characterId observer, characterId culpritId) const;

    // w.knows(culpritId) — the card's rule, verbatim:
    //   (a) acquaintance: affinity >= ACQUAINTANCE_AFFINITY (A-54) — by id,
    //       appearance-proof, lapse-proof;
    //   (b) sighting memory: a recorded sighting whose appearanceVersion
    //       still equals the culprit's CURRENT version (`culpritVersionNow`)
    //       and whose stamp is within STRANGER_MEMORY_S of `now` (lapsed at
    //       strictly-more; the boundary second still knows).
    // `observer` is the observer's own id (the sightings map's key — NpcState
    // deliberately carries no id, D-STORE-1 keeps it a sidecar record);
    // `observerState` is that id's NpcState (the affinity half's source).
    // `culpritVersionNow` is the caller's version read (0 when no
    // AppearanceHistory is bound — a world without appearance changes never
    // invalidates a sighting).
    bool knowsIdentity(
        characterId observer,
        const NpcState &observerState,
        characterId culpritId,
        std::uint64_t culpritVersionNow,
        std::uint64_t now
    ) const;

    // Deterministic iteration over one observer's memories, culprit-id
    // ascending (R7) — the save writer's (7.1) and the tests' read path.
    const std::map<characterId, Sighting> &sightingsOf(characterId observer) const;

    // The whole memory (read view — 4.1's missing-persons sweep and the save
    // writer scan it; observer-id ascending, R7).
    const std::map<characterId, std::map<characterId, Sighting>> &sightingsAll() const
    {
        return sightings_;
    }

    void clearFor(
        characterId observer
    )
    {
        sightings_.erase(observer);
    }

private:
    // observer -> culprit -> sighting (R7 maps; one memory per pair — the
    // latest look the observer last identified the culprit wearing).
    std::map<characterId, std::map<characterId, Sighting>> sightings_;
};

// The live perceive seam (task 3.9 implements; the plugin binds it into
// WorldContext::perception — 4.1's crime pipeline and 4.5's suspicion loop
// consume it). Binding is pointer-shaped like every store seam (the stores
// outlive the sink; unbound pointers read as the empty answer, D-SINK-1).
class PerceptionSink final : public IPerceptionSink
{
public:
    // The runtime wiring (the plugin calls this once at wire time).
    void bind(
        const world::SpatialGrid *grid,
        const character::CharacterStore *characters,
        const NpcStore *npcs,
        const data::DataRegistry *data
    );
    void bindAppearanceHistory(const character::AppearanceHistory *history);
    void bindWorldTime(
        std::uint64_t now
    )
    {
        now_ = now;
    } // R8: the clock step's value

    // The store (sightings are sim state — 4.5's drivers record through it).
    PerceptionStore &store()
    {
        return store_;
    }
    const PerceptionStore &store() const
    {
        return store_;
    }

    // ---- the seam (IPerceptionSink) ------------------------------------------
    //
    // perceiveWithin — the spatial half: every ALIVE character within
    // `radiusMeters`, ids ascending (the grid's R7-sorted output, existence-
    // and HP-filtered). Qualification (sapience, age, knowledge) is the
    // caller's next step through 3.9's relationships API — the card's
    // witness loop qualifies per candidate, and the player (no NpcState)
    // must stay in the candidate list for 4.1's player-witness handling.
    std::vector<characterId> perceiveWithin(const Position &position, double radiusMeters) override;

    // knowsIdentity — 4.1's identity half through the seam: observers with an
    // NpcState run the full affinity+sighting rule; the player (no NpcState)
    // runs the sighting-only answer below.
    bool knowsIdentity(characterId observer, characterId culprit) override;

    // lastSightingOf — 4.1's missing-persons sweep input: the freshest
    // sighting stamp of `who` across every observer's memory (0 = none
    // recorded). A daily-cadence caller; the O(observers) scan is honest.
    std::uint64_t lastSightingOf(characterId who) override;

    // ---- the composed identity answer (the report half's input) ---------------

    // The culprit's current appearance version through the bound history
    // (0 without one — no recorded changes means one permanent look).
    std::uint64_t culpritAppearanceVersionNow(characterId culprit) const;

    // knows(observer, culprit): the PerceptionStore rule with the bound
    // version source and clock — the one call 4.1's witnessReports needs.
    bool knowsIdentity(characterId observer, const NpcState &observerState, characterId culprit) const;

    // knows for a candidate with no NpcState (the player observer): the
    // player keeps no affinity map in v1, so the answer is the sighting half
    // only — evaluated against the PLAYER's own sighting rows (the player
    // records sightings through the same store when a future driver covers
    // them; with none recorded the answer is false, the honest zero-value).
    bool knowsIdentityNoAffinity(characterId observer, characterId culprit) const;

private:
    const world::SpatialGrid *grid_              = nullptr;
    const character::CharacterStore *characters_ = nullptr;
    const NpcStore *npcs_                        = nullptr;
    const data::DataRegistry *data_              = nullptr;
    const character::AppearanceHistory *history_ = nullptr;
    std::uint64_t now_                           = 0;
    PerceptionStore store_;
};

} // namespace harrowmere::npc

#endif // HARROWMERE_CORE_NPC_PERCEPTION_HPP
