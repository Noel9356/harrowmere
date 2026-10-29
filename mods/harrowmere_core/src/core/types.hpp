#ifndef HARROWMERE_CORE_CORE_TYPES_HPP
#define HARROWMERE_CORE_CORE_TYPES_HPP

// The doc-exact type aliases every harrowmere_core header includes (common.md
// §3.2, rule R10: "ids are opaque" - never encode meaning into an id, never
// compare ids across id spaces).
//
// Provenance (task 0.7): this file's full content is pinned verbatim by task
// 0.2 ("types.hpp - verbatim aliases from design reference §Conventions"), but
// 0.2 runs in parallel with 0.7 and 0.7's event structs reference the aliases,
// so the pinned block lands here first. Task 0.2 owns the file from then on
// and completes it (see DEVIATIONS.md); until that card lands the file is
// append-only and carries exactly the pinned block below.

#include <cstdint>

namespace harrowmere
{

// types.hpp - verbatim aliases from design reference §Conventions
using characterId = std::uint64_t;
using territoryId = std::uint64_t;
using factionId   = std::uint32_t;
using regionId    = std::uint16_t;
using speciesId   = std::uint16_t;

struct Position
{
    double x, y, z; // f64 x/y/z per docs
};

} // namespace harrowmere

#endif // HARROWMERE_CORE_CORE_TYPES_HPP
