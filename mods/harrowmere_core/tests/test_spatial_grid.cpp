// test_spatial_grid.cpp — Task 1.6's pinned test suite.
//
// Implements the four tests pinned in phase-1-walk.md card 1.6's Tests block
// (test names verbatim — common.md §3.5: "Test names are pinned; keep them"):
//
//   TestGrid_RadiusQueryIncludesBoundaryCell   the scan reaches the square's
//                                              edge cells; the circle test is
//                                              INCLUSIVE at d == r
//   TestGrid_MoveBetweenCellsNoDupNoLoss       move() keeps the one-cell
//                                              invariant: never duplicated,
//                                              never lost, removes stay removed
//   TestGrid_QuerySortedDeterministic          results sorted ascending by id
//                                              and identical across runs and
//                                              across insertion orders (R7)
//   TestGrid_FullPopulationMoveWithinBudget    10k moves < 10 ms release at
//                                              the full planned population
//
// Plus three structural extras that pin the card's own sentences: nearest()
// (the card's third query, prose-named) orders by distance then id and
// honors its filter; the cell projection clamps off-plane positions onto the
// border cell; and y is cosmetic (card goal + pitfall "keep y out of cell
// math").
//
// The budget test's chrono use is TEST-ONLY wall time — R8 bans it from the
// sim, not from a stopwatch (the same note test_worldgen.cpp carries for the
// 2 s worldgen budget). The 10 ms assertion runs in release builds (NDEBUG)
// exactly as the card scopes it; debug builds run the same workload and
// report the time without the release bound.
//
// Test harness: doctest v2.4.12 (vendored per task 0.6); no main here —
// tests/main.cpp owns DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/constants.hpp"     // CELL_SIZE_M (A-39), REGION_GRID / REGION_SIZE_M (A-40)
#include "core/types.hpp"         // characterId, Position
#include "world/spatial_grid.hpp" // SPATIAL_RESERVE_SLOTS, CellCoord, SpatialGrid

using namespace harrowmere;
using namespace harrowmere::world;

namespace
{

// A query/insert position with y pinned at the cosmetic default.
Position posAt(
    double x, double z
)
{
    return Position{x, 0.0, z};
}

// A deterministic scatter over the plane with no <random> include (R6
// spirit: test fixtures draw nothing from the game's RNG streams) —
// golden-ratio multiplicative hashing spreads the ids across the A-40 plane,
// always at least CELL_SIZE_M from the border so the budget test's +/- moves
// never clamp.
Position spread(
    std::uint64_t i
)
{
    constexpr std::uint64_t kBorder = CELL_SIZE_M * 2;
    const std::uint64_t span        = static_cast<std::uint64_t>(REGION_GRID) * REGION_SIZE_M - 2 * kBorder;
    const double x                  = static_cast<double>(kBorder + (i * 2654435761ull) % span);
    const double z                  = static_cast<double>(kBorder + (i * 40503ull) % span);
    return posAt(x, z);
}

constexpr auto alwaysTrue = [](characterId) { return true; };

} // namespace

// ---------------------------------------------------------------------------
// TestGrid_RadiusQueryIncludesBoundaryCell
// The scan covers every cell the query square touches (including its edge
// cells) and the circle test is INCLUSIVE: a member at exactly d == r is
// found, a member one metre further is not, and a member in the square's far
// corner cell but beyond r is filtered by the distance even though its cell
// was scanned.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestGrid_RadiusQueryIncludesBoundaryCell"
)
{
    SpatialGrid grid;
    const Position center    = posAt(1024.0, 1024.0);                  // 3 cells from the border on every side
    constexpr double kRadius = 3.0 * static_cast<double>(CELL_SIZE_M); // 96 m — spans 3 cells

    // Exact-boundary members, one per cardinal direction: d == 96 == radius.
    grid.insert(1, posAt(1024.0 - 96.0, 1024.0));
    grid.insert(2, posAt(1024.0 + 96.0, 1024.0));
    grid.insert(3, posAt(1024.0, 1024.0 + 96.0));
    // Inside the circle but out at cell offset (+2, +2): the diagonal cell.
    grid.insert(4, posAt(1024.0 + 64.0, 1024.0 + 64.0)); // d = 64 * sqrt(2) ~ 90.5 <= 96
    // In the query square's far corner CELL but outside the circle: the cell
    // is scanned, the member is rejected by the exact distance.
    grid.insert(5, posAt(1024.0 + 96.0, 1024.0 + 96.0)); // d = 96 * sqrt(2) ~ 135.8 > 96
    // One metre past the radius on an axis.
    grid.insert(6, posAt(1024.0 + 97.0, 1024.0)); // d = 97 > 96

    const std::vector<characterId> hit = grid.idsWithinRadius(center, kRadius);
    REQUIRE(hit.size() == 4);
    CHECK(hit[0] == 1); // sorted ascending by id (pinned again in
    CHECK(hit[1] == 2); // TestGrid_QuerySortedDeterministic)
    CHECK(hit[2] == 3);
    CHECK(hit[3] == 4);

    // The same boundary, probed from the member side: the radius-0 query
    // finds only the member sitting exactly at the query point.
    grid.insert(7, posAt(500.0, 500.0));
    const std::vector<characterId> atPoint = grid.idsWithinRadius(posAt(500.0, 500.0), 0.0);
    REQUIRE(atPoint.size() == 1);
    CHECK(atPoint[0] == 7);
}

