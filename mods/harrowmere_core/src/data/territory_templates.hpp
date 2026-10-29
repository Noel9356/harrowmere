// territory_templates.hpp — Territory template loader (Task 1.2).
//
// Loads `mods/harrowmere_core/data/territory_templates.json` into a typed,
// validated TerritoryTemplateSet. Territory templates are worldgen's
// blueprint (task 1.5) — they tell worldgen how many villages per city,
// cities per kingdom, kingdoms per empire; what population range each
// territory type draws; which law set is the territory default; whether
// the territory is a safe zone; and what region flag bit meanings exist
// (so mods can declare new flags in data, per design reference §World:
// "Flags ... defined in data files").
//
// The JSON shape (Appendix B.8) wraps three groups of fields:
//   * `types`           — per-TerritoryType blueprint {population, lawSet,
//                         safe, arena?}.
//   * count multipliers  — villages_per_city, cities_per_kingdom,
//                         kingdoms_per_empire.
//   * `regionFlags`     — string -> bit (1, 2, 4, 8, ...). Flag bits are
//                         declared in data; the game never hardcodes a
//                         flag meaning (design rule).
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/constants.hpp" // SCALE / kScale (Appendix A row A-1)
#include "core/enums.hpp"

namespace harrowmere::data
{

// Per-TerritoryType blueprint used by worldgen (task 1.5).
struct TerritoryTypeTemplate
{
    TerritoryType type          = TerritoryType::VILLAGE;
    std::uint32_t populationMin = 0; // inclusive
    std::uint32_t populationMax = 0; // inclusive; min <= max
    std::string lawSet;              // key into LawSetRegistry
    bool safe  = false;
    bool arena = false; // city: arenaId != 0 (task 2.7)
};

struct TerritoryDataError
{
    std::string file;
    std::string jsonPointer;
    std::string message;
};

// The whole file, parsed and validated. Storage uses std::map for
// deterministic iteration (R7).
class TerritoryTemplateSet
{
public:
    TerritoryTemplateSet() = default;

    // True iff every TerritoryType has a blueprint.
    bool complete() const noexcept;

    // O(1) lookup by TerritoryType (array-indexed).
    const TerritoryTypeTemplate *byType(TerritoryType t) const noexcept;
    TerritoryTypeTemplate *byType(TerritoryType t) noexcept;

    // Tree multipliers.
    std::uint32_t villagesPerCity() const noexcept
    {
        return villages_per_city_;
    }
    std::uint32_t citiesPerKingdom() const noexcept
    {
        return cities_per_kingdom_;
    }
    std::uint32_t kingdomsPerEmpire() const noexcept
    {
        return kingdoms_per_empire_;
    }

    // Region flag table (string -> bit). Looked up by string because flags
    // are content-defined; the bit values are data (design rule:
    // "Flags ... defined in data files").
    const std::map<std::string, std::uint32_t> &regionFlags() const noexcept
    {
        return region_flags_;
    }
    std::optional<std::uint32_t> flagBit(std::string_view name) const noexcept;

private:
    std::array<TerritoryTypeTemplate, static_cast<std::size_t>(TerritoryType::COUNT)> types_{};
    std::array<bool, static_cast<std::size_t>(TerritoryType::COUNT)> present_{};
    std::uint32_t villages_per_city_   = 4;
    std::uint32_t cities_per_kingdom_  = 3;
    std::uint32_t kingdoms_per_empire_ = 4;
    std::map<std::string, std::uint32_t> region_flags_;
    friend bool
    loadTerritoryTemplates(const std::string &path, TerritoryTemplateSet &out, std::vector<TerritoryDataError> &errors);
};

// Loads `territory_templates.json` into `out`.
//
// Validation rules (phase-1-walk.md card 1.2):
//   * `types` block has an entry for every TerritoryType (VILLAGE / CITY /
//     KINGDOM / EMPIRE).
//   * `population` is [min, max] with min <= max.
//   * `lawSet` is a non-empty string (cross-checked against LawSetRegistry
//     by task 1.3's DataRegistry).
//   * `villages_per_city` / `cities_per_kingdom` / `kingdoms_per_empire`
//     are positive integers (worldgen divides by these — guard against
//     zero, R2).
//   * `regionFlags` is map<string, int>; bit values are positive powers
//     of two (1, 2, 4, 8, ...). Non-power-of-two values are an error.
bool loadTerritoryTemplates(
    const std::string &path, TerritoryTemplateSet &out, std::vector<TerritoryDataError> &errors
);

// Convenience: looks up `TerritoryType` from its string tag.
std::optional<TerritoryType> territoryTypeFromKey(std::string_view tag);
std::string_view territoryTypeKey(TerritoryType t);

} // namespace harrowmere::data
