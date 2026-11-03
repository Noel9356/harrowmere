#ifndef HARROWMERE_CORE_NPC_RELATIONSHIPS_HPP
#define HARROWMERE_CORE_NPC_RELATIONSHIPS_HPP

// relationships.hpp — the social graph the justice system reads (task 3.9).
//
// Four things live here, exactly the card's Goal list:
//
//   * AFFINITY — one value per (observer, other) pair in NpcState.affinity
//     (the 3.4 record's own map, already pinned `-SCALE..SCALE (3.9)`). The
//     sources are the card's list: NEAR co-presence (+20 per full game-hour,
//     saturating at +4000 from this source), trade/gift events (+100 each),
//     the same-faction base (+2000), and the ± species bias from territory
//     culture (data: laws.json `cultureBias` per law set; the doc's
//     xenophobic village carries beastkin −6000). The map clamp is
//     [-SCALE, +SCALE] — NOT [0, SCALE]: the witness weight formula
//     multiplies (SCALE - affinity)/SCALE, so a NEGATIVE affinity RAISES the
//     weight (enemies testify harder) and the clamp keeps the term in
//     [0, 2*SCALE] (values > SCALE would zero the weight through max(0, ...)
//     — the card's own pitfall note; the enemy case is pinned by
//     TestAffinity_EnemiesTestifyHarder).
//
//   * IDENTITY KNOWLEDGE — `w.knows(culpritId)`: true iff acquaintance
//     (affinity >= A-54) or a sighting memory recorded at <= A-55 with the
//     culprit unmasked. Lives in perception.hpp (the sightings are
//     perception's store); this file owns the qualification half.
//
//   * WITNESS QUALIFICATION — canTestify(w): alive && sapient && ageYears >=
//     16 (A-44). roles.json's `child` row is documentation of the same rule
//     (its cannotTestify flag restates the age gate for the data readers);
//     the code path reads the age. Integrity is the 0..SCALE trait drawn from
//     the role's integrityRange at spawn (3.6's draw); it never mutates in v1.
//
//   * THE DOC WEIGHT FORMULA — witnessReportWeight pins
//     `max(0, ROLE_BASE_WEIGHT[w.role] * w.integrity / SCALE *
//     (SCALE - affinity) / SCALE)` as a pure function so 4.1's witnessReports
//     reads it through this API (Done-when: no justice-side reimplementation)
//     and the formula direction has a test home before 4.1 lands.
//
// Determinism: every mutation runs on the NpcState.affinity std::map keyed by
// the opaque characterId (R7/R10); all math is integer on the SCALE grid
// (R1/R5). No wall clock — callers pass worldTime (R8).

#include <cstdint>

#include "character/character.hpp" // Character (the doc struct)
#include "core/constants.hpp"      // A-54, the affinity sources, SCALE (A-1)
#include "core/math.hpp"           // clampScalar
#include "core/types.hpp"          // characterId, speciesId, territoryId
#include "npc/state.hpp"           // NpcState (affinity + integrity + ageYears)

namespace harrowmere::data
{
class DataRegistry; // registry.hpp (task 1.3) — role/sapience rows
class SpeciesData;  // species.hpp
} // namespace harrowmere::data

namespace harrowmere
{
class IWorldQueries; // world_context.hpp — the culture-bias walk (D-JUR-1)
}

