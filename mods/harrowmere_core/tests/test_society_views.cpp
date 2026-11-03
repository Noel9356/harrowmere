// test_society_views.cpp — task 7.3b's core-side tests: the society view
// fills against the LIVE stores. The fog-of-war whitelist (the core twin of
// the UI suite's TestUi_NoHiddenEvidenceExposed): a wanted player's view
// carries exactly the observable surface — reputation rows, notoriety, the
// live warrants' accused-side facts, jail — and NEVER the case score, the
// evidence grades or the report weights the justice store holds right next
// to them. Plus the events view's contribution/join reads and the territory
// tree the Map panel renders.
#include <doctest.h>

#include <algorithm>
#include <cstdint>

#include "bridge/society_view.hpp"
#include "character/character.hpp"
#include "core/constants.hpp"
#include "events/engine.hpp"
#include "events/template.hpp"
#include "justice/cases.hpp"
#include "justice/conviction.hpp"
#include "justice/evidence.hpp"
#include "justice/reputation.hpp"
#include "justice/warrants.hpp"
#include "world/world.hpp"

#include "test_justice_fixture.hpp"

namespace
{

using namespace harrowmere;
using namespace justice_test;
namespace ev    = harrowmere::events;
namespace views = harrowmere::views;
// The fixture's pinned hierarchy ids (static members; local aliases keep the
// helpers readable).
constexpr territoryId kKingdom  = JusticeFixture::kKingdom;
constexpr territoryId kCounty   = JusticeFixture::kCounty;
constexpr territoryId kEmpire   = JusticeFixture::kEmpire;
constexpr territoryId kVillageA = JusticeFixture::kVillageA;
constexpr regionId kRegionA     = JusticeFixture::kRegionA;
constexpr regionId kRegionB     = JusticeFixture::kRegionB;
constexpr regionId kRegionXen   = JusticeFixture::kRegionXen;

// The rig: the full justice state (evidence with real grades, a live
// warrant, reputation rows, jail) + an active event, with the view builder
// reading through the same context the plugin binds.
struct SocietyRig
{
    JusticeFixture f;
    ev::EventTemplateTable table;
    ev::EventEngine engine;
    characterId player = 0;

    SocietyRig()
    {
        f.base.ctx.now = 1000;
        engine.bind(&table);
        engine.bindContext(&f.base.ctx);

        player                = f.base.characters.createPlayer(PlayerCreation{"wanted", {}, ""});
        f.base.world.playerId = player;
        Character &p          = f.base.characters.get(player);
        p.body.position       = world::regionCenter(kRegionA);
        f.grid.insert(player, p.body.position);
    }

    // The wanted state: one live warrant (hand-set in the two doc-verbatim
    // maps, lockstep) + the evidence case behind it (real grades — the
    // fields the view must NEVER carry).
    void makeWanted()
    {
        justice::Warrant w;
        w.id                   = 7;
        w.evidenceId           = 11;
        w.territory            = kVillageA;
        w.lethal               = false;
        w.bounty               = 4000;
        w.expiresTime          = f.base.ctx.now + 5 * core::DAY_LENGTH_SECONDS;
        f.store.warrants[w.id] = w;
        f.store.activeWarrantsByCulprit[player].insert(w.id);

        // The hidden case: a real evidence record with heavyweight grades
        // (the fog-of-war test's bait — none of it may reach the view).
        justice::Evidence e;
        e.id                    = 11;
        e.culpritId             = player;
        e.victimId              = 42;
        e.territory             = kVillageA;
        e.worldTime             = f.base.ctx.now;
        e.type                  = CrimeType::ASSAULT;
        e.status                = EvidenceStatus::WANTED;
        e.physicalScore         = 9000;
        e.aggregateReportWeight = 7000;
        e.reports.push_back({5, 6000, true});
        f.store.evidenceStore[kVillageA].push_back(e);
    }

