#ifndef HARROWMERE_CORE_WORLD_SPATIAL_GRID_HPP
#define HARROWMERE_CORE_WORLD_SPATIAL_GRID_HPP

// SpatialGrid — the uniform 2D spatial index (task 1.6).
//
// Fast x/z queries every other system needs: witnesses around a crime (4.1's
// WITNESS_RADIUS sweep), guards near a player (4.6), NPC distance for the
// LOD tiers (3.5). The card pins the shape: a uniform grid of CELL_SIZE_M =
// 32 m cells (A-39) over the A-40 world plane, "radius <= 100 m — a cell
// scan is trivially fast; no tree structures in v1". y is cosmetic (card
// goal) — cell math and distances read x and z only, never y.
//
// THE one spatial-query path (card Done-when): the later consumers — LOD
// distance checks (3.5) and justice's perceiveWithin (4.1) — route through
// this class when they land; no ad-hoc neighbour scan is allowed outside it.
//
// State shape: each cell row holds its members as (id, x, z) — the position
// lives EXACTLY once, in the cell row, so queries run with zero map lookups;
// the id -> cell index below is pure membership bookkeeping. That is also
// why the card pins move(id, from, to): `to` supplies the new position (the
// grid has no other write path for it) and `from` lets debug builds check
// the caller's bookkeeping against the stored cell.
//
// Determinism (common.md §3.2):
//   * R7 — every query RESULT is sorted (idsWithinRadius ascending by id;
//     nearest by distance then id), so no container's internal iteration
//     order can reach an outcome. The id -> cell index is std::map (ordered
//     even though it is never iterated for outcomes), the cell table is a
//     flat vector indexed arithmetically, and per-cell row order is only
//     ever observed through a sort. No unordered container anywhere.
//   * R5 — Position stays f64 and the per-candidate squared-distance
//     compare is geometric query work, not a game rule: exactly the form R5
//     allows ("distances compare as f64"). All cell math is pure integer
//     projection — the same truncate-then-divide pattern region.hpp pins
//     for the region grid (on the non-negative in-plane domain truncation
//     equals floor, so there is no float edge case at cell borders). The
//     card's pitfall "keep y out of cell math" is structural: the
//     projection and the stored member positions never read p.y.
//   * The grid is NOT save data: it is derived, per-tick-mutable state, on
//     7.1's do-not-save list (Appendix D.2) — a load clears it and replays
//     every character position through insert; it never round-trips.
//
// Membership invariant (card pitfall): an id is in EXACTLY ONE cell,
// always. Every mutation goes through the id -> cell index, so a duplicate
// is unrepresentable; debug builds additionally assert the invariant after
// each radius query (std::adjacent_find on the sorted output) and at the
// mutation entry points. The card's "idsWithinRadius must dedupe when
// radius spans multiple cells" is satisfied by this invariant — a member is
// collected from its one cell exactly once — and pinned by
// TestGrid_MoveBetweenCellsNoDupNoLoss.
//
// Error handling (§3.4): contract violations (double insert, move of an
// untracked id, remove of an unknown id, a `from` that disagrees with the
// stored cell) are programmer errors — assert in debug, recover
// deterministically in release (the recovery is written at each site; the
// stored cell stays the single source of truth). The structure logs
// nothing: it sits on the per-tick path (§3.4 hot-loop rule) and has no
// system-entry events of its own.
//
// Capacity (card spec): sized for the full planned population — D-WGEN-2's
// measured plan peaks at ~5,253 NPCs and the card pins "size for 5,500
// slots with headroom" — SPATIAL_RESERVE_SLOTS below. The cell table is a
// fixed flat vector dimensioned once by the world plane (one allocation,
// O(1) cell lookup); membership itself grows per insert with no fixed slot
// array to exhaust, and the budget test runs at exactly
// SPATIAL_RESERVE_SLOTS members.
//
// Threading: none. The sim is single-threaded by the engine's event-bus
// contract (§4.0); the grid carries no locks, like every store (§4.5).
//
// D-WORLD-1 seam: cell coordinates derive from the same world plane the
// region projection uses (region.hpp). The plane tiles evenly into cells
// (static_assert below); should a spherical projection ever replace the
// flat one, the two projection helpers next to cellOf are the only cell
// math to revisit.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "core/constants.hpp" // CELL_SIZE_M (A-39), REGION_GRID / REGION_SIZE_M (A-40)
#include "core/types.hpp"     // characterId, Position (task 0.2)

