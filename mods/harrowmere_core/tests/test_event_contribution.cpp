// test_event_contribution.cpp — task 5.3's pinned tests: the validation
// guard set (self-harm, non-targets, consent-or-warrant, the heal-economy
// triple-min, the objective budget), the contribution math (cap-zero guard,
// weighted normalized sum, isqrt damping, the MIN_CONTRIBUTION floor), the
// snapshot watermark, and the hpLost accrual arm through the runtime router.
#include <doctest.h>

#include <algorithm>
#include <cstdint>

#include "character/status.hpp"
#include "core/constants.hpp"
#include "events/contribution.hpp"
#include "events/engine.hpp"
#include "events/template.hpp"
#include "justice/evidence.hpp"
#include "justice/warrants.hpp"
#include "progression/power.hpp"

#include "test_justice_fixture.hpp"

namespace
{

using namespace harrowmere;
using namespace justice_test;
using harrowmere::justice::Evidence;
using harrowmere::justice::Report;
namespace ev = harrowmere::events;

// A two-action template (DAMAGE cap 10000 / weight 8000; HEALING cap 5000 /
// weight 6000) — the shapes the contribution math chews on.
ev::EventTemplate combatTemplate()
{
    ev::EventTemplate tpl;
    tpl.key                                                         = "combat_event";
    tpl.global                                                      = false;
    tpl.initialStateId                                              = 1;
    tpl.initialDifficulty                                           = 0;
    tpl.initialSuccessRate                                          = 5000;
    tpl.actionCap[static_cast<std::size_t>(ActionType::DAMAGE)]     = 10000;
    tpl.actionWeight[static_cast<std::size_t>(ActionType::DAMAGE)]  = 8000;
    tpl.actionCap[static_cast<std::size_t>(ActionType::HEALING)]    = 5000;
    tpl.actionWeight[static_cast<std::size_t>(ActionType::HEALING)] = 6000;

    ev::TemplateState a;
    a.stateId = 1;
    ev::Transition toB;
    toB.to = 2;
    ev::Condition timer;
    timer.kind = ev::ConditionKind::Timer;
    timer.days = 30;
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

// A template with one OBJECTIVE-type objective (the object_delivered shape).
ev::EventTemplate deliveryTemplate()
{
    ev::EventTemplate tpl                                             = combatTemplate();
    tpl.key                                                           = "delivery_event";
    tpl.actionCap[static_cast<std::size_t>(ActionType::DAMAGE)]       = 0;
    tpl.actionWeight[static_cast<std::size_t>(ActionType::DAMAGE)]    = 0;
    tpl.actionCap[static_cast<std::size_t>(ActionType::HEALING)]      = 0;
    tpl.actionWeight[static_cast<std::size_t>(ActionType::HEALING)]   = 0;
    tpl.actionCap[static_cast<std::size_t>(ActionType::OBJECTIVE)]    = 5000;
    tpl.actionWeight[static_cast<std::size_t>(ActionType::OBJECTIVE)] = 8000;

    ev::Objective o;
    o.objectiveId = 1;
    o.actionType  = ActionType::OBJECTIVE;
    o.cap         = 5000;
    tpl.states[0].objectives.push_back(o);
    return tpl;
}

struct ContributionRig
{
    JusticeFixture f;
    ev::EventTemplateTable table;
    ev::EventEngine engine;
    ev::ContributionRouter router;

    ContributionRig()
    {
        f.base.ctx.now = 1000;
        engine.bind(&table);
        engine.bindContext(&f.base.ctx);
        router.bind(&engine, &table, &f.base.ctx);
        f.base.ctx.events = &router;
    }

    ev::Event &start(
        const char *key, regionId region
    )
    {
        const std::uint64_t id = engine.startEventByKey(key, region, 0, nullptr, f.base.ctx);
        REQUIRE(id != 0);
        ev::Event *ev = engine.findMutable(id);
        REQUIRE(ev != nullptr);
        return *ev;
    }

    // The event id of the single active event (the rig starts exactly one).
    std::uint64_t currentId()
    {
        REQUIRE(engine.activeCount() == 1);
        return engine.active().begin()->first;
    }
};

} // namespace

// ============================================================================
// Pinned test — the self-harm farm guard: a target hurting itself records
// nothing (a manhunt culprit punching itself pays no contribution).
// ============================================================================
TEST_CASE(
    "TestValidation_SelfDamageZero"
)
{
    ContributionRig rig;
    rig.table.insert(combatTemplate());
    ev::Event &ev       = rig.start("combat_event", JusticeFixture::kRegionA);
    const characterId p = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "guard", 8000);
    rig.engine.join(p, rig.currentId(), rig.f.base.ctx);
    ev.targetIds.insert(p);
    Character &c                 = rig.f.base.characters.get(p);
    c.body.maxHp                 = 20000;
    c.body.currentHp             = 20000;
    const ev::EventTemplate *tpl = rig.table.byKey("combat_event");
    REQUIRE(tpl != nullptr);
    CHECK(ev::validateActionAmount(ev, ActionType::DAMAGE, 5000, c, c, *tpl, rig.f.base.ctx) == 0);
}
// ============================================================================
// Pinned test — only template targets pay: trading hits with a friend (a
// participant not in targetIds) records nothing.
// ============================================================================
TEST_CASE(
    "TestValidation_NonTargetDamageZero"
)
{
    ContributionRig rig;
    rig.table.insert(combatTemplate());
    ev::Event &ev       = rig.start("combat_event", JusticeFixture::kRegionA);
    const characterId a = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "guard", 8000);
    const characterId b = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "farmer", 8000);
    rig.engine.join(a, rig.currentId(), rig.f.base.ctx);
    rig.engine.join(b, rig.currentId(), rig.f.base.ctx);
    // NEITHER id lands in targetIds — the doc's player-view note.
    Character &ca                = rig.f.base.characters.get(a);
    Character &cb                = rig.f.base.characters.get(b);
    const ev::EventTemplate *tpl = rig.table.byKey("combat_event");
    REQUIRE(tpl != nullptr);
    CHECK(ev::validateActionAmount(ev, ActionType::DAMAGE, 5000, ca, cb, *tpl, rig.f.base.ctx) == 0);
    CHECK(ev::validateActionAmount(ev, ActionType::DAMAGE, 5000, cb, ca, *tpl, rig.f.base.ctx) == 0);
}

