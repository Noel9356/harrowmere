// test_event_difficulty.cpp — task 5.2's pinned tests: the signed-delta
// update (the doc's underflow warning is the R1 founding example), the
// clamp arms, the continuous performance's cap-zero guard and per-objective
// normalization, and the D-DIFF-1 cadence (one difficulty update per state
// attempt — transition or forced exit, never per tick).
#include <doctest.h>

#include <cstdint>

#include "core/constants.hpp"
#include "events/contribution.hpp" // recordAction (the validate+accumulate path)
#include "events/difficulty.hpp"
#include "events/engine.hpp"
#include "events/template.hpp"

#include "test_justice_fixture.hpp"

namespace
{

using namespace harrowmere;
using namespace justice_test;
namespace ev = harrowmere::events;

// A template whose first state declares one DAMAGE objective with a cap
// (the continuous-performance shape) and one transition.
ev::EventTemplate objectiveTemplate()
{
    ev::EventTemplate tpl;
    tpl.id                 = 1;
    tpl.key                = "objective_event";
    tpl.global             = false;
    tpl.initialStateId     = 1;
    tpl.initialDifficulty  = 3000;
    tpl.initialSuccessRate = 4000;
    tpl.maxDurationDays    = 30;

    ev::TemplateState a;
    a.stateId = 1;
    ev::Objective o;
    o.objectiveId = 1;
    o.actionType  = ActionType::DAMAGE;
    o.cap         = 10000;
    a.objectives.push_back(o);
    ev::Transition toB;
    toB.to = 2;
    ev::Condition timer;
    timer.kind = ev::ConditionKind::Timer;
    timer.days = 1;
    toB.conditions.push_back(timer);
    a.transitions.push_back(toB);

    ev::TemplateState b;
    b.stateId = 2;
    ev::Action end;
    end.kind    = ev::ActionKind::EndEvent;
    end.success = true;
    b.entryActions.push_back(end);

    tpl.states.push_back(a);
    tpl.states.push_back(b);
    return tpl;
}

struct DifficultyRig
{
    JusticeFixture f;
    ev::EventTemplateTable table;
    ev::EventEngine engine;

    DifficultyRig()
    {
        f.base.ctx.now = 1000;
        engine.bind(&table);
        engine.bindContext(&f.base.ctx);
    }
};

} // namespace

// ============================================================================
// Pinned test 1 — the doc's own division numbers: successRate 4000,
// performance 0 → 0 at attempts 1, 3200 at attempts 5 (truncation toward
// zero on the negative delta; the pitfall forbids "fixing" to floor-div).
// ============================================================================
TEST_CASE(
    "TestDifficulty_FailureLowersRate_SignedMath"
)
{
    ev::EventState st;
    st.difficulty  = 4000;
    st.successRate = 4000;
    st.attempts    = 0;

    ev::updateStateDifficulty(st, /*performance=*/0);
    CHECK(st.attempts == 1);
    CHECK(st.successRate == 0); // 4000 + (-4000 / 1), clamped at the floor
    CHECK(st.difficulty == 0);

    // The attempts-5 arm: the moving average divides by the attempt count.
    ev::EventState st5;
    st5.difficulty  = 4000;
    st5.successRate = 4000;
    st5.attempts    = 4; // the attempt ledger's prior state
    ev::updateStateDifficulty(st5, 0);
    CHECK(st5.attempts == 5);
    CHECK(st5.successRate == 3200); // 4000 + (-4000 / 5) — 3200, never 3199
    CHECK(st5.difficulty == 3200);
}

// ============================================================================
// Pinned test 2 — a successful attempt raises BOTH fields.
// ============================================================================
TEST_CASE(
    "TestDifficulty_SuccessRaisesBoth"
)
{
    ev::EventState st;
    st.difficulty  = 3000;
    st.successRate = 4000;
    st.attempts    = 0;

    ev::updateStateDifficulty(st, ev::binaryPerformance(true)); // SCALE
    CHECK(st.attempts == 1);
    CHECK(st.successRate == SCALE); // 4000 + 6000, clamped at the ceiling
    CHECK(st.difficulty == 9000);   // 3000 + 6000
}

// ============================================================================
// Pinned test 3 — the clamp arms: repeated failure floors at 0, repeated
// success ceilings at SCALE, on both fields.
// ============================================================================
TEST_CASE(
    "TestDifficulty_ClampsAtZeroAndScale"
)
{
    ev::EventState st;
    st.difficulty  = 0;
    st.successRate = 0;
    for (int i = 0; i < 10; ++i)
    {
        ev::updateStateDifficulty(st, 0); // a failure against a zeroed rate: delta 0
    }
    CHECK(st.successRate == 0);
    CHECK(st.difficulty == 0);

    // A failure against a tiny rate: the negative delta clamps at 0.
    ev::EventState low;
    low.difficulty  = 10;
    low.successRate = 100;
    low.attempts    = 0;
    ev::updateStateDifficulty(low, 0); // delta -100, /1 → clamps both to 0
    CHECK(low.successRate == 0);
    CHECK(low.difficulty == 0);

    ev::EventState high;
    high.difficulty  = SCALE;
    high.successRate = SCALE;
    for (int i = 0; i < 10; ++i)
    {
        ev::updateStateDifficulty(high, SCALE); // success at the ceiling: delta 0
    }
    CHECK(high.successRate == SCALE);
    CHECK(high.difficulty == SCALE);
}

