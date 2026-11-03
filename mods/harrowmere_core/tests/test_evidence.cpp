// tests/test_evidence.cpp — Task 4.1's pinned test suite.
//
// The card's Tests block (names verbatim — common.md §3.5):
//
//   * TestEvidence_NonPlayerCulpritNoRecord
//   * TestEvidence_ExemptSpeciesNoRecord   (xenophobic village MURDER vs
//                                           beastkin — the doc's own example)
//   * TestEvidence_SafeZoneWardReportAppended
//   * TestEvidence_ReportCapMergesExcessWeight
//   * TestEvidence_TerritorySnapshotNeverRecomputed
//   * TestWitness_ChildAndNonSapientSkip
//   * TestWitness_WeightFormulaDocExact
//   * TestBribery_HalvesOnceThenFails
//   * TestBribery_FailureCreatesObstructionEvidence
//   * TestSelfReport_AddsScoreNeverIdentity
//   * TestDelayed_DiscoveredBodyAppendsReport
//   * TestAccomplice_HealOnWantedCreatesEvidence
//
// The rolls (bribery) pre-probe the pinned "justice" stream: the test seeds,
// learns the first draw, re-seeds, and replays — deterministic either way.
//
// Test harness: doctest v2.4.12; no main here (tests/main.cpp owns it).
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "core/constants.hpp" // A-59..A-69, SCALE
#include "core/enums.hpp"
#include "justice/cases.hpp" // updateEvidence (the wanted setup)
#include "justice/evidence.hpp"
#include "justice/warrants.hpp"  // hasWarrantIn (the accomplice test's wanted state)
#include "npc/relationships.hpp" // the weight formula's doc-oracle inputs

#include "test_justice_fixture.hpp"

namespace
{
using namespace justice_test;
using harrowmere::justice::Evidence;
using harrowmere::justice::Report;

// Finds the first justice-stream seed whose first draw is below/at the bound
// (success) or above it (failure), so the bribery/escape pins are
// deterministic without reimplementing the roll.
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
// Pinned test 1 — only PLAYERS generate evidence: an NPC brawl leaves no case.
// ============================================================================
TEST_CASE(
    "TestEvidence_NonPlayerCulpritNoRecord"
)
{
    JusticeFixture f;
    const characterId npcCulprit = f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "farmer", 5000);
    const characterId victim     = f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "farmer", 5000);

    justice::recordCrime(
        CrimeType::ASSAULT,
        f.base.characters.get(npcCulprit),
        f.base.characters.get(victim),
        JusticeFixture::villageCenter(JusticeFixture::kRegionB),
        f.base.ctx
    );

    CHECK(f.store.evidenceStore.empty());
}

// ============================================================================
// Pinned test 2 — the exemption HARD gate: no evidence object ever exists
// (xenophobic village MURDER vs beastkin — the doc's own example). The gate
// returns BEFORE construction, so the store has no entry and the id counter
// never moved.
// ============================================================================
TEST_CASE(
    "TestEvidence_ExemptSpeciesNoRecord"
)
{
    JusticeFixture f;
    const characterId killer   = f.spawnPlayerAt(JusticeFixture::kXenophobic, JusticeFixture::kRegionXen);
    const characterId beastkin = f.spawnBeastkin(JusticeFixture::kXenophobic, JusticeFixture::kRegionXen);

    // The shipped data: xenophobic_village exempts species 2 (beastkin) from
    // MURDER — the doc's own example pair.
    REQUIRE(f.world.lawsAt(JusticeFixture::kXenophobic)
            .exemptions.at(static_cast<harrowmere::speciesId>(2))
            .at(static_cast<std::size_t>(CrimeType::MURDER)));

    const std::uint64_t counterBefore = f.store.peekEvidenceId();
    justice::recordCrime(
        CrimeType::MURDER,
        f.base.characters.get(killer),
        f.base.characters.get(beastkin),
        JusticeFixture::villageCenter(JusticeFixture::kRegionXen),
        f.base.ctx
    );

    CHECK(f.store.evidenceStore.empty());
    CHECK(f.store.peekEvidenceId() == counterBefore); // no Evidence object was ever constructed
}

