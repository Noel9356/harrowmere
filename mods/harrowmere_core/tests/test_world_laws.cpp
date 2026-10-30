// test_world_laws.cpp — Task 1.4's pinned test suite.
//
// Implements the six tests pinned in phase-1-walk.md card 1.4's Tests block
// (test names verbatim — common.md §3.5: "Test names are pinned; keep
// them"). The suite builds a hand-made territory hierarchy in memory (card
// Done-when: "World is constructible in-memory (empty population) with a
// hand-built test hierarchy") and pins the three doc walks' semantics
// exactly as the design reference §Laws / §Justice / §Safe Zones states
// them:
//
//   lawsAt:        explicit laws win; the root resolves DEFAULT_LAWS
//   socialClassOf: the DEEPEST (most local) title wins; COMMONER fallback
//   jurisdiction:  isAncestorOrSelf covers the issuing chain, never siblings
//   territoryAt:   the explicit region -> territory mapping answers for
//                  every region; unmapped space resolves the null territory
//
// Test harness: doctest v2.4.12 (vendored per task 0.6); no main here —
// tests/main.cpp owns DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <utility>

#include "core/constants.hpp" // REGION_COUNT (A-40)
#include "core/enums.hpp"
#include "core/types.hpp"
#include "data/laws.hpp"
#include "world/world.hpp"

using namespace harrowmere;
using namespace harrowmere::world;

namespace
{

// A distinctive LawSet: only the MURDER conviction threshold is set, so
// every law-resolution assertion can compare one readable number.
data::LawSet makeLawSet(
    std::string key, std::uint32_t murderThreshold
)
{
    data::LawSet set;
    set.key                                                              = std::move(key);
    set.convictionThreshold[static_cast<std::size_t>(CrimeType::MURDER)] = murderThreshold;
    return set;
}

// A territory node with the structural fields set (laws/titles/safe stay at
// their defaults; tests override what they exercise).
Territory makeTerritory(
    TerritoryType type, territoryId id, territoryId parent
)
{
    Territory t;
    t.type   = type;
    t.id     = id;
    t.parent = parent;
    t.region = kNoRegion;
    return t;
}

// The pinned hierarchy of the jurisdiction test (card 1.4):
//
//   empire(1)  <-  kingdom(2)  <-  city(3)  <-  villageA(4)
//                                           <-  villageB(5)
//
// A village warrant (issued for villageA) is active in villageA, city,
// kingdom and empire — never in sibling villageB. The ids are arbitrary
// (R10: ids are opaque); the SHAPE is what the walks read.
struct Hierarchy
{
    data::LawSet defaultLaws = makeLawSet("default", 7000);
    WorldStore store{defaultLaws};

    Hierarchy()
    {
        store.addTerritory(makeTerritory(TerritoryType::EMPIRE, 1, 0));
        store.addTerritory(makeTerritory(TerritoryType::KINGDOM, 2, 1));
        store.addTerritory(makeTerritory(TerritoryType::CITY, 3, 2));
        store.addTerritory(makeTerritory(TerritoryType::VILLAGE, 4, 3));
        store.addTerritory(makeTerritory(TerritoryType::VILLAGE, 5, 3));
    }
};

// The murder threshold a law set carries, read back through a walk.
std::uint32_t murderThresholdAt(
    const WorldStore &store, territoryId t
)
{
    return store.lawsAt(t).convictionThreshold[static_cast<std::size_t>(CrimeType::MURDER)];
}

} // namespace

// ---------------------------------------------------------------------------
// TestLawsAt_InheritsToDefaultAtRoot
// With no explicit laws anywhere on the chain, every level — village, city,
// kingdom, root empire — resolves the same DEFAULT_LAWS set the store was
// constructed with (design reference §Laws: lawsAt's parent==0 arm).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestLawsAt_InheritsToDefaultAtRoot"
)
{
    Hierarchy h;

    CHECK(murderThresholdAt(h.store, 4) == 7000); // villageA
    CHECK(murderThresholdAt(h.store, 3) == 7000); // city
    CHECK(murderThresholdAt(h.store, 2) == 7000); // kingdom
    CHECK(murderThresholdAt(h.store, 1) == 7000); // root empire

    // Every level resolves the SAME object — the root fallback, not copies.
    CHECK(&h.store.lawsAt(4) == &h.store.lawsAt(1));
    CHECK(&h.store.lawsAt(1) == &h.defaultLaws);

    // The null territory (unmapped frontier position) resolves the default
    // too — the doc walk terminates at parent 0 immediately.
    CHECK(murderThresholdAt(h.store, 0) == 7000);
}

