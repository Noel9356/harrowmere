// test_quests.cpp — task 6.4's pinned tests: a quest IS an event instance
// (the same engine lifecycle, no separate path), the gathering objective
// pays on delivery through 5.4's pool, the story chain nests through
// SpawnNestedEvent, the contribution rules hold for quest rewards (a
// zero-contribution accepter is paid 0), no reward is time-exclusive (the
// leaver and the timer-failed event still pay for work done), and the
// content checklist warns on damage-only authoring.
#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "character/character.hpp"
#include "core/constants.hpp"
#include "data/json.hpp"
#include "events/contribution.hpp"
#include "events/rewards.hpp"
#include "events/engine.hpp"
#include "events/template.hpp"
#include "quests/quests.hpp"

#include "test_justice_fixture.hpp"

namespace
{

using namespace harrowmere;
using namespace justice_test;
namespace ev = harrowmere::events;
namespace qu = harrowmere::quests;

// The fixture's pinned hierarchy ids (static members; local aliases keep the
// helpers readable).
constexpr territoryId kVillageA = JusticeFixture::kVillageA;
constexpr regionId kRegionA     = JusticeFixture::kRegionA;

// The rig: the justice fixture + the SHIPPED quest content (the same files
// the runtime loads — the tests are the data's customers, no invented rows).
struct QuestRig
{
    JusticeFixture f;
    ev::EventTemplateTable templates;
    qu::QuestTable quests;
    ev::EventEngine engine;
    characterId player = 0;

    QuestRig()
    {
        const std::string dataDir = phase2_test::resolveDataDir().string();
        std::vector<data::DataError> errors;
        REQUIRE_MESSAGE(
            ev::loadEventTemplates(dataDir + "/event_templates.json", templates, errors),
            "shipped event_templates.json failed to load"
        );
        REQUIRE_MESSAGE(qu::loadQuests(dataDir + "/quests.json", quests, errors), "shipped quests.json failed to load");
        engine.bind(&templates);
        engine.bindContext(&f.base.ctx);

        player                = f.base.characters.createPlayer(PlayerCreation{"quester", {}, ""});
        f.base.world.playerId = player;
    }

