// test_event_engine.cpp — task 5.1's pinned tests: the lifecycle (all four
// EventState fields), scale uniqueness, the depth cap, the participant
// bookkeeping (join/leave/end verbatim), the declared-order transition
// evaluation, the no-player resolution, the validator, and the two shipped
// Appendix-B templates running headless end-to-end.
#include <doctest.h>

#include <cstdint>

#include "core/constants.hpp"
#include "events/engine.hpp"
#include "events/template.hpp"
#include "world/world.hpp"

#include "test_justice_fixture.hpp"

namespace
{

using namespace harrowmere;
using namespace justice_test;
namespace ev = harrowmere::events;

// A minimal template: state A --(condition)--> state B (entry EndEvent).
ev::EventTemplate twoStateTemplate(
    std::uint64_t id, const char *key, bool global, std::uint32_t timerDays = 1
)
{
    ev::EventTemplate tpl;
    tpl.id                 = id;
    tpl.key                = key;
    tpl.global             = global;
    tpl.initialStateId     = 1;
    tpl.initialDifficulty  = 3000;
    tpl.initialSuccessRate = 5000;
    tpl.maxDurationDays    = 30;

    ev::TemplateState a;
    a.stateId = 1;
    ev::Transition toB;
    toB.to = 2;
    ev::Condition timer;
    timer.kind = ev::ConditionKind::Timer;
    timer.days = timerDays;
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

struct EventRig
{
    JusticeFixture f;
    ev::EventTemplateTable table;
    ev::EventEngine engine;

    EventRig()
    {
        f.base.ctx.now = 1000; // a non-zero epoch so timer math reads cleanly
        engine.bind(&table);
        engine.bindContext(&f.base.ctx);
    }

    void insert(
        ev::EventTemplate tpl
    )
    {
        table.insert(std::move(tpl));
    }
};

} // namespace

TEST_CASE(
    "TestEvent_StartInitializesFromTemplate"
)
{
    EventRig rig;
    rig.insert(twoStateTemplate(1, "test_event", /*global=*/false));

    const std::uint64_t id = rig.engine.startEventByKey("test_event", /*region=*/0, 0, nullptr, rig.f.base.ctx);
    REQUIRE(id != 0);
    const ev::Event *ev = rig.engine.find(id);
    REQUIRE(ev != nullptr);
    // All four EventState fields from the template (the doc comment pins
    // them); the participant books start empty.
    CHECK(ev->currentState.stateId == 1);
    CHECK(ev->currentState.difficulty == 3000);
    CHECK(ev->currentState.attempts == 0);
    CHECK(ev->currentState.successRate == 5000);
    CHECK(ev->currentState.powerSnapshots.empty());
    CHECK(ev->participants.empty());
    CHECK(ev->actionAmounts.empty());
    CHECK(ev->hpLost.empty());
    CHECK(ev->templateId == 1);
    CHECK(ev->region == 0); // regional scale carries the region (0 here = the doc's global value)
    CHECK(ev->depth == 0);
    CHECK(ev->startedAt == rig.f.base.ctx.now);
}

TEST_CASE(
    "TestEvent_OnlyOneGlobalActive"
)
{
    EventRig rig;
    rig.insert(twoStateTemplate(1, "global_one", /*global=*/true));
    rig.insert(twoStateTemplate(2, "global_two", /*global=*/true));

    const std::uint64_t first = rig.engine.startEventByKey("global_one", 0, 0, nullptr, rig.f.base.ctx);
    REQUIRE(first != 0);
    // A second global refuses while the first is live.
    CHECK(rig.engine.startEventByKey("global_two", 0, 0, nullptr, rig.f.base.ctx) == 0);
    CHECK(rig.engine.activeCount() == 1);

    // After the first ends, the second starts.
    rig.engine.finishEvent(first, true, rig.f.base.ctx);
    CHECK(rig.engine.startEventByKey("global_two", 0, 0, nullptr, rig.f.base.ctx) != 0);
}

TEST_CASE(
    "TestEvent_RegionalCoexistDifferentRegions_RefuseSameRegion"
)
{
    EventRig rig;
    rig.insert(twoStateTemplate(1, "regional_one", /*global=*/false));
    rig.insert(twoStateTemplate(2, "regional_two", /*global=*/false));

    const std::uint64_t inRegionA
        = rig.engine.startEventByKey("regional_one", /*region=*/7, 0, nullptr, rig.f.base.ctx);
    REQUIRE(inRegionA != 0);
    // A different region coexists; the same region refuses.
    CHECK(rig.engine.startEventByKey("regional_two", /*region=*/9, 0, nullptr, rig.f.base.ctx) != 0);
    CHECK(rig.engine.startEventByKey("regional_two", /*region=*/7, 0, nullptr, rig.f.base.ctx) == 0);
}

TEST_CASE(
    "TestEvent_DepthCapRefusesGrandchild"
)
{
    EventRig rig;
    // root --entry SpawnNestedEvent{child}--> child --entry SpawnNestedEvent{grandchild}-->
    ev::EventTemplate root = twoStateTemplate(1, "root", false, 30);
    ev::Action spawnChild;
    spawnChild.kind        = ev::ActionKind::SpawnNestedEvent;
    spawnChild.templateKey = "child";
    root.states[0].entryActions.push_back(spawnChild);
    rig.insert(root);

    ev::EventTemplate child = twoStateTemplate(2, "child", false, 30);
    ev::Action spawnGrand;
    spawnGrand.kind        = ev::ActionKind::SpawnNestedEvent;
    spawnGrand.templateKey = "grandchild";
    child.states[0].entryActions.push_back(spawnGrand);
    rig.insert(child);

    rig.insert(twoStateTemplate(3, "grandchild", false, 30));

    const std::uint64_t rootId = rig.engine.startEventByKey("root", 5, 0, nullptr, rig.f.base.ctx);
    REQUIRE(rootId != 0);
    // The child spawned at depth 1; the grandchild refused (A-103: an event
    // may spawn a child — no grandchildren).
    const ev::Event *childEv = rig.engine.findActiveByKey("child");
    REQUIRE(childEv != nullptr);
    CHECK(childEv->depth == 1);
    CHECK(rig.engine.findActiveByKey("grandchild") == nullptr);
}

TEST_CASE(
    "TestEvent_JoinSetsEventId_LeaveKeepsParticipant"
)
{
    EventRig rig;
    rig.insert(twoStateTemplate(1, "joinable", false));
    const characterId who
        = phase2_test::spawnNpc(rig.f.base, 1, JusticeFixture::kVillageB, JusticeFixture::kRegionB, 30);

    const std::uint64_t id = rig.engine.startEventByKey("joinable", 0, 0, nullptr, rig.f.base.ctx);
    REQUIRE(id != 0);

    rig.engine.join(who, id, rig.f.base.ctx);
    REQUIRE(rig.engine.find(id) != nullptr);
    CHECK(rig.engine.find(id)->participants.size() == 1);
    CHECK(rig.engine.find(id)->participants.front() == who); // sorted (R7)
    CHECK(rig.f.base.characters.get(who).eventId == id);

    // leaveEarly: the id STAYS in participants, the eventId STAYS on the
    // character (the card pitfall — the actor is still "in an event" until
    // the event ends), and the leave mark lands.
    rig.engine.leaveEarly(who, id, rig.f.base.ctx);
    CHECK(rig.engine.find(id)->participants.size() == 1);
    CHECK(rig.f.base.characters.get(who).eventId == id);
    CHECK(rig.engine.hasLeftEarly(id, who));
}

TEST_CASE(
    "TestEvent_EndEventClearsMapsAndEventIds"
)
{
    EventRig rig;
    rig.insert(twoStateTemplate(1, "ending", false));
    const characterId who
        = phase2_test::spawnNpc(rig.f.base, 1, JusticeFixture::kVillageB, JusticeFixture::kRegionB, 30);
    const characterId leaver
        = phase2_test::spawnNpc(rig.f.base, 1, JusticeFixture::kVillageB, JusticeFixture::kRegionB, 30);

    const std::uint64_t id = rig.engine.startEventByKey("ending", 0, 0, nullptr, rig.f.base.ctx);
    REQUIRE(id != 0);
    rig.engine.join(who, id, rig.f.base.ctx);
    rig.engine.join(leaver, id, rig.f.base.ctx);
    rig.engine.leaveEarly(leaver, id, rig.f.base.ctx);

    // Contribution state on the event (5.3's writes; 5.1 clears them).
    ev::Event *ev = rig.engine.findMutable(id);
    REQUIRE(ev != nullptr);
    ev->actionAmounts[who][static_cast<std::size_t>(ActionType::DAMAGE)] = 555;
    ev->hpLost[who]                                                      = 33;
    ev->currentState.powerSnapshots[who]                                 = 77;

    rig.engine.finishEvent(id, true, rig.f.base.ctx);

    // Verbatim assertions: the contribution maps clear, EVERY participant's
    // eventId clears (leavers included), the event left the active set.
    CHECK(rig.engine.findMutable(id) == nullptr);
    REQUIRE(rig.engine.find(id) != nullptr); // retained (ended) for the observers
    CHECK(rig.engine.find(id)->actionAmounts.empty());
    CHECK(rig.engine.find(id)->hpLost.empty());
    CHECK(rig.engine.find(id)->currentState.powerSnapshots.empty());
    CHECK(rig.f.base.characters.get(who).eventId == 0);
    CHECK(rig.f.base.characters.get(leaver).eventId == 0);
}

TEST_CASE(
    "TestEvent_TransitionsEvaluateInDeclaredOrder"
)
{
    EventRig rig;
    ev::EventTemplate tpl = twoStateTemplate(1, "ordered", false, 1);
    // A SECOND transition that also fires; the first-declared wins.
    ev::Transition alsoToThree;
    alsoToThree.to = 3;
    ev::Condition timer;
    timer.kind = ev::ConditionKind::Timer;
    timer.days = 1;
    alsoToThree.conditions.push_back(timer);
    tpl.states[0].transitions.push_back(alsoToThree);

    ev::TemplateState three;
    three.stateId = 3;
    tpl.states.push_back(three);
    rig.insert(tpl);

    const std::uint64_t id = rig.engine.startEventByKey("ordered", 0, 0, nullptr, rig.f.base.ctx);
    REQUIRE(id != 0);

    rig.f.base.ctx.now += core::DAY_LENGTH_SECONDS; // both conditions hold now
    rig.engine.tick(rig.f.base.ctx);

    REQUIRE(rig.engine.find(id) != nullptr);
    CHECK(rig.engine.find(id)->currentState.stateId == 2); // the FIRST declared transition fired
}

TEST_CASE(
    "TestEvent_TimerFallbackResolvesWithoutPlayers"
)
{
    EventRig rig;
    rig.insert(twoStateTemplate(1, "timer_resolved", false, /*timerDays=*/2));
    const std::uint64_t id = rig.engine.startEventByKey("timer_resolved", 0, 0, nullptr, rig.f.base.ctx);
    REQUIRE(id != 0);

    // No player ever acts; the timer fallback resolves the world.
    rig.f.base.ctx.now += core::DAY_LENGTH_SECONDS;
    rig.engine.tick(rig.f.base.ctx);
    REQUIRE(rig.engine.find(id) != nullptr);
    CHECK(rig.engine.find(id)->currentState.stateId == 1); // the timer has not run out

    rig.f.base.ctx.now += core::DAY_LENGTH_SECONDS;
    rig.engine.tick(rig.f.base.ctx);
    // The transition fired; state 2's entry EndEvent{success:true} ended it.
    REQUIRE(rig.engine.find(id) != nullptr);
    CHECK(rig.engine.find(id)->currentState.stateId == 2);
    CHECK(rig.engine.activeCount() == 0); // in the ended set, out of the active one
}

TEST_CASE(
    "TestEvent_TemplateValidatorRejectsPlayerOnlyStates"
)
{
    ev::EventTemplate tpl = twoStateTemplate(1, "player_only", false);
    tpl.maxDurationDays   = 0; // no forced exit either
    // The only transition is player-driven (a target death nobody causes
    // without players): no TimerCondition fallback, no maxDurationDays.
    tpl.states[0].transitions[0].conditions[0].kind = ev::ConditionKind::TargetDead;

    std::vector<std::string> problems;
    std::vector<std::string> warnings;
    ev::validateTemplate(tpl, problems, warnings);
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().find("no non-player-reachable exit") != std::string::npos);

