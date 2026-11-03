// combat/attacks.cpp — Task 3.2 implementation.
#include "combat/attacks.hpp"

#include <algorithm>

#include "character/species.hpp"
#include "character/status.hpp"
#include "combat/damage.hpp"
#include "core/constants.hpp"
#include "core/constants_phase2.hpp"
#include "core/world_context.hpp"
#include "data/registry.hpp"
#include "progression/progression.hpp"

namespace harrowmere
{

std::uint64_t hitChance(
    const data::DataRegistry &data, const Character &attacker, const Character &defender
)
{
    const std::int64_t agiA = static_cast<std::int64_t>(getStatEffectiveness(data, attacker, StatType::AGILITY));
    const std::int64_t agiD = static_cast<std::int64_t>(getStatEffectiveness(data, defender, StatType::AGILITY));
    // A-28 verbatim; the agility gap is a signed delta (R1) before it scales.
    std::int64_t chance = static_cast<std::int64_t>(SCALE / 2) + (agiA - agiD) * 100 / static_cast<std::int64_t>(SCALE);
    if (chance < static_cast<std::int64_t>(HIT_CHANCE_MIN))
        chance = static_cast<std::int64_t>(HIT_CHANCE_MIN);
    if (chance > static_cast<std::int64_t>(HIT_CHANCE_MAX))
        chance = static_cast<std::int64_t>(HIT_CHANCE_MAX);
    return static_cast<std::uint64_t>(chance);
}

std::uint64_t rawDamage(
    const data::DataRegistry &data, const Character &attacker, const Character &defender, const data::ItemData *weapon
)
{
    // A-29's per-item override rides the SCALE grid (items.json 1.5 -> 15000);
    // the A-29/A-30 factors are pinned on the per-mille grid — convert once.
    const std::uint64_t wf = (weapon != nullptr && weapon->weaponFactor != 0)
                               ? weapon->weaponFactor / (SCALE / DAMAGE_FACTOR_GRID)
                               : WEAPON_FACTOR_UNARMED;
    const std::int64_t str = static_cast<std::int64_t>(getStatEffectiveness(data, attacker, StatType::STRENGTH));
    const std::int64_t def = static_cast<std::int64_t>(getStatEffectiveness(data, defender, StatType::DEFENCE));
    // A-29/A-30 verbatim; the subtraction is a signed delta (R1) before the floor.
    const std::int64_t dmg
        = str * static_cast<std::int64_t>(wf) / static_cast<std::int64_t>(DAMAGE_FACTOR_GRID)
        - def * static_cast<std::int64_t>(DEFENCE_FACTOR) / static_cast<std::int64_t>(DAMAGE_FACTOR_GRID);
    std::int64_t floored = dmg < static_cast<std::int64_t>(MIN_DAMAGE) ? static_cast<std::int64_t>(MIN_DAMAGE) : dmg;
    return static_cast<std::uint64_t>(floored);
}

AttackResult resolveAttack(
    Character &attacker, Character &defender, const data::ItemData *weapon, WorldContext &ctx
)
{
    const std::uint64_t chance = hitChance(*ctx.data, attacker, defender);
    const std::uint64_t roll   = ctx.rng->pct("combat");
    if (roll >= chance)
        return AttackResult::err(AttackError::Missed);
    const DamageResult applied = applyDamage(
        DamageEvent{attacker.id, defender.id, rawDamage(*ctx.data, attacker, defender, weapon), DamageType::Melee}, ctx
    );
    if (applied.isErr())
        return AttackResult::err(AttackError::Rejected);
    return AttackResult::ok();
}

AttackResult castSpark(
    Character &caster, Character &target, WorldContext &ctx
)
{
    // Casting cadence stands in for the (undocumented) mana pool (D-LORE-2, A-31).
    if (ctx.combatQueue->castOnCooldown(caster.id, ctx.now))
        return AttackResult::err(AttackError::OnCooldown);
    if (caster.progression.skills[static_cast<std::size_t>(SkillType::ARCANA)] < SPELL_SPARK_MIN_ARCANA)
        return AttackResult::err(AttackError::RequirementNotMet);

    // v1 spells auto-hit (A-28's agility math is weapon attack resolution); the
    // damage is the ARCANA effectiveness with no weapon factor (task 3.2 spec).
    const std::uint64_t dmg = getSkillEffectiveness(caster, SkillType::ARCANA);
    ctx.combatQueue->markCast(caster.id, ctx.now + ARCANA_COOLDOWN_S);
    const DamageResult applied = applyDamage(DamageEvent{caster.id, target.id, dmg, DamageType::Spell}, ctx);
    // The pipeline's refusals are the attack layer's Rejected (one outcome, the
    // rule-level reason stays with the pipeline's own logging).
    if (applied.isErr())
        return AttackResult::err(AttackError::Rejected);
    return AttackResult::ok();
}

void CombatResolutionSystem::run(
    WorldContext &ctx
)
{
    const std::vector<AttackRequest> requests = ctx.combatQueue->drain();
    for (const AttackRequest &r : requests)
    {
        if (!ctx.characters->exists(r.attacker) || !ctx.characters->exists(r.target))
            continue;
        Character &attacker = ctx.characters->get(r.attacker);
        Character &defender = ctx.characters->get(r.target);
        if (r.isSpell)
        {
            (void)castSpark(attacker, defender, ctx);
        } else
        {
            const data::ItemData *weapon
                = (r.weaponItemId != 0 && ctx.data != nullptr) ? ctx.data->itemById(r.weaponItemId) : nullptr;
            (void)resolveAttack(attacker, defender, weapon, ctx);
        }
    }
}

} // namespace harrowmere
