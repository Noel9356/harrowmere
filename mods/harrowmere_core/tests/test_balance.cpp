// tests/test_balance.cpp — task 8.4's card tests. The full instrumented run
// is the standalone soak binary's `--balance` mode (the numbers the balance
// report cites); these cases pin the measurements' verdicts in-suite over
// short windows with generous bands and pinned seeds — flaky-tolerant by
// design (the card's own note), deterministic by the sim's R6/R7 pins.
//
//   TestBalance_LevelCurve             — (a) the A-120 curve read
//   TestBalance_EventEquilibrium       — (f) the difficulty loop's equilibrium
//   TestBalance_ConvictionRatesByLawSet — (d) the conviction rates by law set
//
// Every assertion traces to a number the report carries; nothing here tunes
// a constant (the pass's rule: adjustments ride Appendix A + data only, and
// the as-built read needed none — see docs/balance_report.md).

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "core/constants.hpp"
#include "events/difficulty.hpp"
#include "events/engine.hpp"
#include "events/template.hpp"
#include "progression/progression.hpp"

#include "soak_main.cpp" // the harness (HARROWMERE_SOAK_NO_MAIN is set for this target)

namespace
{

using namespace harrowmere;
namespace ev = harrowmere::events;

std::filesystem::path soakDataDir()
{
#ifdef HARROWMERE_CORE_DATA_DIR
    return std::filesystem::path(HARROWMERE_CORE_DATA_DIR);
#else
    return std::filesystem::path("mods") / "harrowmere_core" / "data";
#endif
}

// The (a) curve's faucet model read: matching-level kills per 10 levels =
// bandCost(L) / (A-34/A-35's kill payout at the adventurer's own level). The
// payout is the kill probe's verified constant — 0.8x of the victim's 2.0x
// loss, the exact integer expression runKillXpProbe pins — so the kills
// count is the cost table's own geometric identity (~12.7 at every anchor —
// the faucet self-scales with the cost curve).
double balMatchingLevelKills(
    std::uint64_t fromLevel
)
{
    const std::vector<std::uint64_t> &costs = levelCosts();
    const std::uint64_t bandCost            = soak::balTenLevelCost(fromLevel);
    const std::uint64_t payout = NPC_VICTIM_PENALTY_FACTOR * costs[fromLevel] / SCALE * NPC_KILL_XP_GAIN / SCALE;
    return static_cast<double>(bandCost) / static_cast<double>(payout);
}

} // namespace

// ============================================================================
// TestBalance_LevelCurve — the card's (a). The cost side reads the A-7/A-8
// table at the four anchors; the income side is the kill faucet, verified
// through the REAL death path (the playerless world's NPC income is
// structurally zero — the report carries that read). The A-120 target
// (~15 game-days per 10 levels early, doubling per band) splits honestly:
// the early band's DAYS meet the target under matching-level hunting (12.7
// kills x the hunt pacing); the DOUBLING cannot come from a constant — the
// kill faucet scales with the same 1.15^L the cost table grows by, so
// days-per-band are level-invariant under it, and a flat-XP income (quest /
// event reward tables — data rows) grows days 1.15^10 = 4.05x per band. The
// doubling needs ~a third of the late-band income to be level-flat: a
// content composition, recorded in the report's known-notes.
// ============================================================================
TEST_CASE(
    "TestBalance_LevelCurve"
)
{
    // The faucet's one sim-side constant, measured through the real death
    // path: a player kill pays A-35's 0.8 x the A-34 loss, i.e.
    // 1.6 x levelCosts[victim.level] exactly (the --balance run's MATCH line).
    soak::SoakRuntime rt;
    std::string error;
    const std::string newGameError = "soak newGame failed: " + error;
    REQUIRE_MESSAGE(soak::newGame(rt, 20261102ULL, soakDataDir().string(), error), newGameError.c_str());

    const soak::KillXpProbe kill = soak::runKillXpProbe(rt);
    REQUIRE(kill.ran);
    CHECK_MESSAGE(kill.gainedXp == kill.expectedXp, "the kill faucet must pay 1.6 x levelCosts[victim.level]");

    // The cost side at the four anchors: matching-level kills per 10 levels.
    // Generous band around the geometric identity's ~12.7 (the 20.3 band
    // sum over the 1.6 payout factor); any drift here is a cost-table or
    // faucet-constant change — both Appendix A doors.
    for (const std::uint64_t anchor : {1ull, 25ull, 50ull, 75ull})
    {
        const double kills = balMatchingLevelKills(anchor);
        const bool inBand  = kills > 10.0 && kills < 16.0;
        CHECK_MESSAGE(inBand, "matching-level kills per 10 levels at anchor ", anchor, ": ", kills);
    }

    // Band invariance under the self-scaling faucet: the anchors' kill
    // counts agree within [0.7x, 1.4x] (the identity is exact; the integer
    // table's per-step floor drifts a fraction of a percent). A drift BEYOND
    // the band means the faucet stopped scaling with the cost curve — the
    // A-120 doubling conversation reopens (the report's known-note).
    const double early = balMatchingLevelKills(1);
    const double late  = balMatchingLevelKills(75);
    const double ratio = late / early;
    const bool stable  = ratio > 0.7 && ratio < 1.4;
    CHECK_MESSAGE(stable, "kill-count band drift early->late: ", ratio);
}

