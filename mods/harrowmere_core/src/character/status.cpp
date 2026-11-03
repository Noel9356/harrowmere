// status.cpp — derived statuses, PvP consent, and cooldowns (task 2.1).
//
// Every branch below is design-reference or card-verbatim; the ownership of
// each derived status is noted inline. Grep contract (card Done-when): no
// code writes statusFlags bits for the four derived statuses — the only
// statusFlags mentions in the tree are the struct field, this file's
// documentation, and the test's negative assertions.

#include "character/status.hpp"

#include <cassert>

namespace harrowmere
{

bool hasStatus(
    const Character &c, StatusType flag, const WorldContext &ctx
)
{
    switch (flag)
    {
    case StatusType::IN_EVENT:
        // design reference note: "Setting IN_EVENT always pairs with
        // eventId" — the event id IS the state; 0 = not in an event.
        return c.eventId != 0;

    case StatusType::IN_COMBAT:
        // A-4: the 30-game-second window after the last damage application
        // (3.2 stamps lastCombatTime on both sides of every hit).
        // lastCombatTime == 0 means "never fought / respawned" (3.3 resets
        // it to 0 on respawn) — and the A-129 epoch opens worldTime at
        // 10:00, so a fresh stamp can never be 0; the guard keeps an
        // untouched character out of combat during any test's early ticks.
        if (c.lastCombatTime == 0)
            return false;
        return ctx.now >= c.lastCombatTime && ctx.now - c.lastCombatTime < COMBAT_TIMEOUT_S;

    case StatusType::IN_WAR:
        // The war registry is 6.2's WarStore (null-false, D-SINK-1): the
        // registry's isCombatant answers warEffectivelyActive — the notice
        // window's members are not yet IN_WAR.
        return ctx.wars != nullptr && ctx.wars->isCombatant(c.faction);

    case StatusType::ARRESTED:
        // Jail state lives in justice's jailUntil map, NOT on the character
        // (design rule: "Jail state lives here, not on Player") — null
        // justice store until 4.3 lands reads as nobody arrested.
        return ctx.justice != nullptr && ctx.justice->isArrested(c.id);
    }

    // Unreachable today: the enum is closed over the four derived values
    // (D-CHAR-1). Non-derived extension flags (none in v1) would read
    // c.statusFlags here — (c.statusFlags & flag) != 0.
    return false;
}

void setStatus(
    Character &c, StatusType flag, bool enabled
)
{
    // Non-derived flags only. All four StatusType values are derived
    // (D-CHAR-1): reaching this function with one of them is a programmer
    // error — assert in debug, no-op deterministically in release (3.4's
    // rule-level-vs-programmer-error split).
    (void)c;
    (void)flag;
    (void)enabled;
    assert(
        false
        && "setStatus: no non-derived StatusType flags exist in v1 "
           "(D-CHAR-1: derived statuses are computed from their owning state, "
           "never stored — mutate the owning state instead)"
    );
}

bool consentsToPvp(
    const Character &victim, const Character &attacker
)
{
    // character.md §Character, verbatim:
    //   consentsToPvp(victim, attacker):
    //       return victim.duelOpponent == attacker.id
    //           or (victim.arenaId != 0 and victim.arenaId == attacker.arenaId)
    return victim.duelOpponent == attacker.id || (victim.arenaId != 0 && victim.arenaId == attacker.arenaId);
}

bool cooldownActive(
    const Character &c, CooldownType type, std::uint64_t now
)
{
    return c.cooldowns[static_cast<std::size_t>(type)] > now;
}

void setCooldown(
    Character &c, CooldownType type, std::uint64_t expiresAt
)
{
    c.cooldowns[static_cast<std::size_t>(type)] = expiresAt;
}

} // namespace harrowmere
