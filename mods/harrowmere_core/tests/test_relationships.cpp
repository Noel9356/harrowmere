// test_relationships.cpp — Task 3.9's pinned test suite.
//
// The card's Tests block (names verbatim — common.md §3.5):
//
//   * TestAffinity_CoPresenceAccruesAndCaps
//   * TestAffinity_EnemiesTestifyHarder      (pins the doc formula direction:
//                                             negative affinity raises weight)
//   * TestKnows_AcquaintanceAlways
//   * TestKnows_MaskedStrangerNo
//   * TestKnows_AppearanceChangeResetsStrangerNotAcquaintance
//   * TestCanTestify_ChildFalse
//   * TestBias_XenophobicVillageLowersAffinity
//
// Plus the fill-in pins the Done-when leans on: the sighting lapse/refresh
// window (A-56), the store's deterministic iteration (R7), the doc weight
// formula's exact arithmetic, and the live PerceptionSink's spatial half.
//
// Test harness: doctest v2.4.12 (vendored per task 0.6); no main here -
// tests/main.cpp owns DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.
#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <vector>

#include "character/appearance.hpp" // AppearanceHistory (2.6 — the version source)
#include "core/constants.hpp"       // A-54/A-55/A-56, the affinity sources, SCALE
#include "core/enums.hpp"
#include "core/types.hpp"
#include "npc/perception.hpp"     // PerceptionStore + PerceptionSink (the seam)
#include "npc/relationships.hpp"  // affinity, bias, qualification, the weight formula
#include "npc/state.hpp"          // NpcState
#include "world/spatial_grid.hpp" // the sink's spatial source

#include "test_phase2_fixture.hpp" // the shipped registry (species/laws/roles rows)

namespace
{

constexpr harrowmere::characterId kObserver = 100;
constexpr harrowmere::characterId kCulprit  = 200;

harrowmere::NpcState emptyNpc()
{
    harrowmere::NpcState npc{};
    npc.birthWorldTime = 0;
    npc.roleKey        = "farmer";
    npc.integrity      = 5000;
    return npc;
}

} // namespace

