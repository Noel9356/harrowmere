// tests/test_cases_warrants.cpp — Task 4.2's pinned test suite.
//
// The card's Tests block (names verbatim — common.md §3.5):
//
//   * TestCase_SuppressionDampensNobleScore  (6000-suppression on THEFT damps
//                                             an 8000 report total — the
//                                             corruption mechanic)
//   * TestCase_NotorietyLowersThreshold
//   * TestCase_InvestigatingBelowFullThreshold
//   * TestCase_WarrantRequiresIdentity       (huge score, no identity -> no warrant)
//   * TestWarrant_JurisdictionVillageToKingdomYes_SiblingNo
//   * TestWarrant_ExpiredPurgedLazily
//   * TestWarrant_CaseDowngradesToInvestigatingAfterExpiry
//   * TestSweep_ArchivesOldUnwantedCases
//   * TestArchive_CompactRecordWrittenAndRemoved
//   * TestIdentity_OnlyNpcReportsCount       (player self-report + ward report
//                                             alone never satisfy identity)
//
// Test harness: doctest v2.4.12; no main here (tests/main.cpp owns it).
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "core/constants.hpp" // A-70..A-72, A-74, SCALE
#include "core/enums.hpp"
#include "justice/cases.hpp"
#include "justice/evidence.hpp"
#include "justice/reputation.hpp"
#include "justice/warrants.hpp"
#include "world/world.hpp"

#include "test_justice_fixture.hpp"

namespace
{
using namespace justice_test;
using harrowmere::justice::Evidence;
using harrowmere::justice::Report;

// A tip-off: a case the tests can drive without the full crime pipeline.
// Returns the case ID — later commits may reallocate the territory's vector,
// so tests hold IDs and re-read through the store (caseRef), never references.
std::uint64_t openCase(
    JusticeFixture &f,
    harrowmere::characterId culprit,
    CrimeType type,
    harrowmere::territoryId territory,
    std::vector<Report> reports,
    std::uint32_t physical
)
{
    return justice::commitEvidence(type, culprit, /*victim=*/0, territory, f.base.ctx, std::move(reports), physical).id;
}

// The fresh read of one case (by id).
Evidence &caseRef(
    JusticeFixture &f, std::uint64_t caseId
)
{
    Evidence *e = f.caseById(caseId);
    REQUIRE(e != nullptr);
    return *e;
}

// The store holds a case with this id (scan — references can dangle after an
// archive/erase, so tests read through the store).
bool storeHasCase(
    JusticeFixture &f, std::uint64_t evidenceId
)
{
    for (const auto &[territory, cases] : f.store.evidenceStore)
    {
        for (const Evidence &e : cases)
        {
            if (e.id == evidenceId)
            {
                return true;
            }
        }
    }
    return false;
}
} // namespace

// ============================================================================
// Pinned test 1 — the corruption mechanic (D-CORRUPT-1): a NOBLE's THEFT case
// damps at 6000 — an 8000 report total lands at 4800, below the 5000
// threshold. The commoner's identical case scores its full 8000.
// ============================================================================
TEST_CASE(
    "TestCase_SuppressionDampensNobleScore"
)
{
    JusticeFixture f;
    // The score math reads the territory TITLES (id-keyed, no Character reads):
    // two bare culprits, one noble, one commoner (SP allows one player — the
    // ids are opaque, R10).
    const characterId noble                                       = 9001;
    const characterId commoner                                    = 9002;
    f.world.territory(JusticeFixture::kCorrupt)->titles[noble]    = SocialClassId::NOBLE;
    f.world.territory(JusticeFixture::kCorrupt)->titles[commoner] = SocialClassId::COMMONER;

    const std::vector<Report> oneHeavyReport{Report{/*source=*/55, 8000, false, false}};

    const std::uint64_t nobleCaseId = openCase(f, noble, CrimeType::THEFT, JusticeFixture::kCorrupt, oneHeavyReport, 0);
    const std::uint64_t commonerCaseId
        = openCase(f, commoner, CrimeType::THEFT, JusticeFixture::kCorrupt, oneHeavyReport, 0);
    Evidence &nobleCase    = caseRef(f, nobleCaseId);
    Evidence &commonerCase = caseRef(f, commonerCaseId);

    // 8000 * 6000 / 10000 = 4800 (the damp); the commoner reads the full 8000.
    CHECK(justice::caseScore(nobleCase, f.base.ctx) == 4800);
    CHECK(justice::caseScore(commonerCase, f.base.ctx) == 8000);

    // The damp alone hides the noble: 4800 < 5000 threshold -> no warrant, but
    // the case still opens an investigation (4800 >= 5000/2) — the doc's SOFT
    // form: overwhelming evidence could still convict later.
    f.base.ctx.now += GAME_HOUR_SECONDS;
    justice::updateEvidence(nobleCase, f.base.ctx);
    CHECK(nobleCase.status == EvidenceStatus::INVESTIGATING);
    CHECK(f.store.warrants.empty());
}

