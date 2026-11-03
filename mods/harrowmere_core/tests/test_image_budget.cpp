// Task 5.3.1 - The image budget gate: the pinned test suite.
//
// Three pinned tests (card 5.3.1):
//   * TestImageBudget_PercentilesWithinA146  — the gate asserts p95 ≤ A-146.
//   * TestImageBudget_CullOrderRespected     — the escalation ladder is a
//                                              pinned function, unit-tested
//                                              with synthetic profiles.
//   * TestImageBudget_ReportCitesProfilerRows — the report cites the
//                                                profiler's per-pass numbers.
//
// The gate runs the 5.1.1 suite's heaviest scenarios (walk_village, coast,
// rain) and asserts the render-side percentile report against A-146 (16.6 ms
// on the reference machine). The escalation ladder for a failure is the §4.5
// posture applied to rendering, in this pinned order:
//   1. distance-cull scatter (the A-152 per-view cap first)
//   2. decimate terrain grid outside a pinned near radius (A-160)
//   3. drop bloom before it drops frames
//   4. only then revisit instance budgets with a new Appendix A row + rationale
//
// Lavapipe numbers are for SHAPE (regression detection), never for the
// absolute gate. The reference machine (Core i5-8400 / Ryzen 5 2600 class,
// release build) is the absolute gate's anchor.

#include <doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/constants_phase5.hpp" // A-146, A-152, A-160, RENDER_FRAME_BUDGET_MS

using harrowmere::RENDER_FRAME_BUDGET_MS;
using harrowmere::SCATTER_INSTANCES_PER_VIEW;
using harrowmere::TERRAIN_NEAR_RADIUS_M;

// ---- The escalation ladder (§4.5 posture applied to rendering) -----------
//
// A pinned function: given a frame-time profile (p50, p95, p99 per pass),
// returns the FIRST escalation step that would bring p95 under A-146. The
// ladder is a fixed order — never skip a step because a pattern is
// fashionable (§4.5). Each step needs the profiler's per-pass numbers
// attached to the deviation row that invokes it.

enum class EscalationStep : std::uint8_t
{
    None            = 0, // p95 is already under A-146 — no action
    CullScatter     = 1, // step 1: distance-cull scatter (A-152 cap)
    DecimateTerrain = 2, // step 2: decimate terrain grid outside A-160 near radius
    DropBloom       = 3, // step 3: drop bloom before it drops frames
    ReviseBudgets   = 4, // step 4: revisit instance budgets (new A-row + rationale)
};

// One pass's frame-time profile (a single rendering pass, e.g. "geometry",
// "lighting", "composite"). The profiler publishes these per frame.
struct PassProfile
{
    std::string name;
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
};

// A scenario's full profile: all passes + the aggregate p95.
struct ScenarioProfile
{
    std::string scenarioName;
    std::vector<PassProfile> passes;
    double aggregateP95Ms = 0.0; // the sum of per-pass p95s
};

// Evaluate the escalation ladder for one scenario profile. Returns the
// FIRST step that would bring aggregateP95 under A-146. Each step's
// estimated savings are pinned (the card's spec: "distance-cull scatter
// (the A-152 per-view cap first) → decimate terrain grid outside a pinned
// near radius → drop bloom → only then revisit instance budgets").
//
// The savings estimates are presentation constants (not Appendix A rows —
// they are the gate's internal tuning, not game-rule constants). They are
// based on the profiler's measured per-pass contributions and represent
// the expected frame-time reduction from each escalation step.
EscalationStep evaluateLadder(
    const ScenarioProfile &profile
) noexcept
{
    const double budget = RENDER_FRAME_BUDGET_MS; // A-146 = 16.6 ms

    if (profile.aggregateP95Ms <= budget)
        return EscalationStep::None;

    // Step 1: cull scatter (A-152 per-view cap). Estimated savings: the
    // scatter geometry pass's p95. If the scatter pass exists and its p95
    // alone would bring the aggregate under budget, this step suffices.
    double scatterSavings = 0.0;
    for (const auto &p : profile.passes)
    {
        if (p.name == "scatter_geometry")
            scatterSavings = p.p95Ms;
    }
    if (profile.aggregateP95Ms - scatterSavings <= budget)
        return EscalationStep::CullScatter;

    // Step 2: decimate terrain grid outside A-160 near radius (1.5 km).
    // Estimated savings: the terrain geometry pass's p95 reduction from
    // dropping to a coarser grid beyond the near radius.
    double terrainSavings = 0.0;
    for (const auto &p : profile.passes)
    {
        if (p.name == "terrain_geometry")
            terrainSavings = p.p95Ms * 0.5; // decimation saves ~50% of the pass
    }
    if (profile.aggregateP95Ms - scatterSavings - terrainSavings <= budget)
        return EscalationStep::DecimateTerrain;

    // Step 3: drop bloom. Estimated savings: the bloom pass's p95.
    double bloomSavings = 0.0;
    for (const auto &p : profile.passes)
    {
        if (p.name == "bloom")
            bloomSavings = p.p95Ms;
    }
    if (profile.aggregateP95Ms - scatterSavings - terrainSavings - bloomSavings <= budget)
        return EscalationStep::DropBloom;

    // Step 4: revisit instance budgets — the last resort (changes the world
    // the owner approved). Requires a new Appendix A row + a rationale.
    return EscalationStep::ReviseBudgets;
}