// ============================================================================
// Pinned test — a player target needs consent or a live warrant on the
// target's territory; the invulnerable arm refuses before any of it.
// ============================================================================
TEST_CASE(
    "TestValidation_PlayerTargetNeedsConsentOrWarrant"
)
{
    ContributionRig rig;
    rig.table.insert(combatTemplate());
    ev::Event &ev            = rig.start("combat_event", JusticeFixture::kRegionB);
    const characterId hunter = rig.f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "guard", 8000);
    const characterId player = rig.f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    rig.engine.join(hunter, rig.currentId(), rig.f.base.ctx);
    ev.targetIds.insert(player);
    Character &ch                = rig.f.base.characters.get(hunter);
    Character &cp                = rig.f.base.characters.get(player);
    cp.body.maxHp                = 20000;
    cp.body.currentHp            = 20000;
    const ev::EventTemplate *tpl = rig.table.byKey("combat_event");
    REQUIRE(tpl != nullptr);

    // No consent, no warrant: nothing pays.
    CHECK(ev::validateActionAmount(ev, ActionType::DAMAGE, 5000, ch, cp, *tpl, rig.f.base.ctx) == 0);

    // Consent (2.1's duel channel) licenses the damage.
    cp.duelOpponent = hunter;
    CHECK(ev::validateActionAmount(ev, ActionType::DAMAGE, 5000, ch, cp, *tpl, rig.f.base.ctx) == 5000);
    cp.duelOpponent = 0;

    // A live warrant on the player in the damage's territory licenses it too.
    const std::uint64_t caseId = justice::commitEvidence(
        CrimeType::THEFT, player, /*victim=*/0, JusticeFixture::kVillageB, rig.f.base.ctx, {}, 0
    )
                                     .id;
    Evidence *e = rig.f.caseById(caseId);
    REQUIRE(e != nullptr);
    e->reports.push_back(Report{55, 6000, true, false});
    justice::issueWarrant(*e, rig.f.base.ctx);
    CHECK(ev::validateActionAmount(ev, ActionType::DAMAGE, 5000, ch, cp, *tpl, rig.f.base.ctx) == 5000);
}

