// progression.hpp — the XP economy engine (task 2.3).
//
// The design reference's (progression.md) math, line for line:
// precomputed cost/cap tables, the soft-cap effectiveness function, the
// literal refund (D-REFUND-1), peaceful/respec gating, level/stat/skill
// purchases with the D-LORE-1 mana rule, and skill requirements (§Skills:
// "Skill-buying requirements can be stats, other skills, or actions").
//
// Naming (§3.3): the docs' function names ARE the code's function names —
// precomputeCosts, precomputeCaps, getEffectiveness, getRefund, isPeaceful,
// canRespec, getStatEffectiveness (2.2's file), getSkillEffectiveness,
// canRespecStat, canRespecSkill.
//
// Determinism notes:
//   * The cost/cap tables are constexpr (multiply BEFORE divide — the doc's
//     form; the divide-first misreading collapses the 1.15 growth to
//     integers within a few entries). The vector accessors below hand out
//     the same values as const refs to function-local statics (magic
//     statics: thread-safe, initialized once, identical every run).
//   * availableXp is i64: every subtraction flows through applyXpDelta
//     (R4's -MAX_XP floor) and every cost is widened SIGNED before the
//     negate (R1 — never assign availableXp from a u64 expression).
//   * Purchases draw RNG only through D-LORE-1's mana rule, from the pinned
//     "combat" stream (0.3) — nothing else in this file rolls.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "character/character.hpp"
#include "core/constants.hpp"     // SCALE, MAX_LEVEL, A-4..A-16, D-LORE-1 constants
#include "core/enums.hpp"         // StatType, SkillType, CooldownType
#include "core/math.hpp"          // isqrt64, applyXpDelta (task 0.2)
#include "core/result.hpp"        // Result<void, E> (task 0.2)
#include "core/world_context.hpp" // WorldContext + seams (task 2.1)
#include "progression/power.hpp"  // markPowerDirty (2.5's seam)

namespace harrowmere::data
{
class DataRegistry; // registry.hpp (task 1.3) — forward-declared
struct ClassData;   // classes.hpp (task 1.1) — forward-declared
} // namespace harrowmere::data

