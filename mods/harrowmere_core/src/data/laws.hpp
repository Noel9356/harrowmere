// laws.hpp — LawSet loader (Task 1.2).
//
// Loads `mods/harrowmere_core/data/laws.json` into typed, validated LawSet
// records. A LawSet is the *content* side of the justice system: it tells
// the simulation what conviction threshold a crime must clear in a given
// territory, how social-class corruption damps the case score, which
// species×crime pairs are hard-exempted, and which suspicion triggers apply.
//
// Semantics are pinned by the design reference (small.md §Death + justice.md
// §Laws / §Crime and Evidence). Reproduced verbatim here so this header is
// self-contained:
//
//   * `suppression`     — case-score damping per social class per crime,
//                         0..SCALE. SCALE = NONE (no damping). Keyed by
//                         SocialClassId (D-CORRUPT-1 — corruption mechanic).
//                         SOFT: damped cases can still convict on
//                         overwhelming evidence.
//   * `exemptions`      — speciesId → crimeType → bool. HARD GATE: no
//                         evidence object is ever recorded. Checked BEFORE
//                         any evidence exists (recordCrime early-returns).
//   * `cultureBias`     — speciesId → i32. Feeds 3.9's affinity base
//                         (negative = xenophobia). Signed because the doc
//                         ships negative biases (e.g. -6000 for beastkins
//                         in xenophobic villages).
//   * `suspicion`       — overrides add/replace the global suspicion table
//                         for the territory using this LawSet. Empty list
//                         means "inherit defaults from suspicion.json".
//   * `safe`            — true for `safe_zone_laws`. Where safe=true the
//                         engine's ward-report logic still applies but
//                         suppression and titles are ignored, and
//                         non-consensual PvP doubles the killer's debt
//                         and reputation loss (design reference §Safe Zones).
//
// Keyed-by-speciesId maps use `speciesId` (u16) keys, NOT species `key`
// strings — that is the design reference's own convention. The loader
// validates every species id against the SpeciesData registry (task 1.1)
// when wired into DataRegistry (task 1.3); here it only checks the JSON
// shape.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/constants.hpp" // SCALE / kScale (Appendix A row A-1)
#include "core/enums.hpp"

