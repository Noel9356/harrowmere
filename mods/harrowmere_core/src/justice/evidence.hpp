#ifndef HARROWMERE_CORE_JUSTICE_EVIDENCE_HPP
#define HARROWMERE_CORE_JUSTICE_EVIDENCE_HPP

// evidence.hpp — task 4.1: `recordCrime` and everything it implies — atomic
// evidence creation, witness scoring, report capping, safe-zone ward reports,
// delayed reports, bribery, accomplices, victim self-reports.
//
// Types are verbatim from the design reference (the card's Spec block):
//   * Report  { source, weight, knowsIdentity } — source 0 = ward/anonymous;
//     `bribed` is 4.1's own flag (the doc's bribery rule: one halving, then a
//     second attempt on the same report auto-fails).
//   * Evidence — the per-case record. `territoryId` is the SNAPSHOT taken at
//     commit and NEVER recomputed (the doc note; TestEvidence_
//     TerritorySnapshotNeverRecomputed pins it). `status` rides the closed
//     EvidenceStatus list (1.1).
//   * JusticeStore::evidenceStore — map<territoryId, vector<Evidence>>,
//     insertion-ordered (R7).
//
// The hard rules this module enforces (each with a pinned test):
//   * Only PLAYERS generate evidence (recordCrime's first gate).
//   * The species-exemption hard gate returns BEFORE constructing the
//     Evidence — order matters for exploits (evidence-then-exempt would still
//     leak into archives).
//   * Nothing about a crime is visible to the player at the act (the doc's
//     rule) — recordCrime publishes CrimeRecorded (a fact, 0.7 rule 1) and
//     touches no UI surface.
//   * Witness weight reads ONLY through 3.9's API (qualifyWitness +
//     witnessReportWeight) and identity knowledge ONLY through the perception
//     seam — no justice-side reimplementation (the Done-when).

#include <cstdint>
#include <vector>

#include "combat/justice_sink.hpp" // CrimeContext + the IPerceptionSink seam (the perceive→report door)
#include "core/enums.hpp"          // CrimeType, EvidenceStatus (1.1's closed lists)
#include "core/types.hpp"          // characterId, territoryId, Position
#include "justice/justice_store.hpp"

namespace harrowmere
{
struct Character;
struct WorldContext;
} // namespace harrowmere