// ============================================================================
// Pinned test 2 — repeat offenders convict on weaker evidence: the threshold
// scales down by (SCALE + notoriety) / SCALE.
// ============================================================================
TEST_CASE(
    "TestCase_NotorietyLowersThreshold"
)
{
    JusticeFixture f;
    const characterId culprit = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const std::uint64_t freshId
        = openCase(f, culprit, CrimeType::THEFT, JusticeFixture::kVillageB, {Report{55, 0, false, false}}, 0);
    CHECK(justice::caseThreshold(caseRef(f, freshId), f.base.ctx) == 5000); // the shipped default for THEFT

    justice::addNotoriety(f.store, culprit, 5000); // a hardened criminal
    CHECK(
        justice::caseThreshold(caseRef(f, freshId), f.base.ctx) == 3333
    ); // 5000 * 10000 / 15000 (truncating division, R2)
}

// ============================================================================
// Pinned test 3 — half the threshold opens the investigation (A-72); the full
// threshold + identity issues the warrant.
// ============================================================================
TEST_CASE(
    "TestCase_InvestigatingBelowFullThreshold"
)
{
    JusticeFixture f;
    const characterId culprit = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);

    // 2600 of score: under the 5000 threshold, over the 2500 investigate line.
    const std::uint64_t quietId
        = openCase(f, culprit, CrimeType::THEFT, JusticeFixture::kVillageB, {Report{55, 2600, false, false}}, 0);
    justice::updateEvidence(caseRef(f, quietId), f.base.ctx);
    CHECK(caseRef(f, quietId).status == EvidenceStatus::INVESTIGATING);
    CHECK(f.store.warrants.empty()); // investigating alone never warrants

    // 6000 with a knowing NPC report: the full arm fires.
    const std::uint64_t loudId
        = openCase(f, culprit, CrimeType::THEFT, JusticeFixture::kVillageB, {Report{56, 6000, true, false}}, 0);
    justice::updateEvidence(caseRef(f, loudId), f.base.ctx);
    CHECK(caseRef(f, loudId).status == EvidenceStatus::WANTED);
    CHECK(f.store.warrants.size() == 1);
}

// ============================================================================
// Pinned test 4 — identity gates the warrant: a huge score with no knowing
// NPC report and sub-A-70 physical evidence never warrants.
// ============================================================================
TEST_CASE(
    "TestCase_WarrantRequiresIdentity"
)
{
    JusticeFixture f;
    const characterId culprit = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);

    // 8000 of stranger-reported score + 1000 physical: score >= threshold, no
    // identity anywhere (no NPC knows the face, physical under A-70).
    const std::uint64_t anonymousId = openCase(
        f,
        culprit,
        CrimeType::THEFT,
        JusticeFixture::kVillageB,
        {Report{55, 4000, false, false}, Report{56, 4000, false, false}},
        1000
    );
    Evidence &anonymous = caseRef(f, anonymousId);
    REQUIRE(justice::caseScore(anonymous, f.base.ctx) >= justice::caseThreshold(anonymous, f.base.ctx));
    REQUIRE_FALSE(justice::hasIdentity(anonymous, f.base.ctx));

    justice::updateEvidence(anonymous, f.base.ctx);
    CHECK(anonymous.status == EvidenceStatus::INVESTIGATING); // the score arm runs, the identity arm does not
    CHECK(f.store.warrants.empty());
}