// ---------------------------------------------------------------------------
// TestLawsAt_TerritoryLawOverridesParent
// An explicit law set wins over the parent's at every depth: the village
// with no laws inherits its kingdom's set; the kingdom's set beats the
// empire's; the empire resolves its own; a village WITH its own set
// overrides the kingdom's.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestLawsAt_TerritoryLawOverridesParent"
)
{
    data::LawSet empireLaws  = makeLawSet("empire_laws", 7000);
    data::LawSet kingdomLaws = makeLawSet("kingdom_laws", 6000);
    data::LawSet villageLaws = makeLawSet("village_laws", 3000);

    Hierarchy h;
    h.store.territory(1)->laws = &empireLaws;
    h.store.territory(2)->laws = &kingdomLaws;

    CHECK(murderThresholdAt(h.store, 4) == 6000); // village inherits the kingdom's set
    CHECK(murderThresholdAt(h.store, 2) == 6000); // the kingdom resolves its own
    CHECK(murderThresholdAt(h.store, 1) == 7000); // the empire resolves its own

    // A village with its OWN set stops the walk one level up.
    h.store.territory(5)->laws = &villageLaws;
    CHECK(murderThresholdAt(h.store, 5) == 3000);
    CHECK(&h.store.lawsAt(5) == &villageLaws);
    CHECK(murderThresholdAt(h.store, 4) == 6000); // sibling village unaffected
}

// ---------------------------------------------------------------------------
// TestSocialClass_FindsTitleAtDeepestLevel
// socialClassOf walks UP from the given territory and returns the FIRST
// level carrying the character's title — the deepest (most local) one; a
// character titled only at the empire reads NOBLE from a village start.
// Card 1.4's pitfall: the first hit wins, not the last.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestSocialClass_FindsTitleAtDeepestLevel"
)
{
    Hierarchy h;
    h.store.territory(1)->titles[100] = SocialClassId::NOBLE;    // empire
    h.store.territory(2)->titles[100] = SocialClassId::ROYAL;    // kingdom
    h.store.territory(4)->titles[100] = SocialClassId::COMMONER; // villageA

    // The deepest title on the chain wins, starting from each level.
    CHECK(h.store.socialClassOf(100, 4) == SocialClassId::COMMONER);
    CHECK(h.store.socialClassOf(100, 3) == SocialClassId::ROYAL);
    CHECK(h.store.socialClassOf(100, 2) == SocialClassId::ROYAL);
    CHECK(h.store.socialClassOf(100, 1) == SocialClassId::NOBLE);

    // Character 101 is titled only at the empire: every start level below
    // walks up to it (first — and only — hit).
    h.store.territory(1)->titles[101] = SocialClassId::NOBLE;
    CHECK(h.store.socialClassOf(101, 4) == SocialClassId::NOBLE);
    CHECK(h.store.socialClassOf(101, 2) == SocialClassId::NOBLE);

    // Sibling structure does not leak: villageB (5) shares the chain, so
    // character 100 reads COMMONER there only if villageA's title is the
    // hit — start at villageB and the walk skips villageA's level.
    CHECK(h.store.socialClassOf(100, 5) == SocialClassId::ROYAL);
}