// ---------------------------------------------------------------------------
// TestGrid_MoveBetweenCellsNoDupNoLoss
// move() is the only membership mutation: after cross-cell moves, same-cell
// moves and a multi-hop chain, every member still answers exactly once (the
// card's one-cell invariant), nothing is lost, and remove() drops a member
// permanently.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestGrid_MoveBetweenCellsNoDupNoLoss"
)
{
    SpatialGrid grid;
    for (characterId id = 1; id <= 12; ++id)
        grid.insert(id, posAt(1000.0 + static_cast<double>(id), 2000.0));

    const std::vector<characterId> before = grid.idsWithinRadius(posAt(1000.0, 2000.0), 500.0);
    REQUIRE(before.size() == 12);
    CHECK(grid.size() == 12);

    // Cross-cell move: id 12 leaves its cell but stays queryable, exactly once.
    grid.move(12, posAt(1012.0, 2000.0), posAt(1490.0, 2000.0));
    std::vector<characterId> after = grid.idsWithinRadius(posAt(1000.0, 2000.0), 500.0);
    REQUIRE(after.size() == 12);
    CHECK(std::adjacent_find(after.begin(), after.end()) == after.end()); // no duplicates
    CHECK(std::find(after.begin(), after.end(), characterId{12}) != after.end());

    // Same-cell move: position updates, membership untouched (still once).
    grid.move(1, posAt(1001.0, 2000.0), posAt(1005.0, 2000.0));
    after = grid.idsWithinRadius(posAt(1000.0, 2000.0), 500.0);
    REQUIRE(after.size() == 12);
    CHECK(std::count(after.begin(), after.end(), characterId{1}) == 1);

    // A three-hop chain across the plane: the member is in exactly one cell
    // at every step, and the last `to` is the position queries see.
    grid.move(3, posAt(1003.0, 2000.0), posAt(3000.0, 2600.0));
    grid.move(3, posAt(3000.0, 2600.0), posAt(400.0, 1800.0));
    grid.move(3, posAt(400.0, 1800.0), posAt(1000.0, 2016.0));
    const std::vector<characterId> whole = grid.idsWithinRadius(posAt(8192.0, 8192.0), 25000.0);
    REQUIRE(whole.size() == 12); // the full-plane sweep loses nobody
    CHECK(std::count(whole.begin(), whole.end(), characterId{3}) == 1);
    const std::vector<characterId> atRest = grid.idsWithinRadius(posAt(1000.0, 2016.0), 0.0);
    REQUIRE(atRest.size() == 1);
    CHECK(atRest[0] == 3);

    // remove() drops the member exactly; a removed id is gone for good.
    grid.remove(5);
    CHECK_FALSE(grid.contains(5));
    CHECK(grid.size() == 11);
    after = grid.idsWithinRadius(posAt(1000.0, 2000.0), 500.0);
    REQUIRE(after.size() == 11);
    CHECK(std::find(after.begin(), after.end(), characterId{5}) == after.end());
}

