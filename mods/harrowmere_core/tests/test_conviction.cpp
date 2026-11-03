// tests/test_conviction.cpp — Task 4.3's pinned test suite.
//
// The card's Tests block (names verbatim — common.md §3.5):
//
//   * TestConvict_DebtMathDocExact  (oracle-computed at level 50, rep -2000,
//                                    safe zone — matches the i128 reference)
//   * TestConvict_ZoneMultiplierReadsEvidenceTerritory (flee the safe zone
//                                    before conviction — still doubled)
//   * TestConvict_WarrantDeletedFromBothMaps
//   * TestConvict_RepLossPropagatesUpward (with 4.4)
//   * TestSentence_CurrencyMayGoNegative
//   * TestSurrender_ConvictsAndEndsManhunt
//   * TestEscape_SuccessClearsJailRecordsFreshCrime
//   * TestEscape_FailureDoublesSentence
//   * TestBreakout_RescuerGetsEvidence
//
// The escape rolls pre-probe the pinned "justice" stream (see
// test_evidence.cpp's helper) so both branches are deterministic.
//
// Test harness: doctest v2.4.12; no main here (tests/main.cpp owns it).
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "core/clock.hpp"     // DAY_LENGTH_SECONDS (the sentence assertion)
#include "core/constants.hpp" // A-77..A-83, SCALE
#include "core/math.hpp"      // mulDiv128 (the wide path)
#include "justice/conviction.hpp"
#include "justice/evidence.hpp"
#include "justice/reputation.hpp"
#include "justice/warrants.hpp"
#include "progression/progression.hpp" // levelCosts() (the debt's level term)

#include "test_justice_fixture.hpp"

namespace
{
using namespace justice_test;
using harrowmere::justice::Evidence;
using harrowmere::justice::Report;

// The oracle's independent wide product — __extension__ exempts the alias
// from -Wpedantic exactly as core/math.cpp's own wide typedefs do.
__extension__ typedef __int128 justice_test_wide_i128;

// Drives a case to WANTED so conviction/escape/surrender have their warrant.
// Returns the case ID (later commits may reallocate the territory's vector —
// tests re-read through the store, never across commits by reference).
std::uint64_t makeWanted(
    JusticeFixture &f, harrowmere::characterId culprit, harrowmere::territoryId territory
)
{
    justice::commitEvidence(
        CrimeType::THEFT, culprit, 0, territory, f.base.ctx, std::vector<Report>{Report{55, 9000, true, false}}, 0
    );
    Evidence &e = f.store.evidenceStore[territory].back(); // the commit just appended it
    justice::issueWarrant(e, f.base.ctx);
    REQUIRE(e.status == EvidenceStatus::WANTED);
    REQUIRE(justice::hasWarrantIn(f.base.ctx, culprit, territory));
    return e.id;
}

Evidence &caseRef(
    JusticeFixture &f, std::uint64_t caseId
)
{
    Evidence *e = f.caseById(caseId);
    REQUIRE(e != nullptr);
    return *e;
}

std::uint64_t seedForDrawBelow(
    std::uint64_t bound, bool wantBelow
)
{
    for (std::uint64_t seed = 1;; ++seed)
    {
        harrowmere::core::SeedRng probe;
        probe.seedAll(seed);
        const std::uint32_t draw = probe.pct("justice");
        if ((draw < bound) == wantBelow)
        {
            return seed;
        }
    }
}
} // namespace