namespace harrowmere::npc
{

// ---- affinity (the NpcState.affinity map's only mutation surface) -----------

// The map clamp, applied at every source: [-SCALE, +SCALE] on the i64 grid
// (the card's pitfall note — positive-up-to-SCALE is the deliberate span).
std::int64_t clampAffinity(std::int64_t value);

// NEAR co-presence: +AFFINITY_CO_PRESENCE_PER_HOUR per FULL game-hour of
// co-presence (elapsedGameSeconds / GAME_HOUR_SECONDS; a partial hour accrues
// nothing — integer pacing, the needs-decay cadence), saturating at
// AFFINITY_CO_PRESENCE_CAP from this source. The cap is a SOURCE cap, not a
// map cap: proximity can never push a pair past +4000, and never lowers a
// pair that sits above it (trade/faction sources may put it there). A
// negative pair (xenophobia base) accrues upward toward the cap normally.
void accrueCoPresenceAffinity(NpcState &observer, characterId other, std::uint64_t elapsedGameSeconds);

// Trade/gift events (3.10's interactions are the future caller; the weight is
// card-pinned): +AFFINITY_PER_TRADE_EVENT per event. `delta` is signed so a
// future source (a grudge event) needs no new function — R1's signed-delta
// rule; the result clamps to [-SCALE, +SCALE].
void addAffinityEvent(NpcState &observer, characterId other, std::int64_t delta);

// Seed the pair's BASE affinity (first contact): the same-faction base plus
// the species bias term, clamped. A pair that already has an entry is left
// exactly as it is — the base is a creation value, never a recurring grant
// (calling this twice changes nothing).
void seedRelationship(NpcState &observer, characterId other, bool sameFaction, std::int64_t speciesBias);

// ---- species bias (the 2.2 core's NpcState adapter) --------------------------
//
// DEVIATIONS.md 2026-10-31 (task 2.2): speciesBiasToward ships as the
// data-driven core in character/species.cpp; task 3.9 wraps it. The bias
// toward `otherSpecies` as the OBSERVER at `observerTerritory` sees it is
// the observer's species row xenophobiaBias plus the territory's cultureBias
// toward the other species (laws.json, resolved by the 1.4 lawsAt walk the
// world-queries adapter carries, D-JUR-1). A null world reads 0 culture bias
// (the seam's null-false pattern, D-SINK-1).

std::int64_t speciesBiasFor(std::uint64_t observerXenophobiaBias, std::int64_t territoryCultureTowardOther);

std::int64_t
territoryCultureBiasToward(const IWorldQueries *world, territoryId observerTerritory, speciesId otherSpecies);

// ---- witness qualification ---------------------------------------------------

// The 3.9 witness view 4.1's witnessReports consumes (the card's `qualify(w)`
// product): everything the doc weight formula and the identity rule read,
// gathered once. `affinity` is THIS witness's affinity toward `culpritId`
// (the doc's affinity(w, culpritId) — the input the weight formula clamps);
// `knowsIdentity` is filled by the caller from perception.hpp (it needs the
// sightings store + the culprit's appearance version — see
// PerceptionSink::knowsIdentity).
struct Witness
{
    characterId id               = 0;
    std::uint32_t roleBaseWeight = 0;     // ROLE_BASE_WEIGHT[w.role] (roles.json baseReportWeight)
    std::uint64_t integrity      = 0;     // the 0..SCALE spawn trait (NpcState; the player carries 0)
    std::int64_t affinity        = 0;     // clamped [-SCALE, +SCALE]
    bool knowsIdentity           = false; // 3.9's perception answer (caller fills)
    bool canTestify              = false; // alive && sapient && ageYears >= 16 (A-44)
};

// canTestify — the card's gate, verbatim: alive && sapient && ageYears >= 16.
//   * alive: currentHp > 0 — dead characters are REMOVED from the store
//     (3.3), so existence is the caller's pre-check and this is the
//     belt-and-braces half (a 0-HP record never testifies).
//   * sapient: the species row's flag through the registry ("hunting deer
//     creates no crime" — a wolf row is never a witness).
//   * age: NpcState::ageYears(now) >= ADULT_YEARS (A-44). `witnessNpc ==
//     nullptr` is THE PLAYER (no NpcState exists for the SP player, D-STORE-1)
//     — v1 players carry no age, so the player qualifies as an adult; the
//     pinned rule's target (children never testify) is the NPC side.
bool canTestify(
    const Character &witness, const NpcState *witnessNpc, std::uint64_t now, const data::DataRegistry &data
);

// qualify — gather the Witness view for one candidate against one culprit
// (the caller fetched the Character through the store and checked existence;
// a missing NpcState means the player). Role-less or unknown role keys read
// baseReportWeight 0 and integrity 0 (deterministic zeros — the doc formula
// then yields weight 0, the honest "no basis to report" value). The
// affinity term is the witness's map entry toward `culpritId`, 0 when the
// pair has none (a stranger with no history — the formula's neutral center).
Witness qualifyWitness(
    const Character &witness,
    const NpcState *witnessNpc,
    std::uint64_t now,
    const data::DataRegistry &data,
    characterId culpritId
);

// ---- the doc weight formula (design reference §Crime and Evidence, verbatim) --

// weight = max(0, ROLE_BASE_WEIGHT[w.role] * w.integrity / SCALE
//               * (SCALE - affinity(w, culpritId)) / SCALE)
// with affinity clamped to [-SCALE, +SCALE] FIRST (the card's pitfall: the
// clamp is deliberate — a positive affinity REDUCES the willingness to
// incriminate, an enemy's negative affinity doubles the span term). Grouping
// is the doc's left-to-right form; all-integer, no floating point (R5), the
// intermediate products stay far inside u64 (SCALE-bounded terms). The result
// saturates into u32 (the doc's `saturate(...)`; the worst case is 2*SCALE).
std::uint32_t witnessReportWeight(std::uint32_t roleBaseWeight, std::uint64_t integrity, std::int64_t affinity);

} // namespace harrowmere::npc

#endif // HARROWMERE_CORE_NPC_RELATIONSHIPS_HPP