// ---- The gate assertion ---------------------------------------------------

// Assert that a scenario's aggregate p95 is within A-146. This is the
// reference-machine gate (nightly). Lavapipe numbers are for SHAPE only.
bool gatePasses(
    const ScenarioProfile &profile
) noexcept
{
    return profile.aggregateP95Ms <= RENDER_FRAME_BUDGET_MS;
}

// ---- The report citation check --------------------------------------------

// A budget report entry: one scenario's profile + the profiler rows that
// back the numbers. The card requires "the profiler's per-pass numbers
// attached to the deviation row that invokes [the ladder]".
struct BudgetReportEntry
{
    ScenarioProfile profile;
    std::vector<std::string> profilerRowCitations; // e.g. "profiler.scene_geometry.p95=8.2ms"
    EscalationStep escalation = EscalationStep::None;
};

// Check that every report entry with a non-None escalation cites at least
// one profiler row. The card: "Each step needs the profiler's per-pass
// numbers attached to the deviation row that invokes it."
bool reportCitesProfilerRows(
    const std::vector<BudgetReportEntry> &entries
) noexcept
{
    for (const auto &e : entries)
    {
        if (e.escalation != EscalationStep::None && e.profilerRowCitations.empty())
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// TestImageBudget_PercentilesWithinA146
//
// The gate asserts p95 ≤ A-146 (16.6 ms) for the three gate-anchor scenarios
// (walk_village, coast, rain). This test uses synthetic profiles to verify
// the gate logic: a profile under budget passes; a profile over budget
// fails. The reference-machine assertion is nightly (real GPU); this test
// verifies the gate's logic, not the actual frame times.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestImageBudget_PercentilesWithinA146"
)
{
    const double budget = RENDER_FRAME_BUDGET_MS; // A-146 = 16.6 ms

    // A passing profile: aggregate p95 under budget.
    ScenarioProfile passing;
    passing.scenarioName   = "walk_village";
    passing.aggregateP95Ms = 12.5; // under 16.6
    CHECK(gatePasses(passing));

    // A failing profile: aggregate p95 over budget.
    ScenarioProfile failing;
    failing.scenarioName   = "coast";
    failing.aggregateP95Ms = 20.0; // over 16.6
    CHECK_FALSE(gatePasses(failing));

    // The budget is exactly A-146.
    CHECK(budget == doctest::Approx(16.6).epsilon(1e-9));

    // The three gate-anchor scenarios are the heaviest (5.1.1's pinned list).
    // The gate asserts all three.
    std::vector<ScenarioProfile> anchors = {
        {"walk_village", {}, 14.2},
        {"coast", {}, 15.8},
        {"rain", {}, 16.1},
    };
    for (const auto &a : anchors)
        CHECK(gatePasses(a));

    // One anchor over budget fails the gate.
    anchors[1].aggregateP95Ms = 18.0;
    CHECK_FALSE(gatePasses(anchors[1]));
}

// ---------------------------------------------------------------------------
// TestImageBudget_CullOrderRespected
//
// The escalation ladder is a pinned function, unit-tested with synthetic
// profiles. The ladder's order is:
//   1. Cull scatter (A-152 cap)
//   2. Decimate terrain (A-160 near radius)
//   3. Drop bloom
//   4. Revise budgets (last resort)
//
// The test verifies that each step fires at the right threshold and that
// the order is never skipped.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestImageBudget_CullOrderRespected"
)
{
    // A-146 is the pinned budget (16.6 ms). The ladder's thresholds are
    // relative to it; the evaluateLadder function reads RENDER_FRAME_BUDGET_MS
    // directly, so no local variable is needed here.

    // Profile under budget: no escalation.
    {
        ScenarioProfile p;
        p.aggregateP95Ms = 10.0;
        CHECK(evaluateLadder(p) == EscalationStep::None);
    }

    // Profile over budget by a small margin, with scatter as the heaviest
    // pass: step 1 (cull scatter) suffices.
    {
        ScenarioProfile p;
        p.aggregateP95Ms = 18.0; // 1.4 ms over
        p.passes         = {{"scatter_geometry", 4.0, 5.0, 6.0}};
        // 18.0 - 5.0 = 13.0 ≤ 16.6 → step 1
        CHECK(evaluateLadder(p) == EscalationStep::CullScatter);
    }

    // Profile over budget, scatter alone not enough, terrain needed: step 2.
    {
        ScenarioProfile p;
        p.aggregateP95Ms = 22.0; // 5.4 ms over
        p.passes         = {
            {"scatter_geometry", 3.0, 4.0, 5.0},
            {"terrain_geometry", 6.0, 8.0, 10.0},
        };
        // 22.0 - 4.0 (scatter) = 18.0 > 16.6 → not step 1
        // 22.0 - 4.0 - 4.0 (terrain 50%) = 14.0 ≤ 16.6 → step 2
        CHECK(evaluateLadder(p) == EscalationStep::DecimateTerrain);
    }

    // Profile over budget, scatter + terrain not enough, bloom needed: step 3.
    {
        ScenarioProfile p;
        p.aggregateP95Ms = 28.0; // 11.4 ms over
        p.passes         = {
            {"scatter_geometry", 3.0, 4.0, 5.0},
            {"terrain_geometry", 6.0, 8.0, 10.0},
            {"bloom", 3.0, 5.0, 7.0},
        };
        // 28.0 - 4.0 - 4.0 = 20.0 > 16.6 → not step 2
        // 28.0 - 4.0 - 4.0 - 5.0 = 15.0 ≤ 16.6 → step 3
        CHECK(evaluateLadder(p) == EscalationStep::DropBloom);
    }

    // Profile way over budget: all three steps not enough → step 4 (revise).
    {
        ScenarioProfile p;
        p.aggregateP95Ms = 40.0; // 23.4 ms over
        p.passes         = {
            {"scatter_geometry", 3.0, 4.0, 5.0},
            {"terrain_geometry", 6.0, 8.0, 10.0},
            {"bloom", 3.0, 5.0, 7.0},
        };
        // 40.0 - 4.0 - 4.0 - 5.0 = 27.0 > 16.6 → step 4
        CHECK(evaluateLadder(p) == EscalationStep::ReviseBudgets);
    }

    // The ladder never skips: if scatter alone suffices, it returns step 1
    // even if terrain/bloom passes exist.
    {
        ScenarioProfile p;
        p.aggregateP95Ms = 18.0;
        p.passes         = {
            {"scatter_geometry", 3.0, 5.0, 6.0},
            {"terrain_geometry", 6.0, 8.0, 10.0},
            {"bloom", 3.0, 5.0, 7.0},
        };
        // 18.0 - 5.0 = 13.0 ≤ 16.6 → step 1 (not step 2 or 3)
        CHECK(evaluateLadder(p) == EscalationStep::CullScatter);
    }

    // A-152 and A-160 are the pinned constants the ladder references.
    CHECK(SCATTER_INSTANCES_PER_VIEW == 12000);
    CHECK(TERRAIN_NEAR_RADIUS_M == 1500.0);
}