// ---------------------------------------------------------------------------
// TestSocialClass_DefaultsCommoner
// The doc walk's fallback: no title anywhere on the chain, an unknown start
// territory, and the null territory all resolve COMMONER.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestSocialClass_DefaultsCommoner"
)
{
    Hierarchy h;

    // No titles at all.
    CHECK(h.store.socialClassOf(77, 4) == SocialClassId::COMMONER);
    CHECK(h.store.socialClassOf(77, 1) == SocialClassId::COMMONER);

    // The walk terminators: null territory and an unknown id.
    CHECK(h.store.socialClassOf(77, 0) == SocialClassId::COMMONER);
    CHECK(h.store.socialClassOf(77, 999) == SocialClassId::COMMONER);

    // A title at a sibling branch does not leak into this chain.
    h.store.territory(5)->titles[88] = SocialClassId::NOBLE;
    CHECK(h.store.socialClassOf(88, 4) == SocialClassId::COMMONER);
    CHECK(h.store.socialClassOf(88, 5) == SocialClassId::NOBLE);
}

// ---------------------------------------------------------------------------
// TestJurisdiction_VillageWarrantCoversCountyNotSibling
// The jurisdiction walk (design reference `warrantActiveIn`, generalized as
// isAncestorOrSelf(a, t) = "a is t or an ancestor of t"): a warrant issued
// for villageA is active in exactly the territories whose ancestor chain
// contains villageA — villageA itself, its city (county), kingdom, empire —
// never a sibling village, and a root warrant only at the root.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestJurisdiction_VillageWarrantCoversCountyNotSibling"
)
{
    Hierarchy h;

    // The issuing chain: the warrant is active at the village itself and at
    // every territory above it (each is an ancestor-or-self of villageA).
    CHECK(h.store.isAncestorOrSelf(4, 4)); // the village's own ground
    CHECK(h.store.isAncestorOrSelf(3, 4)); // city ("county")
    CHECK(h.store.isAncestorOrSelf(2, 4)); // kingdom
    CHECK(h.store.isAncestorOrSelf(1, 4)); // empire

    // Never the sibling branch: the sibling is not an ancestor of the
    // issuing village — in either direction.
    CHECK_FALSE(h.store.isAncestorOrSelf(5, 4)); // sibling village
    CHECK_FALSE(h.store.isAncestorOrSelf(4, 5));

    // A root (empire) warrant is active only at the root: nothing below the
    // root is an ancestor of it (jurisdiction walks up, never down).
    CHECK(h.store.isAncestorOrSelf(1, 1));
    CHECK_FALSE(h.store.isAncestorOrSelf(4, 1));

    // Terminators: the null territory and unknown ids cover nothing and are
    // covered by nothing.
    CHECK_FALSE(h.store.isAncestorOrSelf(4, 0));
    CHECK_FALSE(h.store.isAncestorOrSelf(0, 4));
    CHECK_FALSE(h.store.isAncestorOrSelf(999, 4));
    CHECK_FALSE(h.store.isAncestorOrSelf(4, 999));

    // The tree wiring the walk relies on: city holds both villages as
    // children, the villages are leaves.
    CHECK(h.store.childrenOf(3).size() == 2);
    CHECK(h.store.childrenOf(4).empty());
    CHECK(h.store.parentOf(4) == 3);
}