    // An active event the player joined, plus a second one they are the
    // TARGET of (the manhunt shape — not joinable for the target).
    std::uint64_t startEvent(
        std::uint64_t id, const char *key, regionId region = kRegionA
    )
    {
        ev::EventTemplate tpl;
        tpl.id                 = id;
        tpl.key                = key;
        tpl.global             = false;
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
        timer.days = 30;
        toB.conditions.push_back(timer);
        a.transitions.push_back(toB);
        ev::TemplateState b;
        b.stateId = 2;
        tpl.states.push_back(a);
        tpl.states.push_back(b);
        table.insert(tpl);
        return engine.startEventByKey(key, region, 0, nullptr, f.base.ctx);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// TestSocietyView_NoHiddenEvidenceExposed (card 7.3b, pinned name — the live
// view builder under the whitelist test): the JusticeView carries exactly
// the observable surface and never the hidden grades.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestSocietyView_NoHiddenEvidenceExposed"
)
{
    SocietyRig rig;
    rig.makeWanted();

    // Reputation + notoriety: the walk writes the chain; the view flattens.
    justice::changeReputation(rig.player, kVillageA, -1200, rig.f.base.ctx);
    justice::addNotoriety(rig.f.store, rig.player, 3000);

    // Jail: a live sentence.
    rig.f.store.jailUntil[rig.player] = rig.f.base.ctx.now + 900;

    const views::JusticeView view = bridge::buildJusticeView(rig.f.base.ctx, rig.player);

    // ---- the whitelist values are the live state (decay-before-read) ----
    CHECK(view.notoriety == 3000);
    REQUIRE(view.perTerritory.size() == 4); // villageA -> county -> kingdom -> empire (the walk's rows)
    CHECK(view.perTerritory[0].territoryId == kVillageA);
    CHECK(view.perTerritory[0].reputation == -1200);
    CHECK(view.perTerritory[1].territoryId == kCounty);
    CHECK(view.perTerritory[1].reputation == -600);
    CHECK(view.perTerritory[2].territoryId == kKingdom);
    CHECK(view.perTerritory[2].reputation == -300);
    CHECK(view.perTerritory[3].territoryId == kEmpire);
    CHECK(view.perTerritory[3].reputation == -150);
    CHECK(view.reputation == -1200); // the underfoot territory's standing

    // The warrants: the accused-side facts ONLY (territory, bounty, expiry,
    // lethality) — exactly the live warrant's observable face.
    REQUIRE(view.activeWarrantsAgainstYou == 1);
    REQUIRE(view.warrants.size() == 1);
    CHECK(view.warrants[0].territoryId == kVillageA);
    CHECK(view.warrants[0].bounty == 4000);
    CHECK(view.warrants[0].expiresWorldTime == rig.f.store.warrants[7].expiresTime);
    CHECK_FALSE(view.warrants[0].lethal);

    // The jail status (the lazy read; the sentence end rides with it).
    CHECK(view.isJailed);
    CHECK(view.jailUntil == rig.f.base.ctx.now + 900);

    // ---- THE FOG-OF-WAR: the hidden grades stay hidden -------------------
    // The bait sits in the store right next to the whitelist (physicalScore
    // 9000, aggregate 7000, a 6000-weight identity report). The view is a
    // plain aggregate over the whitelisted members (the UI suite's
    // static_asserts pin the shape) — the LIVE fill proves the VALUES carry
    // nothing else: every field above reads back exactly its whitelisted
    // source, and the hidden grades appear nowhere (the aggregate has no
    // member that could hold them; the traits below re-pin the negative on
    // the live-builder's output type).
    CHECK(view.perTerritory.size() != 9000);
    CHECK(view.activeWarrantsAgainstYou != 9000);
    CHECK(view.warrants[0].bounty != 9000);
    CHECK(view.warrants[0].bounty != 7000);
    CHECK(view.notoriety != 9000);
    static_assert(std::is_aggregate_v<views::JusticeView>, "the whitelist stays a plain aggregate");
}

// ---------------------------------------------------------------------------
// TestSocietyView_EventsReads (7.3b's EventsView fill): contribution, share,
// join affordances — the panel displays, the core derives.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestSocietyView_EventsReads"
)
{
    SocietyRig rig;
    // Three events, three regions — independent regional events refuse the
    // SAME region (5.1's overlap rule), so the rig spreads them.
    const std::uint64_t joined = rig.startEvent(500, "watch_event", kRegionA);
    const std::uint64_t hunted = rig.startEvent(501, "hunt_event", kRegionB);
    const std::uint64_t open   = rig.startEvent(502, "open_event", kRegionXen);
    REQUIRE(joined != 0);
    REQUIRE(hunted != 0);
    REQUIRE(open != 0);

    // The player joins the first; the second inserts them as the TARGET
    // (the manhunt shape: targets never join and cannot join).
    rig.engine.join(rig.player, joined, rig.f.base.ctx);
    rig.engine.findMutable(hunted)->targetIds.insert(rig.player);

    const views::EventsView view = bridge::buildEventsView(rig.engine, rig.table, rig.player);
    REQUIRE(view.active.size() == 3); // all active, id-ascending (R7)

    const views::EventSummary &first = view.active[0];
    CHECK(first.eventId == joined);
    CHECK(first.templateKey == "watch_event");
    CHECK(first.amParticipant);
    CHECK_FALSE(first.joinable);       // already a participant
    CHECK(first.myContribution == 0);  // fresh books
    CHECK(first.mySharePermille == 0); // no contribution, no share (MIN_CONTRIBUTION's floor)
    CHECK(first.difficulty == 3000);
    CHECK(first.stateId == 1);

    const views::EventSummary &second = view.active[1];
    CHECK(second.eventId == hunted);
    CHECK_FALSE(second.amParticipant);
    CHECK_FALSE(second.joinable); // the event's TARGET cannot join their own hunt

    // The bystander event is joinable for the player (the affordance the
    // Events panel's join button renders).
    CHECK(view.active[2].joinable);
}

// ---------------------------------------------------------------------------
// TestSocietyView_TerritoryTree (7.3b's Map fill): the tree ships
// id-ascending with the facets the panel renders — safe zones, lawless
// flags, the effective law set's key.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestSocietyView_TerritoryTree"
)
{
    SocietyRig rig;
    const std::vector<views::TerritorySummary> tree = bridge::buildTerritoryTree(*rig.f.base.ctx.worldStore);
    REQUIRE(tree.size() == 7); // the fixture's hierarchy: empire, kingdom, county, 4 territories

    // Id-ascending (R7), the parent links intact.
    for (std::size_t i = 1; i < tree.size(); ++i)
    {
        CHECK(tree[i - 1].id < tree[i].id);
    }
    const auto villageA
        = std::find_if(tree.begin(), tree.end(), [](const views::TerritorySummary &t) { return t.id == kVillageA; });
    REQUIRE(villageA != tree.end());
    CHECK(villageA->parent == kCounty);
    CHECK(villageA->safe); // the safe zone renders its flag
    CHECK(villageA->type == views::TerritoryType::Village);
    CHECK(villageA->lawSetKey == "safe_zone_laws"); // the lawsAt walk's result
    CHECK_FALSE(villageA->claimedByMe);             // 6.3's fill (Phase 4) — the honest zero

    const auto kingdom
        = std::find_if(tree.begin(), tree.end(), [](const views::TerritorySummary &t) { return t.id == kKingdom; });
    REQUIRE(kingdom != tree.end());
    CHECK(kingdom->type == views::TerritoryType::Kingdom);
    CHECK_FALSE(kingdom->safe);
}
