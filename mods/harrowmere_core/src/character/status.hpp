// status.hpp — derived statuses, PvP consent, and cooldowns (task 2.1).
//
// The doc's two seemingly conflicting lines are BOTH honored here:
//
//   * "statusFlags — non-derived flags only" (character.md field comment):
//     Character::statusFlags is u32 STORAGE for flags that are not derived
//     from owning state. Nothing in v1 writes a bit into it — the field
//     exists so the doc struct is verbatim and future non-derived flags
//     don't resize saves (D-CHAR-1).
//   * "StatusType — bit flags; derived, never stored" (character.md enum
//     comment): every StatusType value is COMPUTED from its owning state at
//     read time — IN_EVENT from eventId, IN_COMBAT from lastCombatTime +
//     COMBAT_TIMEOUT_S (A-4), IN_WAR from the war registry (6.2), ARRESTED
//     from justice's jailUntil map (4.3).
//
// The design reference's literal hasStatus/setStatus pseudocode reads and
// writes statusFlags — that is the doc describing the non-derived HALF of
// the mechanism ("see hasStatus" points at the flags it stores). The card's
// Spec is the engineering resolution: hasStatus derives the four statuses
// from their owning state through the WorldContext's cross-store lookups;
// setStatus remains for non-derived extension flags only and asserts on the
// derived ones (there are no non-derived flags in v1, so v1 code has no
// caller — grep review: nothing writes statusFlags).
//
// consentsToPvp is THE consent check of the whole codebase (card Done-when):
// combat (3.2), the death paths (3.3), justice (4.1) and the duel/arena
// framework (2.7) all call it — never reimplement it.

#pragma once

#include <cstddef>
#include <cstdint>

#include "character/character.hpp" // Character
#include "core/constants.hpp"      // COMBAT_TIMEOUT_S (A-4)
#include "core/enums.hpp"          // StatusType, CooldownType (closed lists, task 1.1)
#include "core/types.hpp"          // characterId
#include "core/world_context.hpp"  // WorldContext + the war/justice seams

namespace harrowmere
{

// Derive the status from its owning state (design reference: "derived, never
// stored"). ctx may carry null seams — a null war registry means nobody is a
// combatant, a null justice store means nobody is arrested (D-SINK-1's
// null-false pattern, world_context.hpp).
bool hasStatus(const Character &c, StatusType flag, const WorldContext &ctx);

// Set a NON-derived status flag on the u32 storage. Every StatusType value
// that exists today is derived (D-CHAR-1), so calling this in v1 is a
// programmer error: aborts in debug, no-ops deterministically in release.
// The derived statuses change by changing their OWNING state (eventId,
// lastCombatTime, the war registry, justice's jailUntil map) — never here.
void setStatus(Character &c, StatusType flag, bool enabled);

// The doc's consent rule, verbatim (character.md §Character): the victim
// consents when the attacker is their accepted duel opponent, or when both
// stand in the same arena zone (the doc's shared-id consent; 0 = not in an
// arena — the != 0 guard keeps two outsiders from "consenting" through a
// shared 0). One-directional read of the VICTIM's fields: task 2.7 sets
// duelOpponent on both sides when a duel starts.
bool consentsToPvp(const Character &victim, const Character &attacker);

// Cooldown slots hold the worldTime at which the cooldown expires (task
// 2.0's field note). A cooldown is active strictly BEFORE its expiry:
// cooldowns[type] > now. The doc's canRespec reads the same slot as
// "cooldowns[type] <= worldTime" being allowed — active at expiry-1,
// free at expiry.
bool cooldownActive(const Character &c, CooldownType type, std::uint64_t now);

// Arm a cooldown: the slot holds the expiry stamp, so callers pass
// now + duration (card 2.1's setCooldown(c, type, now + duration)).
void setCooldown(Character &c, CooldownType type, std::uint64_t expiresAt);

} // namespace harrowmere
