// combat/damage.cpp — Task 3.2 implementation. The hook order inside applyDamage is
// pinned: evidence before death (a killing blow records assault evidence AND routes to
// the murder death handler — "recorded at commit time" means before the corpse exists).
#include "combat/damage.hpp"

#include <algorithm>

#include "character/species.hpp"
#include "character/status.hpp"
#include "combat/death.hpp"
#include "npc/state.hpp"
#include "core/world_context.hpp"

namespace harrowmere
{

bool warrantCovers(
    const Character &target, const Character & /*attacker*/, territoryId territory, const WorldContext &ctx
)
{
    // 4.2's API; the null sink answers false for every territory until it lands.
    return ctx.justice->hasWarrantIn(target.id, territory);
}

DamageResult applyDamage(
    DamageEvent ev, WorldContext &ctx
)
{
    if (!ctx.characters->exists(ev.target))
        return DamageResult::err(DamageError::InvalidTarget);
    Character &target = ctx.characters->get(ev.target);

    // 0. invulnerable check (design reference validateActionAmount guard) -> reject.
    //    D-INVULN-1: the flag is NpcState state; the player analog is always false.
    if (!ctx.characters->isPlayer(target.id) && ctx.npcs->contains(target.id) && ctx.npcs->get(target.id).invulnerable)
    {
        return DamageResult::err(DamageError::Invulnerable);
    }

    // 3. species damageTakenMultiplier (2.2, goo beasts) applied BEFORE clamping.
    // The 2.2 field rides the species row (data, never code — goo 0.25).
    const std::uint64_t takenMult = (ctx.data != nullptr && ctx.data->hasSpecies(target.species))
                                      ? ctx.data->species(target.species).damageTakenMultiplier
                                      : SCALE;
    const std::uint64_t mitigated = ev.amount * takenMult / SCALE;

    // 1. clamping: applied = min(amount, target.body.currentHp); u64 never negative.
    const std::uint64_t applied = mitigated < target.body.currentHp ? mitigated : target.body.currentHp;
    target.body.currentHp -= applied;

    // 2. both sides: lastCombatTime = now (IN_COMBAT derivation, 2.1).
    target.lastCombatTime = ctx.now;
    Character *source
        = (ev.source != 0 && ctx.characters->exists(ev.source)) ? &ctx.characters->get(ev.source) : nullptr;
    if (source != nullptr)
        source->lastCombatTime = ctx.now;

    // 4. justice hook — evidence before death (pinned order, 3.2 pitfalls).
    if (source != nullptr && ctx.characters->isPlayer(source->id) && isSapient(*ctx.data, target.species)
        && !consentsToPvp(target, *source)
        && !warrantCovers(target, *source, ctx.world->territoryAt(target.body.position), ctx))
    {
        ctx.justice->noteAssault(source->id, target.id, applied); // 4.1 aggregates vs ASSAULT_THRESHOLD
    }

    // 5. event hook: 5.3 routes hpLost for targetIds members and the attacker's
    //    validateActionAmount DAMAGE contribution; null no-op until 5.3 lands.
    if ((source != nullptr && source->eventId != 0) || target.eventId != 0)
    {
        DamageEvent contribution = ev;
        contribution.amount      = applied;
        ctx.events->onCombatDamage(contribution);
    }

    // 6. death — immediate inside step 10 (D-TICK-1); do not touch `target` after
    //    die(): it may erase the victim from the store.
    if (target.body.currentHp == 0)
    {
        die(target, ev.source, ctx);
        return DamageResult::ok();
    }

    return DamageResult::ok();
}

} // namespace harrowmere