namespace harrowmere::data
{

// Bit-set of suspicion-context masks. Mirror of the masks in
// suspicion.json: SETTLEMENT, NIGHT, PRIVATE_PROPERTY, MARKET.
// Bits are pinned — the design reference lists exactly four (extend by
// deviation row only, common.md §1.4).
enum class SuspicionMask : std::uint32_t
{
    NONE             = 0u,
    SETTLEMENT       = 1u << 0,
    NIGHT            = 1u << 1,
    PRIVATE_PROPERTY = 1u << 2,
    MARKET           = 1u << 3,
};

// Bitwise operators for SuspicionMask — the data layer composes mask
// sets via `SuspicionMask::SETTLEMENT | SuspicionMask::MARKET`. Enum
// class does not provide these by default; defining them keeps the call
// sites readable while staying type-safe.
inline constexpr SuspicionMask operator|(
    SuspicionMask a, SuspicionMask b
) noexcept
{
    return static_cast<SuspicionMask>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
inline constexpr SuspicionMask &operator|=(
    SuspicionMask &a, SuspicionMask b
) noexcept
{
    a = a | b;
    return a;
}
inline constexpr std::uint32_t toBits(
    SuspicionMask m
) noexcept
{
    return static_cast<std::uint32_t>(m);
}

// One row of the suspicion trigger table. Mirrors design reference
// §Suspicion and Trust's `SuspicionTrigger` struct verbatim.
struct SuspicionTrigger
{
    SuspicionAct act     = SuspicionAct::CROUCH;
    std::uint32_t mask   = 0u; // OR of SuspicionMask bits
    std::uint32_t weight = 0u; // 0..SCALE
};

// Errors share the same shape as CrimeDataError. When task 0.4 lands, the
// canonical `DataError` replaces all three (file/jsonPointer/message).
struct LawDataError
{
    std::string file;
    std::string jsonPointer;
    std::string message;
};

// The Laws struct verbatim from design reference §Laws, plus the
// `cultureBias` map the docs' `xenophobic_village` example ships and a
// `safe` flag pinned by §Safe Zones.
//
// Storage choices:
//   * `std::map` everywhere a doc map appears — R7 (determinism rule):
//     iteration order is well-defined (key ascending) so two runs from the
//     same seed produce identical hashes.
//   * `std::uint32_t` for SCALE-grid values (0..SCALE = 0..10'000 — fits
//     comfortably in u32).
//   * `std::int32_t` for `cultureBias` (signed — biases can be negative).
//
// The `suppression` map uses SocialClassId keys (D-CORRUPT-1) — NOT the
// RPG "class" concept from classes.json. The docs themselves call this
// field `clsid`; it is the social-class enum from design reference
// §Territories.
struct LawSet
{
    std::string key; // e.g. "default", "xenophobic_village"

    // convictionThreshold[CrimeType] — report weight needed to convict.
    // Stored as an array indexed by CrimeType for O(1) lookup in caseScore;
    // populated from the JSON map (which the loader validates covers every
    // CrimeType). A value of 0 means "use CrimeData.defaultThreshold".
    std::array<std::uint32_t, static_cast<std::size_t>(CrimeType::COUNT)> convictionThreshold{};

    // suppression[SocialClassId][CrimeType] = damping factor 0..SCALE.
    // Missing entry = SCALE (no damping). The default LawSet has an empty
    // map — every class damps at SCALE.
    std::map<SocialClassId, std::array<std::uint32_t, static_cast<std::size_t>(CrimeType::COUNT)>> suppression;

    // exemptions[speciesId][CrimeType] = true. Missing entry = false
    // (no exemption). Hard gate: recordCrime returns before constructing
    // an Evidence object.
    std::map<std::uint16_t, std::array<bool, static_cast<std::size_t>(CrimeType::COUNT)>> exemptions;

    // cultureBias[speciesId] — signed. Feeds 3.9's affinity base.
    std::map<std::uint16_t, std::int32_t> cultureBias;

    // suspicion overrides for this LawSet. Empty = inherit suspicion.json
    // verbatim. Non-empty = REPLACE the global table (matches the doc's
    // "overrides" semantics; task 1.3's DataRegistry resolves the merge).
    std::vector<SuspicionTrigger> suspicion;

    // safe flag — true only for the `safe_zone_laws` set. Where safe=true:
    //   * ward reports auto-attach with WARD_WEIGHT (design reference)
    //   * suppression and titles are ignored (all lives equal)
    //   * non-consensual PvP doubles killer's debt and reputation loss
    bool safe = false;
};

// Owned by task 1.2: the single source of truth for LawSet content.
//
// LawSets are looked up by key string ("default", "tolerant_city", ...).
// Uses std::map for deterministic iteration (R7).
class LawSetRegistry
{
public:
    LawSetRegistry() = default;

    // True iff the registry contains the `default` set (DEFAULT_LAWS in
    // the design reference). Other sets may be added by mods.
    bool hasDefaults() const noexcept;

    // O(log n) lookup by key. Returns nullptr if absent.
    const LawSet *byKey(std::string_view key) const noexcept;
    LawSet *byKey(std::string_view key) noexcept;

    // Deterministic iteration (key-ascending, R7).
    const std::map<std::string, LawSet> &sets() const noexcept
    {
        return sets_;
    }

private:
    std::map<std::string, LawSet> sets_;
    friend bool loadLawSets(const std::string &path, LawSetRegistry &out, std::vector<LawDataError> &errors);
};

// Loads `laws.json` into `out`. The file MUST be a JSON object with a
// `sets` array; every set MUST have all eight conviction thresholds (the
// closed CrimeType list from task 1.1). Suppression / exemptions /
// cultureBias / suspicion / safe are optional. On any schema violation
// the loader appends a `LawDataError` and returns false; `out` is left
// partial and MUST NOT be consumed.
//
// Pinned validations (phase-1-walk.md card 1.2):
//   * Every conviction threshold is in [0, SCALE].
//   * Every suppression value is in [0, SCALE]; missing entries implicitly
//     equal SCALE.
//   * Every exemption key is a valid speciesId (we accept any u16 here;
//     task 1.3's DataRegistry cross-references against SpeciesData).
//   * Every `cultureBias` value is in [-SCALE, SCALE] (signed).
//   * Every suspicion act tag is one of the six pinned SuspicionAct
//     values; every mask is one of the four pinned SuspicionMask names.
//   * The file MUST contain at least the `default` set (DEFAULT_LAWS).
bool loadLawSets(const std::string &path, LawSetRegistry &out, std::vector<LawDataError> &errors);

// Convenience: parses a comma-or-array mask spec into a SuspicionMask
// bit-set. Used by the loader; exported for tests + task 4.5.
std::uint32_t parseSuspicionMasks(const std::vector<std::string> &tags);

// Loads `suspicion.json` (the global SuspicionTrigger table) into `out`.
// The file MUST be a JSON array of rows; every row's `act` MUST be one of
// the six pinned SuspicionAct tags, every `masks` entry one of the four
// pinned SuspicionMask tags, and `weight` in [0, SCALE].
//
// The global table is the default for every territory whose LawSet has an
// empty `suspicion` block; per-LawSet overrides (loaded inline by
// loadLawSets) REPLACE the global table for that territory.
bool loadSuspicionTriggers(
    const std::string &path, std::vector<SuspicionTrigger> &out, std::vector<LawDataError> &errors
);

// Convenience: looks up `SocialClassId` from its string tag
// ("NONE", "COMMONER", "NOBLE", "ROYAL").
std::optional<SocialClassId> socialClassFromKey(std::string_view tag);

// Convenience: looks up `SuspicionAct` from its string tag.
std::optional<SuspicionAct> suspicionActFromKey(std::string_view tag);

// Convenience: looks up `SuspicionMask` bit from its string tag.
std::optional<SuspicionMask> suspicionMaskFromKey(std::string_view tag);

} // namespace harrowmere::data