    // The player joins and delivers `amount` progress units of the quest.
    std::uint64_t play(
        const char *questKey, regionId region, std::uint32_t amount
    )
    {
        const std::uint64_t eventId = qu::startQuest(engine, quests, templates, questKey, region, f.base.ctx);
        REQUIRE(eventId != 0);
        engine.join(player, eventId, f.base.ctx);
        if (amount != 0)
        {
            const std::uint32_t delivered
                = qu::deliverQuestObjective(engine, templates, eventId, player, amount, f.base.ctx);
            REQUIRE(delivered == amount); // the delivery validates in full (fresh quest, empty books)
        }
        return eventId;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// TestQuest_IsAnEventInstance (card 6.4, pinned name): the started quest is a
// regional event instance of the bound template — the same lifecycle, the
// same join bookkeeping, no parallel structure.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestQuest_IsAnEventInstance"
)
{
    QuestRig rig;
    const ev::EventTemplate *tpl = rig.templates.byKey("GATHER_HERBS");
    REQUIRE(tpl != nullptr);

    const std::uint64_t eventId
        = qu::startQuest(rig.engine, rig.quests, rig.templates, "gather_herbs", kRegionA, rig.f.base.ctx);
    REQUIRE(eventId != 0);
    const ev::Event *ev = rig.engine.find(eventId);
    REQUIRE(ev != nullptr);
    CHECK(ev->templateId == tpl->id); // the quest's event IS the bound template's instance
    CHECK(ev->region == kRegionA);    // regional, region-bound
    CHECK(ev->depth == 0);
    CHECK(ev->participants.empty()); // the doc comment: participants start empty
    CHECK(ev->currentState.stateId == tpl->initialStateId);
    CHECK(ev->currentState.difficulty == tpl->initialDifficulty);
    CHECK(ev->currentState.successRate == tpl->initialSuccessRate);

    // The same join bookkeeping as any world event (5.1's join).
    rig.engine.join(rig.player, eventId, rig.f.base.ctx);
    CHECK(ev::getActionAmount(*ev, rig.player, ActionType::OBJECTIVE) == 0); // the books start empty
    const ev::Event *afterJoin = rig.engine.find(eventId);
    REQUIRE(afterJoin != nullptr);
    CHECK(
        std::find(afterJoin->participants.begin(), afterJoin->participants.end(), rig.player)
        != afterJoin->participants.end()
    );

    // Unknown quests and unknown templates refuse (0), never crash.
    CHECK(qu::startQuest(rig.engine, rig.quests, rig.templates, "no_such_quest", kRegionA, rig.f.base.ctx) == 0);
}

// ---------------------------------------------------------------------------
// TestQuest_GatheringObjectivePaysOnDelivery (card 6.4, pinned name): the
// delivery verb accrues validated OBJECTIVE progress, the ActionThreshold
// transition fires on the tick, and the payout lands through 5.4's pool —
// the giver's rates price CONTRIBUTION POINTS (the bound template's table).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestQuest_GatheringObjectivePaysOnDelivery"
)
{
    QuestRig rig;
    const std::int64_t purseBefore = rig.f.base.characters.get(rig.player).progression.currency;
    const std::uint64_t eventId    = rig.play("gather_herbs", kRegionA, /*amount=*/3);

    // The objective cap is met; the transition fires on the next tick and
    // the state's exit ends the event (payout BEFORE the clears — 5.4's
    // doc order inside finishEvent).
    rig.engine.tick(rig.f.base.ctx);
    const ev::Event *ended = rig.engine.find(eventId);
    CHECK(rig.engine.findActiveByKey("GATHER_HERBS") == nullptr); // the event ended
    CHECK(ended != nullptr); // the ended record stays findable (the retention window)

    // The pool: contribution 10000 (the cap met) x the A-107 difficulty
    // multiplier at the payout point (post-transition: 7000 -> 13500).
    const std::int64_t purseAfter = rig.f.base.characters.get(rig.player).progression.currency;
    CHECK(purseAfter == purseBefore + 13500 * 4);
    // The XP leg rode the same pool through applyXpDelta (totalXp up).
    CHECK(rig.f.base.characters.get(rig.player).progression.totalXp == 13500 * 2);

    // Over-delivery is a no-op (the OBJECTIVE clamp: the remaining budget
    // was 0 at the end — nothing negative, nothing extra).
    CHECK(qu::deliverQuestObjective(rig.engine, rig.templates, eventId, rig.player, 5, rig.f.base.ctx) == 0);
}

// ---------------------------------------------------------------------------
// TestQuest_StoryChainViaNestedSpawn (card 6.4, pinned name): the story
// quest's stage transition spawns the chained stage through the engine's
// nested-spawn rule (depth + 1, the child in the parent's region).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestQuest_StoryChainViaNestedSpawn"
)
{
    QuestRig rig;
    const std::uint64_t parentId = rig.play("the_missing_charter", kRegionA, /*amount=*/1);
    (void)parentId; // the parent stage ends at the tick; the chain lives on in the child

    // The threshold fires on the tick; the onFire SpawnNestedEvent starts
    // the CHARTER_SCRIBE stage (the doc's story chain: SpawnNestedEvent +
    // the contribution rules carry over).
    rig.engine.tick(rig.f.base.ctx);
    CHECK(rig.engine.findActiveByKey("THE_MISSING_CHARTER") == nullptr); // the parent stage ended
    const ev::Event *child = rig.engine.findActiveByKey("CHARTER_SCRIBE");
    REQUIRE(child != nullptr); // the chained stage is live
    CHECK(child->depth == 1);  // the nesting cap's own bookkeeping (A-103)

    // The chain continues on the same verbs: deliver the scribe's leg.
    std::uint64_t childId = 0;
    for (const auto &[id, ev] : rig.engine.active())
    {
        (void)ev;
        childId = id; // exactly one active event remains — the child
    }
    REQUIRE(childId != 0);
    rig.engine.join(rig.player, childId, rig.f.base.ctx);
    CHECK(qu::deliverQuestObjective(rig.engine, rig.templates, childId, rig.player, 1, rig.f.base.ctx) == 1);
    rig.engine.tick(rig.f.base.ctx);
    CHECK(rig.engine.findActiveByKey("CHARTER_SCRIBE") == nullptr); // the chain resolved
}

// ---------------------------------------------------------------------------
// TestQuest_RewardsFollowContributionRules (card 6.4, pinned name): the
// zero-contribution accepter is paid 0 — the doc's shared-rewards rule, on
// the event payout path (there is no quest-specific payout anywhere).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestQuest_RewardsFollowContributionRules"
)
{
    QuestRig rig;
    const std::uint64_t eventId = rig.play("deliver_grain", kRegionA, /*amount=*/5);

    // A second participant joins and contributes NOTHING (the accepter).
    const characterId accepter
        = phase2_test::spawnNpc(rig.f.base, phase2_test::speciesIdByKey("human"), kVillageA, kRegionA, 30);
    rig.engine.join(accepter, eventId, rig.f.base.ctx);
    const std::int64_t accepterBefore = rig.f.base.characters.get(accepter).progression.currency;

    rig.engine.tick(rig.f.base.ctx);
    CHECK(rig.engine.findActiveByKey("DELIVER_GRAIN") == nullptr);

    // The worker takes the whole pool; the accepter earns nothing.
    CHECK(rig.f.base.characters.get(rig.player).progression.currency > STARTER_CURRENCY);
    CHECK(rig.f.base.characters.get(accepter).progression.currency == accepterBefore);
    // The accepter's books: zero contribution, exactly as they played.
    const ev::Event *ended = rig.engine.find(eventId);
    REQUIRE(ended != nullptr);
    CHECK(ev::getContribution(*ended, accepter, *rig.templates.byKey("DELIVER_GRAIN")) == 0);
}

