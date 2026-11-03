// test_event_rewards.cpp — task 5.4's pinned tests: the single-pass payout
// (the instrumentation counts effective-contribution evaluations == n), the
// exclusion and remainder rules, the R7 tie-break, A-107's difficulty
// scaling, the pool invariant under fuzzed shares, and the MANHUNT loop
// end-to-end (hunters earn, the culprit never does, capture resolves).
#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/constants.hpp"
#include "events/contribution.hpp"
#include "events/engine.hpp"
#include "events/rewards.hpp"
#include "events/template.hpp"
#include "justice/evidence.hpp"
#include "justice/warrants.hpp"

#include "test_justice_fixture.hpp"

namespace
{

using namespace harrowmere;
using namespace justice_test;
using harrowmere::justice::Evidence;
using harrowmere::justice::Report;
namespace ev = harrowmere::events;

// A reward-bearing template: DAMAGE cap 10000 / weight 10000, the payout
// rates the invariant math reads (currency rate 1 keeps the currency deltas
// equal to the pays; the fuzz test relies on exactly that). id/key are
// parameters so a rig can hold two difficulty variants at once.
ev::EventTemplate rewardTemplate(
    std::uint64_t id, const char *key, std::uint32_t difficulty
)
{
    ev::EventTemplate tpl;
    tpl.id                              = id;
    tpl.key                             = key;
    tpl.global                          = false;
    tpl.initialStateId                  = 1;
    tpl.initialDifficulty               = difficulty;
    tpl.initialSuccessRate              = 5000;
    tpl.maxDurationDays                 = 30;
    tpl.hasRewardTable                  = true;
    tpl.rewards.xpPerContribution       = 0;
    tpl.rewards.currencyPerContribution = 1;

    tpl.actionCap[static_cast<std::size_t>(ActionType::DAMAGE)]    = 10000;
    tpl.actionWeight[static_cast<std::size_t>(ActionType::DAMAGE)] = 10000;

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

struct RewardRig
{
    JusticeFixture f;
    RecordingSimEvents sink;
    ev::EventTemplateTable table;
    ev::EventEngine engine;
    ev::ContributionRouter router;

    RewardRig()
    {
        f.base.ctx.now       = 1000;
        f.base.ctx.simEvents = &sink;
        engine.bind(&table);
        engine.bindContext(&f.base.ctx);
        router.bind(&engine, &table, &f.base.ctx);
        f.base.ctx.events = &router;
    }

    ~RewardRig()
    {
        f.base.ctx.simEvents = nullptr;
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

    std::uint64_t currentId()
    {
        REQUIRE(engine.activeCount() == 1);
        return engine.active().begin()->first;
    }
};

} // namespace

// ============================================================================
// Pinned test — the single pass: distributing among n participants
// evaluates effective contribution exactly n times (never n² — the card
// pitfall's getRewardShare-in-the-loop shape would read 6+ here).
// ============================================================================
TEST_CASE(
    "TestRewards_TotalComputedOnce"
)
{
    RewardRig rig;
    rig.table.insert(rewardTemplate(1, "reward_event", 0));
    ev::Event &ev = rig.start("reward_event", JusticeFixture::kRegionA);
    std::vector<characterId> ids;
    for (int i = 0; i < 3; ++i)
    {
        const characterId id = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "guard", 8000);
        rig.engine.join(id, rig.currentId(), rig.f.base.ctx);
        ev.actionAmounts[id][static_cast<std::size_t>(ActionType::DAMAGE)] = 1000 * (i + 1);
        ids.push_back(id);
    }
    const ev::EventTemplate *tpl = rig.table.byKey("reward_event");
    REQUIRE(tpl != nullptr);

    ev::resetEffectiveContributionEvaluations();
    ev::distributeRewards(ev, /*pool=*/60000, *tpl, rig.f.base.ctx);
    CHECK(ev::effectiveContributionEvaluations() == 3); // n, not n²
}

// ============================================================================
// Pinned test — ineligible participants never shrink the others' shares
// (excluded smugglers and sub-floor contributors leave the total alone).
// ============================================================================
TEST_CASE(
    "TestRewards_IneligibleDoNotShrinkShares"
)
{
    const ev::EventTemplate tpl = rewardTemplate(1, "reward_event", 0);

    // Baseline: two eligible contributors.
    RewardRig rig;
    rig.table.insert(rewardTemplate(1, "reward_event", 0));
    ev::Event &ev       = rig.start("reward_event", JusticeFixture::kRegionA);
    const characterId a = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "guard", 8000);
    const characterId b = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "guard", 8000);
    rig.engine.join(a, rig.currentId(), rig.f.base.ctx);
    rig.engine.join(b, rig.currentId(), rig.f.base.ctx);
    ev.actionAmounts[a][static_cast<std::size_t>(ActionType::DAMAGE)] = 6000;
    ev.actionAmounts[b][static_cast<std::size_t>(ActionType::DAMAGE)] = 4000;
    const std::int64_t aBefore = rig.f.base.characters.get(a).progression.currency;
    const std::int64_t bBefore = rig.f.base.characters.get(b).progression.currency;
    ev::distributeRewards(ev, /*pool=*/50000, tpl, rig.f.base.ctx);
    const std::uint64_t aPaid = static_cast<std::uint64_t>(rig.f.base.characters.get(a).progression.currency - aBefore);
    const std::uint64_t bPaid = static_cast<std::uint64_t>(rig.f.base.characters.get(b).progression.currency - bBefore);
    CHECK(aPaid == 30000); // 50000 * 6000 / 10000
    CHECK(bPaid == 20000);

    // The first event leaves the active set before the second starts (the
    // payout point pays again — unmeasured; each arm captures its own
    // before).
    rig.engine.finishEvent(rig.currentId(), true, rig.f.base.ctx);

    // The same two contributors plus an EXCLUDED id (4.7's smuggling hook):
    // the shares are identical.
    ev::Event &ev2 = rig.start("reward_event", JusticeFixture::kRegionB);
    const characterId smuggler
        = rig.f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "farmer", 8000);
    rig.engine.join(a, rig.currentId(), rig.f.base.ctx);
    rig.engine.join(b, rig.currentId(), rig.f.base.ctx);
    rig.engine.join(smuggler, rig.currentId(), rig.f.base.ctx);
    ev2.actionAmounts[a][static_cast<std::size_t>(ActionType::DAMAGE)]        = 6000;
    ev2.actionAmounts[b][static_cast<std::size_t>(ActionType::DAMAGE)]        = 4000;
    ev2.actionAmounts[smuggler][static_cast<std::size_t>(ActionType::DAMAGE)] = 9000; // big, but excluded
    ev2.excludedFromRewards.insert(smuggler);
    const std::int64_t aBefore2 = rig.f.base.characters.get(a).progression.currency;
    const std::int64_t bBefore2 = rig.f.base.characters.get(b).progression.currency;
    ev::distributeRewards(ev2, /*pool=*/50000, tpl, rig.f.base.ctx);
    CHECK(static_cast<std::uint64_t>(rig.f.base.characters.get(a).progression.currency - aBefore2) == aPaid);
    CHECK(static_cast<std::uint64_t>(rig.f.base.characters.get(b).progression.currency - bBefore2) == bPaid);
}