// ============================================================================
// Pinned test — the invulnerable arm (D-INVULN-1): objective-critical
// actors pay nothing when hit.
// ============================================================================
TEST_CASE(
    "TestValidation_InvulnerableTargetZero"
)
{
    ContributionRig rig;
    rig.table.insert(combatTemplate());
    ev::Event &ev       = rig.start("combat_event", JusticeFixture::kRegionA);
    const characterId a = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "guard", 8000);
    const characterId b = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "farmer", 8000);
    rig.engine.join(a, rig.currentId(), rig.f.base.ctx);
    ev.targetIds.insert(b);
    rig.f.base.npcs.get(b).invulnerable = true;
    Character &ca                       = rig.f.base.characters.get(a);
    Character &cb                       = rig.f.base.characters.get(b);
    const ev::EventTemplate *tpl        = rig.table.byKey("combat_event");
    REQUIRE(tpl != nullptr);
    CHECK(ev::validateActionAmount(ev, ActionType::DAMAGE, 5000, ca, cb, *tpl, rig.f.base.ctx) == 0);
}

// ============================================================================
// Pinned test — the HEALING triple-min: a healer earns only for hp the
// targets removed (heal loops impossible — the doc's player-view note).
// ============================================================================
TEST_CASE(
    "TestValidation_HealCappedByHpLost"
)
{
    ContributionRig rig;
    rig.table.insert(combatTemplate());
    ev::Event &ev            = rig.start("combat_event", JusticeFixture::kRegionA);
    const characterId healer = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "healer", 8000);
    const characterId target = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "farmer", 8000);
    rig.engine.join(healer, rig.currentId(), rig.f.base.ctx);
    rig.engine.join(target, rig.currentId(), rig.f.base.ctx);
    Character &chl               = rig.f.base.characters.get(healer);
    Character &ctg               = rig.f.base.characters.get(target);
    ctg.body.maxHp               = 120;
    ctg.body.currentHp           = 20; // 100 hp of room
    ev.hpLost[target]            = 30; // the targets removed exactly 30
    const ev::EventTemplate *tpl = rig.table.byKey("combat_event");
    REQUIRE(tpl != nullptr);
    CHECK(ev::validateActionAmount(ev, ActionType::HEALING, 100, chl, ctg, *tpl, rig.f.base.ctx) == 30);
    // No target-inflicted damage: the heal pays nothing (the loop dies here).
    ev.hpLost.erase(target);
    CHECK(ev::validateActionAmount(ev, ActionType::HEALING, 100, chl, ctg, *tpl, rig.f.base.ctx) == 0);
}

