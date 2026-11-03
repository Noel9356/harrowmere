// tests/test_justice_fixture.hpp — the shared harness for the Phase-3 justice
// cards (tasks 4.1-4.4).
//
// Builds on phase2_test::Fixture (the shipped data registry + stores + rng)
// and swaps the real world/justice state in:
//   * a hand-built territory hierarchy (1.4's jurisdiction test shape —
//     empire <- kingdom <- county <- {villageA(safe), villageB}, plus a
//     xenophobic village and a custom-law territory for the corruption tests),
//   * a real SpatialGrid + the live PerceptionSink (3.9's seam — witnesses
//     come from the grid, identity knowledge from the sighting rows),
//   * the real JusticeStore + the real JusticeSystem bound into the context
//     (the plugin's swap, exercised headless).
//
// The fixture's territory anchor is each village's region center (the same
// anchor runCaseTicks reads), so `territoryAt` resolves every spawn.
#pragma once
#include <cstdint>
#include <map>
#include <vector>

#include "character/store.hpp"
#include "core/constants.hpp"
#include "core/enums.hpp"
#include "core/game_events.hpp" // the 0.7 payload types the recording sink captures
#include "core/types.hpp"
#include "data/laws.hpp"
#include "data/registry.hpp"
#include "justice/conviction.hpp"
#include "justice/cases.hpp"
#include "justice/evidence.hpp"
#include "justice/justice_store.hpp"
#include "justice/reputation.hpp"
#include "justice/warrants.hpp"
#include "npc/perception.hpp"
#include "npc/state.hpp"
#include "world/spatial_grid.hpp"
#include "world/world.hpp"

#include "test_phase2_fixture.hpp"

namespace justice_test
{

using namespace harrowmere;

// The recording sim-event sink: the 4.x publications (CrimeRecorded,
// WarrantIssued, WarrantResolved) captured for the pinned assertions.
struct RecordingSimEvents final : public ISimEventSink
{
    std::vector<CrimeRecorded> crimes;
    std::vector<WarrantIssued> issued;
    std::vector<WarrantResolved> resolved;

    void onCrimeRecorded(
        const CrimeRecorded &ev
    ) override
    {
        crimes.push_back(ev);
    }
    void onWarrantIssued(
        const WarrantIssued &ev
    ) override
    {
        issued.push_back(ev);
    }
    void onWarrantResolved(
        const WarrantResolved &ev
    ) override
    {
        resolved.push_back(ev);
    }
};

// The pinned hierarchy (ids are opaque; the SHAPE is what the walks read):
//
//   empire(1) <- kingdom(2) <- county(3) <- villageA(4, SAFE)
//                                        <- villageB(5)
//   kingdom(2) <- xenophobic(6)            (xenophobic_village laws)
//   kingdom(2) <- corrupt(7)               (test-local NOBLE-THEFT suppression)
struct JusticeFixture
{
    phase2_test::Fixture base;

    static constexpr territoryId kEmpire     = 1;
    static constexpr territoryId kKingdom    = 2;
    static constexpr territoryId kCounty     = 3;
    static constexpr territoryId kVillageA   = 4; // the SAFE zone (region 0)
    static constexpr territoryId kVillageB   = 5; // regular ground (region 1)
    static constexpr territoryId kXenophobic = 6; // beastkin-murder-exempt (region 2)
    static constexpr territoryId kCorrupt    = 7; // the NOBLE-THEFT damp (region 3)

    // Region centers are the village anchors (the D-WORLD-1 inverse): region r
    // sits at (r * REGION_SIZE_M + half, 0, half).
    static Position villageCenter(
        regionId region
    )
    {
        return world::regionCenter(region);
    }
    static constexpr regionId kRegionA       = 0;
    static constexpr regionId kRegionB       = 1;
    static constexpr regionId kRegionXen     = 2;
    static constexpr regionId kRegionCorrupt = 3;