// ============================================================================
// Pinned test 1 — co-presence accrues +20 per FULL game-hour and saturates
// at +4000 from the proximity source (without lowering an above-cap pair).
// ============================================================================
TEST_CASE(
    "TestAffinity_CoPresenceAccruesAndCaps"
)
{
    harrowmere::NpcState observer = emptyNpc();

    // A partial hour accrues nothing (per-game-hour cadence, integer math).
    harrowmere::npc::accrueCoPresenceAffinity(observer, kCulprit, 3599);
    CHECK(observer.affinity.find(kCulprit) == observer.affinity.end());

    // One full hour: +20. 199 more: +3980 -> exactly the +4000 cap.
    harrowmere::npc::accrueCoPresenceAffinity(observer, kCulprit, harrowmere::GAME_HOUR_SECONDS);
    CHECK(observer.affinity.at(kCulprit) == 20);
    harrowmere::npc::accrueCoPresenceAffinity(observer, kCulprit, harrowmere::GAME_HOUR_SECONDS * 199);
    CHECK(observer.affinity.at(kCulprit) == 4000);

    // The source cap holds: more proximity never pushes past +4000...
    harrowmere::npc::accrueCoPresenceAffinity(observer, kCulprit, harrowmere::GAME_HOUR_SECONDS * 1000);
    CHECK(observer.affinity.at(kCulprit) == harrowmere::AFFINITY_CO_PRESENCE_CAP);

    // ...and never lowers a pair another source (trades) put above the cap.
    harrowmere::NpcState traded = emptyNpc();
    for (int i = 0; i < 51; ++i)
    {
        harrowmere::npc::addAffinityEvent(traded, kCulprit, harrowmere::AFFINITY_PER_TRADE_EVENT);
    }
    CHECK(traded.affinity.at(kCulprit) == 5100);
    harrowmere::npc::accrueCoPresenceAffinity(traded, kCulprit, harrowmere::GAME_HOUR_SECONDS * 1000);
    CHECK(traded.affinity.at(kCulprit) == 5100);

    // A faction-based pair (+2000) accrues up to the cap, not beyond it.
    harrowmere::NpcState sameFaction = emptyNpc();
    harrowmere::npc::seedRelationship(sameFaction, kCulprit, true, 0);
    CHECK(sameFaction.affinity.at(kCulprit) == harrowmere::AFFINITY_SAME_FACTION_BASE);
    harrowmere::npc::accrueCoPresenceAffinity(sameFaction, kCulprit, harrowmere::GAME_HOUR_SECONDS * 100);
    CHECK(sameFaction.affinity.at(kCulprit) == 4000);

    // A negative (xenophobia) base climbs toward the cap normally.
    harrowmere::NpcState hostile = emptyNpc();
    harrowmere::npc::seedRelationship(hostile, kCulprit, false, -6000);
    harrowmere::npc::accrueCoPresenceAffinity(hostile, kCulprit, harrowmere::GAME_HOUR_SECONDS);
    CHECK(hostile.affinity.at(kCulprit) == -5980);

    // Trade/gift events clamp at +SCALE (the map span, not the source cap).
    harrowmere::NpcState clamped = emptyNpc();
    harrowmere::npc::addAffinityEvent(clamped, kCulprit, 500'000);
    CHECK(clamped.affinity.at(kCulprit) == static_cast<std::int64_t>(harrowmere::SCALE));
}

// ============================================================================
// Pinned test 2 — the doc weight formula's direction: a NEGATIVE affinity
// RAISES the report weight (enemies testify harder); a positive one lowers
// it to zero at +SCALE. weight = max(0, ROLE_BASE * integrity / SCALE *
// (SCALE - affinity) / SCALE), affinity clamped to [-SCALE, +SCALE].
// ============================================================================
TEST_CASE(
    "TestAffinity_EnemiesTestifyHarder"
)
{
    constexpr std::uint32_t kFarmerBase = 4000; // roles.json's farmer baseReportWeight
    constexpr std::uint64_t kIntegrity  = 8000; // the farmer range's top

    // Neutral stranger (no affinity entry): 4000 * 8000 / 10000 * 10000 / 10000 = 3200.
    const std::uint32_t neutral = harrowmere::npc::witnessReportWeight(kFarmerBase, kIntegrity, 0);
    CHECK(neutral == 3200);

    // Max enemy (-SCALE): the span term doubles -> 6400 — strictly harder.
    const std::uint32_t enemy
        = harrowmere::npc::witnessReportWeight(kFarmerBase, kIntegrity, -static_cast<std::int64_t>(harrowmere::SCALE));
    CHECK(enemy == 6400);
    CHECK(enemy == 2 * neutral);
    CHECK(enemy > neutral);

    // A moderate grudge scales the span linearly (-4000 -> span 14000 -> 4480).
    CHECK(harrowmere::npc::witnessReportWeight(kFarmerBase, kIntegrity, -4000) == 4480);

    // Max friend (+SCALE): the span term zeroes the weight (never incriminates).
    CHECK(
        harrowmere::npc::witnessReportWeight(kFarmerBase, kIntegrity, static_cast<std::int64_t>(harrowmere::SCALE)) == 0
    );

    // An UNclamped input beyond +SCALE would zero the weight through
    // max(0, ...) — the map clamp is what keeps the span valid (the card's
    // pitfall); the formula's door clamps defensively, so the outcome is the
    // same zero either way.
    CHECK(harrowmere::npc::witnessReportWeight(kFarmerBase, kIntegrity, 50'000) == 0);

    // A zero-integrity or zero-role witness never reports, whatever the grudge.
    CHECK(harrowmere::npc::witnessReportWeight(kFarmerBase, 0, -static_cast<std::int64_t>(harrowmere::SCALE)) == 0);
    CHECK(harrowmere::npc::witnessReportWeight(0, kIntegrity, -static_cast<std::int64_t>(harrowmere::SCALE)) == 0);
}

// ============================================================================
// Pinned test 3 — an acquaintance knows by id, always: no sighting needed,
// appearance changes irrelevant, no lapse.
// ============================================================================
TEST_CASE(
    "TestKnows_AcquaintanceAlways"
)
{
    harrowmere::npc::PerceptionStore store;
    harrowmere::NpcState observer = emptyNpc();
    observer.affinity[kCulprit]   = harrowmere::ACQUAINTANCE_AFFINITY; // exactly the threshold

    // No sighting rows at all; a wildly different current version; ages of
    // time since any hypothetical sighting — the acquaintance still knows.
    CHECK(observer.affinity.at(kCulprit) >= harrowmere::ACQUAINTANCE_AFFINITY);
    CHECK(store.knowsIdentity(kObserver, observer, kCulprit, 999, 1'000'000'000ull));

    // Just below the threshold the id match is gone (an enemy is not an
    // acquaintance even at the same magnitude).
    harrowmere::NpcState enemy = emptyNpc();
    enemy.affinity[kCulprit]   = harrowmere::ACQUAINTANCE_AFFINITY - 1;
    CHECK_FALSE(store.knowsIdentity(kObserver, enemy, kCulprit, 0, 1'000'000'000ull));
}

// ============================================================================
// Pinned test 4 — a MASKED culprit leaves no stranger memory (no new record
// and no refresh of an existing one).
// ============================================================================
TEST_CASE(
    "TestKnows_MaskedStrangerNo"
)
{
    harrowmere::npc::PerceptionStore store;
    harrowmere::NpcState observer = emptyNpc();

    // Masked at sighting time: nothing is recorded, so the stranger does not
    // know — even with the version matching and no time passed.
    store.recordSighting(kObserver, harrowmere::npc::Sighting{kCulprit, 0, 1000}, true);
    CHECK(store.sightingOf(kObserver, kCulprit) == nullptr);
    CHECK_FALSE(store.knowsIdentity(kObserver, observer, kCulprit, 0, 1000));

    // And a mask does not refresh an existing memory either: an unmasked
    // sighting at t=1000 goes stale exactly on schedule even though the
    // observer saw the (masked) culprit again at t=1400.
    store.recordSighting(kObserver, harrowmere::npc::Sighting{kCulprit, 0, 1000}, false);
    store.recordSighting(kObserver, harrowmere::npc::Sighting{kCulprit, 0, 1400}, true);
    const harrowmere::npc::Sighting *memory = store.sightingOf(kObserver, kCulprit);
    REQUIRE(memory != nullptr);
    CHECK(memory->worldTime == 1000); // the masked pass left the memory alone
    CHECK(store.knowsIdentity(kObserver, observer, kCulprit, 0, 1000 + harrowmere::STRANGER_MEMORY_S));
    CHECK_FALSE(store.knowsIdentity(kObserver, observer, kCulprit, 0, 1000 + harrowmere::STRANGER_MEMORY_S + 1));
}

// ============================================================================
// Pinned test 5 — an appearance change resets the STRANGER's sighting match
// (stale version) and never the ACQUAINTANCE's id match.
// ============================================================================
TEST_CASE(
    "TestKnows_AppearanceChangeResetsStrangerNotAcquaintance"
)
{
    harrowmere::npc::PerceptionStore store;

    // The stranger saw the culprit at t=1000 wearing look version 0.
    harrowmere::NpcState stranger = emptyNpc();
    store.recordSighting(kObserver, harrowmere::npc::Sighting{kCulprit, 0, 1000}, false);
    CHECK(store.knowsIdentity(kObserver, stranger, kCulprit, 0, 1100)); // same look: known

    // The culprit changes appearance (2.6): version 1 is now current. The
    // sighting keyed to version 0 no longer matches — the disguise reset.
    CHECK_FALSE(store.knowsIdentity(kObserver, stranger, kCulprit, 1, 1100));

    // The acquaintance knows the culprit by id regardless of the change.
    harrowmere::NpcState acquaintance = emptyNpc();
    acquaintance.affinity[kCulprit]   = 4000; // co-presence made them acquainted
    CHECK(store.knowsIdentity(kObserver, acquaintance, kCulprit, 1, 1100));
    CHECK(store.knowsIdentity(kObserver, acquaintance, kCulprit, 7, 1'000'000'000ull)); // ages later, any look

    // And a FRESH sighting of the new look re-identifies the stranger.
    store.recordSighting(kObserver, harrowmere::npc::Sighting{kCulprit, 1, 1200}, false);
    CHECK(store.knowsIdentity(kObserver, stranger, kCulprit, 1, 1300));
}

// ============================================================================
// Pinned test 6 — witness qualification: a child never testifies (A-44 = 16);
// the dead and the non-sapient never do; the player (no NpcState) qualifies.
// ============================================================================
TEST_CASE(
    "TestCanTestify_ChildFalse"
)
{
    phase2_test::Fixture fixture;
    fixture.ctx.now = 10u * 3600u; // the epoch open; ages ride NpcState::ageYears

    const harrowmere::speciesId human  = phase2_test::speciesIdByKey("human");
    const harrowmere::speciesId wolf   = phase2_test::speciesIdByKey("wolf");
    const harrowmere::territoryId home = 7;
    const harrowmere::regionId region  = 40;

    // A 15-year-old is one year short; the `child` role row documents the
    // same rule the code pins (roles.json cannotTestify).
    const harrowmere::characterId child15 = phase2_test::spawnNpc(fixture, human, home, region, 15, "child");
    const harrowmere::characterId child16 = phase2_test::spawnNpc(fixture, human, home, region, 16, "farmer");
    const harrowmere::characterId adult   = phase2_test::spawnNpc(fixture, human, home, region, 25, "farmer");
    const harrowmere::characterId wolfNpc = phase2_test::spawnNpc(fixture, wolf, home, region, 5, "");

    CHECK_FALSE(
        harrowmere::npc::canTestify(
            fixture.characters.get(child15), &fixture.npcs.get(child15), fixture.ctx.now, phase2_test::shippedRegistry()
        )
    );
    CHECK(
        harrowmere::npc::canTestify(
            fixture.characters.get(child16), &fixture.npcs.get(child16), fixture.ctx.now, phase2_test::shippedRegistry()
        )
    );
    CHECK(
        harrowmere::npc::canTestify(
            fixture.characters.get(adult), &fixture.npcs.get(adult), fixture.ctx.now, phase2_test::shippedRegistry()
        )
    );

    // Non-sapient (the wolf row): never a witness — "hunting deer creates no
    // crime".
    CHECK_FALSE(
        harrowmere::npc::canTestify(
            fixture.characters.get(wolfNpc), &fixture.npcs.get(wolfNpc), fixture.ctx.now, phase2_test::shippedRegistry()
        )
    );

    // The dead: 0 HP never testifies (the store removal is 3.3's half).
    fixture.characters.get(adult).body.currentHp = 0;
    CHECK_FALSE(
        harrowmere::npc::canTestify(
            fixture.characters.get(adult), &fixture.npcs.get(adult), fixture.ctx.now, phase2_test::shippedRegistry()
        )
    );

    // The player: no NpcState exists (D-STORE-1) — an adult by the v1 pin.
    const harrowmere::characterId player = phase2_test::spawnPlayer(fixture);
    CHECK(
        harrowmere::npc::canTestify(
            fixture.characters.get(player), nullptr, fixture.ctx.now, phase2_test::shippedRegistry()
        )
    );

    // qualifyWitness fills the doc-formula inputs: the guard role's
    // baseReportWeight (9000) and the spawn integrity, the affinity toward
    // the culprit, and the child's zero basis.
    const harrowmere::characterId guard        = phase2_test::spawnNpc(fixture, human, home, region, 30, "guard");
    fixture.npcs.get(guard).integrity          = 6000;
    fixture.npcs.get(guard).affinity[kCulprit] = -10000;
    const harrowmere::npc::Witness w           = harrowmere::npc::qualifyWitness(
        fixture.characters.get(guard),
        &fixture.npcs.get(guard),
        fixture.ctx.now,
        phase2_test::shippedRegistry(),
        kCulprit
    );
    CHECK(w.id == guard);
    CHECK(w.roleBaseWeight == 9000);
    CHECK(w.integrity == 6000);
    CHECK(w.affinity == -static_cast<std::int64_t>(harrowmere::SCALE));
    CHECK(w.canTestify == true);
    CHECK(w.knowsIdentity == false); // the perception caller fills it

    // A role-less NPC carries the deterministic zero basis (weight 0).
    const harrowmere::characterId roleless = phase2_test::spawnNpc(fixture, human, home, region, 30, "");
    const harrowmere::npc::Witness nobody  = harrowmere::npc::qualifyWitness(
        fixture.characters.get(roleless),
        &fixture.npcs.get(roleless),
        fixture.ctx.now,
        phase2_test::shippedRegistry(),
        kCulprit
    );
    CHECK(nobody.roleBaseWeight == 0);
    CHECK(harrowmere::npc::witnessReportWeight(nobody.roleBaseWeight, nobody.integrity, nobody.affinity) == 0);
}

// ============================================================================
// Pinned test 7 — species bias from territory culture lowers (or raises) the
// affinity base: the doc's xenophobic village carries beastkin -6000.
// ============================================================================
TEST_CASE(
    "TestBias_XenophobicVillageLowersAffinity"
)
{
    const harrowmere::data::DataRegistry &data = phase2_test::shippedRegistry();
    const harrowmere::speciesId human          = phase2_test::speciesIdByKey("human");
    const harrowmere::speciesId beastkin       = phase2_test::speciesIdByKey("beastkin");
    const harrowmere::speciesId elf            = phase2_test::speciesIdByKey("elf");

    // The DATA row (laws.json): the doc's own example, verbatim.
    const harrowmere::data::LawSet *xenophobic = data.lawSet("xenophobic_village");
    REQUIRE(xenophobic != nullptr);
    REQUIRE(xenophobic->cultureBias.count(beastkin) != 0);
    CHECK(xenophobic->cultureBias.at(beastkin) == -6000);

    // The OBSERVER's species row resolves the self term (the human row
    // carries none); the territory term rides the laws the caller resolved.
    const std::uint64_t humanXenophobia = data.species(human).xenophobiaBias;
    CHECK(humanXenophobia == 0);
    const std::int64_t villageBias
        = harrowmere::npc::speciesBiasFor(humanXenophobia, xenophobic->cultureBias.at(beastkin));
    CHECK(villageBias == -6000);

    // Seeding through the adapter: the xenophobic village's human starts the
    // beastkin pair at -6000; the default-village human (empty cultureBias)
    // starts at 0.
    harrowmere::NpcState villager = emptyNpc();
    harrowmere::npc::seedRelationship(villager, kCulprit, false, villageBias);
    CHECK(villager.affinity.at(kCulprit) == -6000);

    const harrowmere::data::LawSet *defaults = data.lawSet("default");
    REQUIRE(defaults != nullptr);
    CHECK(defaults->cultureBias.count(beastkin) == 0);
    const std::int64_t neutralBias = harrowmere::npc::speciesBiasFor(
        humanXenophobia, harrowmere::npc::territoryCultureBiasToward(nullptr, 0, beastkin)
    );
    harrowmere::NpcState tolerant = emptyNpc();
    harrowmere::npc::seedRelationship(tolerant, kCulprit, false, neutralBias);
    CHECK(tolerant.affinity.at(kCulprit) == 0);
    CHECK(tolerant.affinity.at(kCulprit) > villager.affinity.at(kCulprit)); // the village bias LOWERED it

    // Positive culture bias raises: tolerant_city welcomes elves (+1000), and
    // the elf observer's OWN xenophobiaBias (species.json, 2000) adds on top.
    const harrowmere::data::LawSet *tolerantCity = data.lawSet("tolerant_city");
    REQUIRE(tolerantCity != nullptr);
    CHECK(tolerantCity->cultureBias.at(elf) == 1000);
    const std::int64_t elfSelfBias = harrowmere::npc::speciesBiasFor(data.species(elf).xenophobiaBias, 0);
    CHECK(elfSelfBias == 2000);
    CHECK(
        harrowmere::npc::speciesBiasFor(data.species(human).xenophobiaBias, tolerantCity->cultureBias.at(elf)) == 1000
    );

    // The world-queries walk (D-JUR-1) is the runtime bias source; a null
    // world reads 0 (the seam's null-false pattern, D-SINK-1).
    CHECK(harrowmere::npc::territoryCultureBiasToward(nullptr, 7, beastkin) == 0);
}

// ============================================================================
// Fill-in pins: the A-56 lapse window (lapse at strictly-more), the store's
// R7 iteration, and the live sink's spatial half.
// ============================================================================
TEST_CASE(
    "TestPerception_SightingLapseWindowAndRefresh"
)
{
    harrowmere::npc::PerceptionStore store;
    harrowmere::NpcState observer = emptyNpc();

    store.recordSighting(kObserver, harrowmere::npc::Sighting{kCulprit, 0, 1000}, false);
    CHECK(
        store.knowsIdentity(kObserver, observer, kCulprit, 0, 1000 + harrowmere::STRANGER_MEMORY_S)
    ); // boundary: still known
    CHECK_FALSE(
        store.knowsIdentity(kObserver, observer, kCulprit, 0, 1000 + harrowmere::STRANGER_MEMORY_S + 1)
    ); // lapsed

    // NEAR co-presence keeps the memory alive: a re-record refreshes the
    // stamp, so the window restarts (the card's "persists while the observer
    // remains within sight").
    store.recordSighting(kObserver, harrowmere::npc::Sighting{kCulprit, 0, 1500}, false);
    CHECK(store.knowsIdentity(kObserver, observer, kCulprit, 0, 1500 + harrowmere::STRANGER_MEMORY_S));

    // Deterministic iteration (R7): the observer's memories iterate
    // culprit-id ascending regardless of recording order.
    store.recordSighting(kObserver, harrowmere::npc::Sighting{5, 0, 100}, false);
    store.recordSighting(kObserver, harrowmere::npc::Sighting{3, 0, 100}, false);
    store.recordSighting(kObserver, harrowmere::npc::Sighting{4, 0, 100}, false);
    harrowmere::characterId previous = 0;
    std::size_t visits               = 0;
    for (const auto &[culprit, sighting] : store.sightingsOf(kObserver))
    {
        (void)sighting;
        CHECK(culprit > previous);
        previous = culprit;
        ++visits;
    }
    CHECK(visits == 4);
}

TEST_CASE(
    "TestPerception_SinkCandidatesAreAliveAscending"
)
{
    phase2_test::Fixture fixture;
    fixture.ctx.now = 10u * 3600u;

    harrowmere::world::SpatialGrid grid;
    harrowmere::npc::PerceptionSink sink;
    sink.bind(&grid, &fixture.characters, &fixture.npcs, &phase2_test::shippedRegistry());
    sink.bindWorldTime(fixture.ctx.now);

    const harrowmere::territoryId home = 7;
    const harrowmere::regionId region  = 40;
    const harrowmere::speciesId human  = phase2_test::speciesIdByKey("human");

    // Three alive humans around the crime scene, one dead one, one far away.
    const harrowmere::characterId a = phase2_test::spawnNpc(fixture, human, home, region, 25, "farmer");
    const harrowmere::characterId b = phase2_test::spawnNpc(fixture, human, home, region, 25, "vendor");
    const harrowmere::characterId c = phase2_test::spawnNpc(fixture, human, home, region, 25, "guard");
    const harrowmere::characterId d = phase2_test::spawnNpc(fixture, human, home, region, 25, "farmer");

    // Placement: the fixture parks NPCs at the settlement center; spread them
    // deterministically around it.
    const harrowmere::Position center       = fixture.world.settlementCenter(home);
    fixture.characters.get(a).body.position = center;
    fixture.characters.get(b).body.position = harrowmere::Position{center.x + 10.0, 0.0, center.z};
    fixture.characters.get(c).body.position = harrowmere::Position{center.x + 20.0, 0.0, center.z};
    fixture.characters.get(d).body.position = harrowmere::Position{center.x + 400.0, 0.0, center.z}; // outside A-60
    for (const harrowmere::characterId id : {a, b, c, d})
    {
        grid.insert(id, fixture.characters.get(id).body.position);
    }
    fixture.characters.get(c).body.currentHp = 0; // the dead do not perceive

    const std::vector<harrowmere::characterId> candidates = sink.perceiveWithin(center, harrowmere::WITNESS_RADIUS_M);

    REQUIRE(candidates.size() == 2); // a and b: alive, in radius; c dead, d far
    CHECK(candidates[0] == a);       // id-ascending (R7)
    CHECK(candidates[1] == b);

    // The version source: no appearance changes recorded -> 0 (one permanent
    // look); after a recorded change the version follows the history.
    CHECK(sink.culpritAppearanceVersionNow(a) == 0);
    harrowmere::character::AppearanceHistory history;
    history.record(a, fixture.ctx.now + 5);
    sink.bindAppearanceHistory(&history);
    CHECK(sink.culpritAppearanceVersionNow(a) == 1);
    CHECK(sink.culpritAppearanceVersionNow(b) == 0);
}