// ============================================================================
// Pinned test 3 — the safe-zone ward report: maximal testimony with source 0
// and knowsIdentity TRUE (the doc's `Report{0, WARD_WEIGHT, true}` — the ward
// IS an identity source in safe zones; do not "fix" it).
// ============================================================================
TEST_CASE(
    "TestEvidence_SafeZoneWardReportAppended"
)
{
    JusticeFixture f;
    const characterId killer = f.spawnPlayerAt(JusticeFixture::kVillageA, JusticeFixture::kRegionA);
    // The victim is elsewhere (another village): no live witness is in radius,
    // so the ward's testimony is the case's only report.
    const characterId victim = f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "farmer", 5000);

    justice::recordCrime(
        CrimeType::MURDER,
        f.base.characters.get(killer),
        f.base.characters.get(victim),
        JusticeFixture::villageCenter(JusticeFixture::kRegionA),
        f.base.ctx
    );

    const justice::Evidence *e = f.firstEvidence(JusticeFixture::kVillageA);
    REQUIRE(e != nullptr);
    REQUIRE(e->reports.size() == 1); // no live witness reported (the safe zone's ward speaks)
    CHECK(e->reports[0].source == 0);
    CHECK(e->reports[0].weight == WARD_WEIGHT);
    CHECK(e->reports[0].knowsIdentity);
}

// ============================================================================
// Pinned test 4 — the cap: the heaviest REPORT_CAP (A-61) reports stay, the
// excess merges into aggregateReportWeight, and equal weights keep insertion
// order (the stable-sort pin, R7).
// ============================================================================
TEST_CASE(
    "TestEvidence_ReportCapMergesExcessWeight"
)
{
    JusticeFixture f;

    // Ten distinct reports, 100..1000: the top 8 (300..1000) stay, the two
    // lightest (100 + 200) merge.
    std::vector<justice::Report> reports;
    for (std::uint32_t w = 100; w <= 1000; w += 100)
    {
        reports.push_back(justice::Report{w, w, false, false});
    }
    const justice::Evidence stored = justice::commitEvidence(
        CrimeType::THEFT, /*culprit=*/7, /*victim=*/9, JusticeFixture::kVillageB, f.base.ctx, reports, 0
    );

    CHECK(stored.reports.size() == REPORT_CAP);
    CHECK(stored.aggregateReportWeight == 300); // 100 + 200
    // The kept reports are the heaviest, heaviest-first (the cap's order).
    CHECK(stored.reports.front().weight == 1000);
    CHECK(stored.reports.back().weight == 300);
}

// ============================================================================
// Pinned test 5 — the territory snapshot is NEVER recomputed: the case keeps
// its commit-time ground after the culprit crosses the world.
// ============================================================================
TEST_CASE(
    "TestEvidence_TerritorySnapshotNeverRecomputed"
)
{
    JusticeFixture f;
    const characterId killer = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const characterId victim = f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "farmer", 5000);

    justice::recordCrime(
        CrimeType::MURDER,
        f.base.characters.get(killer),
        f.base.characters.get(victim),
        JusticeFixture::villageCenter(JusticeFixture::kRegionB),
        f.base.ctx
    );
    REQUIRE(f.firstEvidence(JusticeFixture::kVillageB) != nullptr);

    // The culprit flees across the world (grid + record move — the walk would
    // now resolve a different territory).
    const Position farAway{15 * 1024.0 + 512.0, 0.0, 15 * 1024.0 + 512.0};
    f.base.characters.get(killer).body.position = farAway;
    f.grid.move(killer, JusticeFixture::villageCenter(JusticeFixture::kRegionB), farAway);

    const justice::Evidence *e = f.firstEvidence(JusticeFixture::kVillageB);
    REQUIRE(e != nullptr);
    CHECK(e->territory == JusticeFixture::kVillageB); // the snapshot stands
}

