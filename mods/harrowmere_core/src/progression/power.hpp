// power.hpp — the cached power pipeline and the matchmaking score (task 2.5).
//
// Design reference §Power Scores, verbatim:
//     getCurrentPower(player):
//         if not player.powerDirty: return player.cachedPower
//         total = getGearScore(player)
//         for statId in STATS:  total += statWeight[statId]  * getStatEffectiveness(player, statId)  / SCALE
//         for skillId in SKILLS: total += skillWeight[skillId] * getSkillEffectiveness(player, skillId) / SCALE
//         player.cachedPower = total; player.peakPower = max(player.peakPower, total)
//         player.powerDirty = false; return total
//     getPotentialPower(player): return isqrt(player.progression.totalXp)
//     getMatchmakingScore(player): return (8 * player.peakPower + 2 * potential) / 10
//
// markPowerDirty (task 2.3's groundwork, kept here unchanged): every mutation
// that enters the formula fires it — the design reference's own list, "Call
// markPowerDirty(player) on stat or skill purchase, respec, and equip or
// unequip" — plus the XP-granting paths the cards pin (3.3's onNpcKill gain,
// 2.4's consumeItem grant). Level changes do NOT dirty the cache: "level
// changes do not enter the formula" (TestPower_LevelUpDoesNotDirtyCache pins
// the negative). The call-site audit is 2.5's deliverable
// (TestPower_DirtyCallSites_Complete).
//
// Deliberately NOT here: a const overload of getCurrentPower. The card's
// pitfall — a const overload silently skips cache updates — stands; callers
// without a mutable character use getPotentialPower or copy.
//
// Registry injection (the repo's one deliberate signature adaptation, the
// species.hpp reading): the docs' pseudocode reads global weight/species
// tables; Harrowmere injects the DataRegistry (species multipliers +
// item rows). The gear term additionally resolves item INSTANCE ids through
// the ItemInstanceRegistry (card 2.4: equippedItems stores instance ids) —
// that registry is the one added parameter beyond the card's pinned
// signature (DEVIATIONS.md carries the note).

#pragma once

#include <cstdint>

#include "character/character.hpp"
#include "core/constants.hpp" // SCALE, A-20/A-21 weight constants
#include "core/enums.hpp"     // StatType, SkillType (closed lists)
#include "core/math.hpp"      // isqrt64 (task 0.2)
#include "items/inventory.hpp"

namespace harrowmere::data
{
class DataRegistry; // registry.hpp (task 1.3)
} // namespace harrowmere::data

namespace harrowmere
{

// Flag the cached power for recompute. Cheap by design: one store, one bool —
// fire it on every qualifying mutation, never "optimistically" skip it.
inline void markPowerDirty(
    Character &c
) noexcept
{
    c.powerDirty = true;
}

// getCurrentPower — the cached pipeline, verbatim (takes a NON-const
// Character: the doc's own pseudocode mutates the cache fields; the 2.5
// Done-when pins that this signature is accepted precisely because of it).
// A dirty flag computes gear + weighted stat/skill effectiveness, updates
// cachedPower/peakPower/powerDirty, and returns the fresh total; a clean
// flag returns the cache untouched.
std::uint64_t
getCurrentPower(Character &c, const data::DataRegistry &data, const items::ItemInstanceRegistry &instances);

// getPotentialPower — isqrt(totalXp), design reference verbatim. Tamper-proof
// by construction: totalXp never decreases, so neither does the potential.
[[nodiscard]] inline std::uint64_t getPotentialPower(
    const Character &c
) noexcept
{
    return isqrt64(c.progression.totalXp);
}

// getMatchmakingScore — the 8:2 blend of the peak watermark and the
// potential (design reference §Matchmaking verbatim): "peakPower never
// decreases, so unequipping gear cannot drop a player into easier brackets;
// getPotentialPower is already tamper-proof because totalXp never
// decreases". The card's DataRegistry parameter rides along unchanged —
// v1's formula reads only the character, but the anti-sandbagging score is
// exactly where data-driven tuning would land first.
[[nodiscard]] std::uint64_t getMatchmakingScore(const Character &c, const data::DataRegistry &data);

} // namespace harrowmere
