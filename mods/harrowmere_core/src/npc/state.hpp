// npc/state.hpp — Task 3.4: the parallel NPC-only record (D-STORE-1).
//
// Everything NPC-only lives here, NEVER inside Character — the doc struct stays
// verbatim so save/load (7.1) mirrors the docs. Fields marked (card) come straight
// from task 3.4's spec; the rest are the per-tick/roll state the tier workloads
// (3.5) and duties (3.6) need, and are in 7.1's do-not-save list where noted.
#pragma once
#include <cstdint>
#include <map>
#include <string>

#include "core/constants.hpp"
#include "core/constants_phase2.hpp"
#include "core/types.hpp"
#include "items/inventory.hpp" // Inventory (2.4; the bag is NpcState-owned, D-STORE-1)
#include "items/inventory.hpp"

namespace harrowmere
{

enum LodTier
{
    NEAR,
    MID,
    FAR,
    LodTier_COUNT
};

struct Needs
{
    std::uint64_t hunger = SCALE; // 0..SCALE each; decay per game-hour (A-37)
    std::uint64_t rest   = SCALE;
    std::uint64_t wealth = SCALE;
    std::uint64_t social = SCALE;

    std::uint64_t lowest() const
    {
        std::uint64_t m = hunger;
        if (rest < m)
            m = rest;
        if (wealth < m)
            m = wealth;
        if (social < m)
            m = social;
        return m;
    }
};

// 0..SCALE trait drawn from the role's integrityRange (roles.json); 3.9 reads it
// as a witness-weight input.
using Integrity = std::uint64_t;

// Per-NPC suspicion memory (4.5) — declared here, filled by 4.5 (task-3.4 card).
// The 4.5 store keeps the score map (per observed id) plus the two stamps the
// card's rules read: the per-entry last-sighting stamp (stranger memories
// lapse after STRANGER_MEMORY_S, A-56) and the lazy decay stamp (SUSPICION_DECAY
// applies per game-day since it). The card's SuspicionMemoryEntry (justice/
// suspicion.hpp) is the VIEW assembled over these fields — TrustMemory stays
// the storage (it lives on NpcState, D-STORE-1).
struct TrustMemory
{
    std::uint32_t suspicionScore = 0;
    std::map<characterId, std::uint64_t> perCharacterScore;
    std::map<characterId, std::uint64_t> lastSeen; // per observed id (4.5)
    std::uint64_t lastSuspicionDecay = 0;          // lazy decay stamp (4.5)
};

// Needs are the only per-tick movement driver in NEAR tier (3.4). Flee is
// 4.6's evacuation intent (a civilian marked fleeing during a manhunt;
// DEVIATIONS.md carries the closed-enum extension row).
enum class NpcIntentKind : std::uint8_t
{
    None,
    Eat,
    Rest,
    Earn,
    Socialize,
    Flee
};

struct NpcIntent
{
    NpcIntentKind kind = NpcIntentKind::None;
    characterId target = 0; // vendor/affinity target; 0 = placeless (home/work)
};

struct NpcState
{
    // ---- task-3.4 card fields ----
    std::string name;
    std::uint64_t birthWorldTime = 0; // persisted (7.1); age is recomputed, never stored
    std::string roleKey;              // roles.json key; "" = none
    Needs needs;
    Integrity integrity = 0; // 0..SCALE trait from role range
    TrustMemory trust;
    std::map<characterId, std::int64_t> affinity;        // -SCALE..SCALE (3.9)
    std::map<std::string, std::uint64_t> actionCounters; // kills_non_sapient, crafts (2.3 reqActions)
    LodTier tier              = LodTier::FAR;
    territoryId homeTerritory = 0; // migration state
    regionId currentRegion    = 0;

    // ---- card-mandated companions from other Phase-2 cards ----
    items::Inventory
        inventory; // 2.4: the bag is NpcState-owned (D-STORE-1)       // 2.4: the bag is NpcState-owned (D-STORE-1)
    bool invulnerable = false; // D-INVULN-1 (A.2); players are always false
    bool migrated     = false; // 3.6: spawnBackupNpc marks frontier migrants

    // ---- 4.6's evacuation state (the manhunt's fleeing marker) ----
    // NEAR civilians carry the Flee intent toward home/safe zone; MID/FAR
    // civilians are abstractly marked by the same flag. fleeingUntil is the
    // manhunt's end (the warrant's expiry) — the flag self-clears on read.
    bool fleeing               = false;
    std::uint64_t fleeingUntil = 0;

    // ---- per-tick sim state (3.4/3.5/3.6) ----
    // Persisted since the 8.3 as-built (the save's per-NPC "perTick" block):
    // the intent/migration walks and the pacing stamps shape what the next
    // ticks do, and the 7.1 continuation contract (a loaded world continues
    // the kept-alive world's exact random sequence) needs them on the save
    // once NEAR-tier work runs from tick 1 — the original do-not-save
    // reading predated RNG-drawing NEAR work existing on day 0 (the 8.3
    // spawn placement made it so; the soak's roundtrip caught the
    // fresh-intent re-pick drawing a different slice than the kept-alive
    // world's live intents).
    std::uint64_t needsCarry                 = 0; // game-seconds toward the next hourly decay
    std::uint64_t nextDutyWorldTime          = 0; // hourly role-duty slot
    std::uint64_t lastMigrationRollWorldTime = 0; // monthly roll cadence
    NpcIntent currentIntent;
    bool migrating = false; // NEAR/MID walk to the new home
    Position migrationTarget;

    // A-38: (worldTime - birthWorldTime) / YEAR_GAME_SECONDS.
    std::uint32_t ageYears(
        std::uint64_t worldTime
    ) const
    {
        return static_cast<std::uint32_t>((worldTime - birthWorldTime) / YEAR_GAME_SECONDS);
    }
};

class NpcStore
{
public:
    NpcState &get(
        characterId id
    )
    {
        return states_.at(id);
    }
    const NpcState &get(
        characterId id
    ) const
    {
        return states_.at(id);
    }
    bool contains(
        characterId id
    ) const
    {
        return states_.count(id) != 0;
    }

    NpcState &create(
        characterId id
    )
    {
        auto [it, inserted] = states_.emplace(id, NpcState{});
        return it->second;
    }

    void erase(
        characterId id
    )
    {
        states_.erase(id);
    }

    std::map<characterId, NpcState> &all()
    {
        return states_;
    }
    const std::map<characterId, NpcState> &all() const
    {
        return states_;
    }

private:
    std::map<characterId, NpcState> states_; // deterministic iteration (R7)
};

} // namespace harrowmere