// ============================================================================
// Pinned test 6 — children and non-sapients never testify (A-44 + sapience);
// a qualifying adult farmer does.
// ============================================================================
TEST_CASE(
    "TestWitness_ChildAndNonSapientSkip"
)
{
    JusticeFixture f;
    const characterId killer = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    f.spawnChild(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const characterId adult = f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "farmer", 5000);

    // A non-sapient bystander (a wolf) inside the witness radius.
    const characterId wolf = phase2_test::spawnNpc(
        f.base, phase2_test::speciesIdByKey("wolf"), JusticeFixture::kVillageB, JusticeFixture::kRegionB, 5
    );
    f.base.characters.get(wolf).body.position = JusticeFixture::villageCenter(JusticeFixture::kRegionB);
    f.grid.insert(wolf, f.base.characters.get(wolf).body.position);

    justice::recordCrime(
        CrimeType::ASSAULT,
        f.base.characters.get(killer),
        f.base.characters.get(adult),
        JusticeFixture::villageCenter(JusticeFixture::kRegionB),
        f.base.ctx
    );

    const justice::Evidence *e = f.firstEvidence(JusticeFixture::kVillageB);
    REQUIRE(e != nullptr);
    REQUIRE(e->reports.size() == 1); // the child and the wolf are gone; the adult reports
    CHECK(e->reports[0].source == adult);
}

// ============================================================================
// Pinned test 7 — the weight formula is doc-exact through 3.9's API: a
// farmer (base 4000) with integrity 8000 and no affinity reports at
// 4000 * 8000 / 10000 * 10000 / 10000 = 3200.
// ============================================================================
TEST_CASE(
    "TestWitness_WeightFormulaDocExact"
)
{
    JusticeFixture f;
    const characterId killer  = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const characterId witness = f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "farmer", 8000);
    const characterId victim  = f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "farmer", 5000);

    justice::recordCrime(
        CrimeType::MURDER,
        f.base.characters.get(killer),
        f.base.characters.get(victim),
        JusticeFixture::villageCenter(JusticeFixture::kRegionB),
        f.base.ctx
    );

    const justice::Evidence *e = f.firstEvidence(JusticeFixture::kVillageB);
    REQUIRE(e != nullptr);
    REQUIRE(e->reports.size() == 2); // the witness + the victim (alive, present, qualifying)
    bool foundWitnessReport = false;
    for (const Report &r : e->reports)
    {
        if (r.source == witness)
        {
            foundWitnessReport = true;
            CHECK(r.weight == 3200);      // the doc formula, computed through 3.9's function
            CHECK_FALSE(r.knowsIdentity); // a stranger with no sighting knows nobody
        }
    }
    CHECK(foundWitnessReport);
}

// ============================================================================
// Pinned test 8 — bribery halves the report weight ONCE; a second attempt on
// the same report auto-fails (the bribed flag closes the surface).
// ============================================================================
TEST_CASE(
    "TestBribery_HalvesOnceThenFails"
)
{
    JusticeFixture f;
    const characterId briber  = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const characterId witness = f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "vendor", 5000);
    const characterId victim  = f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "farmer", 5000);

    justice::recordCrime(
        CrimeType::ASSAULT,
        f.base.characters.get(briber),
        f.base.characters.get(victim),
        JusticeFixture::villageCenter(JusticeFixture::kRegionB),
        f.base.ctx
    );
    justice::Evidence *e = f.firstEvidence(JusticeFixture::kVillageB);
    REQUIRE(e != nullptr);
    REQUIRE(e->reports.size() >= 2); // the vendor witnessed it
    const std::uint32_t before = e->reports[0].weight;

    // Deterministic success: pre-probe the pinned stream for a winning draw.
    const std::uint64_t successSeed = seedForDrawBelow(500, true);
    f.base.rng.seedAll(successSeed);
    CHECK(justice::attemptBribery(briber, witness, f.base.ctx) == justice::BriberyOutcome::Bribed);

    // The FIRST report (the vendor's — heaviest, stable-sorted first) halved
    // exactly once and flagged.
    e = f.firstEvidence(JusticeFixture::kVillageB);
    REQUIRE(e != nullptr);
    const justice::Report *bribed = nullptr;
    for (const Report &r : e->reports)
    {
        if (r.source == witness)
        {
            bribed = &r;
        }
    }
    REQUIRE(bribed != nullptr);
    CHECK(bribed->bribed);
    CHECK(bribed->weight == before / 2);

    // The second attempt on the same report: the surface is gone.
    f.base.rng.seedAll(successSeed);
    CHECK(justice::attemptBribery(briber, witness, f.base.ctx) == justice::BriberyOutcome::NoReport);
}

