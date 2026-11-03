// combat/damage.hpp — Task 3.2: one damage pipeline for every damage source
// (melee, arrow, spell, exposure, fall) with the doc-mandated hooks in the pinned
// order: consent/sapience/evidence, event contribution, death.
#pragma once
#include <cstdint>

#include "core/enums.hpp"  // DamageType (1.1's closed list)
#include "core/result.hpp" // Result<void, DamageError>
#include "core/types.hpp"

namespace harrowmere::data
{
class DataRegistry; // registry.hpp (task 1.3)
}

namespace harrowmere
{

struct WorldContext;
struct Character;

struct DamageEvent
{
    characterId source; // 0 = environment (exposure, fall)
    characterId target;
    std::uint64_t amount;
    DamageType type;
};

// Rule-level refusals of the damage pipeline (§3.4: return values, never
// exceptions). The 0.2 Result<void, E> template is the tree-wide carrier.
enum class DamageError
{
    InvalidTarget, // target id 0 / no such character (the pipeline's guard)
    Invulnerable,  // D-INVULN-1 (design reference validateActionAmount guard)
    COUNT
};
using DamageResult = Result<void, DamageError>;

// The pinned pipeline (order is load-bearing — 3.2's Pitfalls):
//   0. invulnerable check (D-INVULN-1)                     -> reject
//   3. species damageTakenMultiplier BEFORE clamping (2.2) -> goo beasts
//   1. clamping to currentHp (u64, never negative)
//   2. both sides: lastCombatTime = now (IN_COMBAT derivation, 2.1)
//   4. justice hook: player source && sapient(target) && !consentsToPvp &&
//      !warrantCovers -> ctx.justice->noteAssault           (null until 4.1)
//   5. event hook: either side in an event -> ctx.events->onCombatDamage (null until 5.3)
//   6. death: currentHp == 0 -> die(...) immediately       (3.3; D-TICK-1: inside step 10)
DamageResult applyDamage(DamageEvent ev, WorldContext &ctx);

// A live warrant on the VICTIM in the victim's territory licenses enforcement
// damage (jurisdiction-bound, 4.2's rule); null-false until 4.2 lands.
bool warrantCovers(const Character &target, const Character &attacker, territoryId territory, const WorldContext &ctx);

} // namespace harrowmere
