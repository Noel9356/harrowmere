// spatial_grid.cpp — SpatialGrid implementation (task 1.6).
//
// Membership and the radius query live here; the cell projection helpers
// (cellOf / cellIndex) are inline in the header next to their geometry doc,
// and `nearest` is the header template (it must see the filter's type). All
// cell math is integer projection (the header's R5 note); the only float
// work is the per-candidate squared-distance compare, which R5 allows.
//
// Mutations keep two facts in step — the cell row (id + position) and the
// id -> cell index — and every one of them routes through the index first,
// which is what makes the card's "an NPC is in exactly one cell" invariant
// unbreakable: a member can only ever be found where the index says it is.

#include "world/spatial_grid.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace harrowmere::world
{

SpatialGrid::SpatialGrid()
{
    // The fixed cell table: SPATIAL_CELLS_PER_AXIS^2 rows over the A-40
    // plane, allocated once (one ~6 MB reservation of empty rows — no
    // per-cell allocation until a member arrives). Row count is derived
    // geometry (A-39 x A-40), not a member slot count: membership grows
    // per insert against the card's SPATIAL_RESERVE_SLOTS bound.
    cells_.resize(static_cast<std::size_t>(SPATIAL_CELLS_PER_AXIS) * SPATIAL_CELLS_PER_AXIS);
}

// ---- the cell projection (the only place a position becomes a cell) ---------

namespace
{

// The clamped cell-axis coordinate of `v` metres. NaN is a broken position
// (programmer error: assert in debug, deterministic border cell in release);
// negative and past-plane values clamp onto the border cell — the documented
// off-plane behavior, not an error. In-plane values truncate to the integral
// metre (region.hpp's projection pattern) and integer-divide by the A-39 cell
// edge; the range guards above make the cast itself total (no out-of-range
// double -> int64 UB for absurd coordinates). Truncation equals floor on the
// non-negative in-plane domain, so the result is exactly
// clamp(floor(v / CELL_SIZE_M)) as pure integer math.
std::int64_t clampedCellAxis(
    double v
) noexcept
{
    if (std::isnan(v))
    {
        assert(false && "SpatialGrid: NaN coordinate (a broken position)");
        return 0;
    }
    if (v < 0.0)
        return 0;
    if (v >= static_cast<double>(SPATIAL_CELLS_PER_AXIS) * static_cast<double>(CELL_SIZE_M))
        return static_cast<std::int64_t>(SPATIAL_CELLS_PER_AXIS) - 1;
    return static_cast<std::int64_t>(v) / static_cast<std::int64_t>(CELL_SIZE_M);
}

} // namespace

CellCoord SpatialGrid::cellOf(
    const Position &p
) const noexcept
{
    // y is cosmetic (card 1.6) — the projection reads x and z only.
    return CellCoord{static_cast<std::int32_t>(clampedCellAxis(p.x)), static_cast<std::int32_t>(clampedCellAxis(p.z))};
}

std::size_t SpatialGrid::cellIndex(
    const Position &p
) const noexcept
{
    const CellCoord cell = cellOf(p);
    return static_cast<std::size_t>(cell.z) * SPATIAL_CELLS_PER_AXIS + static_cast<std::size_t>(cell.x);
}

// ---- membership -----------------------------------------------------------------

void SpatialGrid::insert(
    characterId id, const Position &p
)
{
    auto it = index_.find(id);
    if (it != index_.end())
    {
        // Programmer error (double insert): assert in debug; release
        // recovers by relocating deterministically — unlink from the stored
        // cell, then fall through to a plain insert at the new position.
        assert(false && "SpatialGrid::insert: id already tracked");
        unlinkAt(id, it);
    }

    const std::size_t cell = cellIndex(p);
    cells_[cell].push_back(CellMember{id, p.x, p.z});
    index_.emplace(id, cell);
}

void SpatialGrid::move(
    characterId id, [[maybe_unused]] const Position &from, const Position &to
)
{
    auto it = index_.find(id);
    if (it == index_.end())
    {
        // Programmer error (untracked id): assert in debug; release
        // recovers deterministically as insert(id, to).
        assert(false && "SpatialGrid::move: untracked id");
        insert(id, to);
        return;
    }

    // `from` is the caller's bookkeeping — debug builds check it still
    // agrees with the stored cell; release trusts the stored cell (see the
    // header doc: the grid has no other way to drift than a missed move).
    assert(cellIndex(from) == it->second && "SpatialGrid::move: stale from position");

    const std::size_t target = cellIndex(to);
    if (target == it->second)
    {
        // Same cell: the position updates in place, membership is untouched.
        auto &members = cells_[it->second];
        const auto member
            = std::find_if(members.begin(), members.end(), [id](const CellMember &m) { return m.id == id; });
        assert(member != members.end() && "SpatialGrid: index/cell rows out of step");
        member->x = to.x;
        member->z = to.z;
        return;
    }

    unlinkAt(id, it);
    cells_[target].push_back(CellMember{id, to.x, to.z});
    index_.emplace(id, target);
}

void SpatialGrid::remove(
    characterId id
)
{
    auto it = index_.find(id);
    if (it == index_.end())
    {
        // Programmer error (unknown id): assert in debug; release is a
        // deterministic no-op.
        assert(false && "SpatialGrid::remove: unknown id");
        return;
    }
    unlinkAt(id, it);
}

void SpatialGrid::clear()
{
    for (auto &members : cells_)
        members.clear();
    index_.clear();
}

// ---- queries ----------------------------------------------------------------------

std::vector<characterId> SpatialGrid::idsWithinRadius(
    const Position &p, double radiusM
) const
{
    if (std::isnan(radiusM))
    {
        assert(false && "SpatialGrid::idsWithinRadius: NaN radius");
        return {};
    }
    if (radiusM < 0.0)
    {
        assert(false && "SpatialGrid::idsWithinRadius: negative radius");
        return {};
    }

    // The cell range covering the query square [p.x - r, p.x + r] x
    // [p.z - r, p.z + r], clamped to the plane. A circle is inside its
    // square, so scanning the square's cells sees every candidate; the
    // exact circle test below discards the square-only corners.
    const std::int64_t xLo = clampedCellAxis(p.x - radiusM);
    const std::int64_t xHi = clampedCellAxis(p.x + radiusM);
    const std::int64_t zLo = clampedCellAxis(p.z - radiusM);
    const std::int64_t zHi = clampedCellAxis(p.z + radiusM);

    std::vector<characterId> out;
    for (std::int64_t cz = zLo; cz <= zHi; ++cz)
    {
        for (std::int64_t cx = xLo; cx <= xHi; ++cx)
        {
            for (const CellMember &member :
                cells_[static_cast<std::size_t>(cz) * SPATIAL_CELLS_PER_AXIS + static_cast<std::size_t>(cx)])
            {
                const double dx = member.x - p.x;
                const double dz = member.z - p.z;
                if (dx * dx + dz * dz <= radiusM * radiusM) // inclusive at the boundary (d <= r)
                    out.push_back(member.id);
            }
        }
    }

    std::sort(out.begin(), out.end()); // ascending by id — the card's R7 pin

    // The card's pitfall as a debug check: a member lives in exactly one
    // cell, so the sorted result can never carry a duplicate. (Release
    // needs no pass — the invariant is structural, see the header doc.)
    assert(std::adjacent_find(out.begin(), out.end()) == out.end() && "SpatialGrid: duplicate member in radius query");
    return out;
}

// ---- internals ----------------------------------------------------------------------

void SpatialGrid::unlinkAt(
    characterId id, std::map<characterId, std::size_t>::iterator it
)
{
    auto &members     = cells_[it->second];
    const auto member = std::find_if(members.begin(), members.end(), [id](const CellMember &m) { return m.id == id; });
    assert(member != members.end() && "SpatialGrid: index/cell rows out of step");
    *member = members.back();
    members.pop_back();
    index_.erase(it);
}

} // namespace harrowmere::world