// ============================================================================
// Pinned test — the OBJECTIVE budget: the claim clamps to the bound
// objective's remaining cap.
// ============================================================================
TEST_CASE(
    "TestValidation_ObjectiveCappedByProgress"
)
{
    ContributionRig rig;
    rig.table.insert(deliveryTemplate());
    ev::Event &ev       = rig.start("delivery_event", JusticeFixture::kRegionA);
    const characterId a = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "farmer", 8000);
    rig.engine.join(a, rig.currentId(), rig.f.base.ctx);
    Character &ca                = rig.f.base.characters.get(a);
    const ev::EventTemplate *tpl = rig.table.byKey("delivery_event");
    REQUIRE(tpl != nullptr);

    CHECK(ev::getProgressDelta(ev, *tpl, a) == 5000);
    CHECK(ev::recordAction(ev, ActionType::OBJECTIVE, 3000, ca, ca, *tpl, rig.f.base.ctx) == 3000);
    CHECK(ev::getProgressDelta(ev, *tpl, a) == 2000);
    CHECK(ev::recordAction(ev, ActionType::OBJECTIVE, 4000, ca, ca, *tpl, rig.f.base.ctx) == 2000);
    CHECK(ev::getProgressDelta(ev, *tpl, a) == 0);
    CHECK(ev::recordAction(ev, ActionType::OBJECTIVE, 4000, ca, ca, *tpl, rig.f.base.ctx) == 0);
}

// ============================================================================
// Pinned test — the cap-zero guard: an unused action type divides nowhere.
// ============================================================================
TEST_CASE(
    "TestContribution_CapZeroGuardNoDivByZero"
)
{
    ContributionRig rig;
    rig.table.insert(deliveryTemplate()); // DAMAGE/HEALING caps are 0
    ev::Event &ev       = rig.start("delivery_event", JusticeFixture::kRegionA);
    const characterId a = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "farmer", 8000);
    rig.engine.join(a, rig.currentId(), rig.f.base.ctx);
    ev.actionAmounts[a][static_cast<std::size_t>(ActionType::DAMAGE)]  = 9000;
    ev.actionAmounts[a][static_cast<std::size_t>(ActionType::HEALING)] = 9000;
    const ev::EventTemplate *tpl                                       = rig.table.byKey("delivery_event");
    REQUIRE(tpl != nullptr);
    // Only the OBJECTIVE slot contributes; the zero caps are skipped.
    CHECK(ev::getContribution(ev, a, *tpl) == 0);
}

// ============================================================================
// Pinned test — the weighted normalized sum (the doc formula).
// ============================================================================
TEST_CASE(
    "TestContribution_WeightedNormalizedSum"
)
{
    ContributionRig rig;
    rig.table.insert(combatTemplate());
    ev::Event &ev       = rig.start("combat_event", JusticeFixture::kRegionA);
    const characterId a = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "guard", 8000);
    ev.actionAmounts[a][static_cast<std::size_t>(ActionType::DAMAGE)]  = 5000; // norm 5000 → 8000*5000/10000 = 4000
    ev.actionAmounts[a][static_cast<std::size_t>(ActionType::HEALING)] = 5000; // norm SCALE → 6000
    const ev::EventTemplate *tpl                                       = rig.table.byKey("combat_event");
    REQUIRE(tpl != nullptr);
    CHECK(ev::getContribution(ev, a, *tpl) == 10000);
}

// ============================================================================
// Pinned test — the snapshot is a MAX watermark: it rises with the
// participant's power and never falls (reset only by endEvent's clear).
// ============================================================================
TEST_CASE(
    "TestSnapshot_MaxWatermarkPersists"
)
{
    ContributionRig rig;
    rig.table.insert(combatTemplate());
    ev::Event &ev       = rig.start("combat_event", JusticeFixture::kRegionA);
    const characterId a = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "guard", 8000);
    rig.engine.join(a, rig.currentId(), rig.f.base.ctx);
    Character &c                                                      = rig.f.base.characters.get(a);
    c.progression.stats[static_cast<std::size_t>(StatType::STRENGTH)] = 5;
    markPowerDirty(c);

    ev::updatePowerSnapshot(ev, a, rig.f.base.ctx);
    const std::uint64_t first = ev.currentState.powerSnapshots.at(a);
    CHECK(first > 0);

    // Power rises: the watermark follows.
    c.progression.stats[static_cast<std::size_t>(StatType::STRENGTH)] = 50;
    markPowerDirty(c);
    ev::updatePowerSnapshot(ev, a, rig.f.base.ctx);
    const std::uint64_t raised = ev.currentState.powerSnapshots.at(a);
    CHECK(raised > first);

    // Power falls: the watermark holds (the anti-sandbagging read).
    c.progression.stats[static_cast<std::size_t>(StatType::STRENGTH)] = 1;
    markPowerDirty(c);
    ev::updatePowerSnapshot(ev, a, rig.f.base.ctx);
    CHECK(ev.currentState.powerSnapshots.at(a) == raised);
}