// ============================================================================
// Pinned test 9 — a failed bribe records the briber: a fresh OBSTRUCTION
// evidence with the doc's literal report {briberId, OBSTRUCTION_WEIGHT, true}.
// ============================================================================
TEST_CASE(
    "TestBribery_FailureCreatesObstructionEvidence"
)
{
    JusticeFixture f;
    const characterId briber  = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const characterId witness = f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "vendor", 5000);
    const characterId victim  = f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "farmer", 5000);

    justice::recordCrime(
        CrimeType::ASSAULT,
        f.base.characters.get(briber),
        f.base.characters.get(victim),
        JusticeFixture::villageCenter(JusticeFixture::kRegionB),
        f.base.ctx
    );
    REQUIRE(f.firstEvidence(JusticeFixture::kVillageB) != nullptr);

    // Deterministic failure: a draw at/above the 500 floor.
    const std::uint64_t failureSeed = seedForDrawBelow(500, false);
    f.base.rng.seedAll(failureSeed);
    CHECK(justice::attemptBribery(briber, witness, f.base.ctx) == justice::BriberyOutcome::Failed);

    // The fresh OBSTRUCTION case sits against the briber in their ground.
    const justice::Evidence *obstruction = f.firstEvidence(JusticeFixture::kVillageB);
    // The insertion-ordered vector: the assault case first, the obstruction second.
    const auto &cases = f.store.evidenceStore.at(JusticeFixture::kVillageB);
    REQUIRE(cases.size() == 2);
    CHECK(cases[1].type == CrimeType::OBSTRUCTION);
    CHECK(cases[1].culpritId == briber);
    REQUIRE(cases[1].reports.size() == 1);
    CHECK(cases[1].reports[0].source == briber);
    CHECK(cases[1].reports[0].weight == OBSTRUCTION_WEIGHT);
    CHECK(cases[1].reports[0].knowsIdentity);
    (void)obstruction;
}

// ============================================================================
// Pinned test 10 — the victim's self-report adds score and NEVER satisfies
// identity: hasIdentity stays false on the self-report alone.
// ============================================================================
TEST_CASE(
    "TestSelfReport_AddsScoreNeverIdentity"
)
{
    JusticeFixture f;
    const characterId killer = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const characterId victim = f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "farmer", 5000);

    justice::recordCrime(
        CrimeType::MURDER,
        f.base.characters.get(killer),
        f.base.characters.get(victim),
        JusticeFixture::villageCenter(JusticeFixture::kRegionB),
        f.base.ctx
    );
    justice::Evidence *e = f.firstEvidence(JusticeFixture::kVillageB);
    REQUIRE(e != nullptr);
    CHECK_FALSE(justice::hasIdentity(*e, f.base.ctx)); // a stranger-only case (daytime: physical 500)

    // The player victim reports post-respawn, within the A-68 window.
    f.base.ctx.now = 1; // the same game-day, well inside 2 game-days
    CHECK(justice::selfReportVictim(victim, f.base.ctx));

    e = f.firstEvidence(JusticeFixture::kVillageB);
    REQUIRE(e != nullptr);
    // The self-report: A-69's weight, never identity. (The victim ALSO
    // live-reported at the crime — source == victim matches twice; the
    // weight is what identifies the self-report.)
    const justice::Report *self = nullptr;
    for (const Report &r : e->reports)
    {
        if (r.source == victim && r.weight == SELF_REPORT_WEIGHT)
        {
            self = &r;
        }
    }
    REQUIRE(self != nullptr);
    CHECK_FALSE(self->knowsIdentity);
    CHECK_FALSE(justice::hasIdentity(*e, f.base.ctx)); // score grew; identity did NOT
}