    // The test-local law set for the corruption mechanic (the card's own
    // example: 6000-suppression on NOBLE THEFT). Lives on the fixture so the
    // territory's laws pointer outlives every test read (the registry swap
    // contract's test form).
    data::LawSet corruptLaws;

    world::WorldStore world{phase2_test::shippedRegistry().defaultLaws()};
    world::SpatialGrid grid;
    npc::PerceptionSink perception;
    justice::JusticeStore store;
    justice::JusticeSystem system;

    JusticeFixture()
    {
        const data::DataRegistry &registry = phase2_test::shippedRegistry();

        // Regions + the generation mapping (1.4's explicit map — runtime
        // lookups are one hit).
        for (const regionId r : {kRegionA, kRegionB, kRegionXen, kRegionCorrupt})
        {
            world::Region region{};
            region.id = r;
            world.addRegion(region);
        }
        world.setRegionTerritory(kRegionA, kVillageA);
        world.setRegionTerritory(kRegionB, kVillageB);
        world.setRegionTerritory(kRegionXen, kXenophobic);
        world.setRegionTerritory(kRegionCorrupt, kCorrupt);

        // The tree: default laws everywhere except villageA (safe-zone laws)
        // and the two special grounds below.
        corruptLaws = makeCorruptLaws();

        addTerritory(kEmpire, TerritoryType::EMPIRE, 0, nullptr, false);
        addTerritory(kKingdom, TerritoryType::KINGDOM, kEmpire, nullptr, false);
        addTerritory(kCounty, TerritoryType::CITY, kKingdom, nullptr, false);
        addTerritory(kVillageA, TerritoryType::VILLAGE, kCounty, registry.lawSet("safe_zone_laws"), true);
        addTerritory(kVillageB, TerritoryType::VILLAGE, kCounty, nullptr, false);
        addTerritory(kXenophobic, TerritoryType::VILLAGE, kKingdom, registry.lawSet("xenophobic_village"), false);
        addTerritory(kCorrupt, TerritoryType::VILLAGE, kKingdom, &corruptLaws, false);

        // The FakeWorld mirror (ctx.world reads — isCriminalAct's safe-zone /
        // territory arms, the death paths): the four anchored territories at
        // their region centers, the three unanchored hierarchy levels far
        // away so nearest-center always resolves the anchored village. The
        // real walks stay on the WorldStore (D-JUR-1); this mirror only makes
        // the IWorldQueries reads resolve like the runtime adapter.
        base.addTerritory(kVillageA, world::regionCenter(kRegionA).x, world::regionCenter(kRegionA).z, kRegionA, true);
        base.addTerritory(kVillageB, world::regionCenter(kRegionB).x, world::regionCenter(kRegionB).z, kRegionB, false);
        base.addTerritory(
            kXenophobic, world::regionCenter(kRegionXen).x, world::regionCenter(kRegionXen).z, kRegionXen, false
        );
        base.addTerritory(
            kCorrupt,
            world::regionCenter(kRegionCorrupt).x,
            world::regionCenter(kRegionCorrupt).z,
            kRegionCorrupt,
            false
        );
        base.addTerritory(kEmpire, -1e6, -1e6, world::kNoRegion, false);
        base.addTerritory(kKingdom, -1e6, 1e6, world::kNoRegion, false);
        base.addTerritory(kCounty, 1e6, -1e6, world::kNoRegion, false);

        // The 4.x state rides the same context the phase-2 fixture built;
        // the justice system + the real perception sink swap in here (the
        // plugin's own swap, headless).
        base.ctx.worldStore   = &world;
        base.ctx.justiceStore = &store;
        base.ctx.justice      = &system;
        system.bind(&base.ctx);
        perception.bind(&grid, &base.characters, &base.npcs, &registry);
        base.ctx.perception = &perception;
        base.ctx.grid       = &grid; // 1.6's spatial path (5.1's SpawnTargets inserts here)
    }