// ============================================================================
// TestBalance_EventEquilibrium — the card's (f): "successRate hovers
// 3000-7000 over time — the doc's regulation loop works". The loop's
// arithmetic (the signed delta / attempts) converges to the OUTCOME MIX's
// mean: mixed outcomes oscillate into the band (the first case below);
// an all-success mix saturates at SCALE (the second case — the headless
// world's actual shape, the report's known-note: the shipped templates
// resolve by declared transitions, the failure arm never fires there).
// Both arms ride the real noteStateAttempt + the 8.4 drain seam.
// ============================================================================
TEST_CASE(
    "TestBalance_EventEquilibrium"
)
{
    ev::clearDifficultySamples();

    // A binary (go/no-fail) state like the shipped BATTLE shape: no capped
    // objectives, one transition.
    ev::EventTemplate tpl;
    tpl.id                 = 1;
    tpl.key                = "balance_equilibrium";
    tpl.global             = false;
    tpl.initialStateId     = 1;
    tpl.initialDifficulty  = 5000;
    tpl.initialSuccessRate = 5000;
    tpl.maxDurationDays    = 30;
    ev::TemplateState a;
    a.stateId = 1;
    ev::Transition next;
    next.to = 1; // self-transition: each attempt re-enters the state
    ev::Condition timer;
    timer.kind = ev::ConditionKind::Timer;
    timer.days = 1;
    next.conditions.push_back(timer);
    a.transitions.push_back(next);
    tpl.states.push_back(a);

    ev::Event event;
    event.templateId               = tpl.id;
    event.currentState             = ev::EventState{};
    event.currentState.stateId     = 1;
    event.currentState.attempts    = 0;
    event.currentState.successRate = tpl.initialSuccessRate;
    event.currentState.difficulty  = tpl.initialDifficulty;

    // Arm 1 — the mixed-outcome convergence: alternate success/failure for
    // 16 attempts. The post-attempt book leaves the doc's band after the
    // first oscillation (5000 -> 10000 -> 5000 -> 6667 -> 5000 ...) and
    // every sample from the third on reads inside [3000, 7000].
    for (std::uint32_t i = 0; i < 16; ++i)
    {
        ev::noteStateAttempt(event, tpl, /*success=*/i % 2 == 0);
    }
    {
        const std::vector<std::uint32_t> drained = ev::drainDifficultySamples();
        REQUIRE(drained.size() == 16);
        std::size_t inBand = 0;
        for (std::size_t i = 2; i < drained.size(); ++i)
        {
            if (drained[i] >= 3000 && drained[i] <= 7000)
            {
                inBand += 1;
            }
        }
        CHECK_MESSAGE(
            inBand == drained.size() - 2,
            "the mixed-outcome loop converges into the doc's band (in-band ",
            inBand,
            "/",
            drained.size() - 2,
            ")"
        );
        // The convergence itself: the final read sits in the band.
        const bool finalInBand = drained.back() >= 3000 && drained.back() <= 7000;
        CHECK_MESSAGE(finalInBand, "final post-attempt read ", drained.back());
    }

    // Arm 2 — the all-success saturation (the headless read's mechanism):
    // a fresh state pushed only successes walks to SCALE and pins there.
    ev::Event saturated;
    saturated.templateId               = tpl.id;
    saturated.currentState             = ev::EventState{};
    saturated.currentState.stateId     = 1;
    saturated.currentState.successRate = tpl.initialSuccessRate;
    saturated.currentState.difficulty  = tpl.initialDifficulty;
    for (std::uint32_t i = 0; i < 4; ++i)
    {
        ev::noteStateAttempt(saturated, tpl, /*success=*/true);
    }
    {
        const std::vector<std::uint32_t> drained = ev::drainDifficultySamples();
        REQUIRE(drained.size() == 4);
        CHECK_MESSAGE(drained.back() == SCALE, "the all-success mix pins at SCALE (read ", drained.back(), ")");
    }

    // The seam drains clean (the probe's contract: drain, then empty).
    CHECK(ev::drainDifficultySamples().empty());
}