namespace harrowmere::world
{

// Card 1.6 capacity pin: "size for 5,500 slots with headroom" over D-WGEN-2's
// measured plan maximum (~5,253). Card-pinned like worldgen.hpp's generation
// constants; awaiting Appendix A promotion with the owner's D-WGEN-2 choice.
inline constexpr std::size_t SPATIAL_RESERVE_SLOTS = 5500;

// Derived grid geometry (no new pin: computed from the A-39/A-40 rows) — the
// world plane [0, REGION_GRID * REGION_SIZE_M)^2 tiles into
// SPATIAL_CELLS_PER_AXIS^2 cells of CELL_SIZE_M metres on edge (512 x 512).
inline constexpr std::uint32_t SPATIAL_CELLS_PER_AXIS = REGION_GRID * REGION_SIZE_M / CELL_SIZE_M;
static_assert(REGION_GRID * REGION_SIZE_M % CELL_SIZE_M == 0, "the A-40 world plane must tile evenly into A-39 cells");

// A clamped cell coordinate: [0, SPATIAL_CELLS_PER_AXIS) on each axis.
// Off-plane positions clamp onto the border cell (see cellOf) — the grid
// covers the A-40 plane and answers for everything near it, with no
// out-of-range special case at call sites.
struct CellCoord
{
    std::int32_t x = 0;
    std::int32_t z = 0;

    bool operator==(const CellCoord &) const = default;
};

class SpatialGrid
{
public:
    SpatialGrid();

    SpatialGrid(const SpatialGrid &)            = delete;
    SpatialGrid &operator=(const SpatialGrid &) = delete;

    // ---- membership -----------------------------------------------------------------

    // Track `id` at `p` (x/z; y is ignored — cosmetic, card 1.6). A double
    // insert is a programmer error (assert in debug); release recovers by
    // deterministically RELOCATING the id to the new position — the
    // "exactly one cell" invariant holds either way.
    void insert(characterId id, const Position &p);

    // The card's update path: "move(id, from, to) updates cell membership".
    // `to` is authoritative — it replaces the stored position and, when it
    // leaves the current cell, the membership. `from` is the caller's
    // bookkeeping: debug builds assert cellOf(from) still matches the
    // stored cell; in release the stored cell stays authoritative, so a
    // stale `from` can never desync the grid. A move whose endpoints share
    // one cell updates the position in place and nothing else. Moving an
    // untracked id is a programmer error (assert in debug); release
    // recovers as insert(id, to).
    void move(characterId id, [[maybe_unused]] const Position &from, const Position &to);

    // Drop `id` and its stored position entirely. Removing an unknown id is
    // a programmer error (assert in debug); release is a deterministic
    // no-op.
    void remove(characterId id);

    // Empty the grid (the 7.1 rebuild path: a load clears, then replays
    // every character position through insert). Keeps the cell table
    // allocated.
    void clear();

    // ---- queries --------------------------------------------------------------------

    // Every tracked id within `radiusM` of `p` (x/z distance; INCLUSIVE at
    // the boundary — d <= r — so a member exactly on the radius is found),
    // sorted ascending by id (the card's determinism pin, R7). The card's
    // v1 consumers stay at radius <= 100 m (WITNESS_RADIUS 30, CONSENT
    // broadcast 50); larger radii stay correct and merely scan more cells.
    // A negative radius is a programmer error (assert in debug; release
    // returns empty).
    std::vector<characterId> idsWithinRadius(const Position &p, double radiusM) const;

