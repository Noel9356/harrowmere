// crime.hpp — CrimeType data loader (Task 1.2).
//
// Loads `mods/harrowmere_core/data/crime_data.json` into a fixed-shape
// in-memory table indexed by CrimeType. This is the *content* side of the
// justice system: severity drives `bountyFor` (task 4.2 / Appendix A row
// A-73 region); `lethalForce` is copied onto warrants at issuance
// (design reference `issueWarrant`); `notorietyGain` is added on conviction
// (design reference `convict`).
//
// Pinned v1 (phase-1-walk.md card 1.2):
//   * MURDER lethalForce = true; all other crimes = false.
//   * One entry per CrimeType (8 entries total — the closed list from
//     task 1.1's enums.hpp). Extending the enum requires a deviation row.
//
// Constants: severity / notorietyGain / defaultThreshold are 0..SCALE on the
// fixed-point grid (SCALE = 10'000, Appendix A row A-1; defined in
// `core/constants.hpp` owned by task 0.2). This file does NOT define SCALE —
// it includes the value via the constants header when the rest of the tree
// lands; for now it uses the literal 10'000u directly with a comment.
//
// Dependencies (provided by other tasks; stubs live in this drop only so the
// build is self-contained):
//   * `core/enums.hpp`        — task 1.1 (stub in this drop; replaced verbatim)
//   * `core/result.hpp`        — task 0.2 (Result<T,E> sum type)
//   * `data/json.hpp`          — task 0.4 (loader + schema DSL wrapper around
//                                 nlohmann_json v3.11.3). Code inside
//                                 `src/data/` MAY include nlohmann directly
//                                 (common.md §4.3 / task 0.4: "game code never
//                                 includes nlohmann headers directly outside
//                                 src/data/"). This file does so.
//
// All function names match design-reference pseudocode where the docs name a
// function (common.md §3.3 — the doc↔code diff stays trivial).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/constants.hpp"  // SCALE / kScale (Appendix A row A-1)
#include "core/enums.hpp"

namespace harrowmere::data {

// (SCALE = 10'000 lives in `harrowmere::SCALE` from core/constants.hpp.
// `harrowmere::kScale` is its u32 alias used throughout the data layer.)

// One content row per CrimeType. Mirrors `crime_data.json`'s shape exactly.
//
// Field semantics (design reference §Laws / §Crime and Evidence):
//   * lethalForce        — copied onto Warrant at issuance (`issueWarrant`).
//                          Pinned v1: MURDER true, all others false.
//   * severity           — drives `bountyFor` (task 4.2); 0..SCALE.
//   * notorietyGain      — added on `convict()` (task 4.3); 0..SCALE.
//   * defaultThreshold   — caseThreshold fallback when a LawSet does not
//                          pin a per-crime threshold; 0..SCALE.
struct CrimeData {
    CrimeType        type             = CrimeType::MURDER;
    bool             lethalForce      = false;
    std::uint32_t    severity         = 0;   // 0..SCALE
    std::uint32_t    notorietyGain    = 0;   // 0..SCALE
    std::uint32_t    defaultThreshold = 0;   // 0..SCALE
};

// Validation error — file + JSON pointer + human message. Same shape as
// task 0.4's `DataError`; reproduced here so the loader can construct one
// without depending on task 0.4's full schema DSL. When task 0.4 lands,
// this struct is replaced by the canonical one (the field set is identical).
struct CrimeDataError {
    std::string file;         // path to the JSON file being loaded
    std::string jsonPointer;  // RFC-6901 JSON pointer into the offending value
    std::string message;      // human-readable diagnostic
};

// Owned by task 1.2: the single source of truth for crime content.
//
// The table is a `std::array` indexed by CrimeType (R9 — every game enum
// that backs a sized array ends in a `COUNT` sentinel exactly as the docs
// do). Lookups are O(1) and branch-free — `caseScore` and `issueWarrant`
// hot paths never iterate a map.
class CrimeTable {
public:
    static constexpr std::size_t kSize =
        static_cast<std::size_t>(CrimeType::COUNT);

    // Construct an empty table; rows are filled by `loadCrimeData`.
    CrimeTable() = default;

    // True iff every CrimeType has a populated row.
    bool complete() const noexcept;

    // O(1) lookup by CrimeType. Returns nullptr if the row is missing —
    // callers (caseScore, issueWarrant, convict) treat a missing row as a
    // hard invariant violation and assert / abort.
    const CrimeData* row(CrimeType t) const noexcept;
    CrimeData*       row(CrimeType t)       noexcept;

    // Iteration helpers (deterministic: CrimeType enum order, R7).
    const std::array<CrimeData, kSize>& rows() const noexcept { return rows_; }

private:
    std::array<CrimeData, kSize> rows_{};
    std::array<bool, kSize>      present_{};
    friend bool loadCrimeData(const std::string& path,
                              CrimeTable& out,
                              std::vector<CrimeDataError>& errors);
};

// Loads `crime_data.json` into `out`. The file MUST contain exactly one row
// per CrimeType (no duplicates, no missing entries). On any schema violation,
// duplicate, unknown enum tag, or out-of-range value, the loader appends a
// `CrimeDataError` to `errors` and returns false; `out` is left in a
// partial state and MUST NOT be consumed by the rest of the game.
//
// The `file` field of every error is set to `path` so callers do not need
// to thread it through.
//
// Behaviour pinned by phase-1-walk.md card 1.2 / common.md §3.6:
//   * nlohmann's silent-overwrite of duplicate object keys is REJECTED
//     (strict parse — common.md task 0.4 pitfalls).
//   * Numeric fields declared integer reject fractional values; the docs'
//     "scaled and casted into integer" rule applies only to fields the
//     schema marks scaled. crime_data.json has no scaled fields — every
//     number is an integer on the SCALE grid.
bool loadCrimeData(const std::string& path,
                   CrimeTable& out,
                   std::vector<CrimeDataError>& errors);

// Convenience: looks up `CrimeType` from its string tag (e.g. "MURDER").
// Returns std::nullopt for unknown tags — used by worldgen (task 1.5) to
// cross-reference data files keyed by string.
std::optional<CrimeType> crimeTypeFromKey(std::string_view tag);

// String spelling of a CrimeType (matches the JSON tags exactly).
std::string_view crimeTypeKey(CrimeType t);

} // namespace harrowmere::data