// ============================================================================
// Pinned test — the isqrt damping: raw * SCALE / (SCALE + isqrt(snapshot));
// an unsnapshotted participant reads undamped.
// ============================================================================
TEST_CASE(
    "TestEffective_IsqrtDamping"
)
{
    ContributionRig rig;
    rig.table.insert(combatTemplate());
    ev::Event &ev       = rig.start("combat_event", JusticeFixture::kRegionA);
    const characterId a = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "guard", 8000);
    ev.actionAmounts[a][static_cast<std::size_t>(ActionType::DAMAGE)] = 5000; // raw 4000
    const ev::EventTemplate *tpl                                      = rig.table.byKey("combat_event");
    REQUIRE(tpl != nullptr);

    CHECK(ev::getEffectiveContribution(ev, a, *tpl) == 4000); // no snapshot: undamped

    ev.currentState.powerSnapshots[a] = 10000; // isqrt = 100
    CHECK(ev::getEffectiveContribution(ev, a, *tpl) == 4000ull * SCALE / (SCALE + 100));
}

// ============================================================================
// Pinned test — the MIN_CONTRIBUTION floor: token participation pays
// nothing (1% of SCALE).
// ============================================================================
TEST_CASE(
    "TestShare_MinContributionFloors"
)
{
    ContributionRig rig;
    rig.table.insert(combatTemplate());
    ev::Event &ev       = rig.start("combat_event", JusticeFixture::kRegionA);
    const characterId a = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "guard", 8000);
    const characterId b = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "farmer", 8000);
    rig.engine.join(a, rig.currentId(), rig.f.base.ctx);
    rig.engine.join(b, rig.currentId(), rig.f.base.ctx);                       // the share walk reads participants
    ev.actionAmounts[a][static_cast<std::size_t>(ActionType::DAMAGE)] = 10000; // raw 8000
    ev.actionAmounts[b][static_cast<std::size_t>(ActionType::DAMAGE)] = 100;   // raw 80 ≤ MIN_CONTRIBUTION
    const ev::EventTemplate *tpl                                      = rig.table.byKey("combat_event");
    REQUIRE(tpl != nullptr);
    CHECK(ev::getRewardShare(ev, a, *tpl) > 0);
    CHECK(ev::getRewardShare(ev, b, *tpl) == 0); // mine <= MIN_CONTRIBUTION → 0
}

// ============================================================================
// Pinned test — the hpLost accrual: when a character in targetIds deals
// damage to an event participant, the applied amount lands in
// ev.hpLost[victim.id] (through the runtime router, 3.2's hook).
// ============================================================================
TEST_CASE(
    "TestHpLost_AccruesWhenTargetDealsDamage"
)
{
    ContributionRig rig;
    rig.table.insert(combatTemplate());
    ev::Event &ev             = rig.start("combat_event", JusticeFixture::kRegionB);
    const characterId culprit = rig.f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    const characterId hunter  = rig.f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "guard", 8000);
    rig.engine.join(hunter, rig.currentId(), rig.f.base.ctx);
    ev.targetIds.insert(culprit);

    // The culprit (a target, not a participant) damages the hunter: the
    // hpLost budget grows; the culprit records no contribution (targets do
    // not earn — and the hunter is not a template target anyway).
    rig.router.onCombatDamage(DamageEvent{culprit, hunter, 40, DamageType::Melee});
    REQUIRE(ev.hpLost.count(hunter) == 1);
    CHECK(ev.hpLost.at(hunter) == 40);
    CHECK(ev.actionAmounts.find(culprit) == ev.actionAmounts.end());

    // A second swing accrues.
    rig.router.onCombatDamage(DamageEvent{culprit, hunter, 25, DamageType::Melee});
    CHECK(ev.hpLost.at(hunter) == 65);
}