// ---------------------------------------------------------------------------
// TestGrid_QuerySortedDeterministic
// Results are sorted ascending by id with no duplicates, are identical
// across repeated runs on one grid, and are identical between two grids
// built in DIFFERENT insertion orders (R7: no container's iteration order
// may reach an outcome).
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestGrid_QuerySortedDeterministic"
)
{
    const characterId ids[] = {900, 12, 77, 4001, 5, 250, 63, 8, 1000, 34};
    std::vector<characterId> expected;
    for (const characterId id : ids)
    {
        const double dx = 2000.0 + static_cast<double>(id % 50) * 6.0 - 2000.0;
        const double dz = 2000.0 + static_cast<double>(id % 40) * 8.0 - 2000.0;
        if (dx * dx + dz * dz <= 400.0 * 400.0)
            expected.push_back(id);
    }
    std::sort(expected.begin(), expected.end());
    REQUIRE(expected.size() > 5); // the fixture really spans several cells

    SpatialGrid first;
    for (const characterId id : ids)
        first.insert(
            id, posAt(2000.0 + static_cast<double>(id % 50) * 6.0, 2000.0 + static_cast<double>(id % 40) * 8.0)
        );

    const std::vector<characterId> runA = first.idsWithinRadius(posAt(2000.0, 2000.0), 400.0);
    const std::vector<characterId> runB = first.idsWithinRadius(posAt(2000.0, 2000.0), 400.0);
    CHECK(runA == expected);
    CHECK(runB == runA);
    CHECK(std::is_sorted(runA.begin(), runA.end()));
    CHECK(std::adjacent_find(runA.begin(), runA.end()) == runA.end());

    // The same members, inserted in reverse order, answer identically.
    SpatialGrid second;
    for (auto it = std::rbegin(ids); it != std::rend(ids); ++it)
        second.insert(
            *it, posAt(2000.0 + static_cast<double>(*it % 50) * 6.0, 2000.0 + static_cast<double>(*it % 40) * 8.0)
        );
    CHECK(second.idsWithinRadius(posAt(2000.0, 2000.0), 400.0) == runA);
    CHECK(second.cellIndex(posAt(2000.0, 2000.0)) == first.cellIndex(posAt(2000.0, 2000.0)));
}

// ---------------------------------------------------------------------------
// TestGrid_FullPopulationMoveWithinBudget
// 10k move() calls at the full planned population (the card's 5,500-slot
// bound over D-WGEN-2) stay under 10 ms in release. The workload is every
// member moving across cell boundaries; the chrono use is TEST-ONLY wall
// time (R8 governs the sim, not a stopwatch) and the bound is asserted in
// release (NDEBUG) builds only, exactly as the card scopes it.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestGrid_FullPopulationMoveWithinBudget"
)
{
    SpatialGrid grid;

    // The full planned population: the card's 5,500-slot bound over D-WGEN-2
    // (~4,600-5,300 planned, peaks ~5,253). Ids 1..SPATIAL_RESERVE_SLOTS.
    std::vector<Position> current(static_cast<std::size_t>(SPATIAL_RESERVE_SLOTS), posAt(0.0, 0.0));
    for (std::size_t i = 0; i < current.size(); ++i)
    {
        current[i] = spread(i + 1);
        grid.insert(static_cast<characterId>(i + 1), current[i]);
    }
    CHECK(grid.size() == current.size());

    // 10,000 cross-cell moves, each id hopping to a fresh deterministic
    // position; the grid's debug bookkeeping assert sees honest `from`s.
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < 10'000; ++i)
    {
        const std::size_t slot = static_cast<std::size_t>(i) % current.size();
        const Position to      = spread(static_cast<std::uint64_t>(i) * 7919ull + slot + 1);
        grid.move(static_cast<characterId>(slot + 1), current[slot], to);
        current[slot] = to;
    }
    const auto finished = std::chrono::steady_clock::now();
    const double ms     = std::chrono::duration<double, std::milli>(finished - started).count();

#ifdef NDEBUG
    CHECK(ms < 10.0);
    MESSAGE("budget: 10k moves over " << current.size() << " members in " << ms << " ms (release, < 10 ms)");
#else
    MESSAGE(
        "budget: 10k moves over " << current.size() << " members in " << ms
                                  << " ms (debug build — the 10 ms bound is pinned for release)"
    );
#endif
}