// ---------------------------------------------------------------------------
// TestQuest_NoTimeExclusiveRewards (card 6.4, pinned name): leaving early
// keeps the earned share (the doc's leave-early rule), and the timer's
// failed end still pays for the work done — no reward is time-exclusive.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestQuest_NoTimeExclusiveRewards"
)
{
    QuestRig rig;
    const std::uint64_t eventId    = rig.play("deliver_grain", kRegionA, /*amount=*/3); // 3 of the 5 cap
    const std::int64_t purseBefore = rig.f.base.characters.get(rig.player).progression.currency;

    // Leaving early: the leave mark refuses later contribution, the id
    // STAYS in participants (the end-of-event reward still pays for the
    // work already done).
    rig.engine.leaveEarly(rig.player, eventId, rig.f.base.ctx);
    CHECK(qu::deliverQuestObjective(rig.engine, rig.templates, eventId, rig.player, 1, rig.f.base.ctx) == 0);

    // The timer forces the (failed) end: 5 days past the start, one tick.
    rig.f.base.ctx.now += 6ULL * core::DAY_LENGTH_SECONDS;
    rig.engine.tick(rig.f.base.ctx);
    CHECK(rig.engine.findActiveByKey("DELIVER_GRAIN") == nullptr);

    // The 3/5 work was paid through the same pool (contribution-based,
    // never success-gated, never time-gated): 3/5 of the cap -> norm 6000
    // -> raw 6000. The forced exit's difficulty update reads CONTINUOUS
    // performance on the capped objective (6000) -> delta 0 -> the
    // difficulty stands at 1500 -> multiplier 10750 -> pool 6450 points
    // -> currency 6450 x 3.
    const std::int64_t purseAfter = rig.f.base.characters.get(rig.player).progression.currency;
    CHECK(purseAfter == purseBefore + 6450 * 3);
}

// ---------------------------------------------------------------------------
// TestQuestValidator_WarnsOnDamageOnlyObjectives (card 6.4, pinned name):
// the content checklist rides the validator warnings — a damage-only quest
// warns (the doc's busywork rule), and the SHIPPED quests warn nothing.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestQuestValidator_WarnsOnDamageOnlyObjectives"
)
{
    // The violation: a template whose objectives are all DAMAGE-type.
    ev::EventTemplate damageOnly;
    damageOnly.id                 = 999;
    damageOnly.key                = "SLAY_TEN_BOARS";
    damageOnly.initialStateId     = 1;
    damageOnly.initialDifficulty  = 5000;
    damageOnly.initialSuccessRate = 5000;
    damageOnly.maxDurationDays    = 5;
    ev::TemplateState a;
    a.stateId = 1;
    ev::Objective slay;
    slay.objectiveId = 1;
    slay.actionType  = ActionType::DAMAGE;
    slay.cap         = 10000;
    a.objectives.push_back(slay);
    ev::Transition toB;
    toB.to = 2;
    ev::Condition timer;
    timer.kind = ev::ConditionKind::Timer;
    timer.days = 5;
    toB.conditions.push_back(timer);
    a.transitions.push_back(toB);
    ev::TemplateState b;
    b.stateId = 2;
    damageOnly.states.push_back(a);
    damageOnly.states.push_back(b);

    qu::QuestDef row;
    row.id               = 99;
    row.key              = "slay_ten_boars";
    row.eventTemplateKey = "SLAY_TEN_BOARS";
    row.description      = "too short";
    row.repeatable       = true;

    std::vector<std::string> warnings;
    qu::questChecklistWarnings(row, &damageOnly, warnings);
    REQUIRE(warnings.size() == 3); // (a) damage-only, (b) short description, (d) the template pays nothing
    CHECK(warnings[0].find("no non-damage objective") != std::string::npos);
    CHECK(warnings[1].find("200 characters") != std::string::npos);
    CHECK(warnings[2].find("pays nothing") != std::string::npos);

    // The shipped content: the checklist is satisfied — zero warnings (the
    // plugin's boot log stays clean; the rule is editorial, the data is not).
    QuestRig rig;
    for (const auto &[id, quest] : rig.quests.rows())
    {
        (void)id;
        std::vector<std::string> shipped;
        qu::questChecklistWarnings(quest, rig.templates.byKey(quest.eventTemplateKey), shipped);
        CHECK(shipped.empty());
    }

    // The null-template row: only the (d) warning fires.
    qu::QuestDef unbound;
    unbound.key = "ghost_quest";
    std::vector<std::string> ghost;
    qu::questChecklistWarnings(unbound, nullptr, ghost);
    REQUIRE(ghost.size() == 1);
    CHECK(ghost[0].find("binds no event template") != std::string::npos);
}
