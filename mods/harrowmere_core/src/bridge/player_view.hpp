#ifndef HARROWMERE_CORE_BRIDGE_PLAYER_VIEW_HPP
#define HARROWMERE_CORE_BRIDGE_PLAYER_VIEW_HPP

// player_view.hpp — the step-13 PlayerView fill as ONE tested free function
// (task 2.8: "the phase-2 playtest-review fixes").
//
// Phase 1's rebuildViews filled only the controller slice (position/worldTime/
// realTime/afk) and stopped — its comment deferred the rest to "Phase 2 fills
// them (2.5)" and phase 2 never came back, so every 7.3 panel read zeros (HUD
// `0/0 HP`, `Lv 0`, `Coin: 0`). This header is the repair: the full observable
// state is filled here, from the player's store record plus the read-only
// progression surfaces, and the plugin's rebuildViews becomes a thin caller.
//
// Contract (task 2.8 Spec, verbatim):
//   * The fill covers the published observable state: `name` (the creation
//     record), `level`, `currentHp`, `maxHp` (maxHpForLevel — A-42/A-43),
//     `currency`, `totalXp`, `availableXp`, `nextLevelCost` (levelCosts[level]
//     — A-7/A-8), `cachedPower`/`peakPower`, `stats`/`skills` +
//     getStatEffectiveness/getSkillEffectiveness, `statCap`/`skillCap`
//     (A-13..A-16), `cooldowns` by ordinal (2.1's slots), `appearanceCostNext`
//     (A-22/A-23), and the derived booleans (`inCombat` per A-4 from
//     lastCombatTime, `inEvent` from `eventId`).
//   * Zero-value rules stay: a field with no data keeps its zero — never a
//     made-up value. Fields the spec does not list (inWar, arrested, inDuel,
//     duelOpponent, arenaId, bag/equipped/recipes) keep their zero/empty
//     defaults exactly as before.
//   * The view is a snapshot rebuilt per frame (step 13, §4.4): the fill reads
//     THROUGH the store (const refs to the caller's records), never hands the
//     panels a Character*, and never mutates a store field. The cached power
//     fields are copied verbatim — recomputing them here would dirty-tick a
//     2.5 cache from a read path.
//   * PlayerView is NOT widened: the shape is published (§4.2) and 7.1's save
//     schema will read it; fill what exists.
//
// The controller slice (position/realTime/afk) and `worldTime` arrive as plain
// parameters: the sim-side authoritative transform is the controller's (3.10),
// and the WorldClock lives outside the store. Passing them as values keeps
// this function engine-free and directly testable.

#include <array>
#include <cstddef>
#include <vector>

#include "harrowmere_capability.hpp"   // the published views::PlayerView shape (never widened here)
#include "character/appearance.hpp"    // appearanceChangeCost (A-22/A-23)
#include "character/character.hpp"     // Character, PlayerCreation, maxHpForLevel
#include "character/status.hpp"        // hasStatus — the derived booleans' one home
#include "core/constants.hpp"          // SCALE (A-1), COMBAT_TIMEOUT_S (A-4)
#include "core/enums.hpp"              // StatType, SkillType, CooldownType, StatusType
#include "core/types.hpp"              // Position, characterId
#include "core/world_context.hpp"      // WorldContext (hasStatus's null-seam bundle)
#include "data/registry.hpp"           // DataRegistry (species multipliers, role rows)
#include "progression/progression.hpp" // levelCosts/statCaps/skillCaps, getSkillEffectiveness
#include "character/species.hpp"       // getStatEffectiveness