namespace harrowmere::justice
{

// ---- the doc structs (design reference verbatim + the pinned extension) ----

struct Report
{
    characterId source   = 0; // 0 = ward/anonymous (the doc's struct comment)
    std::uint32_t weight = 0; // 0..2*SCALE (the doc formula's span)
    bool knowsIdentity   = false;
    bool bribed          = false; // 4.1's bribery flag: halved once, then auto-fail
};

struct Evidence
{
    std::uint64_t id        = 0;
    characterId culpritId   = 0;
    characterId victimId    = 0;
    territoryId territory   = 0; // the doc's `territoryId territoryId` field (the member drops
                                 // the type-name clash -Wchanges-meaning rejects); snapshot at
                                 // commit, NEVER recomputed (the doc note)
    std::uint64_t worldTime = 0;
    CrimeType type          = CrimeType::MURDER;
    EvidenceStatus status   = EvidenceStatus::UNKNOWN;
    std::vector<Report> reports;
    std::uint32_t physicalScore         = 0;
    std::uint32_t aggregateReportWeight = 0; // capReports merges the excess here
};

// ---- the crime-commit pipeline -------------------------------------------------

// recordCrime — the doc-verbatim entry (design reference §Crime and Evidence).
// Order is load-bearing: player gate -> territory snapshot -> exemption HARD
// gate (before any Evidence exists) -> witness reports -> physical score ->
// the ward report in safe zones -> capReports -> store + publish.
void recordCrime(CrimeType t, const Character &culprit, const Character &victim, const Position &p, WorldContext &ctx);

// witnessReports — the doc's witness loop over the perception seam: every
// qualified witness within WITNESS_RADIUS (A-60) reports with the 3.9 weight
// formula; identity knowledge rides the seam's knowsIdentity.
std::vector<Report> witnessReports(characterId culprit, const Position &p, WorldContext &ctx);

// capReports — design reference verbatim: keep the heaviest REPORT_CAP (A-61)
// reports, STABLY sorted (equal weights keep insertion order — R7); the
// excess merges into aggregateReportWeight.
void capReports(Evidence &e);

// scorePhysical — D-PHYS-2 (pinned v1): PHYS_BASE (A-62) + the scene markers
// that have state sources, clamped to [0, SCALE]. The marker table (each
// weight is a named A-row-adjacent constant; the dormant ones name their
// future source — see the .cpp):
//   bloodstained attacker (the suspicion act active) +2000 — the act surface
//     rides ctx.playerActions (no writer in v1: reads false — DEVIATIONS row);
//   forced entry / private-property trespass context +1500 — the property
//     flag is 4.5/6.3 data (dormant in v1);
//   arson: burning structure +4000 — the fire state is event-engine (5.x)
//     data (dormant in v1);
//   theft with a guardMarked item +3000 — fired by holdsGuardGearTheft (the
//     one marker with a live v1 source);
//   night +500 — integer clock math (R5) on clock.hpp's documented anchors.
// `guardMarkedTheft` defaults false so every doc call site keeps the
// doc-verbatim two-argument shape.
std::uint32_t scorePhysical(const Position &p, CrimeType type, WorldContext &ctx, bool guardMarkedTheft = false);

// noteAssault — JusticeSink::noteAssault's implementation (3.2's hook): the
// per-(culprit, victim) accumulator crosses ASSAULT_THRESHOLD -> the ASSAULT
// evidence records at the victim's position and the pair resets.
void noteAssault(characterId culprit, characterId victim, std::uint64_t appliedDamage, WorldContext &ctx);

// ---- delayed reports (the design reference's list) -------------------------------

// (1) Discovered bodies: JusticeSink::onBodyDiscovered's implementation —
// a follow-up Report{discoverer, DELAYED_BODY_WEIGHT (A-63), knowsIdentity
// per the seam} appended to the ORIGINAL murder evidence when one exists and
// is not ARCHIVED.
void appendDiscoveredBodyReport(characterId victim, characterId discoverer, WorldContext &ctx);

// (2) Missing persons: the MISSING case flag. markMissingPerson raises the
// flag (the sweep or a caller stamps it); sweepMissingPersons evaluates the
// doc's condition per sapient NPC (no sighting for MISSING_DAYS, A-64) and
// clears flags for people seen again; appendMissingWeight applies A-65 onto
// the matching murder evidence when the person is found dead (the
// onBodyDiscovered path). The runtime driver that calls the sweep joins with
// the 4.5 perception loop (nothing records sightings per tick yet —
// DEVIATIONS row).
void markMissingPerson(characterId victim, territoryId territory, WorldContext &ctx);
void sweepMissingPersons(WorldContext &ctx);

// (3) Marked stolen items at vendors: the seller's most recent open THEFT
// evidence gains the anonymous VENDOR_MARK_WEIGHT (A-66) report — score,
// never identity (mirrors the doc's aggregate-only rule). The vendor-side
// call joins with 3.7's transaction path (out of these cards).
void reportMarkedSale(characterId seller, WorldContext &ctx);

// ---- bribery (doc: "successful bribery halves report weight once; failure
// adds an obstruction report identifying the briber") ------------------------------

enum class BriberyOutcome
{
    NoReport, // the witness holds no active report on the briber (nothing to bribe)
    Bribed,   // the roll succeeded: that report's weight halved once (bribed flag set)
    Failed    // the roll failed: a fresh OBSTRUCTION evidence carries the briber's report
};

// attemptBribery — interaction (3.10) on a witness holding an active report
// on a WANTED-or-earlier case of the briber. Success chance =
// clamp(PERSUASION_eff * 2 - integrity, 500, 9000) drawn from the pinned
// "justice" stream; a second attempt on an already-bribed report
// auto-fails... as NoReport (the bribe surface is gone).
BriberyOutcome attemptBribery(characterId briber, characterId witness, WorldContext &ctx);

// ---- accomplices ("players aiding culprits earn accomplice evidence") ---------

// recordAccomplice — the doc's pinned v1 trigger set (heal-on-WANTED-culprit,
// travel-together-into-jail-radius, selling flagged-stolen gear) routes here:
// evidence vs the HELPER (type SMUGGLING for aid, or the caller's via-type),
// witnessed at the aid position like any scene. "Watching is never punished"
// — no trigger exists for mere presence; nothing here may add one.
void recordAccomplice(
    characterId helper, characterId aidedCulprit, CrimeType via, const Position &p, WorldContext &ctx
);

// ---- victim self-report --------------------------------------------------------------

// selfReportVictim — the player victim's post-respawn report (within
// SELF_REPORT_WINDOW_DAYS, A-68, of the death): Report{victimId,
// SELF_REPORT_WEIGHT (A-69), knowsIdentity=false} appended to the matching
// murder/assault evidence. Adds score, NEVER satisfies identity (4.2's
// hasIdentity counts NPC reports only). True when a report landed.
bool selfReportVictim(characterId victim, WorldContext &ctx);

// ---- the shared commit path --------------------------------------------------------

// commitEvidence — the ONE construction path every justice evidence takes
// (recordCrime, accomplices, the escape): id + the snapshot (the territory
// taken at commit, NEVER recomputed) + status UNKNOWN + capped reports +
// physical score, stored at the snapshot territory, then published as
// CrimeRecorded (0.7 rule 1 — the fact, after the mutation). Returns the
// stored copy (the id rides it; the store's vector is the truth).
Evidence commitEvidence(
    CrimeType t,
    characterId culpritId,
    characterId victimId,
    territoryId terr,
    WorldContext &ctx,
    std::vector<Report> reports,
    std::uint32_t physicalScore
);

// ---- the real JusticeSink (the plugin swaps this in — combat/justice_sink.hpp) —

class JusticeSystem final : public JusticeSink
{
public:
    // The runtime wiring (the plugin binds once; the sink is a thin router —
    // every rule lives in the free functions above).
    void bind(
        WorldContext *ctx
    )
    {
        ctx_ = ctx;
    }

    void recordCrime(const CrimeContext &crime) override;
    void noteAssault(characterId culprit, characterId victim, std::uint64_t applied) override;
    bool hasWarrantIn(characterId suspect, territoryId territory) const override;
    bool hasLethalWarrantIn(characterId suspect, territoryId territory) const override;
    void holdsGuardGearTheft(characterId culprit, const Position &where) override;
    bool isArrested(characterId who) const override;
    void onBodyDiscovered(characterId victim, characterId discoverer) override;
    void onNpcKilled(characterId victim, characterId killer) override;
    std::int64_t massacreLootCurrency(characterId killer, const Position &at, std::int64_t purse) override;

private:
    WorldContext *ctx_ = nullptr;
};

} // namespace harrowmere::justice

#endif // HARROWMERE_CORE_JUSTICE_EVIDENCE_HPP