// ============================================================================
// Pinned test 1 — the debt math is doc-exact through the wide path: level 50,
// reputation -2000, safe zone (2x). The oracle recomputes the doc formula
// with a local __int128 product (independent of mulDiv128), then the debt
// must match bit-for-bit and land on availableXp through the R4 clamp.
// ============================================================================
TEST_CASE(
    "TestConvict_DebtMathDocExact"
)
{
    JusticeFixture f;
    const characterId culprit  = f.spawnPlayerAt(JusticeFixture::kVillageA, JusticeFixture::kRegionA);
    const std::uint64_t caseId = makeWanted(f, culprit, JusticeFixture::kVillageA);

    Character &c              = f.base.characters.get(culprit);
    c.progression.level       = 50;
    c.progression.availableXp = 1'000'000;
    justice::changeReputation(culprit, JusticeFixture::kVillageA, -2000, f.base.ctx); // rep -2000 at the ground

    // The i128 reference (the card's "matches the i128 reference"):
    //   debt = FACTOR * levelCosts[level] * zoneMult(2x) * repMult / SCALE^2
    //   repMult = SCALE + max(0, -rep) * 2000 / SCALE = 10000 + 400 = 10400
    const std::int64_t factor  = CONVICTION_PENALTY_FACTOR;
    const std::int64_t lc      = static_cast<std::int64_t>(levelCosts()[50]);
    const std::int64_t zone    = 2 * static_cast<std::int64_t>(SCALE);
    const std::int64_t repMult = static_cast<std::int64_t>(SCALE) + 2000 * 2000 / static_cast<std::int64_t>(SCALE);
    const justice_test_wide_i128 oracle
        = static_cast<justice_test_wide_i128>(factor) * lc * zone * repMult
        / (static_cast<justice_test_wide_i128>(SCALE) * static_cast<justice_test_wide_i128>(SCALE));
    REQUIRE(oracle <= INT64_MAX); // the quotient fits the i64 grid (R3's saturation is idle here)

    const std::int64_t before = c.progression.availableXp;
    justice::convict(caseRef(f, caseId), f.base.ctx);

    CHECK(c.progression.availableXp == before - static_cast<std::int64_t>(oracle));
    CHECK(caseRef(f, caseId).status == EvidenceStatus::CONVICTED);
}

// ============================================================================
// Pinned test 2 — the zone multiplier reads the EVIDENCE territory: fleeing
// the safe zone before conviction does not dodge the doubling (the doc note).
// ============================================================================
TEST_CASE(
    "TestConvict_ZoneMultiplierReadsEvidenceTerritory"
)
{
    JusticeFixture f;
    const characterId culprit  = f.spawnPlayerAt(JusticeFixture::kVillageA, JusticeFixture::kRegionA);
    const std::uint64_t caseId = makeWanted(f, culprit, JusticeFixture::kVillageA);

    Character &c              = f.base.characters.get(culprit);
    c.progression.level       = 50;
    c.progression.availableXp = 1'000'000;

    // The culprit flees to the other village (an unsafe ground) BEFORE the
    // conviction lands.
    const Position farAway = JusticeFixture::villageCenter(JusticeFixture::kRegionB);
    c.body.position        = farAway;
    f.grid.move(culprit, JusticeFixture::villageCenter(JusticeFixture::kRegionA), farAway);

    const std::int64_t lc   = static_cast<std::int64_t>(levelCosts()[50]);
    const std::int64_t zone = 2 * static_cast<std::int64_t>(SCALE); // the EVIDENCE ground is still safe
    const justice_test_wide_i128 oracleDoubled
        = static_cast<justice_test_wide_i128>(CONVICTION_PENALTY_FACTOR) * lc * zone
        / static_cast<justice_test_wide_i128>(SCALE); // repMult neutral: /SCALE cancels one SCALE
    const std::int64_t before = c.progression.availableXp;

    justice::convict(caseRef(f, caseId), f.base.ctx);

    // The doubled debt (not the single-SCALE one) proves the multiplier read
    // the evidence territory, not the culprit's position.
    CHECK(c.progression.availableXp == before - static_cast<std::int64_t>(oracleDoubled));
}

// ============================================================================
// Pinned test 3 — the warrant cleanup deletes from BOTH maps (the doc
// invariant, conviction arm).
// ============================================================================
TEST_CASE(
    "TestConvict_WarrantDeletedFromBothMaps"
)
{
    JusticeFixture f;
    const characterId culprit  = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const std::uint64_t caseId = makeWanted(f, culprit, JusticeFixture::kVillageB);
    REQUIRE(f.store.warrants.size() == 1);
    REQUIRE(f.store.activeWarrantsByCulprit.at(culprit).size() == 1);

    justice::convict(caseRef(f, caseId), f.base.ctx);

    CHECK(f.store.warrants.empty());
    CHECK(f.store.activeWarrantsByCulprit.find(culprit) == f.store.activeWarrantsByCulprit.end());
}

