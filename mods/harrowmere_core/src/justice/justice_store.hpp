#ifndef HARROWMERE_CORE_JUSTICE_JUSTICE_STORE_HPP
#define HARROWMERE_CORE_JUSTICE_JUSTICE_STORE_HPP

// justice_store.hpp — the JusticeStore (§4.5): ONE storage truth for the
// justice layer's state, keyed by the opaque id aliases (R10).
//
// The docs' per-territory structures map here exactly as common.md §4.5
// lists them: the per-territory evidence map (insertion-ordered vectors —
// archived cases are REMOVED, R7), the two doc-verbatim warrant maps
// (task 4.2), the per-character reputation records (task 4.4) and the
// jailUntil map (task 4.3 — jail state lives HERE, never on Character, the
// doc's own rule). Task 4.1's side state (the assault accumulator and the
// MISSING case flags) rides the same store so every justice read/write goes
// through one object and the save schema (7.1) serializes one place.
//
// Id spaces (R10): the "evidence" and "warrant" counters draw from one
// IdGenerator (0.2) — two independent named spaces starting at FIRST_ID = 1,
// monotonic, never reused, serialized with the save (7.1).
//
// Threading: none — the sim is single-threaded (§4.0). Determinism: every
// map is std::map keyed by opaque ids (R7).

#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "core/types.hpp" // characterId, territoryId, IdGenerator

namespace harrowmere::justice
{

struct Evidence;   // evidence.hpp (4.1)
struct Warrant;    // warrants.hpp (4.2)
struct Reputation; // reputation.hpp (4.4)

// One MISSING case flag (4.1's delayed-report list, entry 2): a sapient NPC
// with no sighting for MISSING_DAYS (A-64) flags their territory; when found
// dead the matching evidence gains MISSING_WEIGHT (A-65).
struct MissingPerson
{
    territoryId territory  = 0; // the territory that carries the MISSING flag
    std::uint64_t markedAt = 0; // worldTime the flag was raised (the sweep's stamp)
};

class JusticeStore
{
public:
    JusticeStore() = default;

    JusticeStore(const JusticeStore &)            = delete;
    JusticeStore &operator=(const JusticeStore &) = delete;

    // ---- id spaces (R10) -------------------------------------------------------
    //
    // The evidence/warrant counters live here (the doc's `ctx.ids` read on the
    // justice path). Each named space draws independently from FIRST_ID = 1.
    std::uint64_t nextEvidenceId()
    {
        return ids_.nextEvidence();
    }
    std::uint64_t nextWarrantId()
    {
        return ids_.nextWarrant();
    }

    // The counters' current values (the save layer's and the tests' read).
    std::uint64_t peekEvidenceId() const
    {
        return ids_.peek("evidence");
    }
    std::uint64_t peekWarrantId() const
    {
        return ids_.peek("warrant");
    }

    // ---- save seam (task 7.1) ------------------------------------------------
    //
    // The full counter state (the "evidence" + "warrant" spaces) as one
    // IdGenerator snapshot: the save's ids block carries it; the load path
    // hands it back through loadIdState (never rewinds a live generator, the
    // IdGenerator::loadState contract).
    [[nodiscard]] std::map<std::string, std::uint64_t> idState() const
    {
        return ids_.state();
    }
    void loadIdState(
        const std::map<std::string, std::uint64_t> &saved
    )
    {
        ids_.loadState(saved);
    }

    // ---- the doc maps (§4.5; the card pseudocode reads these verbatim) --------

    // 4.1: `evidenceStore` — per-territory case lists, INSERTION-ORDERED (R7:
    // the vector preserves commit order; map iteration is territory-id
    // ascending). Archived cases are erased from the vector (archiveEvidence).
    // Note: common.md §4.5 names this shape `evidence_by_territory`; the card
    // pseudocode (design reference verbatim) reads `evidenceStore` — the
    // card's spelling wins for the code.
    std::map<territoryId, std::vector<Evidence>> evidenceStore;

    // 4.2, design reference verbatim: `warrants` by id +
    // `activeWarrantsByCulprit` (the per-culprit index; every mutation keeps
    // BOTH maps in lockstep — the doc invariant the tests pin).
    std::map<std::uint64_t, Warrant> warrants;
    std::map<characterId, std::set<std::uint64_t>> activeWarrantsByCulprit;

    // 4.4: per-character trust records (lazy decay on every read).
    std::map<characterId, Reputation> reputations;

    // 4.3: jailUntil — worldTime each character serves until (isArrested
    // purges expired entries lazily on read). Lives HERE, never on Character
    // (the doc's own note; the doc struct carries no jail field).
    std::map<characterId, std::uint64_t> jailUntil;

    // ---- 4.1's side state --------------------------------------------------------

    // noteAssault's accumulator: (culprit -> victim -> applied damage since
    // the last recorded ASSAULT). Crossing ASSAULT_THRESHOLD records the
    // evidence and resets the pair.
    std::map<characterId, std::map<characterId, std::uint64_t>> assaultDamage;

    // The MISSING case flags (victim id -> the flag record).
    std::map<characterId, MissingPerson> missingPersons;

    // A-76's pacing map for runCaseTicks: evidenceId -> worldTime of the
    // case's last updateEvidence (one update per case per game-hour). Not doc
    // state — the Evidence struct stays design-reference verbatim, so the
    // cadence bookkeeping rides here (7.1 may treat it as do-not-save pacing).
    std::map<std::uint64_t, std::uint64_t> caseLastTick;

    // ---- 4.6's side state ----------------------------------------------------
    //
    // The signal-before-force invariant's memory: every culprit the
    // Investigated/Wanted signals have ever fired for (attemptArrest refuses
    // without one — the doc's "never arrest with zero prior signals").
    std::set<characterId> signalledCulprits;

    // The posted wanted notices: territory -> the worldTime the poster went
    // up at the territory center (7.3b's panel reads the map; the save
    // schema extends with it).
    std::map<territoryId, std::uint64_t> posters;

    // ---- 4.7's side state ----------------------------------------------------

    // The capture-fatigue stamps: culprit -> the worldTimes of their alive
    // captures (each capture within CAPTURE_FATIGUE_WINDOW_DAYS, A-99, of
    // the previous one compounds the CAPTURE_FATIGUE_FACTOR, A-100).
    std::map<characterId, std::vector<std::uint64_t>> captureStamps;

    // The village-slaughter markers: culprit -> territory -> the worldTime
    // the marker rose (the corpse loot currency zeroes while it stands; the
    // casus belli fired once at the crossing).
    std::map<characterId, std::map<territoryId, std::uint64_t>> massacreMarkers;

    // 4.7's kill tally (the massacre window's source): culprit ->
    // (territory, worldTime) rows of sapient NPC kills, pruned to the
    // MASSACRE_WINDOW_DAYS (A-102) window on every note.
    std::map<characterId, std::vector<std::pair<territoryId, std::uint64_t>>> killTally;

private:
    IdGenerator ids_{}; // the "evidence" + "warrant" spaces (R10)
};

} // namespace harrowmere::justice

#endif // HARROWMERE_CORE_JUSTICE_JUSTICE_STORE_HPP