// ============================================================================
// Pinned test — the flooring remainder goes to the top contributor; the
// payout invariant Σpaid + remainder == pool holds exactly.
// ============================================================================
TEST_CASE(
    "TestRewards_RemainderToTopContributor"
)
{
    RewardRig rig;
    rig.table.insert(rewardTemplate(1, "reward_event", 0));
    ev::Event &ev       = rig.start("reward_event", JusticeFixture::kRegionA);
    const characterId a = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "guard", 8000);
    const characterId b = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "guard", 8000);
    rig.engine.join(a, rig.currentId(), rig.f.base.ctx);
    rig.engine.join(b, rig.currentId(), rig.f.base.ctx);
    ev.actionAmounts[a][static_cast<std::size_t>(ActionType::DAMAGE)] = 6000; // raw 6000 (top)
    ev.actionAmounts[b][static_cast<std::size_t>(ActionType::DAMAGE)] = 4000;
    const ev::EventTemplate *tpl                                      = rig.table.byKey("reward_event");
    REQUIRE(tpl != nullptr);

    const std::int64_t aBefore = rig.f.base.characters.get(a).progression.currency;
    const std::int64_t bBefore = rig.f.base.characters.get(b).progression.currency;
    const std::uint64_t pool   = 10001; // 6000.6 → 6000; 4000.4 → 4000; remainder 1
    ev::distributeRewards(ev, pool, *tpl, rig.f.base.ctx);
    const std::uint64_t aPaid = static_cast<std::uint64_t>(rig.f.base.characters.get(a).progression.currency - aBefore);
    const std::uint64_t bPaid = static_cast<std::uint64_t>(rig.f.base.characters.get(b).progression.currency - bBefore);
    CHECK(aPaid == 6001); // 6000 + the flooring remainder (the TOP contributor)
    CHECK(bPaid == 4000);
    CHECK(aPaid + bPaid == pool); // the invariant, exactly
}