// ============================================================================
// Pinned test 5 — the jurisdiction walk: a village warrant covers its county,
// kingdom and empire; never a sibling village or an unrelated subtree.
// ============================================================================
TEST_CASE(
    "TestWarrant_JurisdictionVillageToKingdomYes_SiblingNo"
)
{
    JusticeFixture f;
    const characterId culprit = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const std::uint64_t caseId
        = openCase(f, culprit, CrimeType::THEFT, JusticeFixture::kVillageB, {Report{55, 6000, true, false}}, 0);
    justice::issueWarrant(caseRef(f, caseId), f.base.ctx);

    CHECK(justice::hasWarrantIn(f.base.ctx, culprit, JusticeFixture::kVillageB));         // the issuing ground
    CHECK(justice::hasWarrantIn(f.base.ctx, culprit, JusticeFixture::kCounty));           // the county
    CHECK(justice::hasWarrantIn(f.base.ctx, culprit, JusticeFixture::kKingdom));          // the kingdom
    CHECK(justice::hasWarrantIn(f.base.ctx, culprit, JusticeFixture::kEmpire));           // the empire
    CHECK_FALSE(justice::hasWarrantIn(f.base.ctx, culprit, JusticeFixture::kVillageA));   // a sibling village
    CHECK_FALSE(justice::hasWarrantIn(f.base.ctx, culprit, JusticeFixture::kXenophobic)); // another subtree
}

// ============================================================================
// Pinned test 6 — the lazy purge: an expired warrant is GONE from both maps
// the moment any read touches it (and the Expired fact published).
// ============================================================================
TEST_CASE(
    "TestWarrant_ExpiredPurgedLazily"
)
{
    JusticeFixture f;
    const characterId culprit = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const std::uint64_t caseId
        = openCase(f, culprit, CrimeType::THEFT, JusticeFixture::kVillageB, {Report{55, 6000, true, false}}, 0);
    justice::issueWarrant(caseRef(f, caseId), f.base.ctx);
    REQUIRE(f.store.warrants.size() == 1);
    REQUIRE(f.store.activeWarrantsByCulprit.at(culprit).size() == 1);

    // The purge is lazy: nothing runs until a read crosses the expiry.
    f.base.ctx.now = caseRef(f, caseId).worldTime + WARRANT_DURATION_GAME_S + 1;

    RecordingSimEvents events; // the test's own recording sink
    f.base.ctx.simEvents = &events;
    CHECK_FALSE(justice::hasWarrantIn(f.base.ctx, culprit, JusticeFixture::kVillageB));

    // BOTH maps cleared — the doc invariant, expiry arm.
    CHECK(f.store.warrants.empty());
    CHECK(f.store.activeWarrantsByCulprit.find(culprit) == f.store.activeWarrantsByCulprit.end());
    REQUIRE(events.resolved.size() == 1);
    CHECK(events.resolved.front().how == WarrantResolved::How::Expired);
    f.base.ctx.simEvents = nullptr;
}

// ============================================================================
// Pinned test 7 — expiry is a pause, not an eraser: the WANTED case
// downgrades to INVESTIGATING (the downgrade runs FIRST — the doc order), the
// case survives the purge, and recovered evidence re-warrants.
// ============================================================================
TEST_CASE(
    "TestWarrant_CaseDowngradesToInvestigatingAfterExpiry"
)
{
    JusticeFixture f;
    const characterId culprit = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const std::uint64_t caseId
        = openCase(f, culprit, CrimeType::THEFT, JusticeFixture::kVillageB, {Report{55, 6000, true, false}}, 0);
    Evidence &e = caseRef(f, caseId);
    justice::issueWarrant(e, f.base.ctx);
    REQUIRE(e.status == EvidenceStatus::WANTED);

    // The case weakens below the full threshold (a witness withdrew), then the
    // warrant expires. The downgrade arm (not the score arm) is what pauses
    // the case — score 3000 sits between thr/2 and thr.
    e.reports.assign({Report{55, 3000, true, false}});
    f.base.ctx.now += WARRANT_DURATION_GAME_S + 1;
    justice::updateEvidence(e, f.base.ctx);

    CHECK(caseRef(f, caseId).status == EvidenceStatus::INVESTIGATING); // paused, NOT archived
    CHECK(f.store.warrants.empty());                                   // and the expired warrant left both maps

    // The evidence recovers: the next tick re-warrants — a pause, not an eraser.
    caseRef(f, caseId).reports.assign({Report{55, 6000, true, false}});
    f.base.ctx.now += GAME_HOUR_SECONDS;
    justice::updateEvidence(caseRef(f, caseId), f.base.ctx);
    CHECK(caseRef(f, caseId).status == EvidenceStatus::WANTED);
    CHECK(f.store.warrants.size() == 1);
}

