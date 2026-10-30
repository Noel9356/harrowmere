// enums.hpp - THE closed enum lists for Harrowmere (task 1.1).
//
// common.md section 1.4: "The docs sometimes say '...and more' for enums.
// The full pinned enum lists are in task 1.1 - those are the lists." The
// lists below are therefore CLOSED: extending any of them requires a
// deviation row (section 1.4).
//
// R9 (section 3.2): every game enum that backs a sized array ends in a
// COUNT sentinel exactly as the design docs do; sized arrays use
// std::array with N = Enum::COUNT. EvidenceStatus, TrustTier and
// SuspicionAct are closed lists that are never array-indexed, so they
// carry no COUNT - exactly as the card pins them.
//
// Notes pinned with the lists (card 1.1):
//   * StatusType values are DERIVED (D-CHAR-1, task 2.1) - the enum exists
//     for hasStatus; statusFlags stores non-derived flags only.
//   * POACHING / SMUGGLING / OBSTRUCTION close the docs' "and more" with
//     the crimes the docs' own prose requires (poaching: unlawful kill of
//     a non-sapient in protected territory, 4.1; smuggling: the
//     citizen-justice rule, 4.7; obstruction: bribery failure, 4.1).
//   * ENDURANCE / PERCEPTION are the two added stats exposure (3.8) and
//     spotting (3.9) need; species JSON gives them multipliers (default
//     1.0 when a row omits them).
//   * ESCAPE backs the jail-escape skill check (4.3); CRAFTING backs
//     recipes (2.4); ARCANA backs the mana rules (2.3, D-LORE-1).
#pragma once

#include <cstdint> // std::uint32_t

namespace harrowmere
{

// StatusType - bit flags (D-CHAR-1, task 2.1). The enum exists for
// hasStatus; the values are derived at runtime from event/combat/war/jail
// state and never stored as-is on Character.statusFlags.
enum class StatusType : std::uint32_t
{
    IN_COMBAT = 1u << 0,
    IN_EVENT  = 1u << 1,
    IN_WAR    = 1u << 2,
    ARRESTED  = 1u << 3,
};

enum class CooldownType
{
    APPEARANCE,
    RESPEC_STAT,
    RESPEC_SKILL,
    COUNT
};

enum class StatType
{
    STRENGTH,
    DEFENCE,
    AGILITY,
    INTELLIGENCE,
    ENDURANCE,
    PERCEPTION,
    COUNT
};

enum class SkillType
{
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

enum class CrimeType
{
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

enum class ActionType
{
    DAMAGE,
    HEALING,
    OBJECTIVE,
    COUNT
};

enum class TerritoryType
{
    VILLAGE,
    CITY,
    KINGDOM,
    EMPIRE,
    COUNT
};

enum class SocialClassId
{
    NONE,
    COMMONER,
    NOBLE,
    ROYAL,
    COUNT
};

// No COUNT sentinel - never used as an array bound (card 1.1).
enum class EvidenceStatus
{
    UNKNOWN,
    INVESTIGATING,
    WANTED,
    CONVICTED,
    ARCHIVED
};

enum class TrustTier
{
    STRANGER,
    WATCHED,
    DISTRUSTED,
    SHUNNED
};

// Pinned suspicion acts (design reference, Suspicion and Trust);
// suspicion.json's act field is one of these. No COUNT sentinel - the
// list is closed but not array-indexed.
enum class SuspicionAct
{
    CROUCH,
    UNLIT,
    MASKED,
    TRESPASS,
    BLOODSTAINED,
    FLEE
};

} // namespace harrowmere