// ============================================================================
// Pinned test — equal shares break ties by the LOWER characterId (R7): the
// remainder lands on the lower id, deterministically.
// ============================================================================
TEST_CASE(
    "TestRewards_TieBreakByLowerId"
)
{
    RewardRig rig;
    rig.table.insert(rewardTemplate(1, "reward_event", 0));
    ev::Event &ev = rig.start("reward_event", JusticeFixture::kRegionA);
    // The FIRST spawn carries the lower id — the tie rule reads the ids.
    const characterId low  = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "guard", 8000);
    const characterId high = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "guard", 8000);
    REQUIRE(low < high);
    rig.engine.join(high, rig.currentId(), rig.f.base.ctx);
    rig.engine.join(low, rig.currentId(), rig.f.base.ctx);
    ev.actionAmounts[high][static_cast<std::size_t>(ActionType::DAMAGE)] = 5000;
    ev.actionAmounts[low][static_cast<std::size_t>(ActionType::DAMAGE)]  = 5000; // the exact tie
    const ev::EventTemplate *tpl                                         = rig.table.byKey("reward_event");
    REQUIRE(tpl != nullptr);

    const std::int64_t highBefore = rig.f.base.characters.get(high).progression.currency;
    const std::int64_t lowBefore  = rig.f.base.characters.get(low).progression.currency;
    ev::distributeRewards(ev, /*pool=*/3, *tpl, rig.f.base.ctx); // 1 + 1, remainder 1
    CHECK(static_cast<std::uint64_t>(rig.f.base.characters.get(low).progression.currency - lowBefore) == 2);
    CHECK(static_cast<std::uint64_t>(rig.f.base.characters.get(high).progression.currency - highBefore) == 1);
}