// ============================================================================
// Pinned test 4 — the reputation loss propagates up the chain (4.4's walk):
// village -3000 -> county -1500 -> kingdom -750 -> empire -375.
// ============================================================================
TEST_CASE(
    "TestConvict_RepLossPropagatesUpward"
)
{
    JusticeFixture f;
    const characterId culprit  = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const std::uint64_t caseId = makeWanted(f, culprit, JusticeFixture::kVillageB);

    justice::convict(caseRef(f, caseId), f.base.ctx);

    const justice::Reputation &rep = f.store.reputations.at(culprit);
    CHECK(rep.perTerritory.at(JusticeFixture::kVillageB) == -CONVICTION_REPUTATION_LOSS);
    CHECK(rep.perTerritory.at(JusticeFixture::kCounty) == -CONVICTION_REPUTATION_LOSS / 2);
    CHECK(rep.perTerritory.at(JusticeFixture::kKingdom) == -CONVICTION_REPUTATION_LOSS / 4);
    CHECK(rep.perTerritory.at(JusticeFixture::kEmpire) == -CONVICTION_REPUTATION_LOSS / 8);
    // And the crime's notoriety landed (the conviction's tail).
    CHECK(rep.notoriety == 2000); // THEFT's crime_data notorietyGain
}

// ============================================================================
// Pinned test 5 — the fine is i64 arithmetic on an i64 field: a murder fine
// (severity 9000 x 10) drives the A-41 starter purse NEGATIVE (the doc rule).
// ============================================================================
TEST_CASE(
    "TestSentence_CurrencyMayGoNegative"
)
{
    JusticeFixture f;
    const characterId culprit = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const std::uint64_t caseId
        = justice::commitEvidence(CrimeType::MURDER, culprit, 0, JusticeFixture::kVillageB, f.base.ctx, {}, 0).id;
    REQUIRE(f.base.characters.get(culprit).progression.currency == STARTER_CURRENCY); // A-41

    justice::convict(caseRef(f, caseId), f.base.ctx);

    // 90000 fine against the 100 purse; the sentence also rode jailUntil.
    CHECK(f.base.characters.get(culprit).progression.currency == STARTER_CURRENCY - 90'000);
    CHECK(f.base.characters.get(culprit).progression.currency < 0);
    CHECK(f.store.jailUntil.at(culprit) == f.base.ctx.now + (1 + 9000 / 5) * core::DAY_LENGTH_SECONDS);
}

// ============================================================================
// Pinned test 6 — surrender: every live warrant active in the guard's
// territory converts to conviction (standard sentence); a sibling ground's
// warrant does not.
// ============================================================================
TEST_CASE(
    "TestSurrender_ConvictsAndEndsManhunt"
)
{
    JusticeFixture f;
    const characterId culprit         = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const std::uint64_t villageCaseId = makeWanted(f, culprit, JusticeFixture::kVillageB);
    const std::uint64_t xenoCaseId    = makeWanted(f, culprit, JusticeFixture::kXenophobic); // another subtree's case
    REQUIRE(f.store.warrants.size() == 2);

    RecordingSimEvents events;
    f.base.ctx.simEvents = &events;

    // Surrender to a guard of the VILLAGE B ground: only that subtree's
    // warrant converts.
    CHECK(
        justice::surrender(culprit, JusticeFixture::kCounty, f.base.ctx)
    ); // any guard under the county covers village B

    CHECK(caseRef(f, villageCaseId).status == EvidenceStatus::CONVICTED);
    CHECK(caseRef(f, xenoCaseId).status == EvidenceStatus::WANTED); // the sibling subtree's case stands
    CHECK(f.store.warrants.size() == 1);
    CHECK(justice::isArrested(f.store, culprit, f.base.ctx.now)); // the standard sentence landed
    REQUIRE(events.resolved.size() == 1);
    CHECK(events.resolved.front().how == WarrantResolved::How::Surrendered);
    f.base.ctx.simEvents = nullptr;
}

// ============================================================================
// Pinned test 7 — the successful escape: the jail records clear, and the
// attempt leaves the fresh OBSTRUCTION evidence (A-82's 7000).
// ============================================================================
TEST_CASE(
    "TestEscape_SuccessClearsJailRecordsFreshCrime"
)
{
    JusticeFixture f;
    const characterId escapee  = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const std::uint64_t caseId = makeWanted(f, escapee, JusticeFixture::kVillageB);
    justice::convict(caseRef(f, caseId), f.base.ctx);
    REQUIRE(justice::isArrested(f.store, escapee, f.base.ctx.now));

    // A dedicated escapee: ESCAPE 45 -> pct = 9000 - 6000 + 2000 = 5000. The
    // probe finds a winning draw.
    f.base.characters.get(escapee).progression.skills[static_cast<std::size_t>(SkillType::ESCAPE)] = 45;
    f.base.rng.seedAll(seedForDrawBelow(5000, true));

    CHECK(justice::attemptEscape(escapee, f.base.ctx));

    CHECK_FALSE(justice::isArrested(f.store, escapee, f.base.ctx.now)); // the records cleared
    REQUIRE(f.store.jailUntil.find(escapee) == f.store.jailUntil.end());
    // The fresh case: OBSTRUCTION vs the escapee at the jail ground, carrying
    // exactly the A-82 report (no failure stack).
    const Evidence &fresh = f.store.evidenceStore.at(JusticeFixture::kVillageB).back();
    CHECK(fresh.type == CrimeType::OBSTRUCTION);
    CHECK(fresh.culpritId == escapee);
    REQUIRE(fresh.reports.size() == 1);
    CHECK(fresh.reports[0].weight == ESCAPE_EVIDENCE_WEIGHT);
    CHECK_FALSE(fresh.reports[0].knowsIdentity);
}

// ============================================================================
// Pinned test 8 — the failed escape: the sentence DOUBLES (extended by the
// remaining term) and the failure's A-83 stack rides the fresh evidence.
// ============================================================================
TEST_CASE(
    "TestEscape_FailureDoublesSentence"
)
{
    JusticeFixture f;
    const characterId escapee  = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const std::uint64_t caseId = makeWanted(f, escapee, JusticeFixture::kVillageB);
    justice::convict(caseRef(f, caseId), f.base.ctx);

    const std::uint64_t remainingBefore = justice::remainingSentence(f.store, escapee, f.base.ctx.now);
    REQUIRE(remainingBefore > 0);

    // ESCAPE 0 -> pct floors at 200; the probe finds a losing draw.
    f.base.rng.seedAll(seedForDrawBelow(200, false));

    CHECK_FALSE(justice::attemptEscape(escapee, f.base.ctx));

    CHECK(justice::isArrested(f.store, escapee, f.base.ctx.now)); // still serving
    CHECK(justice::remainingSentence(f.store, escapee, f.base.ctx.now) == remainingBefore * 2);

    const Evidence &fresh = f.store.evidenceStore.at(JusticeFixture::kVillageB).back();
    CHECK(fresh.type == CrimeType::OBSTRUCTION);
    REQUIRE(fresh.reports.size() == 2); // the A-82 evidence + the A-83 failure stack
    CHECK(fresh.reports[0].weight == ESCAPE_EVIDENCE_WEIGHT);
    CHECK(fresh.reports[1].weight == ESCAPE_FAIL_WEIGHT);
}

// ============================================================================
// Pinned test 9 — breakout: healing/releasing an arrested character is
// accomplice evidence vs the RESCUER (the pinned trigger).
// ============================================================================
TEST_CASE(
    "TestBreakout_RescuerGetsEvidence"
)
{
    JusticeFixture f;
    const characterId prisoner = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const std::uint64_t caseId = makeWanted(f, prisoner, JusticeFixture::kVillageB);
    justice::convict(caseRef(f, caseId), f.base.ctx);
    REQUIRE(justice::isArrested(f.store, prisoner, f.base.ctx.now));

    const characterId rescuer = f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "healer", 8000);
    justice::recordBreakout(rescuer, prisoner, f.base.ctx);

    const Evidence &accomplice = f.store.evidenceStore.at(JusticeFixture::kVillageB).back();
    CHECK(accomplice.type == CrimeType::SMUGGLING);
    CHECK(accomplice.culpritId == rescuer);
    CHECK(accomplice.victimId == prisoner);
}