// ============================================================================
// Extra pin — the leave mark: a participant who left early records nothing
// through the router (the leave mark refuses later contribution).
// ============================================================================
TEST_CASE(
    "TestRouter_LeaveEarlyRefusesContribution"
)
{
    ContributionRig rig;
    rig.table.insert(combatTemplate());
    ev::Event &ev            = rig.start("combat_event", JusticeFixture::kRegionB);
    const characterId hunter = rig.f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "guard", 8000);
    const characterId target = rig.f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "farmer", 5000);
    rig.engine.join(hunter, rig.currentId(), rig.f.base.ctx);
    ev.targetIds.insert(target);
    rig.f.base.characters.get(target).body.maxHp     = 20000;
    rig.f.base.characters.get(target).body.currentHp = 20000;

    rig.engine.leaveEarly(hunter, rig.currentId(), rig.f.base.ctx);
    REQUIRE(rig.engine.hasLeftEarly(rig.currentId(), hunter));
    rig.router.onCombatDamage(DamageEvent{hunter, target, 500, DamageType::Melee});
    CHECK(ev.actionAmounts.find(hunter) == ev.actionAmounts.end());

    // The id STAYS in participants (the end-of-event rewards still pay for
    // work done — 5.1's leave rule).
    REQUIRE(std::find(ev.participants.begin(), ev.participants.end(), hunter) != ev.participants.end());
}

// ============================================================================
// Extra pin — the snapshot cadence: the first sight stamps the baseline;
// the watermark lands one A-106 interval later; an ended event leaves no
// book behind.
// ============================================================================
TEST_CASE(
    "TestSnapshot_CadenceEveryInterval"
)
{
    ContributionRig rig;
    // The walk's reader gate (the 8.3 as-built): the runtime records
    // snapshots only for templates a paying reward table serves — the
    // damping math is the snapshot's one reader. The cadence under test is
    // unchanged; the fixture pays so the walk records for it.
    ev::EventTemplate cadenceTemplate         = combatTemplate();
    cadenceTemplate.hasRewardTable            = true;
    cadenceTemplate.rewards.xpPerContribution = 1;
    rig.table.insert(cadenceTemplate);
    ev::Event &ev       = rig.start("combat_event", JusticeFixture::kRegionA);
    const characterId a = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "guard", 8000);
    rig.engine.join(a, rig.currentId(), rig.f.base.ctx);
    Character &c                                                      = rig.f.base.characters.get(a);
    c.progression.stats[static_cast<std::size_t>(StatType::STRENGTH)] = 5;
    markPowerDirty(c);

    ev::ContributionTickSystem ticker;
    ticker.engine = &rig.engine;

    ticker.run(rig.f.base.ctx); // the baseline stamp
    CHECK(ev.currentState.powerSnapshots.empty());

    rig.f.base.ctx.now += SNAPSHOT_INTERVAL_GAME_S - 1;
    ticker.run(rig.f.base.ctx);
    CHECK(ev.currentState.powerSnapshots.empty()); // the interval has not elapsed

    rig.f.base.ctx.now += 1;
    ticker.run(rig.f.base.ctx);
    REQUIRE(ev.currentState.powerSnapshots.count(a) == 1); // the watermark landed
    CHECK(ev.currentState.powerSnapshots.at(a) > 0);
}
