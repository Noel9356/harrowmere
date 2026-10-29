// roles.hpp — NPC role loader (Task 1.2).
//
// Loads `mods/harrowmere_core/data/roles.json` into typed, validated
// RoleData records keyed by string tag. Roles drive three doc systems:
//
//   * `baseReportWeight` is `ROLE_BASE_WEIGHT[role]` in `witnessReports`
//     (design reference §Crime and Evidence) — a witness's report weight
//     is `max(0, ROLE_BASE_WEIGHT[w.role] * w.integrity / SCALE *
//     (SCALE - affinity(w, culpritId)) / SCALE)`.
//   * `critical = true` marks roles whose death triggers
//     `onCriticalNpcDeath` (design reference §NPCs) — the per-territory
//     delayed replacement hook (task 3.6).
//   * `cannotTestify = true` (set on the `child` role) gates witness
//     testimony in 3.9; the design rule is "children never testify".
//
// Pinned v1 roles (phase-1-walk.md card 1.2): guard (critical, 9000),
// captain (critical, 10000), vendor (7000), farmer (4000), laborer (4000),
// noble (6000), militia_veteran (8000), healer (6000), child
// (cannotTestify). The closed list lives in the data file; mods may add
// more (task 7.5).
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/constants.hpp"  // SCALE / kScale (Appendix A row A-1)
#include "core/enums.hpp"

namespace harrowmere::data {

// Mirrors roles.json's row shape. Field names match the JSON keys exactly.
struct RoleData {
    std::string  key;                       // unique string tag ("guard", "captain", ...)
    std::uint32_t baseReportWeight = 0;     // 0..SCALE; ROLE_BASE_WEIGHT in witnessReports
    std::uint32_t integrityMin     = 0;     // 0..SCALE
    std::uint32_t integrityMax     = 0;     // 0..SCALE; lo <= hi
    bool          critical          = false; // triggers onCriticalNpcDeath
    std::uint32_t priority          = 0;     // tie-breaker for replacement selection
    std::vector<std::string> duties;        // role's duties ("patrol", "trade", ...)
    bool          cannotTestify     = false; // child role: cannot give witness reports
};

struct RoleDataError {
    std::string file;
    std::string jsonPointer;
    std::string message;
};

// Owned by task 1.2: the single source of truth for role content.
//
// Lookups are by string key (roles.json's `key` field). Uses std::map for
// deterministic iteration (R7).
class RoleRegistry {
public:
    RoleRegistry() = default;

    bool has(std::string_view key) const noexcept;
    const RoleData* byKey(std::string_view key) const noexcept;
    RoleData*       byKey(std::string_view key)       noexcept;

    // True iff the registry contains every pinned v1 role (phase-1-walk.md
    // card 1.2's nine roles). The starter file ships all nine; mods may
    // add more.
    bool hasPinnedV1Set() const noexcept;

    // Deterministic iteration (key-ascending, R7).
    const std::map<std::string, RoleData>& roles() const noexcept { return roles_; }

private:
    std::map<std::string, RoleData> roles_;
    friend bool loadRoles(const std::string& path,
                          RoleRegistry& out,
                          std::vector<RoleDataError>& errors);
};

// Loads `roles.json` into `out`. The file MUST be a JSON array; every
// entry MUST have `key`, `baseReportWeight`, `integrityRange`,
// `critical`. `duties`, `priority`, `cannotTestify` are optional.
//
// Validation rules (phase-1-walk.md card 1.2):
//   * `baseReportWeight` in [0, SCALE].
//   * `integrityRange` is a 2-element array [lo, hi] with lo <= hi, both
//     in [0, SCALE].
//   * `critical` is a boolean.
//   * `key` is unique within the file.
bool loadRoles(const std::string& path,
               RoleRegistry& out,
               std::vector<RoleDataError>& errors);

} // namespace harrowmere::data
