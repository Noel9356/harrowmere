// skills.cpp — the §Skills block of the progression engine (task 2.3):
// getSkillEffectiveness, the skill-requirement checks ("stats, other skills,
// or actions" — data in classes.json per class), and buySkill.
//
// Requirement resolution (v1 pin): a character's class row is resolved at
// purchase time — the player's PlayerCreation.classKey (task 2.0's store
// record, read through WorldContext::characters); NPCs have no class in v1
// (NpcSeed assigns class keys to the title chain only, and the NPC-side
// class read joins NpcState in task 3.4 — the resolution point is the one
// function below, so 3.4 extends it, not the callers). A classless character
// has no requirement table and is unconstrained; a class that declares no
// requirement for the skill likewise gates nothing.
//
// reqActions counters arrive through WorldContext::actionCounters (the
// NpcState::actionCounters map for NPCs / the player action log's counter
// projection — writers are 2.4's crafting and 3.3's kills). A null map
// reads as all-zero counters: any declared reqActions entry stays unmet
// (fail closed), a class with no reqActions entry is unaffected.

#include "progression/progression.hpp"

#include <cassert>

#include "character/status.hpp" // cooldown/peaceful reads (2.1)
#include "character/store.hpp"  // CharacterStore (the player's class-key read)
#include "data/classes.hpp"     // ClassData, SkillRequirement
#include "data/registry.hpp"    // DataRegistry (classByKey)

namespace harrowmere
{

std::uint64_t getSkillEffectiveness(
    const Character &c, SkillType skill
)
{
    // design reference §Skills, verbatim:
    //   getSkillEffectiveness(character, skillId):
    //       return getEffectiveness(character.progression.skills[skillId], SKILL_THRESHOLD)
    return getEffectiveness(c.progression.skills[static_cast<std::size_t>(skill)], SKILL_THRESHOLD);
}

namespace
{

// The character's classes.json row, or nullptr when classless (see the file
// comment for the v1 resolution rule).
const data::ClassData *resolveClass(
    const Character &c, const WorldContext &ctx
)
{
    if (ctx.data == nullptr)
        return nullptr;
    std::string classKey;
    if (ctx.characters != nullptr && ctx.characters->isPlayer(c.id))
        classKey = ctx.characters->playerCreation().classKey;
    // else: NPC class keys ride NpcState (task 3.4) — v1 NPCs resolve to
    // classless here. This is THE extension point for 3.4.
    if (classKey.empty())
        return nullptr;
    return ctx.data->classByKey(classKey);
}

// The three requirement kinds, in the doc's order: stats, other skills,
// actions. All comparisons are plain point thresholds (SkillRequirement's
// doc comment); unknown counter keys read 0 and stay unmet (fail closed).
bool requirementsMet(
    const Character &c, const data::SkillRequirement &req, const WorldContext &ctx
)
{
    for (std::size_t i = 0; i < req.reqStats.size(); ++i)
        if (c.progression.stats[i] < req.reqStats[i])
            return false;

    for (std::size_t i = 0; i < req.reqSkills.size(); ++i)
        if (c.progression.skills[i] < req.reqSkills[i])
            return false;

    for (const auto &[counterKey, required] : req.reqActions)
    {
        std::uint64_t have = 0;
        if (ctx.actionCounters != nullptr)
        {
            if (const auto it = ctx.actionCounters->find(counterKey); it != ctx.actionCounters->end())
                have = it->second;
        }
        if (have < required)
            return false;
    }
    return true;
}

} // namespace

bool skillRequirementsMet(
    const Character &c, SkillType skill, const WorldContext &ctx
)
{
    // No registry wired: nothing is known about requirements — unconstrained
    // (the deterministic fail-open for data-free tests; the runtime always
    // has a committed registry before the player can buy).
    const data::ClassData *cls = resolveClass(c, ctx);
    if (cls == nullptr)
        return true;
    return requirementsMet(c, cls->skillRequirements[static_cast<std::size_t>(skill)], ctx);
}

PurchaseResult buySkill(
    Character &c, SkillType skill, const WorldContext &ctx
)
{
    const auto idx = static_cast<std::size_t>(skill);
    if (idx >= c.progression.skills.size())
        return PurchaseResult::err(PurchaseError::BadIndex);

    // Skill requirements first: the gate is data (classes.json per class);
    // an unmet requirement refuses before any cost math. Skills never fail
    // a mana roll (card 2.3: "Skills never fail") — no D-LORE-1 branch here.
    if (!skillRequirementsMet(c, skill, ctx))
        return PurchaseResult::err(PurchaseError::RequirementUnmet);

    const std::uint64_t current = c.progression.skills[idx];

    // skillCosts[i] is the XP to raise a skill from i to i+1 (design
    // reference §Skills). A value at/above the table's end cannot be priced
    // — treat it as the cap it effectively is (deterministic refusal).
    const std::vector<std::uint64_t> &costs = skillCosts();
    if (current >= costs.size())
        return PurchaseResult::err(PurchaseError::AtCap);
    const std::uint64_t cost = costs[current];

    if (static_cast<std::int64_t>(cost) > c.progression.availableXp)
        return PurchaseResult::err(PurchaseError::InsufficientXp);

    // Cap enforcement: skills[skillId] never exceeds skillCaps[level]
    // (design reference §Skills; A-15/A-16).
    if (current + 1 > skillCaps()[c.progression.level])
        return PurchaseResult::err(PurchaseError::AtCap);

    c.progression.availableXp = applyXpDelta(c.progression.availableXp, -static_cast<std::int64_t>(cost));
    c.progression.skills[idx] = current + 1;
    markPowerDirty(c);
    return PurchaseResult::ok();
}

PurchaseResult respecSkill(
    Character &c, SkillType skill, const WorldContext &ctx
)
{
    const auto idx = static_cast<std::size_t>(skill);
    if (idx >= c.progression.skills.size())
        return PurchaseResult::err(PurchaseError::BadIndex);

    // Peaceful + cooldown gate (design reference §Skills: canRespecSkill).
    if (!canRespecSkill(c, ctx))
        return PurchaseResult::err(isPeaceful(c, ctx) ? PurchaseError::CooldownActive : PurchaseError::NotPeaceful);

    // D-REFUND-1: the LITERAL refund — only the last point's cost ("getRefund
    // is used on stat and skill"; the suspected design gap is flagged in
    // DEVIATIONS.md and pinned by TestRespec_RefundsLiteralLastCost).
    const std::uint64_t value  = c.progression.skills[idx];
    const std::uint64_t refund = getRefund(skillCosts(), value);

    c.progression.availableXp = applyXpDelta(c.progression.availableXp, static_cast<std::int64_t>(refund));
    c.progression.skills[idx] = 0;
    harrowmere::setCooldown(c, CooldownType::RESPEC_SKILL, ctx.now + RESPEC_COOLDOWN_GAME_S);
    markPowerDirty(c);
    return PurchaseResult::ok();
}

} // namespace harrowmere