// ---------------------------------------------------------------------------
// TestTerritoryAt_MapsEveryRegion
// The generation mapping answers for EVERY region of the A-40 grid (one map
// hit, no geometric recompute): each region resolves the territory it was
// mapped to, region borders project deterministically, and unmapped space
// (frontier / outside the plane) resolves the null territory. Also pins
// isInSafeZone's verbatim doc form (isSafeTerritory(territoryAt(position))).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestTerritoryAt_MapsEveryRegion"
)
{
    data::LawSet defaultLaws = makeLawSet("default", 7000);
    WorldStore store{defaultLaws};

    store.addTerritory(makeTerritory(TerritoryType::EMPIRE, 1, 0));
    store.addTerritory(makeTerritory(TerritoryType::VILLAGE, 2, 1));
    store.territory(2)->safe = true; // safe zones are spawn points (design rule)

    // Region-geometry probes derived from the A-40 rows (nothing here
    // hardcodes a grid number: the grid IS the constant).
    const double regionEdge = static_cast<double>(harrowmere::REGION_SIZE_M); // 1024 m (A-40)
    const double regionHalf = regionEdge / 2.0;                               // probe: region center offset
    const double planeEdge  = static_cast<double>(harrowmere::REGION_GRID) * regionEdge;
    const double justInside = regionEdge - 0.1; // probe epsilon inside a border

    // Map every region of the grid to the empire — except region 5, the
    // safe village's anchor.
    for (std::uint32_t r = 0; r < harrowmere::REGION_COUNT; ++r)
    {
        Region region;
        region.id           = static_cast<regionId>(r);
        region.manaRichness = 9500u + r; // D-LORE-3's band; contents are 1.5's business
        store.addRegion(region);
        store.setRegionTerritory(static_cast<regionId>(r), 1);
    }
    store.setRegionTerritory(5, 2);
    store.territory(2)->region = 5;

    REQUIRE(store.regionCount() == harrowmere::REGION_COUNT);
    REQUIRE(store.mappedRegionCount() == harrowmere::REGION_COUNT);

    // Every region resolves its mapped territory from its center point.
    for (std::uint32_t r = 0; r < harrowmere::REGION_COUNT; ++r)
    {
        const std::uint32_t xIndex = r % harrowmere::REGION_GRID;
        const std::uint32_t zIndex = r / harrowmere::REGION_GRID;
        const Position center{xIndex * regionEdge + regionHalf, 0.0, zIndex * regionEdge + regionHalf};
        const territoryId expected = (r == 5) ? 2 : 1;
        CHECK_MESSAGE(store.territoryAt(center) == expected, "region ", r, " must resolve its generation mapping");
        CHECK(store.territoryOfRegion(static_cast<regionId>(r)) == expected);
    }

    // Border projection: a position on the far side of the first region edge
    // lands in the next region (the id layout is zIndex * REGION_GRID +
    // xIndex, x fastest — region.hpp pins it).
    CHECK(regionIdAt(Position{0.0, 0.0, 0.0}) == 0);
    CHECK(regionIdAt(Position{justInside, 0.0, 0.0}) == 0);
    CHECK(regionIdAt(Position{regionEdge, 0.0, 0.0}) == 1);
    CHECK(regionIdAt(Position{0.0, 0.0, regionEdge}) == static_cast<regionId>(harrowmere::REGION_GRID));
    CHECK(
        regionIdAt(Position{planeEdge - 0.1, 0.0, planeEdge - 0.1})
        == static_cast<regionId>(harrowmere::REGION_COUNT - 1)
    );

    // Outside the world plane: no region, no territory.
    CHECK(regionIdAt(Position{-1.0, 0.0, 0.0}) == kNoRegion);
    CHECK(regionIdAt(Position{0.0, 0.0, -0.5}) == kNoRegion);
    CHECK(regionIdAt(Position{planeEdge, 0.0, 0.0}) == kNoRegion);
    CHECK(store.territoryAt(Position{-1.0, 0.0, 0.0}) == kNoTerritory);
    CHECK(store.territoryAt(Position{planeEdge, 0.0, 0.0}) == kNoTerritory);

    // The safe village's anchor region reads safe; the next region over
    // does not (doc form: isSafeTerritory(territoryAt(position))).
    CHECK(store.isInSafeZone(Position{5 * regionEdge + regionHalf, 0.0, regionHalf}));
    CHECK_FALSE(store.isInSafeZone(Position{6 * regionEdge + regionHalf, 0.0, regionHalf}));
    CHECK_FALSE(store.isInSafeZone(Position{-1.0, 0.0, 0.0}));

    // A store with NO mapping behaves as unclaimed space everywhere.
    WorldStore frontier{defaultLaws};
    frontier.addTerritory(makeTerritory(TerritoryType::EMPIRE, 1, 0));
    CHECK(frontier.territoryAt(Position{regionHalf, 0.0, regionHalf}) == kNoTerritory);
    CHECK(murderThresholdAt(frontier, frontier.territoryAt(Position{regionHalf, 0.0, regionHalf})) == 7000);
    CHECK(frontier.socialClassOf(1, kNoTerritory) == SocialClassId::COMMONER);
}