namespace harrowmere::bridge
{

// The next-level XP cost for `level` (A-7/A-8: levelCosts[level]), 0 at and
// beyond the table — the HUD's "0 at the cap" reading.
inline std::uint64_t nextLevelCostFor(
    std::uint16_t level
)
{
    const std::vector<std::uint64_t> &costs = levelCosts();
    return static_cast<std::size_t>(level) < costs.size() ? costs[level] : 0;
}

// The stat/skill cap for `level` (A-13..A-16: caps[level]), 0 beyond the
// table (deterministic recovery, §3.4 — the tables span [0, MAX_LEVEL]).
inline std::uint64_t capForLevel(
    const std::vector<std::uint64_t> &caps, std::uint16_t level
)
{
    return static_cast<std::size_t>(level) < caps.size() ? caps[level] : 0;
}

// THE PlayerView fill (task 2.8). `player` is the store record (read-only —
// the caller fetched it through the store this frame), `creation` the
// player-creation metadata (the name lives outside the doc struct, D-STORE-1),
// `data` the committed registry (species multipliers for
// getStatEffectiveness). `simPosition` is the sim-side authoritative player
// transform (the controller's, 3.10), `worldTime`/`realTime` the two clocks
// (R8), `afk` the controller's D-AFK-1 flag.
inline harrowmere::views::PlayerView buildPlayerView(
    const harrowmere::Character &player,
    const harrowmere::PlayerCreation &creation,
    const harrowmere::data::DataRegistry &data,
    const harrowmere::Position &simPosition,
    std::uint64_t worldTime,
    std::uint64_t realTime,
    bool afk
)
{
    harrowmere::views::PlayerView pv{};

    // ---- the controller slice (Phase-1 fields, unchanged shape) -------------
    pv.position.x = simPosition.x;
    pv.position.y = simPosition.y;
    pv.position.z = simPosition.z;
    pv.worldTime  = worldTime;
    pv.realTime   = realTime;
    pv.afk        = afk;

    // ---- the store record's vitals + progression ----------------------------
    pv.name          = creation.name; // the creation record (2.0), never a doc-Character field
    pv.level         = player.progression.level;
    pv.currentHp     = player.body.currentHp;
    pv.maxHp         = player.body.maxHp; // maxHpForLevel(level) by the store's creation invariant (A-42/A-43)
    pv.currency      = player.progression.currency; // i64: conviction fines may take it negative (4.3)
    pv.totalXp       = player.progression.totalXp;
    pv.availableXp   = player.progression.availableXp;
    pv.nextLevelCost = nextLevelCostFor(player.progression.level);
    pv.cachedPower   = player.cachedPower; // copied verbatim: the 2.5 cache is store state,
    pv.peakPower     = player.peakPower;   // recomputed by the pipeline, never by a view read

    // ---- raw points + effectiveness (the Character/Inventory panel reads) ---
    pv.stats  = player.progression.stats;
    pv.skills = player.progression.skills;
    for (std::size_t i = 0; i < pv.statEffectiveness.size(); ++i)
    {
        const auto stat         = static_cast<StatType>(i);
        pv.statEffectiveness[i] = getStatEffectiveness(data, player, stat);
    }
    for (std::size_t i = 0; i < pv.skillEffectiveness.size(); ++i)
    {
        const auto skill         = static_cast<SkillType>(i);
        pv.skillEffectiveness[i] = getSkillEffectiveness(player, skill);
    }
    pv.statCap  = capForLevel(statCaps(), player.progression.level);
    pv.skillCap = capForLevel(skillCaps(), player.progression.level);

    // ---- cooldowns by ordinal (2.1's slots; expiry worldTimes, 0 = expired) --
    pv.cooldowns = player.cooldowns;

    // ---- appearance (A-22/A-23: 50 + 10*level, paid from availableXp) -------
    pv.appearanceCostNext = appearanceChangeCost(player);

    // ---- derived statuses (D-CHAR-1: computed, never stored) ----------------
    // hasStatus's own derivation (status.cpp) with a null-seam bundle: the war
    // registry (6.2) and the justice jailUntil map (4.3) are not this fill's
    // fields — a null seam reads false, exactly the pre-landing reading.
    WorldContext statusCtx{};
    statusCtx.now = worldTime;
    pv.inCombat   = hasStatus(player, StatusType::IN_COMBAT, statusCtx);
    pv.inEvent    = hasStatus(player, StatusType::IN_EVENT, statusCtx);

    return pv;
}

} // namespace harrowmere::bridge

#endif // HARROWMERE_CORE_BRIDGE_PLAYER_VIEW_HPP