// ============================================================================
// Pinned test — A-107: the pool scales with the state difficulty
// (multiplier SCALE + difficulty / 2).
// ============================================================================
TEST_CASE(
    "TestRewards_DifficultyScalesPool"
)
{
    RewardRig rig;
    rig.table.insert(rewardTemplate(1, "reward_easy", 0));
    rig.table.insert(rewardTemplate(2, "reward_hard", 5000)); // the multiplier reads 12500 (1.25x)

    ev::Event &evEasy   = rig.start("reward_easy", JusticeFixture::kRegionA);
    const characterId a = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "guard", 8000);
    rig.engine.join(a, rig.currentId(), rig.f.base.ctx);
    evEasy.actionAmounts[a][static_cast<std::size_t>(ActionType::DAMAGE)] = 8000; // raw 8000
    const std::int64_t beforeEasy = rig.f.base.characters.get(a).progression.currency;
    rig.engine.finishEvent(rig.currentId(), true, rig.f.base.ctx); // the payout point
    const std::uint64_t paidEasy
        = static_cast<std::uint64_t>(rig.f.base.characters.get(a).progression.currency - beforeEasy);
    CHECK(paidEasy == 8000); // pool = 8000 * 10000/10000

    ev::Event &evHard = rig.start("reward_hard", JusticeFixture::kRegionB);
    rig.engine.join(a, rig.currentId(), rig.f.base.ctx);
    evHard.actionAmounts[a][static_cast<std::size_t>(ActionType::DAMAGE)] = 8000;
    const std::int64_t beforeHard = rig.f.base.characters.get(a).progression.currency;
    rig.engine.finishEvent(rig.currentId(), true, rig.f.base.ctx);
    const std::uint64_t paidHard
        = static_cast<std::uint64_t>(rig.f.base.characters.get(a).progression.currency - beforeHard);
    CHECK(paidHard == 10000); // pool = 8000 * 12500/10000 — the 1.25x pin
}

// ============================================================================
// Pinned test — the payout invariant holds under fuzzed shares (the
// Done-when's property test; deterministic draws from the named "events"
// stream, R6).
// ============================================================================
TEST_CASE(
    "TestRewards_FuzzPoolInvariant"
)
{
    RewardRig rig;
    rig.table.insert(rewardTemplate(1, "reward_event", 0));
    const ev::EventTemplate tpl = rewardTemplate(1, "reward_event", 0);
    REQUIRE(rig.f.base.ctx.rng != nullptr);

    for (int round = 0; round < 25; ++round)
    {
        ev::Event &ev            = rig.start("reward_event", JusticeFixture::kRegionA);
        const std::uint64_t pool = rig.f.base.ctx.rng->range("events", 1, 200001);
        std::vector<std::pair<characterId, std::int64_t>> granted;
        for (int i = 0; i < 6; ++i)
        {
            const characterId id
                = rig.f.spawnWitness(JusticeFixture::kVillageA, JusticeFixture::kRegionA, "guard", 8000);
            rig.engine.join(id, rig.currentId(), rig.f.base.ctx);
            ev.actionAmounts[id][static_cast<std::size_t>(ActionType::DAMAGE)]
                = rig.f.base.ctx.rng->range("events", 200, 12000); // every share clears MIN_CONTRIBUTION
            const std::int64_t before = rig.f.base.characters.get(id).progression.currency;
            granted.push_back({id, before});
            if (round % 3 == 0 && i == 2)
            {
                ev.excludedFromRewards.insert(id); // the smuggling hook rides the fuzz too
            }
        }
        ev::distributeRewards(ev, pool, tpl, rig.f.base.ctx);
        std::uint64_t paid = 0;
        for (const auto &[id, before] : granted)
        {
            const std::int64_t delta = rig.f.base.characters.get(id).progression.currency - before;
            REQUIRE(delta >= 0);
            paid += static_cast<std::uint64_t>(delta);
        }
        granted.clear();
        // The invariant: Σpaid + remainder == pool — with the currency rate
        // at 1, the granted currency IS the paid + remainder total.
        CHECK(paid == pool);
        rig.engine.finishEvent(rig.currentId(), true, rig.f.base.ctx); // reset for the next round
    }
}