namespace harrowmere
{

// ---- rule-level failures as return values (§3.4, never exceptions) ---------

enum class PurchaseError
{
    InsufficientXp,   // availableXp below the entry's cost
    AtCap,            // stat/skill at statCaps/skillCaps[level] (A-13..A-16)
    RequirementUnmet, // skill requirement (reqStats/reqSkills/reqActions) unmet
    ManaBacklash,     // D-LORE-1: the ARCANA shortfall roll failed — XP consumed, pain dealt
    NotPeaceful,      // respec while IN_COMBAT / IN_EVENT / IN_WAR / ARRESTED
    CooldownActive,   // respec while its RESPEC_STAT / RESPEC_SKILL cooldown runs
    AtLevelCap,       // level at the buyer's cap (A-6: players 75, NPCs 100)
    BadIndex,         // programmer-error guard: a stat/skill index outside the closed enum
};

using PurchaseResult = Result<void, PurchaseError>;

// ---- cost & cap tables (design reference §Progression, verbatim) -----------

// Every table spans [0, MAX_LEVEL] — MAX_LEVEL + 1 entries, exactly the
// doc loop's range (costs fills 0..MAX_LEVEL; caps evaluates i = 0..MAX_LEVEL).
inline constexpr std::size_t kProgressionTableSize = static_cast<std::size_t>(MAX_LEVEL) + 1;

// costs[0] = baseCost; costs[i] = max(baseCost, costs[i-1] * rateNum / rateDen).
// Multiply BEFORE divide (the doc's form — the plan's #1 progression pitfall:
// divide-first collapses the 1.15 growth to integers early). constexpr, so
// the tables are compile-time constants and every run sees identical bits.
constexpr std::array<std::uint64_t, kProgressionTableSize> precomputeCosts(
    std::uint64_t baseCost, std::uint64_t rateNum, std::uint64_t rateDen
)
{
    std::array<std::uint64_t, kProgressionTableSize> costs{};
    costs[0] = baseCost;
    for (std::size_t i = 1; i < kProgressionTableSize; ++i)
    {
        const std::uint64_t grown = costs[i - 1] * rateNum / rateDen;
        costs[i]                  = grown > baseCost ? grown : baseCost;
    }
    return costs;
}

// caps[i] = base + i / divisor, for i in 0 ... MAX_LEVEL (design reference
// verbatim; i is the LEVEL — statCaps[level] caps every stat at that level).
constexpr std::array<std::uint64_t, kProgressionTableSize> precomputeCaps(
    std::uint64_t base, std::uint64_t divisor
)
{
    std::array<std::uint64_t, kProgressionTableSize> caps{};
    for (std::size_t i = 0; i < kProgressionTableSize; ++i)
        caps[i] = base + i / divisor;
    return caps;
}

// The five pinned tables (design reference's own instantiations), as const
// refs to once-initialized statics — the vector form the card's signatures
// name (getRefund takes const std::vector<u64>&; 3.3 indexes levelCosts).
const std::vector<std::uint64_t> &levelCosts(); // A-7/A-8:  base 100, 115/100
const std::vector<std::uint64_t> &statCosts();  // A-10:     base 50,  112/100
const std::vector<std::uint64_t> &skillCosts(); // A-11:     base 80,  115/100
const std::vector<std::uint64_t> &statCaps();   // A-13/A-14: 10 + level/5
const std::vector<std::uint64_t> &skillCaps();  // A-15/A-16: 10 + level/5

// ---- effectiveness, refunds, peaceful gating -------------------------------

// getEffectiveness — design reference verbatim (the soft-cap knee, A-12):
// points at or below the threshold count at face value; beyond it only
// isqrt(point - threshold) is added per point. constexpr; isqrt64 is the
// exact integer sqrt (math.hpp, task 0.2 — no sqrtl drift, R5).
constexpr std::uint64_t getEffectiveness(
    std::uint64_t point, std::uint64_t threshold
) noexcept
{
    if (point <= threshold)
        return point;
    return threshold + isqrt64(point - threshold);
}

// getRefund — D-REFUND-1, LITERAL doc semantics: the refund for a point is
// exactly the cost of the LAST point bought (costs[point - 1]); point 0
// refunds 0. This is the doc's own definition ("getRefund is used on stat
// and skill"), suspected design gap and all — a full respec therefore
// refunds only the last point's cost, NOT the sum. Pinned by
// TestRespec_RefundsLiteralLastCost and flagged in DEVIATIONS.md; if the
// lead rules otherwise, this one function + its test change.
//
// point beyond the table is a programmer error (assert in debug); release
// clamps to the last entry deterministically.
std::uint64_t getRefund(const std::vector<std::uint64_t> &costs, std::uint64_t point);

// isPeaceful — design reference verbatim: none of the four derived statuses
// (2.1's hasStatus through the WorldContext's seams).
bool isPeaceful(const Character &c, const WorldContext &ctx);

// canRespec — design reference verbatim: peaceful AND the cooldown slot
// expired (cooldowns[type] <= worldTime; 2.1's cooldownActive is the
// complement).
bool canRespec(const Character &c, CooldownType type, const WorldContext &ctx);

// canRespecStat / canRespecSkill — design reference §Stats/§Skills verbatim.
bool canRespecStat(const Character &c, const WorldContext &ctx);
bool canRespecSkill(const Character &c, const WorldContext &ctx);

// ---- skill effectiveness (design reference §Skills, verbatim) --------------

// getSkillEffectiveness — the SKILL_THRESHOLD knee (A-12: 50) over the raw
// skill points; skills carry no species multiplier (the doc's species block
// multiplies stats only). Defined in skills.cpp (the §Skills block's file).
std::uint64_t getSkillEffectiveness(const Character &c, SkillType skill);

// ---- purchases --------------------------------------------------------------

// Advance one level (manual) or loop while affordable (automatic — the NPC
// auto-buy path; players press the button once per call). The XP to advance
// from level i to i+1 is levelCosts[i] (design reference §Levels; the card
// spec's "availableXp -= levelCosts[level], level++"). The automatic loop's
// natural ends — out of XP, at the cap — report Ok (3.4 re-reads the level);
// only the manual path reports them as rule-level failures.
//
// The buyer's cap comes from the character (A-6): species 0 is the player
// (A.2 "species ids" row) and caps at PLAYER_MAX_LEVEL (late-stage mortal);
// NPCs cap at MAX_LEVEL (peak-stage). "buyLevel refuses beyond the caller's
// cap."
//
// markPowerDirty is deliberately NOT fired here: level changes do not enter
// the power formula (design reference; 2.5's TestPower_LevelUpDoesNotDirtyCache).
// totalXp never changes on a purchase — only availableXp moves.
PurchaseResult buyLevel(Character &c, bool automatic);

// Buy one stat point: statCosts[current] XP raises the stat from current to
// current+1, capped at statCaps[level] (design reference §Stats verbatim).
// Above the STAT_THRESHOLD knee the D-LORE-1 mana rule applies (below).
// Fires markPowerDirty on success.
PurchaseResult buyStat(Character &c, StatType stat, const WorldContext &ctx);

// Buy one skill point: skillCosts[current] XP, capped at skillCaps[level],
// gated by the class's skill requirements (reqStats / reqSkills /
// reqActions — data in classes.json; see skills.cpp). Fires markPowerDirty.
PurchaseResult buySkill(Character &c, SkillType skill, const WorldContext &ctx);

// The skill-requirement check (design reference §Skills: "Skill-buying
// requirements can be stats, other skills, or actions"). The requirements
// are data — the character's classes.json row, resolved per the v1 pin in
// skills.cpp (player class key through WorldContext::characters; classless
// characters unconstrained). reqActions counters read through
// WorldContext::actionCounters; a null counter map reads as all zeros.
// Defined in skills.cpp.
bool skillRequirementsMet(const Character &c, SkillType skill, const WorldContext &ctx);

// D-LORE-1 — the mana-enhancement rule (card 2.3, from the design
// reference's "Enhancing requires knowledge and understanding; failing can
// cause pain to death"). Buying a stat point above STAT_THRESHOLD requires
// ARCANA effectiveness >= requiredArcana(point) = point - STAT_THRESHOLD.
//
// On shortfall the purchase RISKS failure: chance
//     pct = min(SCALE, shortfall * MANA_FAILURE_CHANCE_PER_POINT)
// drawn from the pinned "combat" stream (0.3). A failed roll consumes the
// XP for the attempted point AND deals
//     body.maxHp * (MANA_PAIN_BASE_PCT + shortfall / MANA_PAIN_SHORTFALL_DIVISOR)
//                / MANA_PAIN_PCT_DENOMINATOR
// damage ("pain") — currentHp floors at 0, never wraps (u64); death itself
// routes through 3.3's environment-death path once that card lands (death IS
// possible, per the doc). Below STAT_THRESHOLD no requirement, no roll —
// and skills never fail (card).
//
// The three pure helpers pin the formula's parts for tests and UI:
std::uint64_t requiredArcana(std::uint64_t targetPoint);  // point - STAT_THRESHOLD, 0 at/below the knee
std::uint64_t manaFailureChance(std::uint64_t shortfall); // min(SCALE, shortfall * MANA_FAILURE_CHANCE_PER_POINT)
std::uint64_t manaPainDamage(
    std::uint64_t maxHp, std::uint64_t shortfall
); // maxHp * (5 + shortfall/500) / 100, floored at 0 damage for shortfall 0

// The risky half of a stat purchase: runs the D-LORE-1 roll for targetPoint
// when it sits above the knee and ARCANA falls short. Returns true when the
// attempt FAILED (backlash: the XP for `cost` is consumed and the pain is
// dealt — the caller must abort the purchase); false when the purchase may
// proceed (no requirement, sufficient ARCANA, or the roll survived). A null
// ctx.rng with a live shortfall is a wiring programmer error (debug-assert)
// and fails deterministically in release.
bool manaBacklash(Character &c, std::uint64_t targetPoint, std::uint64_t cost, const WorldContext &ctx);

// ---- respec -----------------------------------------------------------------

// Respec one stat back to 0, refunding getRefund(statCosts, value) — the
// LITERAL last-point refund (D-REFUND-1; see getRefund above and the
// DEVIATIONS.md row). Gated by canRespecStat (peaceful + RESPEC_STAT
// cooldown); arms the cooldown for RESPEC_COOLDOWN_GAME_S and fires
// markPowerDirty.
PurchaseResult respecStat(Character &c, StatType stat, const WorldContext &ctx);

// respecSkill — same shape over skills (design reference §Skills:
// getRefund "is used on stat and skill").
PurchaseResult respecSkill(Character &c, SkillType skill, const WorldContext &ctx);

} // namespace harrowmere
