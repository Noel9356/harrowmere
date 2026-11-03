#ifndef HARROWMERE_CORE_JUSTICE_CASES_HPP
#define HARROWMERE_CORE_JUSTICE_CASES_HPP

// cases.hpp — task 4.2's case half: the case state machine from UNKNOWN to
// ARCHIVED/CONVICTED, the score/threshold math, the daily backstop sweep and
// the player-near case ticks.
//
// Pinned here (design reference verbatim):
//   * caseScore — physical + all report weights + the aggregate, damped by
//     the territory's suppression for the culprit's social class (D-CORRUPT-1;
//     SCALE = none). The multiply widens to u64 BEFORE the division (R3).
//   * caseThreshold — the territory's conviction threshold divided by
//     (SCALE + notoriety)/SCALE: repeat offenders convict on weaker evidence
//     (4.4's read; R3 widen again).
//   * hasIdentity — physical >= PHYSICAL_IDENTITY_MIN (A-70) OR any report
//     from an NPC source that knowsIdentity. Player self-reports and ward
//     reports (source 0) NEVER satisfy identity outside safe zones (the
//     TestIdentity_OnlyNpcReportsCount pin).
//   * updateEvidence — the per-case step: CONVICTED immune; WANTED without a
//     live warrant downgrades FIRST (expired warrant = pause, not eraser —
//     the doc order: before the expiry check, or expired-warrant cases would
//     archive a tick early); past LIMIT_DAYS (A-71) archives; score >=
//     threshold + identity issues the warrant; score >= threshold /
//     INVESTIGATE_DIVISOR (A-72) investigates.
//   * Case ticks run ONLY for territories with the player within
//     CASE_TICK_RADIUS_M (A-75 = 500 m); sweepExpiredEvidence is the daily
//     global backstop. One updateEvidence per case per game-hour (A-76) —
//     the pacing map rides the JusticeStore (evidenceId -> last tick).

#include <cstdint>

#include "core/enums.hpp" // CrimeType, SocialClassId (1.1's closed lists)
#include "core/types.hpp" // characterId, territoryId
#include "justice/justice_store.hpp"

namespace harrowmere::data
{
struct LawSet; // laws.hpp (1.2) — the registry-owned law set; reference only
} // namespace harrowmere::data

namespace harrowmere::world
{
struct CompactRecord; // territory.hpp (1.4) — the archive row; returned by value
} // namespace harrowmere::world

namespace harrowmere
{
struct WorldContext;
} // namespace harrowmere

namespace harrowmere::justice
{
struct Evidence; // evidence.hpp — the complete type; pointer/reference only here
} // namespace harrowmere::justice

namespace harrowmere::justice
{

// ---- the score math (design reference verbatim) --------------------------------

std::uint32_t caseScore(const Evidence &e, WorldContext &ctx);

// caseThreshold — the territory's conviction threshold for the crime, scaled
// down by the culprit's notoriety (4.4's read, decayed first).
std::uint32_t caseThreshold(const Evidence &e, WorldContext &ctx);

// hasIdentity — A-70's physical arm or an NPC report that knowsIdentity.
bool hasIdentity(const Evidence &e, WorldContext &ctx);

// suppressionFor — D-CORRUPT-1's lookup: the LawSet's suppression value for
// (class, crime); SCALE (none) when the set does not damp the pair.
std::uint32_t suppressionFor(const data::LawSet &laws, SocialClassId cls, CrimeType type);

// findEvidence — the live case with this id (territory-id ascending, insertion
// order within a territory; nullptr when absent). Surrender (4.3) resolves
// warrants' underlying cases through it.
Evidence *findEvidence(JusticeStore &justice, std::uint64_t evidenceId);

// ---- the case state machine -------------------------------------------------------

// updateEvidence — one case tick (see the header doc for the pinned order).
void updateEvidence(Evidence &e, WorldContext &ctx);

// issueWarrant lives in warrants.hpp (the two-maps invariant is its home).

// archiveEvidence — design reference verbatim: the case leaves the store and
// lands in the territory's archive as a CompactRecord (1.4's POD row, layout
// finalized in 4.1; burning the archive destroys these — 6.3's hook).
void archiveEvidence(Evidence &e, WorldContext &ctx);

// compactRecord — the identification row for the territory archive.
world::CompactRecord compactRecord(const Evidence &e);

// sweepExpiredEvidence — the daily backstop (step 1): archive every
// un-wanted, past-window case everywhere; WANTED cases with a live warrant
// are skipped (the warrant's own expiry path owns them). Iterates a COPY of
// each territory's case list (archiveEvidence erases mid-walk).
void sweepExpiredEvidence(WorldContext &ctx);

// runCaseTicks — the step-6 entry: for each territory with the player within
// A-75, one updateEvidence per case per game-hour (A-76; the per-case pacing
// map lives on the JusticeStore). Deterministic: territory-id ascending, case
// insertion order within a territory (R7).
void runCaseTicks(WorldContext &ctx);

} // namespace harrowmere::justice

#endif // HARROWMERE_CORE_JUSTICE_CASES_HPP