// ============================================================================
// Pinned test — the manhunt's culprit earns NOTHING: their damage to
// hunters records no contribution (targets do not pay) and the reward
// exclusion stands even if amounts existed. The test never ticks (the
// completion poll would resolve the seed's warrant-id against the empty
// store — the completion arm is TestManhunt_EndsOnCapturePublishesResolved's).
// ============================================================================
TEST_CASE(
    "TestManhunt_CulpritEarnsNothing"
)
{
    ev::EventTemplateTable shipped;
    std::vector<data::DataError> errors;
    REQUIRE(ev::loadEventTemplates("mods/harrowmere_core/data/event_templates.json", shipped, errors));
    RewardRig rig;
    rig.table = std::move(shipped);
    rig.router.bind(&rig.engine, &rig.table, &rig.f.base.ctx);

    const characterId culprit = rig.f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    ManhuntSeed seed;
    seed.warrantId = 77; // a test seed; the test never ticks (the poll would resolve it — see the capture test)
    seed.culprit   = culprit;
    seed.territory = JusticeFixture::kVillageB;
    seed.region    = JusticeFixture::kRegionB;

    REQUIRE(rig.engine.spawnEvent("MANHUNT", seed));
    const ev::Event *manhunt = rig.engine.findActiveByKey("MANHUNT");
    REQUIRE(manhunt != nullptr);
    CHECK(manhunt->targetIds.count(culprit) == 1);
    CHECK(manhunt->excludedFromRewards.count(culprit) == 1); // culpritEarnsNothing

    const characterId hunter = rig.f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "guard", 8000);
    rig.engine.join(hunter, rig.engine.active().begin()->first, rig.f.base.ctx);

    // The culprit swings at a hunter: the hpLost budget grows, no
    // contribution exists for the culprit.
    Character &cp     = rig.f.base.characters.get(culprit);
    cp.body.maxHp     = 20000;
    cp.body.currentHp = 20000;
    rig.router.onCombatDamage(DamageEvent{culprit, hunter, 50, DamageType::Melee});
    const ev::Event *after = rig.engine.findActiveByKey("MANHUNT");
    REQUIRE(after != nullptr);
    CHECK(after->hpLost.at(hunter) == 50);
    CHECK(after->actionAmounts.find(culprit) == after->actionAmounts.end());
}

// ============================================================================
// Pinned test — hunter damage PAYS: the hunter's validated damage against
// the target (the culprit) becomes contribution and pays at the payout
// point (A-107's difficulty multiplier rides the pool).
// ============================================================================
TEST_CASE(
    "TestManhunt_HunterDamagePays"
)
{
    ev::EventTemplateTable shipped;
    std::vector<data::DataError> errors;
    REQUIRE(ev::loadEventTemplates("mods/harrowmere_core/data/event_templates.json", shipped, errors));
    RewardRig rig;
    rig.table = std::move(shipped);
    rig.router.bind(&rig.engine, &rig.table, &rig.f.base.ctx);

    const characterId culprit       = rig.f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    Character &cp                   = rig.f.base.characters.get(culprit);
    cp.body.maxHp                   = 20000; // the hunt's hp cap must not bite the pinned amount
    cp.body.currentHp               = 20000;
    const std::int64_t culpritPurse = cp.progression.currency;

    // The live warrant is the manhunt's own shape (the hunt exists because
    // the warrant does — and the validation's consent-or-warrant arm reads
    // it for the player target).
    const std::uint64_t caseId = justice::commitEvidence(
        CrimeType::THEFT,
        culprit,
        /*victim=*/0,
        JusticeFixture::kVillageB,
        rig.f.base.ctx,
        {Report{55, 6000, true, false}},
        0
    )
                                     .id;
    Evidence *warrantCase = rig.f.caseById(caseId);
    REQUIRE(warrantCase != nullptr);
    justice::issueWarrant(*warrantCase, rig.f.base.ctx);
    REQUIRE(rig.f.store.warrants.size() == 1);

    ManhuntSeed seed;
    seed.warrantId = rig.f.store.warrants.begin()->first; // live: the poll stays quiet
    seed.culprit   = culprit;
    seed.territory = JusticeFixture::kVillageB;
    seed.region    = JusticeFixture::kRegionB;
    REQUIRE(rig.engine.spawnEvent("MANHUNT", seed));
    const std::uint64_t manhuntId = rig.engine.active().begin()->first;

    const characterId hunter = rig.f.spawnWitness(JusticeFixture::kVillageB, JusticeFixture::kRegionB, "guard", 8000);
    rig.engine.join(hunter, manhuntId, rig.f.base.ctx);

    rig.router.onCombatDamage(DamageEvent{hunter, culprit, 5000, DamageType::Melee});
    const ev::Event *ev = rig.engine.find(manhuntId);
    REQUIRE(ev != nullptr);
    REQUIRE(ev->actionAmounts.count(hunter) == 1);
    CHECK(ev->actionAmounts.at(hunter)[static_cast<std::size_t>(ActionType::DAMAGE)] == 5000);

    // The payout point: raw 5000 at difficulty 5000 → pool 6250 → the
    // hunter's currency grows by 6250 * 2 (the MANHUNT table's rate).
    const std::int64_t before = rig.f.base.characters.get(hunter).progression.currency;
    rig.engine.finishEvent(manhuntId, true, rig.f.base.ctx);
    const std::uint64_t paid
        = static_cast<std::uint64_t>(rig.f.base.characters.get(hunter).progression.currency - before);
    CHECK(paid == 12500);
    // The culprit (excluded) earned nothing.
    CHECK(rig.f.base.characters.get(culprit).progression.currency == culpritPurse);
}