// ---------------------------------------------------------------------------
// TestImageBudget_ReportCitesProfilerRows
//
// The report must cite the profiler's per-pass numbers for any escalation
// step invoked. The card: "Each step needs the profiler's per-pass numbers
// attached to the deviation row that invokes it."
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestImageBudget_ReportCitesProfilerRows"
)
{
    // A report where every escalation entry has profiler citations: passes.
    {
        std::vector<BudgetReportEntry> entries = {
            {{"walk_village", {}, 14.2}, {}, EscalationStep::None},
            {{"coast", {{"scatter_geometry", 3, 5, 6}}, 18.0},
                {"profiler.scatter_geometry.p95=5.0ms"},
                EscalationStep::CullScatter},
            {{"rain", {{"bloom", 3, 5, 7}}, 20.0},
                {"profiler.bloom.p95=5.0ms", "profiler.scene_composite.p95=8.0ms"},
                EscalationStep::DropBloom},
        };
        CHECK(reportCitesProfilerRows(entries));
    }

    // A report where an escalation entry has NO profiler citations: fails.
    {
        std::vector<BudgetReportEntry> entries = {
            {{"coast", {}, 18.0}, {}, EscalationStep::CullScatter}, // no citations!
        };
        CHECK_FALSE(reportCitesProfilerRows(entries));
    }

    // A report with no escalations: always passes (no citations needed).
    {
        std::vector<BudgetReportEntry> entries = {
            {{"walk_village", {}, 14.2}, {}, EscalationStep::None},
            {{"coast", {}, 15.8}, {}, EscalationStep::None},
        };
        CHECK(reportCitesProfilerRows(entries));
    }
}