    // The k tracked ids nearest `p` (x/z distance) that pass `filter`,
    // ordered by distance first and by LOWER ID on ties — deterministic
    // under R7 like every query here. Fewer than k matching members yield
    // all of them; k == 0 yields empty. `filter` is any callable accepting
    // a characterId and returning bool (exclude self, keep guards only,
    // ...); it must be a pure predicate — the grid is const and the
    // predicate must not mutate sim state mid-query.
    //
    // Expansion: Chebyshev cell rings around p's cell; candidates feed a
    // bounded max-heap of the k best. Termination is exact: after ring r
    // completes, every unscanned cell sits in ring r+1 or beyond, and on
    // such a ring's far axis the gap to p exceeds one full cell width per
    // ring step — every point there is STRICTLY more than
    // r * CELL_SIZE_M away (a single-axis gap lower-bounds the Euclidean
    // distance). With the heap full and its worst candidate at most
    // r * CELL_SIZE_M away, nothing unscanned can enter the heap, so the
    // scan stops; the geometry is strict (unscanned rings sit BEYOND the
    // bound, never on it), so stopping at `<=` cannot lose a candidate that
    // would have won the id tiebreak.
    template <typename Filter>
    std::vector<characterId> nearest(
        const Position &p, Filter &&filter, std::size_t k
    ) const
    {
        std::vector<characterId> out;
        if (k == 0)
            return out;

        // The k best candidates so far as (dist2, id); std::push_heap with
        // `closer` keeps the WORST candidate at heap.front() for O(log k)
        // eviction. Ties resolve toward the smaller id, matching the result
        // order this function documents.
        struct Candidate
        {
            double dist2;
            characterId id;
        };
        const auto closer = [](const Candidate &a, const Candidate &b) {
            return a.dist2 != b.dist2 ? a.dist2 < b.dist2 : a.id < b.id;
        };
        std::vector<Candidate> heap;
        const auto offer = [&heap, &closer, k](double dist2, characterId id) {
            const Candidate candidate{dist2, id};
            if (heap.size() < k)
            {
                heap.push_back(candidate);
                std::push_heap(heap.begin(), heap.end(), closer);
            } else if (closer(candidate, heap.front()))
            {
                std::pop_heap(heap.begin(), heap.end(), closer);
                heap.back() = candidate;
                std::push_heap(heap.begin(), heap.end(), closer);
            }
        };

        // One ring segment cell: test every member against p. Cell bounds
        // are checked here (rings are enumerated unclamped; the guard makes
        // the four-segment enumeration visit each in-plane cell exactly
        // once — no clamping overlaps, so no member is offered twice).
        const auto visit = [&](std::int64_t cx, std::int64_t cz) {
            if (cx < 0 || cx >= static_cast<std::int64_t>(SPATIAL_CELLS_PER_AXIS) || cz < 0
                || cz >= static_cast<std::int64_t>(SPATIAL_CELLS_PER_AXIS))
                return;
            for (const CellMember &member :
                cells_[static_cast<std::size_t>(cz) * SPATIAL_CELLS_PER_AXIS + static_cast<std::size_t>(cx)])
            {
                if (!filter(member.id))
                    continue;
                const double dx = member.x - p.x;
                const double dz = member.z - p.z;
                offer(dx * dx + dz * dz, member.id);
            }
        };

        const CellCoord center = cellOf(p);
        const std::int64_t cx  = center.x;
        const std::int64_t cz  = center.z;
        // Rings beyond this hold no in-plane cell: the farthest grid cell is
        // max(cx, AXIS-1-cx, cz, AXIS-1-cz) Chebyshev steps from the center.
        const std::int64_t maxRing = std::max<std::int64_t>(
            std::max(cx, static_cast<std::int64_t>(SPATIAL_CELLS_PER_AXIS) - 1 - cx),
            std::max(cz, static_cast<std::int64_t>(SPATIAL_CELLS_PER_AXIS) - 1 - cz)
        );

        for (std::int64_t ring = 0;; ++ring)
        {
            if (ring == 0)
            {
                visit(cx, cz);
            } else
            {
                const std::int64_t xLo = cx - ring;
                const std::int64_t xHi = cx + ring;
                const std::int64_t zLo = cz - ring;
                const std::int64_t zHi = cz + ring;
                // The two rows (corners included) and the two columns
                // (z strictly between) — four disjoint segments, one visit
                // per cell of the ring.
                for (std::int64_t x = xLo; x <= xHi; ++x)
                {
                    visit(x, zHi);
                    visit(x, zLo);
                }
                for (std::int64_t z = zLo + 1; z <= zHi - 1; ++z)
                {
                    visit(xLo, z);
                    visit(xHi, z);
                }
            }

            // Exact termination (see the doc block): everything unscanned is
            // strictly beyond ring * CELL_SIZE_M away.
            if (heap.size() == k)
            {
                const double bound = static_cast<double>(ring) * static_cast<double>(CELL_SIZE_M);
                if (heap.front().dist2 <= bound * bound)
                    break;
            }
            if (ring >= maxRing)
                break; // the whole plane has been scanned
        }

        std::sort(heap.begin(), heap.end(), closer);
        out.reserve(heap.size());
        for (const Candidate &candidate : heap)
            out.push_back(candidate.id);
        return out;
    }

    // The clamped cell holding `p` (x/z only — y never enters cell math).
    // Coordinates truncate to a cell index exactly as region.hpp's region
    // projection truncates to a region index; negative and past-edge
    // coordinates clamp onto the border cell, so the answer is always a
    // valid in-plane cell.
    CellCoord cellOf(const Position &p) const noexcept;

    // The flat cell index of `p`: cellOf(p).z * SPATIAL_CELLS_PER_AXIS +
    // cellOf(p).x — x fastest, the same layout convention region.hpp pins
    // for region ids ("the id layout ... derived from one place").
    std::size_t cellIndex(const Position &p) const noexcept;

    // ---- bookkeeping ------------------------------------------------------------------

    bool contains(
        characterId id
    ) const noexcept
    {
        return index_.find(id) != index_.end();
    }

    std::size_t size() const noexcept
    {
        return index_.size();
    }

private:
    // One tracked member inside a cell row: the id and its current x/z
    // position (y is cosmetic — never stored, never compared).
    struct CellMember
    {
        characterId id;
        double x;
        double z;
    };

    // Erase the member `it` points at from its cell row and the index
    // (swap-and-pop: per-cell row order is never observed — every query
    // sorts its output — so O(1) removal beats order preservation).
    void unlinkAt(characterId id, std::map<characterId, std::size_t>::iterator it);

    std::vector<std::vector<CellMember>> cells_; // the flat cell table, dimensioned once
    std::map<characterId, std::size_t> index_;   // id -> the one cell holding it (R7-ordered)
};

} // namespace harrowmere::world

#endif // HARROWMERE_CORE_WORLD_SPATIAL_GRID_HPP
