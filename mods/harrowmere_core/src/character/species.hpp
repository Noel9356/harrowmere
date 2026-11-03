// species.hpp — species application at its three documented touch points
// (task 2.2): stat multipliers, sapience checks, and social-bias reads.
//
// The design reference's rule set, verbatim:
//   * getStatEffectiveness multiplies the soft-capped stat effectiveness by
//     the species row's statMultiplier (character.md §Species).
//   * isSapient reads the species row's sapient flag (beastkins true; the
//     shipped wolf/goo_beast rows false — "hunting deer creates no crime").
//   * effectiveLifespan grows the species lifespan with level (A-5, the doc
//     line "lifespan increases as level increases": up to 2x at level 100,
//     linear).
//   * The doc's species-conflict flavor is DATA, not code: kingdoms'
//     xenophobic law sets + NPC bias reads (3.9) produce "kill beastkins on
//     sight" through justice exemptions and militia decisions — no
//     hardcoded species list anywhere in this file (grep contract, card
//     Done-when: "No species switch statements exist outside data-driven
//     paths").
//
// Data ownership: every species lookup goes through the DataRegistry
// (task 1.3). The docs' pseudocode indexes a global `speciesData[...]`
// table; Harrowmere's registry is injected, so the registry is the first
// parameter of every function here — that is the one deliberate signature
// adaptation (engineering convention, §1.4: the plan owns how code is
// organized).
//
// Vampires are "any species infected by vampiric disease" — in v1 vampires
// are just a species row (species.json id 5; infectableVampire is irrelevant
// for them); the infection system is Appendix E. Goo beasts' "neutralise
// attack" is the pinned damageTakenMultiplier field (0.25 in species.json),
// consumed by 3.2 — kept data, nothing here reads it.

#pragma once

#include <cstdint>

#include "character/character.hpp"     // Character
#include "core/constants.hpp"          // STAT_THRESHOLD (A-12)
#include "core/enums.hpp"              // StatType
#include "core/types.hpp"              // speciesId
#include "progression/progression.hpp" // getEffectiveness (2.2 depends on 2.3's effectiveness function)

namespace harrowmere::data
{
class DataRegistry; // registry.hpp (task 1.3)
}

namespace harrowmere
{

// isSapient — design reference verbatim: speciesData[speciesId].sapient.
// The registry's zero-row release fallback makes an unknown species id
// deterministically non-sapient (an unknown id is a data bug, 1.3's rule).
bool isSapient(const data::DataRegistry &data, speciesId species);

// getStatEffectiveness — design reference §Species, verbatim:
//   effectiveness = getEffectiveness(point, STAT_THRESHOLD)
//   return effectiveness * speciesData[c.species].statMultiplier[st] / SCALE
// Multiply BEFORE divide (the doc's form); the v1 caps bind the point at 30
// so the product cannot overflow u64. Elves read 4x INTELLIGENCE from
// species.json (the doc's own table row), never from code.
std::uint64_t getStatEffectiveness(const data::DataRegistry &data, const Character &c, StatType stat);

// effectiveLifespan — A-5: lifespan * (SCALE + level * 100) / SCALE, the
// doc's "lifespan increases as level increases" (linear, 2x at level 100).
// The result is in YEARS (the species row's lifespan_years unit); task 3.4
// compares it against NpcState::ageYears().
std::uint64_t effectiveLifespan(const data::DataRegistry &data, const Character &c);

// speciesBiasToward — the data-driven bias core (task 2.2's spec line:
// "xenophobiaBias + territory culture bias (3.9)"): the species row's
// xenophobiaBias term plus the territory's culture-bias term toward the
// other species (laws.json cultureBias, resolved by the caller's territory).
// Both terms are SCALE-grid; the sum is the SCALE-grid bias 3.9's perception
// consumes. See the DEVIATIONS.md row for the signature note: the card's
// NpcState-taking form joins task 3.4's NpcState (2.2's dependency row
// excludes 3.4); 3.9 wraps this core with that adapter.
std::int64_t speciesBiasToward(std::uint64_t selfXenophobiaBias, std::int64_t territoryCultureBias);

} // namespace harrowmere