// ============================================================================
// TestBalance_ConvictionRatesByLawSet — the card's (d) over the living
// world: one game-month of the real pipeline (pinned seed 20261102 — the
// --balance run's seed), then the crime probe's three reads with generous
// bands: the crowd arm (a market punch convicts in every law set), the
// single-witness thin arm (the thresholds' operating band — neither
// saturated nor dead), the exemption hard gate (the doc's species-conflict
// flavor) and the repeat-offender damper (the rate may only rise).
// ============================================================================
TEST_CASE(
    "TestBalance_ConvictionRatesByLawSet"
)
{
    soak::SoakRuntime rt;
    std::string error;
    const std::string newGameError = "soak newGame failed: " + error;
    REQUIRE_MESSAGE(soak::newGame(rt, 20261102ULL, soakDataDir().string(), error), newGameError.c_str());

    // One game-month of the real pipeline (the daily sweeps, the economy,
    // the drift — the probe reads a LIVED world, not worldgen).
    soak::runTicks(rt, soak::kTicksPerGameDay * 365 / 12);

    const std::vector<soak::LawSetTrials> probe
        = soak::runCrimeProbe(rt, /*settlementsPerLaw=*/1, /*assaultTrials=*/6, /*murderTrials=*/4, /*thinTrials=*/6);
    REQUIRE(!probe.empty());

    double thinPooledConvicted = 0.0;
    double thinPooledCommitted = 0.0;
    for (const soak::LawSetTrials &row : probe)
    {
        // The crowd arm saturates (the report's crowd read): a witnessed
        // market assault / murder convicts in every shipped law set.
        REQUIRE(row.assault.committed > 0);
        REQUIRE(row.murder.committed > 0);
        const double crowdAssaultRate
            = 100.0 * static_cast<double>(row.assault.convicted) / static_cast<double>(row.assault.committed);
        const double crowdMurderRate
            = 100.0 * static_cast<double>(row.murder.convicted) / static_cast<double>(row.murder.committed);
        const bool assaultSaturated = crowdAssaultRate >= 80.0;
        const bool murderSaturated  = crowdMurderRate >= 80.0;
        CHECK_MESSAGE(assaultSaturated, row.lawKey, ": crowd assault conviction rate ", crowdAssaultRate, "%");
        CHECK_MESSAGE(murderSaturated, row.lawKey, ": crowd murder conviction rate ", crowdMurderRate, "%");

        // The thin arm operates inside the thresholds' band: convictions
        // happen (the pipeline convicts on real reports) without
        // saturating (a single report cannot cross every threshold).
        if (row.thin.committed > 0)
        {
            thinPooledConvicted += row.thin.convicted;
            thinPooledCommitted += row.thin.committed;
        }
    }
    REQUIRE(thinPooledCommitted > 0);
    const double thinRate = 100.0 * thinPooledConvicted / thinPooledCommitted;
    const bool thinInBand = thinRate > 25.0 && thinRate < 95.0;
    CHECK_MESSAGE(thinInBand, "the single-witness band operates (pooled thin conviction rate ", thinRate, "%)");

    // The (d) species-conflict flavor: the exemption hard gate refuses the
    // beastkin murder row outright while the human control records.
    bool beastkinRefused = false;
    bool humanRecorded   = false;
    const bool ran       = soak::runExemptionSpotCheck(rt, beastkinRefused, humanRecorded);
    REQUIRE_MESSAGE(ran, "the exemption spot-check needs a staging settlement + a living beastkin");
    CHECK_MESSAGE(beastkinRefused, "the xenophobic village must refuse the beastkin murder row");
    CHECK_MESSAGE(humanRecorded, "the human control must record its murder row");

    // The repeat-offender damper: with the notoriety floor escalating, the
    // conviction rate may only rise (second half >= first half).
    const std::vector<territoryId> homes = soak::balSettlementsOfLaw(rt, "default", 1);
    REQUIRE(!homes.empty());
    const soak::RampSeries ramp = soak::balRampSeries(rt, homes[0], 10);
    const double firstRate  = ramp.firstHalfN > 0
                                ? static_cast<double>(ramp.firstHalfConvicted) / static_cast<double>(ramp.firstHalfN)
                                : 0.0;
    const double secondRate = ramp.secondHalfN > 0
                                ? static_cast<double>(ramp.secondHalfConvicted) / static_cast<double>(ramp.secondHalfN)
                                : 0.0;
    CHECK_MESSAGE(
        secondRate >= firstRate, "the notoriety damper may only widen convictions (", firstRate, " -> ", secondRate, ")"
    );
}