// ============================================================================
// Pinned test 4 — continuous performance skips cap-zero objectives.
// ============================================================================
TEST_CASE(
    "TestContinuous_ObjectiveCapZeroSkipped"
)
{
    std::vector<ev::ObjectiveSample> objectives;
    objectives.push_back(ev::ObjectiveSample{0, 5000});    // cap 0: skipped (R2's guard)
    objectives.push_back(ev::ObjectiveSample{5000, 2500}); // min(SCALE, 2500 * 10000 / 5000) = 5000
    CHECK(ev::continuousPerformance(objectives) == 5000);
}

// ============================================================================
// Pinned test 5 — an empty (or all-skipped) span reads 0.
// ============================================================================
TEST_CASE(
    "TestContinuous_EmptyReturnsZero"
)
{
    std::vector<ev::ObjectiveSample> empty;
    CHECK(ev::continuousPerformance(empty) == 0);
    std::vector<ev::ObjectiveSample> uncapped;
    uncapped.push_back(ev::ObjectiveSample{0, 9999});
    CHECK(ev::continuousPerformance(uncapped) == 0);
}

// ============================================================================
// Pinned test 6 — each capped objective normalizes to its own cap, then the
// samples average (min(SCALE, amount * SCALE / cap)).
// ============================================================================
TEST_CASE(
    "TestContinuous_NormalizedPerObjective"
)
{
    std::vector<ev::ObjectiveSample> objectives;
    objectives.push_back(ev::ObjectiveSample{10000, 5000}); // 5000
    objectives.push_back(ev::ObjectiveSample{5000, 5000});  // 10000 (at the clamp)
    CHECK(ev::continuousPerformance(objectives) == 7500);   // (5000 + 10000) / 2
}

// ============================================================================
// Pinned test 7 — the D-DIFF-1 cadence: one update per state attempt. The
// transition fires once → attempts 1 and the difficulty moved by the
// continuous performance of the exited state; a second tick without a
// transition moves nothing.
// ============================================================================
TEST_CASE(
    "TestDifficulty_TransitionUpdatesOncePerAttempt"
)
{
    DifficultyRig rig;
    rig.table.insert(objectiveTemplate());

    const characterId hunter = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "guard", 8000);
    const std::uint64_t id
        = rig.engine.startEventByKey("objective_event", JusticeFixture::kRegionA, 0, nullptr, rig.f.base.ctx);
    REQUIRE(id != 0);
    rig.engine.join(hunter, id, rig.f.base.ctx);

    // The participant deals 5000 validated damage against a template target.
    const characterId target = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "farmer", 5000);
    ev::Event *live          = rig.engine.findMutable(id);
    REQUIRE(live != nullptr);
    live->targetIds.insert(target);
    Character &hunterChar        = rig.f.base.characters.get(hunter);
    Character &targetChar        = rig.f.base.characters.get(target);
    targetChar.body.maxHp        = 20000; // the hp cap must not bite the pinned amount
    targetChar.body.currentHp    = 20000;
    const ev::EventTemplate *tpl = rig.table.byKey("objective_event");
    REQUIRE(tpl != nullptr);
    CHECK(ev::recordAction(*live, ActionType::DAMAGE, 5000, hunterChar, targetChar, *tpl, rig.f.base.ctx) == 5000);

    // The transition fires (the 1-day timer): attempts 1, the difficulty
    // moves by the continuous performance (5000 amount / 10000 cap → 5000;
    // delta 5000 - 4000 = 1000, /1 → successRate 5000, difficulty 4000).
    rig.f.base.ctx.now += core::DAY_LENGTH_SECONDS;
    rig.engine.tick(rig.f.base.ctx);
    const ev::Event *ev = rig.engine.find(id);
    REQUIRE(ev != nullptr);
    CHECK(ev->currentState.attempts == 1);
    CHECK(ev->currentState.successRate == 5000);
    CHECK(ev->currentState.difficulty == 4000);
    CHECK(ev->currentState.stateId == 2);

    // The event ends from state 2's entry (EndEvent); no further attempt.
    CHECK(rig.engine.activeCount() == 0);
}

// ============================================================================
// Pinned test 8 — the forced maxDuration exit reads the FAILURE verdict:
// an objective-less state's binary performance is 0 (the successRate drops
// by the moving average; the attempt is still ledgered exactly once).
// ============================================================================
TEST_CASE(
    "TestDifficulty_ForcedExitReadsFailure"
)
{
    ev::EventTemplate tpl;
    tpl.id                 = 1;
    tpl.key                = "timer_only";
    tpl.global             = false;
    tpl.initialStateId     = 1;
    tpl.initialDifficulty  = 5000;
    tpl.initialSuccessRate = 5000;
    tpl.maxDurationDays    = 1;
    ev::TemplateState a;
    a.stateId = 1;
    // No objectives (binary), no transitions: the maxDuration backstop owns
    // the exit (the validator accepts a terminal state).
    ev::TemplateState terminal;
    terminal.stateId = 2;
    tpl.states.push_back(a);
    tpl.states.push_back(terminal);

    DifficultyRig rig;
    rig.table.insert(tpl);
    const std::uint64_t id
        = rig.engine.startEventByKey("timer_only", JusticeFixture::kRegionA, 0, nullptr, rig.f.base.ctx);
    REQUIRE(id != 0);

    rig.f.base.ctx.now += core::DAY_LENGTH_SECONDS;
    rig.engine.tick(rig.f.base.ctx);
    CHECK(rig.engine.activeCount() == 0);
    const ev::Event *ev = rig.engine.find(id);
    REQUIRE(ev != nullptr);
    // binary(false): delta -5000, /1 → both fields clamp at 0.
    CHECK(ev->currentState.attempts == 1);
    CHECK(ev->currentState.successRate == 0);
    CHECK(ev->currentState.difficulty == 0);
}