// ============================================================================
// Pinned test 11 — the discovered body: a follow-up report rides the ORIGINAL
// murder evidence (not ARCHIVED), with the discoverer's 3.9 identity answer.
// ============================================================================
TEST_CASE(
    "TestDelayed_DiscoveredBodyAppendsReport"
)
{
    JusticeFixture f;
    const characterId killer = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const characterId victim = f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "farmer", 5000);

    justice::recordCrime(
        CrimeType::MURDER,
        f.base.characters.get(killer),
        f.base.characters.get(victim),
        JusticeFixture::villageCenter(JusticeFixture::kRegionB),
        f.base.ctx
    );
    justice::Evidence *e = f.firstEvidence(JusticeFixture::kVillageB);
    REQUIRE(e != nullptr);

    // A stranger finds the body days later: the report lands on the original case.
    const characterId discoverer = f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "farmer", 5000);
    f.base.ctx.now               = 5 * REPUTATION_DECAY_INTERVAL;
    justice::appendDiscoveredBodyReport(victim, discoverer, f.base.ctx);

    e = f.firstEvidence(JusticeFixture::kVillageB);
    REQUIRE(e != nullptr);
    const justice::Report *body = nullptr;
    for (const Report &r : e->reports)
    {
        if (r.source == discoverer)
        {
            body = &r;
        }
    }
    REQUIRE(body != nullptr);
    CHECK(body->weight == DELAYED_BODY_WEIGHT);
    CHECK_FALSE(body->knowsIdentity); // the discoverer never saw the culprit (no sighting, no acquaintance)

    // An acquaintance discoverer KNOWS the culprit (the 3.9 rule rides along).
    const characterId friend_ = f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "farmer", 5000);
    f.base.npcs.get(friend_).affinity[killer] = ACQUAINTANCE_AFFINITY; // an acquaintance of the killer
    justice::appendDiscoveredBodyReport(victim, friend_, f.base.ctx);
    e = f.firstEvidence(JusticeFixture::kVillageB);
    REQUIRE(e != nullptr);
    const justice::Report *friends = nullptr;
    for (const Report &r : e->reports)
    {
        if (r.source == friend_)
        {
            friends = &r;
        }
    }
    REQUIRE(friends != nullptr);
    CHECK(friends->knowsIdentity);
}

// ============================================================================
// Pinned test 12 — aiding a WANTED culprit earns accomplice evidence vs the
// helper (the pinned v1 trigger: heal-on-WANTED-culprit).
// ============================================================================
TEST_CASE(
    "TestAccomplice_HealOnWantedCreatesEvidence"
)
{
    JusticeFixture f;
    const characterId culprit = f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const characterId victim  = f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "vendor", 8000);
    const characterId healer  = f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "healer", 8000);

    // Drive the culprit's case to WANTED: an open-and-shut THEFT (an
    // acquaintance vendor's report satisfies the identity arm).
    f.base.npcs.get(victim).affinity[culprit] = ACQUAINTANCE_AFFINITY; // the vendor knows the culprit
    justice::recordCrime(
        CrimeType::THEFT,
        f.base.characters.get(culprit),
        f.base.characters.get(victim),
        JusticeFixture::villageCenter(JusticeFixture::kRegionB),
        f.base.ctx
    );
    justice::Evidence *e = f.firstEvidence(JusticeFixture::kVillageB);
    REQUIRE(e != nullptr);
    f.base.ctx.now += GAME_HOUR_SECONDS; // the A-76 cadence allows the first case tick
    justice::updateEvidence(*e, f.base.ctx);
    REQUIRE(e->status == EvidenceStatus::WANTED);
    REQUIRE(justice::hasWarrantIn(f.base.ctx, culprit, JusticeFixture::kVillageB));

    // The healer aids the wanted culprit: the trigger site calls the API —
    // the evidence lands vs the HELPER.
    justice::recordAccomplice(
        healer, culprit, CrimeType::SMUGGLING, JusticeFixture::villageCenter(JusticeFixture::kRegionB), f.base.ctx
    );

    const auto &cases = f.store.evidenceStore.at(JusticeFixture::kVillageB);
    REQUIRE(cases.size() == 2);
    CHECK(cases[1].type == CrimeType::SMUGGLING);
    CHECK(cases[1].culpritId == healer);
    CHECK(cases[1].victimId == culprit);
}
