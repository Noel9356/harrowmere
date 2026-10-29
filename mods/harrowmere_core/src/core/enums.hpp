// enums.hpp — THE closed enum lists for Harrowmere.
//
// ============================================================================
//  TASK 1.1 STUB — TO BE OVERWRITTEN WHEN TASK 1.1 LANDS
// ============================================================================
//  This header is a faithful stub of the enum registry that task 1.1 owns
//  (phase-1-walk.md, card 1.1). It is shipped here ONLY so that task 1.2's
//  loaders can compile and pass their pinned tests in isolation, before
//  task 1.1 is merged.
//
//  The values below mirror EXACTLY what task 1.1's spec pins:
//    * StatusType     — bit flags (D-CHAR-1, derived; values never stored)
//    * CooldownType   — APPEARANCE, RESPEC_STAT, RESPEC_SKILL, COUNT
//    * StatType       — STR/DEF/AGI/INT/END/PER, COUNT
//    * SkillType      — MELEE/ARCHERY/STEALTH/ARCANA/SURVIVAL/PERSUASION/CRAFTING/ESCAPE, COUNT
//    * CrimeType      — MURDER/ASSAULT/THEFT/ARSON/TRESPASS/POACHING/SMUGGLING/OBSTRUCTION, COUNT
//    * ActionType     — DAMAGE/HEALING/OBJECTIVE, COUNT
//    * TerritoryType  — VILLAGE/CITY/KINGDOM/EMPIRE, COUNT
//    * SocialClassId  — NONE/COMMONER/NOBLE/ROYAL, COUNT
//    * EvidenceStatus — UNKNOWN/INVESTIGATING/WANTED/CONVICTED/ARCHIVED (no COUNT: not array-indexed)
//    * TrustTier      — STRANGER/WATCHED/DISTRUSTED/SHUNNED
//    * SuspicionAct   — CROUCH/UNLIT/MASKED/TRESPASS/BLOODSTAINED/FLEE (no COUNT)
//
//  When task 1.1 lands, REPLACE this file in full. The enum tag names and
//  enumerator order below MUST match task 1.1 exactly so the rest of the
//  game's code keeps compiling.
//
//  R9 (§3.2): every game enum that backs a sized array ends in a `COUNT`
//  sentinel exactly as the design docs do; sized arrays use
//  `std::array<T, N>` with `N = Enum::COUNT`.
// ============================================================================
#pragma once

#include <cstdint>   // std::uint32_t

namespace harrowmere {

// StatusType — bit flags (D-CHAR-1, task 2.1). The enum exists for hasStatus;
// the actual values are *derived* at runtime from event/combat/war/jail state
// and never stored on Character.statusFlags (which holds non-derived flags
// only). See phase-1-walk.md card 2.1 / common.md D-CHAR-1.
enum class StatusType : std::uint32_t {
    IN_COMBAT = 1u << 0,
    IN_EVENT  = 1u << 1,
    IN_WAR    = 1u << 2,
    ARRESTED  = 1u << 3,
    // And more — extend only via deviation row (§1.4)
};

enum class CooldownType {
    APPEARANCE,
    RESPEC_STAT,
    RESPEC_SKILL,
    // And more — extend only via deviation row (§1.4)
    COUNT
};

enum class StatType {
    STRENGTH,
    DEFENCE,
    AGILITY,
    INTELLIGENCE,
    ENDURANCE,
    PERCEPTION,
    COUNT
};

enum class SkillType {
    MELEE,
    ARCHERY,
    STEALTH,
    ARCANA,
    SURVIVAL,
    PERSUASION,
    CRAFTING,
    ESCAPE,
    COUNT
};

enum class CrimeType {
    MURDER,
    ASSAULT,
    THEFT,
    ARSON,
    TRESPASS,
    POACHING,
    SMUGGLING,
    OBSTRUCTION,
    COUNT
};

enum class ActionType {
    DAMAGE,
    HEALING,
    OBJECTIVE,
    COUNT
};

enum class TerritoryType {
    VILLAGE,
    CITY,
    KINGDOM,
    EMPIRE,
    COUNT
};

enum class SocialClassId {
    NONE,
    COMMONER,
    NOBLE,
    ROYAL,
    COUNT
};

// EvidenceStatus has no COUNT sentinel — it is never used as an array bound.
// Pinned by justice.md / common.md task 4.x.
enum class EvidenceStatus {
    UNKNOWN,
    INVESTIGATING,
    WANTED,
    CONVICTED,
    ARCHIVED
};

enum class TrustTier {
    STRANGER,
    WATCHED,
    DISTRUSTED,
    SHUNNED
};

// SuspicionAct — pinned list of suspicion trigger acts (design reference
// §Suspicion and Trust; suspicion.json's `act` field is one of these).
// No COUNT sentinel — the list is closed but not array-indexed.
enum class SuspicionAct {
    CROUCH,
    UNLIT,
    MASKED,
    TRESPASS,
    BLOODSTAINED,
    FLEE
};

} // namespace harrowmere
