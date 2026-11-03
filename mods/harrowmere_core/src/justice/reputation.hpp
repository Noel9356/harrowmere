#ifndef HARROWMERE_CORE_JUSTICE_REPUTATION_HPP
#define HARROWMERE_CORE_JUSTICE_REPUTATION_HPP

// reputation.hpp — task 4.4: per-territory trust + the global notoriety
// record, with LAZY decay ("lazy decay replaces global ticks" — there is no
// periodic reputation tick anywhere; every update and read decays first).
//
// Consumers (the card's list): conviction (4.3's debt multiplier + rep loss),
// prices (3.7's hook), greetings/enforcement (4.6's tone + shadowing),
// militias (4.7's odds). All of them read through THIS file's functions —
// nobody walks the reputation map directly.
//
// Semantics pinned by the card (design reference verbatim):
//   * decayIfDue — the lazy quantum: whole REPUTATION_DECAY_INTERVAL (A-84,
//     1 game-day) steps since the last decay apply at once; per-territory
//     values move TOWARD ZERO by REPUTATION_DECAY (A-85, 500) per step — the
//     sign-aware term (negative values decay UP; pinned by
//     TestRep_DecaySignAware) — and notoriety drops by NOTORIETY_DECAY
//     (A-86, 1000) per step, floored at 0 (TestNotoriety_FloorsAtZero).
//   * changeReputation — the propagation walk over 1.4's chainToRoot: the
//     ORIGIN territory first (delta at full weight), then each ancestor at
//     REPUTATION_PROPAGATION (A-88, 0.5x) per level — "crime tourism works
//     locally, dies kingdom-wide" (TestRep_PropagationHalvesPerLevel). Every
//     level clamps to ±MAX_REPUTATION (A-87) on the i64 grid before storing
//     back into the i32 map (R1's signed-delta rule).
//   * Readers decay-before-read (Done-when): getNotoriety,
//     getReputationIn, getReputationMultiplier all run decayIfDue on the
//     record they read (TestRep_LazyDecayCatchUp: 10 idle days apply 10
//     steps exactly once).
//
// Storage: `reputations` lives on the JusticeStore (§4.5). The walk reads the
// territory tree through WorldContext::worldStore (1.4's chainToRoot — D-JUR-1:
// no system reimplements a parent-chain walk).

#include <cstdint>
#include <map>

#include "core/types.hpp" // characterId, territoryId

namespace harrowmere
{
struct WorldContext;
} // namespace harrowmere

namespace harrowmere::justice
{

class JusticeStore; // justice_store.hpp — forward: the functions below take the store

// The doc's Reputation record, verbatim (design reference §Reputation).
struct Reputation
{
    std::map<territoryId, std::int32_t> perTerritory;
    std::int32_t notoriety      = 0;
    std::uint64_t lastDecayTime = 0;
};

// decayIfDue — design reference verbatim (see the header doc). Safe at
// now < lastDecayTime (a rewind: zero steps apply, the stamp stands — no
// underflow, R1).
void decayIfDue(Reputation &rep, std::uint64_t now);

// changeReputation — the doc's verbatim walk (origin included; see above).
// Debug-asserts the context carries the justice store + the world walk;
// release no-ops deterministically (the D-SINK-1 recovery pattern).
void changeReputation(characterId c, territoryId origin, std::int32_t delta, WorldContext &ctx);

// ---- readers (all decay-before-read) ------------------------------------------

// The global notoriety value (conviction adds crime_data.notorietyGain via
// addNotoriety below; caseThreshold divides by SCALE + notoriety).
std::int32_t getNotoriety(JusticeStore &justice, characterId c, std::uint64_t now);

// The per-territory trust value (0 where the pair has no record — the honest
// zero-value rule; a stranger is neutral, never hated).
std::int32_t getReputationIn(JusticeStore &justice, characterId c, territoryId territory, std::uint64_t now);

// D-REP-2 (4.3's conviction multiplier): SCALE + max(0, -rep) * 2000 / SCALE
// — a sinner's debt grows by up to +20% at MAX_REPUTATION-negative; a neutral
// or liked reputation multiplies by exactly SCALE. `territory` is the
// EVIDENCE territory (4.3's own rule for the zone multiplier; the rep
// multiplier reads the same one — pinned by TestConvict_DebtMathDocExact).
std::uint64_t getReputationMultiplier(JusticeStore &justice, characterId c, territoryId territory, std::uint64_t now);

// ---- writers ---------------------------------------------------------------------

// addNotoriety — conviction's tail (4.3): crime_data.notorietyGain onto the
// record (i64 math, floored at +i32 max; the value only ever grows here —
// decayIfDue owns the way down). No decay run: the writer runs mid-mutation;
// the next reader decays.
void addNotoriety(JusticeStore &justice, characterId c, std::int32_t gain);

} // namespace harrowmere::justice

#endif // HARROWMERE_CORE_JUSTICE_REPUTATION_HPP
