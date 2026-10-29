// species.hpp — typed view over data/species.json.
//
// Loaded by SpeciesLoader (built on task 0.4's JSON layer). The runtime
// form expands the JSON statMultiplier map to a std::array<u32, StatType::COUNT>
// so the hot path is array-indexed (task 1.1 pitfall: "Don't store
// statMultiplier as a map at runtime"). Missing JSON keys default to SCALE
// (1.0x multiplier).
//
// Species ids are DATA-ASSIGNED (design reference §Conventions); only id 0
// (player) is a hardcoded constant. Worldgen (1.5) references species by
// `key`, never by literal id, except for player=0 (task 1.1 pitfall).
//
// Design reference (common.md Appendix F.2 §Species):
//   Species data:
//       u16                  id
//       bool                 sapient
//       u32                  lifespan
//       u32[StatType::COUNT] statMultiplier
//       string               appearance
//
// Task 1.1 extension fields (pinned): darkvision, xenophobiaBias,
// infectableVampire. Appendix B.1 also pins damageTakenMultiplier (for
// goo beasts' "neutralise attack" — modeled as 0.25). All four are loaded
// here so 1.5/3.x can read them without re-touching the loader.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/enums.hpp"
#include "core/types.hpp"  // speciesId alias (task 0.2)

namespace harrowmere::data {

// Forward declaration from task 0.4 (JSON data layer). The full type lives
// in src/data/schema.hpp and is { std::string file; std::string jsonPointer;
// std::string message; }. Forward-declared here so this header does not
// pull the JSON layer into every includer.
struct DataError;

// SpeciesData — runtime form of one species.json row.
//
// All scaled fields use the SCALE grid (§3.2). statMultiplier values are
// stored as the integer representation of (float × SCALE): a 2.0x multiplier
// is 20000, 1.0x is 10000 (== SCALE). damageTakenMultiplier follows the
// same convention; 0.25 becomes 2500.
struct SpeciesData {
    speciesId    id = 0;
    std::string  key;
    bool         sapient = false;
    std::uint32_t lifespan_years = 0;  // design reference "u32 lifespan"

    // Expanded from the JSON map at load time. Missing keys → SCALE (1.0x).
    // Hot-path access is array-indexed: statMultiplier[StatType::STRENGTH].
    std::array<std::uint32_t, static_cast<std::size_t>(StatType::COUNT)> statMultiplier{};

    std::string  appearance;
    bool         darkvision = false;
    std::uint32_t xenophobiaBias = 0;          // 0..SCALE
    bool         infectableVampire = false;     // species data hook (task 1.1)
    std::uint32_t damageTakenMultiplier = 0;    // 0..SCALE (1.0 = SCALE; 0.25 = 2500)

    // Convenience: returns the scaled stat multiplier for a stat, defaulting
    // to SCALE if the index is out of range (defensive — callers should be
    // array-indexed already).
    std::uint32_t multiplier(StatType s) const {
        const auto i = static_cast<std::size_t>(s);
        return i < statMultiplier.size() ? statMultiplier[i] : 10000u;  // SCALE
    }
};

// SpeciesLoader — parses + validates data/species.json.
//
// Validation rules (hard errors, reported via `errors`):
//   * File missing or JSON-malformed (delegated to 0.4's loadJsonFile).
//   * Duplicate id, duplicate key.
//   * id 0 not "player" (the one hardcoded constant — design reference).
//   * id out of [0, 65535] (speciesId is u16 per design reference §Conventions).
//   * Missing required field (id, key, sapient, lifespan_years, statMultiplier,
//     appearance).
//   * statMultiplier entry < 0 (negative multiplier makes no sense).
//   * xenophobiaBias / damageTakenMultiplier outside [0, SCALE*10] (loose
//     upper bound — the loader clamps to SCALE on read for damageTaken).
//
// Referential checks (species referenced by NPC seed, item, etc.) land in
// 1.3's DataRegistry cross-ref pass — this loader only validates the file
// in isolation.
class SpeciesLoader {
public:
    SpeciesLoader() = default;
    ~SpeciesLoader() = default;
    SpeciesLoader(const SpeciesLoader&) = delete;
    SpeciesLoader& operator=(const SpeciesLoader&) = delete;

    // Parses + validates species.json. On success, populates the internal
    // table and returns true. On any hard error, populates `errors` with
    // file + jsonPointer + message entries (0.4's DataError shape) and
    // returns false; the internal table is left empty.
    bool load(const std::string& jsonPath, std::vector<DataError>& errors);

    // O(1) lookup by id (ids are dense and small). Returns nullptr if the
    // id is unknown — callers MUST null-check (task 1.1 pitfall: worldgen
    // references by key, but other systems reference by id; an unknown id
    // is a data bug, not a runtime crash).
    // Test pin: TestDataLoader_RejectsUnknownSpeciesId.
    const SpeciesData* speciesById(speciesId id) const;

    // O(n) lookup by key. Returns nullptr if not found. Called at data-load
    // time (worldgen, cross-ref), never on the hot path.
    const SpeciesData* speciesByKey(const std::string& key) const;

    bool        empty() const noexcept { return species_.empty(); }
    std::size_t size()  const noexcept { return species_.size(); }
    const std::vector<SpeciesData>& all() const noexcept { return species_; }

private:
    std::vector<SpeciesData> species_;
};

} // namespace harrowmere::data
