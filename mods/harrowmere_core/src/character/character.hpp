#ifndef HARROWMERE_CORE_CHARACTER_CHARACTER_HPP
#define HARROWMERE_CORE_CHARACTER_CHARACTER_HPP

// character.hpp - THE doc-verbatim character record (task 2.0).
//
// The design reference's Character struct exists here field-for-field, with
// the doc's field names, field order, types and defaults (the field order
// below IS the design reference's declaration order; the pinned tests
// static_assert both presence and types). Players and NPCs share the
// structure: design rule "NPC actors share the same structure" - the doc's
// own note ("species exists on every character ... NPC actors carry the same
// eventId, statusFlags, and cachedPower fields").
//
// D-STORE-1 (task 2.0): everything NPC-only - name, age, role, needs, brain,
// LOD tier, relationship lists - lives in NpcState (task 3.4,
// src/npc/state.hpp), NEVER inside Character. The struct stays exactly the
// doc's so save/load (7.1) mirrors the doc; the compile-time reflection
// table in tests/test_character_store.cpp pins the negative too (no role /
// age / needs / jail fields exist here - jail is explicitly not on the
// character, design rule: "Jail state lives here, not on Player").
//
// Field notes from the design reference (character.md):
//   * statusFlags  - u32 storage for NON-derived flags only (D-CHAR-1, task
//     2.1: StatusType values are computed from their owning state, never
//     stored). Nothing in v1 writes bits; the field exists so the doc struct
//     is verbatim and future flags don't resize saves.
//   * duelOpponent / arenaId - the two PvP-consent channels (2.1's
//     consentsToPvp, 2.7's duel/arena framework); 0 = not dueling / not in
//     an arena.
//   * equippedItems - ITEM INSTANCE ids (task 2.4), which is what makes
//     getGearScore well-defined: two iron swords score twice.
//   * cooldowns - indexed by CooldownType (core/enums.hpp, R9); each slot
//     holds the worldTime at which the cooldown expires (2.1).
//   * cachedPower / peakPower / powerDirty - the cached power pipeline
//     (task 2.5): peakPower is the anti-sandbagging watermark (persists,
//     never decreases), cachedPower does not persist and is recomputed via
//     powerDirty on load. Docs default powerDirty to true.
//   * progression.availableXp is i64 and may go negative to -MAX_XP exactly
//     (R4, applyXpDelta); totalXp is u64 and never decreases;
//     currency is i64 and may go negative only through conviction fines.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/constants.hpp" // BASE_HP (A-42), HP_PER_LEVEL (A-43) - maxHpForLevel
#include "core/enums.hpp"     // CooldownType, StatType, SkillType (closed lists, task 1.1)
#include "core/types.hpp"     // characterId, factionId, speciesId, Position (task 0.2)

namespace harrowmere
{

// Body - design reference (Conventions), verbatim.
struct Body
{
    std::uint64_t currentHp = 0;
    std::uint64_t maxHp     = 0;
    Position position{};
};

// Progression - design reference (Conventions), verbatim. stats/skills are
// sized arrays over the closed StatType/SkillType lists (R9); the docs'
// "arrays start at 0" is the creation floor (task 2.0: every purchase path
// grows them from there, never below).
struct Progression
{
    std::uint64_t totalXp    = 0; // lifetime XP; never decreases (design reference)
    std::int64_t availableXp = 0; // spendable XP; negative = XP debt (R4)
    std::uint16_t level      = 1; // a fresh character exists at level 1 (task 2.0's pinned starting state)
    std::array<std::uint64_t, static_cast<std::size_t>(StatType::COUNT)> stats{};
    std::array<std::uint64_t, static_cast<std::size_t>(SkillType::COUNT)> skills{};
    std::int64_t currency = 0; // negative = debt (conviction fines only, 4.3)
};

// Character - design reference (character.md), fields verbatim in doc order.
// For player and NPC alike (D-STORE-1 above; NPC-only state lives in
// NpcState, task 3.4).
struct Character
{
    characterId id            = 0;
    speciesId species         = 0; // players are species 0, the hardcoded "player" row
    std::uint32_t statusFlags = 0; // non-derived flags only (D-CHAR-1)

    characterId duelOpponent = 0; // 0 = not dueling
    std::uint64_t arenaId    = 0; // 0 = not in an arena

    std::vector<std::uint64_t> appearanceComponents; // social-perception input, never stats (2.6)
    std::vector<std::uint64_t> equippedItems;        // item instance ids -> getGearScore (2.4)

    Body body{};
    Progression progression{};

    std::array<std::uint64_t, static_cast<std::size_t>(CooldownType::COUNT)> cooldowns{};

    factionId faction = 0; // 0 = factionless

    std::uint64_t eventId        = 0; // 0 = not in an event
    std::uint64_t lastCombatTime = 0; // worldTime stamp of the last damage application (2.1's IN_COMBAT)

    std::uint64_t cachedPower = 0;    // 2.5's cache; not persisted (recomputed via powerDirty)
    std::uint64_t peakPower   = 0;    // 2.5's watermark; persists, never decreases
    bool powerDirty           = true; // docs default true
};

// The doc's HP rule (task 2.0, A-42 + A-43): maxHp = BASE_HP + level *
// HP_PER_LEVEL. v1 pins HP growth to LEVEL only - species do NOT modify HP
// (the doc species table lists no HP column; do not invent one). Both
// creation paths (createPlayer, createNpc) derive maxHp through this one
// function so the formula exists exactly once.
constexpr std::uint64_t maxHpForLevel(
    std::uint16_t level
) noexcept
{
    return BASE_HP + static_cast<std::uint64_t>(level) * HP_PER_LEVEL;
}

// PlayerCreation - the player-creation flow's input (task 2.0). Plain data;
// the store records it verbatim as the SP player's creation metadata (the
// name and class preset are NOT doc-Character fields, so they never touch
// the doc struct).
struct PlayerCreation
{
    // The display name: free text from the creation UI, or a draw from the
    // package's data/names/ tables - both arrive here as resolved text (the
    // name tables' loader is worldgen-side, task 1.5; the store has no RNG
    // and draws nothing). Recorded verbatim; presentation (7.3) reads it.
    std::string name;

    // Appearance components as component ids. Empty = the v1 default set:
    // no component vocabulary exists in v1 (no data file, no Appendix A row
    // invents one), and 7.2 colors characters by species.json's appearance,
    // not by components. The field exists so the doc struct's
    // appearanceComponents carries the creation input verbatim.
    std::vector<std::uint64_t> appearanceComponents;

    // Optional classes.json preset key ("warrior", ...); "" = none. The
    // class's startingSkills/statWeights are NPC-generation data (1.5); v1
    // does NOT grant a player class bonuses at creation - the pinned
    // starting state below is class-independent (level 1, all stats/skills
    // at the doc floor 0). The key is recorded for the Character panel and
    // later class-gated content; validating it against the registry is the
    // caller's job (the store has no DataRegistry dependency).
    std::string classKey;
};

} // namespace harrowmere

#endif // HARROWMERE_CORE_CHARACTER_CHARACTER_HPP
