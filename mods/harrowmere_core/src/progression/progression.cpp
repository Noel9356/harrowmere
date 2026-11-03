// progression.cpp — the XP economy core (task 2.3): tables, effectiveness,
// refunds, peaceful gating, level/stat purchases, the D-LORE-1 mana rule,
// and respec. The skill-side block (requirements + buySkill) lives in
// skills.cpp, the §Skills block's file.

#include "progression/progression.hpp"

#include <cassert>

#include "character/status.hpp" // hasStatus / setCooldown (task 2.1)
#include "core/rng.hpp"         // SeedRng (D-LORE-1's "combat" stream draw)
#include "data/classes.hpp"     // ClassData (requirement reads, via skills.cpp too)

namespace harrowmere
{

namespace
{

// The constexpr master tables — every accessor below hands out views over
// these exact bits (built once at compile time; identical every run).
constexpr auto kLevelCostTable = precomputeCosts(BASE_LEVEL_COST, LEVEL_COST_RATE_NUM, LEVEL_COST_RATE_DEN);
constexpr auto kStatCostTable  = precomputeCosts(BASE_STAT_COST, STAT_COST_RATE_NUM, STAT_COST_RATE_DEN);
constexpr auto kSkillCostTable = precomputeCosts(BASE_SKILL_COST, SKILL_COST_RATE_NUM, SKILL_COST_RATE_DEN);
constexpr auto kStatCapTable   = precomputeCaps(BASE_STAT_CAP, STAT_CAP_DIVISOR);
constexpr auto kSkillCapTable  = precomputeCaps(BASE_SKILL_CAP, SKILL_CAP_DIVISOR);

template <std::size_t N>
std::vector<std::uint64_t> asVector(
    const std::array<std::uint64_t, N> &table
)
{
    return {table.begin(), table.end()};
}

// The buyer's level cap (A-6): species 0 IS the player (A.2 "species ids"
// row) — players stop at late-stage mortal, NPCs at peak-stage. Deriving
// the cap from the character (not from the automatic flag) keeps a stray
// automatic=true call from lifting a player past the lore cap.
std::uint16_t levelCapFor(
    const Character &c
)
{
    return c.species == PLAYER_SPECIES_ID ? PLAYER_MAX_LEVEL : MAX_LEVEL;
}

} // namespace

const std::vector<std::uint64_t> &levelCosts()
{
    static const std::vector<std::uint64_t> table = asVector(kLevelCostTable);
    return table;
}

const std::vector<std::uint64_t> &statCosts()
{
    static const std::vector<std::uint64_t> table = asVector(kStatCostTable);
    return table;
}

const std::vector<std::uint64_t> &skillCosts()
{
    static const std::vector<std::uint64_t> table = asVector(kSkillCostTable);
    return table;
}

const std::vector<std::uint64_t> &statCaps()
{
    static const std::vector<std::uint64_t> table = asVector(kStatCapTable);
    return table;
}

const std::vector<std::uint64_t> &skillCaps()
{
    static const std::vector<std::uint64_t> table = asVector(kSkillCapTable);
    return table;
}

std::uint64_t getRefund(
    const std::vector<std::uint64_t> &costs, std::uint64_t point
)
{
    // D-REFUND-1, literal doc semantics (progression.md):
    //   getRefund(costs, point):
    //       if point == 0: return 0
    //       return costs[point - 1]
    // Only the LAST point's cost comes back — not the sum of the ladder.
    // Flagged in DEVIATIONS.md (suspected design gap; implemented as
    // written; TestRespec_RefundsLiteralLastCost pins it).
    if (point == 0)
        return 0;
    assert(point <= costs.size() && "getRefund: point outside the cost table (programmer error)");
    if (point > costs.size())
        point = costs.size(); // deterministic release recovery: the last entry
    return costs[point - 1];
}

bool isPeaceful(
    const Character &c, const WorldContext &ctx
)
{
    // design reference verbatim — none of the four derived statuses
    // (2.1's hasStatus derives each from its owning state through ctx).
    return !hasStatus(c, StatusType::IN_COMBAT, ctx) && !hasStatus(c, StatusType::IN_EVENT, ctx)
        && !hasStatus(c, StatusType::IN_WAR, ctx) && !hasStatus(c, StatusType::ARRESTED, ctx);
}

bool canRespec(
    const Character &c, CooldownType type, const WorldContext &ctx
)
{
    // design reference verbatim: peaceful, and the cooldown slot expired.
    return isPeaceful(c, ctx) && c.cooldowns[static_cast<std::size_t>(type)] <= ctx.now;
}

bool canRespecStat(
    const Character &c, const WorldContext &ctx
)
{
    return canRespec(c, CooldownType::RESPEC_STAT, ctx);
}

bool canRespecSkill(
    const Character &c, const WorldContext &ctx
)
{
    return canRespec(c, CooldownType::RESPEC_SKILL, ctx);
}

PurchaseResult buyLevel(
    Character &c, bool automatic
)
{
    // automatic = the NPC auto-buy loop (3.4 drives it; players press the
    // button once per call). Loop while affordable. The loop's two natural
    // ends — out of XP, at the cap — are SUCCESS ends for the caller (3.4
    // re-reads the level), so automatic reports Ok; only the MANUAL path
    // reports them as rule-level failures (§3.4: the UI reports the refusal
    // of the button press it just made).
    const std::uint16_t cap = levelCapFor(c);
    do
    {
        if (c.progression.level >= cap)
            return automatic ? PurchaseResult::ok() : PurchaseResult::err(PurchaseError::AtLevelCap);

        // levelCosts[i] is the XP to advance from level i to i+1 (design
        // reference §Levels). level < cap <= MAX_LEVEL keeps the index in
        // [1, MAX_LEVEL-1] — always inside the table.
        const std::uint64_t cost = levelCosts()[c.progression.level];
        if (static_cast<std::int64_t>(cost) > c.progression.availableXp)
            return automatic ? PurchaseResult::ok() : PurchaseResult::err(PurchaseError::InsufficientXp);

        // R1: widen signed BEFORE negating; R4: applyXpDelta owns the floor.
        c.progression.availableXp = applyXpDelta(c.progression.availableXp, -static_cast<std::int64_t>(cost));
        ++c.progression.level;
        // totalXp untouched (never decreases — design reference).
        // markPowerDirty deliberately NOT fired: level changes do not enter
        // the power formula (design reference; 2.5's audit test pins it).
    } while (automatic);
    return PurchaseResult::ok();
}

std::uint64_t requiredArcana(
    std::uint64_t targetPoint
)
{
    // D-LORE-1: requiredArcana(point) = point - STAT_THRESHOLD. At or below
    // the knee no knowledge is required (0).
    return targetPoint <= STAT_THRESHOLD ? 0 : targetPoint - STAT_THRESHOLD;
}

std::uint64_t manaFailureChance(
    std::uint64_t shortfall
)
{
    // pct = min(SCALE, shortfall * MANA_FAILURE_CHANCE_PER_POINT), guarded
    // against overflow (a huge shortfall saturates at SCALE = guaranteed
    // failure) instead of wrapping (R2/R3 hygiene).
    if (shortfall > SCALE / MANA_FAILURE_CHANCE_PER_POINT)
        return SCALE;
    return shortfall * MANA_FAILURE_CHANCE_PER_POINT;
}

std::uint64_t manaPainDamage(
    std::uint64_t maxHp, std::uint64_t shortfall
)
{
    // maxHp * (5 + shortfall/500) / 100 — the integer division inside the
    // parens is the doc's own form (a +1% step per full 500 points of
    // shortfall). Shortfall 0 deals no pain.
    if (shortfall == 0)
        return 0;
    const std::uint64_t painPct = MANA_PAIN_BASE_PCT + shortfall / MANA_PAIN_SHORTFALL_DIVISOR;
    return maxHp * painPct / MANA_PAIN_PCT_DENOMINATOR;
}

bool manaBacklash(
    Character &c, std::uint64_t targetPoint, std::uint64_t cost, const WorldContext &ctx
)
{
    const std::uint64_t required = requiredArcana(targetPoint);
    if (required == 0)
        return false; // below the knee: no requirement, no roll (design lore)

    const std::uint64_t arcanaEff = getSkillEffectiveness(c, SkillType::ARCANA);
    if (arcanaEff >= required)
        return false; // knowledge suffices: clean purchase, no roll

    const std::uint64_t shortfall = required - arcanaEff;

    // The roll: fail iff draw < failPct. pct() draws [0, SCALE), so
    // failPct == SCALE fails ALWAYS — the deterministic no-RNG test path.
    // A null ctx.rng is a wiring programmer error: assert in debug,
    // deterministic failure in release (never a free success).
    bool failed = true;
    if (ctx.rng != nullptr)
        failed = ctx.rng->pct("combat") < manaFailureChance(shortfall);
    else
        assert(false && "manaBacklash: WorldContext::rng missing for a D-LORE-1 roll");

    if (!failed)
        return false; // risky but survived — caller completes the purchase

    // Failure consumes the XP for the attempted point...
    c.progression.availableXp = applyXpDelta(c.progression.availableXp, -static_cast<std::int64_t>(cost));

    // ...and deals the pain. currentHp floors at 0 (u64, never negative);
    // reaching 0 routes through 3.3's environment death once that card
    // lands — death IS possible, per the doc.
    const std::uint64_t pain = manaPainDamage(c.body.maxHp, shortfall);
    c.body.currentHp         = pain >= c.body.currentHp ? 0 : c.body.currentHp - pain;

    return true;
}

PurchaseResult buyStat(
    Character &c, StatType stat, const WorldContext &ctx
)
{
    const auto idx = static_cast<std::size_t>(stat);
    if (idx >= c.progression.stats.size())
        return PurchaseResult::err(PurchaseError::BadIndex);

    const std::uint64_t current = c.progression.stats[idx];
    const std::uint64_t target  = current + 1;

    // statCosts[i] is the XP to raise a stat from i to i+1 (design
    // reference §Stats). A value at/above the table's end cannot be priced
    // — treat it as the cap it effectively is (deterministic refusal).
    const std::vector<std::uint64_t> &costs = statCosts();
    if (current >= costs.size())
        return PurchaseResult::err(PurchaseError::AtCap);
    const std::uint64_t cost = costs[current];

    // Refuse what cannot be paid BEFORE any side effect (a mana backlash
    // consumes the XP it names — the attempt must be affordable to begin).
    if (static_cast<std::int64_t>(cost) > c.progression.availableXp)
        return PurchaseResult::err(PurchaseError::InsufficientXp);

    // D-LORE-1: above the knee the point risks the ARCANA backlash — a
    // failed roll consumes the XP and deals pain (see manaBacklash).
    if (manaBacklash(c, target, cost, ctx))
        return PurchaseResult::err(PurchaseError::ManaBacklash);

    // Cap enforcement: stats[st] never exceeds statCaps[level] (design
    // reference §Stats; A-13/A-14). Checked after the mana attempt so the
    // pinned mana tests exercise the public entry point; the v1 caps bind
    // below the knee, so the ordering is unobservable in normal play
    // (DEVIATIONS.md 2026-10-31).
    const std::uint64_t cap = statCaps()[c.progression.level];
    if (target > cap)
        return PurchaseResult::err(PurchaseError::AtCap);

    c.progression.availableXp = applyXpDelta(c.progression.availableXp, -static_cast<std::int64_t>(cost));
    c.progression.stats[idx]  = target;
    markPowerDirty(c);
    return PurchaseResult::ok();
}

PurchaseResult respecStat(
    Character &c, StatType stat, const WorldContext &ctx
)
{
    const auto idx = static_cast<std::size_t>(stat);
    if (idx >= c.progression.stats.size())
        return PurchaseResult::err(PurchaseError::BadIndex);

    // Peaceful + cooldown gate (design reference §Stats: canRespecStat).
    if (!canRespecStat(c, ctx))
        return PurchaseResult::err(isPeaceful(c, ctx) ? PurchaseError::CooldownActive : PurchaseError::NotPeaceful);

    // D-REFUND-1: the LITERAL refund — only the last point's cost
    // (getRefund; the suspected design gap is flagged in DEVIATIONS.md and
    // pinned by TestRespec_RefundsLiteralLastCost).
    const std::uint64_t value  = c.progression.stats[idx];
    const std::uint64_t refund = getRefund(statCosts(), value);

    c.progression.availableXp = applyXpDelta(c.progression.availableXp, static_cast<std::int64_t>(refund));
    c.progression.stats[idx]  = 0;
    harrowmere::setCooldown(c, CooldownType::RESPEC_STAT, ctx.now + RESPEC_COOLDOWN_GAME_S);
    markPowerDirty(c);
    return PurchaseResult::ok();
}

} // namespace harrowmere
