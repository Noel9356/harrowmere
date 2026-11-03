// territory.cpp — the territory-tree predicates (task 1.4).
//
// D-JUR-1's spirit applies to the structural predicates too: sovereign vs
// unclaimed is decided HERE, once, from the design rule pinned on card 1.4 —
// no system re-derives it from the raw fields.

#include "world/territory.hpp"

namespace harrowmere::world
{

bool isSovereign(
    const Territory &territory
) noexcept
{
    // Design rule (card 1.4): parent == 0 && !children.empty() == sovereign.
    return territory.parent == 0 && !territory.children.empty();
}

bool isUnclaimed(
    const Territory &territory
) noexcept
{
    // Design rule (card 1.4): parent == 0 && children.empty() && laws == null
    // == unclaimed. `safe`/titles/archive do not affect the classification.
    return territory.parent == 0 && territory.children.empty() && territory.laws == nullptr;
}

} // namespace harrowmere::world