// ---------------------------------------------------------------------------
// TestGrid_NearestSortedByDistanceThenId_HonorsFilter
// The card's third query (prose-named): k nearest members, distance first
// and LOWER ID on exact ties, filter applied before ranking, k respected,
// short results allowed, k == 0 empty.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestGrid_NearestSortedByDistanceThenId_HonorsFilter"
)
{
    SpatialGrid grid;
    grid.insert(10, posAt(1010.0, 1000.0)); // d = 10   — tied with 20
    grid.insert(20, posAt(1010.0, 1000.0)); // d = 10   — tie broken toward id 10 first
    grid.insert(30, posAt(1005.0, 1000.0)); // d = 5
    grid.insert(40, posAt(960.0, 1000.0));  // d = 40
    grid.insert(50, posAt(1000.0, 1080.0)); // d = 80

    const std::vector<characterId> top3 = grid.nearest(posAt(1000.0, 1000.0), alwaysTrue, 3);
    REQUIRE(top3.size() == 3);
    CHECK(top3[0] == 30);
    CHECK(top3[1] == 10);
    CHECK(top3[2] == 20);

    const std::vector<characterId> all = grid.nearest(posAt(1000.0, 1000.0), alwaysTrue, 99);
    REQUIRE(all.size() == 5); // fewer members than k: everyone, still ordered
    CHECK(all[0] == 30);
    CHECK(all[1] == 10);
    CHECK(all[2] == 20);
    CHECK(all[3] == 40);
    CHECK(all[4] == 50);

    const auto evenId                       = [](characterId id) { return id % 20 == 0; };
    const std::vector<characterId> filtered = grid.nearest(posAt(1000.0, 1000.0), evenId, 2);
    REQUIRE(filtered.size() == 2);
    CHECK(filtered[0] == 20); // 10 and 30 fail the filter; distance order follows
    CHECK(filtered[1] == 40);

    const std::vector<characterId> none = grid.nearest(posAt(1000.0, 1000.0), [](characterId) { return false; }, 4);
    CHECK(none.empty());
    CHECK(grid.nearest(posAt(1000.0, 1000.0), alwaysTrue, 0).empty());

    // The expansion reaches every ring it needs: a lone member far out in
    // the plane is still the nearest answer.
    grid.insert(60, posAt(8000.0, 9000.0));
    const std::vector<characterId> far = grid.nearest(posAt(1000.0, 1000.0), alwaysTrue, 1);
    REQUIRE(far.size() == 1);
    CHECK(far[0] == 30);
}

// ---------------------------------------------------------------------------
// TestGrid_CellOfAndCellIndex_ClampToThePlane
// The projection: 32 m cells (A-39) over the A-40 plane, x fastest (the
// region.hpp layout convention), off-plane coordinates clamped onto the
// border cell so the answer is always a valid in-plane cell.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestGrid_CellOfAndCellIndex_ClampToThePlane"
)
{
    SpatialGrid grid;

    CHECK(grid.cellOf(posAt(0.0, 0.0)) == CellCoord{0, 0});
    CHECK(grid.cellOf(posAt(31.9, 0.0)) == CellCoord{0, 0});
    CHECK(grid.cellOf(posAt(32.0, 0.0)) == CellCoord{1, 0});
    CHECK(grid.cellOf(posAt(16383.9, 16383.9)) == CellCoord{511, 511});

    // Off-plane clamps (documented behavior, not an error).
    CHECK(grid.cellOf(posAt(-5.0, -0.5)) == CellCoord{0, 0});
    CHECK(grid.cellOf(posAt(20000.0, 16384.0)) == CellCoord{511, 511});

    // Flat index: z * 512 + x — x fastest, matching region.hpp's region-id
    // layout convention.
    CHECK(grid.cellIndex(posAt(16383.9, 16383.9)) == 511u * 512u + 511u);
    CHECK(grid.cellIndex(posAt(0.0, 64.0)) == 2u * 512u + 0u);
    CHECK(grid.cellIndex(posAt(64.0, 0.0)) == 0u * 512u + 2u);
}

// ---------------------------------------------------------------------------
// TestGrid_YIsCosmetic_InCellMathAndDistance
// The card's "2D (x,z; y is cosmetic)" and its pitfall "keep y out of cell
// math": members differing ONLY in y share one cell, and distances ignore y
// entirely.
// ---------------------------------------------------------------------------
TEST_CASE(
    "TestGrid_YIsCosmetic_InCellMathAndDistance"
)
{
    SpatialGrid grid;
    const Position high{1000.0, 500.0, 1000.0};  // 500 m in the air
    const Position deep{1000.0, -500.0, 1000.0}; // 500 m underground
    CHECK(grid.cellOf(high) == grid.cellOf(deep));

    grid.insert(1, high);
    grid.insert(2, deep);

    // A 5 m radius would miss both if y entered the distance.
    const std::vector<characterId> hit = grid.idsWithinRadius(posAt(1000.0, 1000.0), 5.0);
    REQUIRE(hit.size() == 2);
    CHECK(hit[0] == 1);
    CHECK(hit[1] == 2);

    // nearest() sees the same d = 0 tie and breaks it toward the lower id.
    const std::vector<characterId> top1 = grid.nearest(posAt(1000.0, 1000.0), alwaysTrue, 1);
    REQUIRE(top1.size() == 1);
    CHECK(top1[0] == 1);

    // Moves keep ignoring y: a "moved" member keeps its x/z cell.
    grid.move(1, high, Position{1032.0, 9000.0, 1000.0});
    CHECK(grid.cellIndex(Position{1032.0, 9000.0, 1000.0}) == grid.cellIndex(posAt(1032.0, 1000.0)));
    CHECK(grid.idsWithinRadius(posAt(1032.0, 1000.0), 0.0)[0] == 1);
}