// ============================================================================
// Pinned test 8 — the daily backstop: un-wanted cases past the A-71 window
// archive everywhere; WANTED cases with a live warrant are skipped.
// ============================================================================
TEST_CASE(
    "TestSweep_ArchivesOldUnwantedCases"
)
{
    JusticeFixture f;
    const characterId culprit = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);

    const std::uint64_t staleId
        = openCase(f, culprit, CrimeType::THEFT, JusticeFixture::kVillageB, {Report{55, 100, false, false}}, 0);

    // The wanted case commits late in the window, so its 7-day warrant is
    // still LIVE at the sweep's 30-day mark (a stale warrant would not skip
    // the case — the sweep reads warrants through the lazy purge).
    f.base.ctx.now = LIMIT_DAYS * core::DAY_LENGTH_SECONDS - WARRANT_DURATION_GAME_S / 2;
    const std::uint64_t wantedId
        = openCase(f, culprit, CrimeType::THEFT, JusticeFixture::kVillageB, {Report{56, 6000, true, false}}, 0);
    justice::issueWarrant(caseRef(f, wantedId), f.base.ctx);

    f.base.ctx.now = LIMIT_DAYS * core::DAY_LENGTH_SECONDS + 1;
    justice::sweepExpiredEvidence(f.base.ctx);

    // The stale case left the store (references into the erased vector would
    // dangle — the tests read through the store); the wanted case stands (its
    // warrant is live).
    CHECK_FALSE(storeHasCase(f, staleId));
    CHECK(storeHasCase(f, wantedId));
    REQUIRE(f.world.territory(JusticeFixture::kVillageB) != nullptr);
    REQUIRE(f.world.territory(JusticeFixture::kVillageB)->archive.size() == 1); // the stale case's compact row
    CHECK(f.world.territory(JusticeFixture::kVillageB)->archive.front().evidenceId == staleId);
}

// ============================================================================
// Pinned test 9 — the archive: the compact record lands on the territory
// (identification minimum + the 4.1 victim field) and the case leaves the
// store. Burning the archive would destroy exactly these rows (6.3's hook).
// ============================================================================
TEST_CASE(
    "TestArchive_CompactRecordWrittenAndRemoved"
)
{
    JusticeFixture f;
    const characterId culprit  = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const std::uint64_t caseId = openCase(f, culprit, CrimeType::MURDER, JusticeFixture::kVillageB, {}, 0);

    justice::archiveEvidence(caseRef(f, caseId), f.base.ctx);

    const world::Territory *territory = f.world.territory(JusticeFixture::kVillageB);
    REQUIRE(territory != nullptr);
    REQUIRE(territory->archive.size() == 1);
    const world::CompactRecord &row = territory->archive.front();
    CHECK(row.evidenceId == caseId);
    CHECK(row.worldTime == 0); // the tip-off was committed at now == 0
    CHECK(row.crimeType == static_cast<std::uint32_t>(CrimeType::MURDER));
    CHECK(row.culprit == culprit);
    CHECK(row.victim == 0); // the tip-off case carried no victim

    CHECK_FALSE(storeHasCase(f, caseId));
}

// ============================================================================
// Pinned test 10 — identity counts NPC reports ONLY: a player's self-report
// and a ward report (source 0, knowsIdentity true) never satisfy identity.
// The A-70 physical arm is its own proof and stands alone.
// ============================================================================
TEST_CASE(
    "TestIdentity_OnlyNpcReportsCount"
)
{
    JusticeFixture f;
    const characterId culprit = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);

    // A ward report (source 0, knowsIdentity TRUE) plus the culprit's own
    // player report (the self-report form): the two identity-shaped sources
    // the doc's rule excludes, together. (SP allows one player — the self-
    // report IS the player-source pin.)
    const std::uint64_t caseId = openCase(
        f,
        culprit,
        CrimeType::MURDER,
        JusticeFixture::kVillageB,
        {Report{0, WARD_WEIGHT, true, false}, Report{culprit, 9000, true, false}},
        0
    );
    CHECK_FALSE(justice::hasIdentity(caseRef(f, caseId), f.base.ctx));

    // The same shape from an NPC witness satisfies identity.
    const std::uint64_t npcCaseId = openCase(
        f, culprit, CrimeType::MURDER, JusticeFixture::kVillageB, {Report{/*npc=*/77, 9000, true, false}}, 0
    );
    CHECK(justice::hasIdentity(caseRef(f, npcCaseId), f.base.ctx));

    // The physical arm stands alone: A-70 overwhelms without any report.
    const std::uint64_t physicalCaseId
        = openCase(f, culprit, CrimeType::MURDER, JusticeFixture::kVillageB, {}, PHYSICAL_IDENTITY_MIN);
    CHECK(justice::hasIdentity(caseRef(f, physicalCaseId), f.base.ctx));
}