    // The maxDurationDays guarantee satisfies the rule (the MANHUNT shape).
    tpl.maxDurationDays = 7;
    problems.clear();
    ev::validateTemplate(tpl, problems, warnings);
    CHECK(problems.empty());
}

TEST_CASE(
    "TestEvent_ShippedTemplatesResolveHeadlessEndToEnd"
)
{
    // The Done-when: the two Appendix-B templates run headless end-to-end
    // with zero player input. The real event_templates.json loads through
    // the real loader; the engine resolves both off the timer fallback.
    EventRig rig;
    ev::EventTemplateTable shipped;
    std::vector<data::DataError> errors;
    REQUIRE(ev::loadEventTemplates("mods/harrowmere_core/data/event_templates.json", shipped, errors));
    rig.engine.bind(&shipped);

    // -- bandit_raid: the targets spawn, the raid resolves, the world flag lands.
    const std::size_t populationBefore = rig.f.base.characters.population(); // before the raid's spawns
    const std::uint64_t raidId
        = rig.engine.startEventByKey("bandit_raid", JusticeFixture::kRegionA, 0, nullptr, rig.f.base.ctx);
    REQUIRE(raidId != 0);
    CHECK(rig.engine.find(raidId)->targetIds.size() == 6); // SpawnTargets{count: 6}

    rig.f.base.ctx.now += 5 * core::DAY_LENGTH_SECONDS; // the raid's own timer (== maxDuration)
    rig.engine.tick(rig.f.base.ctx);
    // State 200's exits ran (DespawnTargets + the persistent raided flag);
    // state 201's entry ended the event successfully.
    REQUIRE(rig.engine.find(raidId) != nullptr);
    CHECK(rig.engine.activeCount() == 0);
    const world::Region *regionA = rig.f.world.region(JusticeFixture::kRegionA);
    REQUIRE(regionA != nullptr);
    CHECK((regionA->flags & 1) != 0); // territory_templates.json: "raided": 1
    // The transient targets despawned; the world population is whole again.
    CHECK(rig.f.base.characters.population() == populationBefore);

    // -- MANHUNT: the warrant seed's culprit lands in targetIds; the timer
    //    fallback resolves it (the TargetDead/TargetCaptured arms never fire
    //    without a hunter).
    const characterId culprit = rig.f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    ManhuntSeed seed;
    seed.warrantId = 77;
    seed.culprit   = culprit;
    seed.territory = JusticeFixture::kVillageB;
    seed.region    = JusticeFixture::kRegionB;
    REQUIRE(rig.engine.spawnEvent("MANHUNT", seed));
    REQUIRE(rig.engine.findActiveByKey("MANHUNT") != nullptr);
    CHECK(rig.engine.findActiveByKey("MANHUNT")->targetIds.count(culprit) == 1);
    CHECK(rig.engine.findActiveByKey("MANHUNT")->excludedFromRewards.count(culprit) == 1); // culpritEarnsNothing

    // The evacuation entry action ran: the villageB civilians (non-guard
    // roles) carry the fleeing intent.
    for (const auto &[id, npc] : rig.f.base.npcs.all())
    {
        if (npc.homeTerritory == JusticeFixture::kVillageB && npc.roleKey != "guard")
        {
            CHECK(npc.fleeing);
        }
    }

    rig.f.base.ctx.now += 7 * core::DAY_LENGTH_SECONDS; // maxDurationDays
    rig.engine.tick(rig.f.base.ctx);
    CHECK(rig.engine.activeCount() == 0); // the world resolved without any player
    REQUIRE(rig.engine.findActiveByKey("MANHUNT") == nullptr);
    // The manhunt's end leaves the ended record (the EventEnded consumers read it).
    CHECK(rig.engine.ended().size() == 2);
}