    void addTerritory(
        territoryId id, TerritoryType type, territoryId parent, const data::LawSet *laws, bool safe
    )
    {
        world::Territory t;
        t.type   = type;
        t.id     = id;
        t.parent = parent;
        t.laws   = laws;
        t.safe   = safe;
        world.addTerritory(t);
    }

    static data::LawSet makeCorruptLaws()
    {
        // The shipped default thresholds with ONE damped pair: NOBLE THEFT at
        // 6000 (the card's "6000-suppression on THEFT" example).
        data::LawSet laws;
        laws.key = "test_corrupt";
        for (std::size_t i = 0; i < laws.convictionThreshold.size(); ++i)
        {
            laws.convictionThreshold[i] = 5000;
        }
        laws.convictionThreshold[static_cast<std::size_t>(CrimeType::MURDER)]              = 7000;
        laws.suppression[SocialClassId::NOBLE][static_cast<std::size_t>(CrimeType::THEFT)] = 6000;
        return laws;
    }

    // ---- spawn helpers ------------------------------------------------------------

    // An adult NPC witness at the given territory's anchor, with a role and
    // integrity the weight formula can chew on. Joins the grid (the perceive
    // half) and stamps a fresh sighting of `knowsCulprit` when set (the
    // identity half).
    characterId spawnWitness(
        territoryId home, regionId region, const char *roleKey, std::uint64_t integrity, characterId knowsCulprit = 0
    )
    {
        const characterId id
            = phase2_test::spawnNpc(base, phase2_test::speciesIdByKey("human"), home, region, 30, roleKey);
        base.npcs.get(id).integrity = integrity;
        Character &c                = base.characters.get(id);
        c.body.position             = villageCenter(region);
        grid.insert(id, c.body.position);
        if (knowsCulprit != 0)
        {
            npc::Sighting sighting;
            sighting.culpritId         = knowsCulprit;
            sighting.appearanceVersion = 0;
            sighting.worldTime         = base.ctx.now;
            perception.store().recordSighting(id, sighting, /*culpritMasked=*/false);
        }
        return id;
    }

    characterId spawnChild(
        territoryId home, regionId region
    )
    {
        // The under-16 witness: canTestify's age gate skips (A-44).
        const characterId id
            = phase2_test::spawnNpc(base, phase2_test::speciesIdByKey("human"), home, region, 10, "child");
        base.npcs.get(id).integrity = 8000;
        Character &c                = base.characters.get(id);
        c.body.position             = villageCenter(region);
        grid.insert(id, c.body.position);
        return id;
    }

    characterId spawnBeastkin(
        territoryId home, regionId region
    )
    {
        const characterId id = phase2_test::spawnNpc(base, phase2_test::speciesIdByKey("beastkin"), home, region, 30);
        Character &c         = base.characters.get(id);
        c.body.position      = villageCenter(region);
        grid.insert(id, c.body.position);
        return id;
    }

    characterId spawnPlayerAt(
        territoryId /*home*/, regionId region
    )
    {
        const characterId id = phase2_test::spawnPlayer(base);
        Character &c         = base.characters.get(id);
        c.body.position      = villageCenter(region);
        grid.insert(id, c.body.position);
        return id;
    }

    // The live case with this id (fresh lookup — vector growth on later
    // commits invalidates references, so tests NEVER hold Evidence& across a
    // commit; they re-read through the store).
    justice::Evidence *caseById(
        std::uint64_t evidenceId
    )
    {
        for (auto &[territory, cases] : store.evidenceStore)
        {
            for (justice::Evidence &e : cases)
            {
                if (e.id == evidenceId)
                {
                    return &e;
                }
            }
        }
        return nullptr;
    }

    // The evidence the tests read back (the first case of a territory) — the
    // mutable record, so tests can drive the case machine through it.
    justice::Evidence *firstEvidence(
        territoryId territory
    )
    {
        const auto it = store.evidenceStore.find(territory);
        return it == store.evidenceStore.end() || it->second.empty() ? nullptr : &it->second.front();
    }
};

} // namespace justice_test