// ============================================================================
// Pinned test — the manhunt ends on capture and the resolution publishes:
// the capture path (4.7's terminal arm) pays the warrant (WarrantResolved
// published), and the event's completion poll ends the hunt at the same
// tick ("any manhunt ends").
// ============================================================================
TEST_CASE(
    "TestManhunt_EndsOnCapturePublishesResolved"
)
{
    ev::EventTemplateTable shipped;
    std::vector<data::DataError> errors;
    REQUIRE(ev::loadEventTemplates("mods/harrowmere_core/data/event_templates.json", shipped, errors));
    RewardRig rig;
    rig.table = std::move(shipped);
    rig.router.bind(&rig.engine, &rig.table, &rig.f.base.ctx);

    const characterId culprit = rig.f.spawnPlayerAt(JusticeFixture::kVillageB, JusticeFixture::kRegionB);
    // A live warrant against the culprit on the issuing ground.
    const std::uint64_t caseId = justice::commitEvidence(
        CrimeType::THEFT,
        culprit,
        /*victim=*/0,
        JusticeFixture::kVillageB,
        rig.f.base.ctx,
        {Report{55, 6000, true, false}},
        0
    )
                                     .id;
    Evidence *e = rig.f.caseById(caseId);
    REQUIRE(e != nullptr);
    justice::issueWarrant(*e, rig.f.base.ctx);
    REQUIRE(rig.f.store.warrants.size() == 1);
    const std::uint64_t warrantId = rig.f.store.warrants.begin()->first;

    ManhuntSeed seed;
    seed.warrantId = warrantId;
    seed.culprit   = culprit;
    seed.territory = JusticeFixture::kVillageB;
    seed.region    = JusticeFixture::kRegionB;
    REQUIRE(rig.engine.spawnEvent("MANHUNT", seed));
    REQUIRE(rig.engine.findActiveByKey("MANHUNT") != nullptr);

    // The capture: the culprit sits in jail (isArrested reads jailUntil)
    // and the bounty pays — the justice side's WarrantResolved{Paid} fact.
    rig.f.store.jailUntil[culprit] = rig.f.base.ctx.now + 1000;
    justice::payWarrant(warrantId, rig.f.base.ctx);
    REQUIRE(rig.f.store.warrants.empty()); // BOTH maps cleared (the doc invariant)
    REQUIRE(rig.sink.resolved.size() == 1);
    CHECK(rig.sink.resolved.front().how == WarrantResolved::How::Paid);

    // The completion poll: the warrant left the active set → the hunt ends
    // at this tick (the state's exits carry the success verdict).
    rig.engine.tick(rig.f.base.ctx);
    CHECK(rig.engine.findActiveByKey("MANHUNT") == nullptr);
    CHECK(rig.engine.activeCount() == 0);
}
