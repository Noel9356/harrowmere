// combat/death.hpp — Task 3.3: the three doc death paths, isCriminalAct verbatim,
// corpse entities, respawn. All justice calls go through the JusticeSink (D-SINK-1);
// combat/death code never includes justice internals.
#pragma once
#include <cstdint>
#include <map>
#include <vector>

#include "core/enums.hpp" // CrimeType (1.1's closed list)
#include "core/scheduler.hpp"
#include "core/types.hpp"
#include "items/inventory.hpp" // ItemStack (the Corpse loot rows)

namespace harrowmere
{

struct WorldContext;
struct Character;
struct NpcState;

// Doc struct verbatim (3.3 spec). Loot = equipped + bag (v1: everything drops);
// natural deaths stage a corpse with no loot at all.
struct Corpse
{
    characterId victim = 0;
    Position pos;
    std::uint64_t deathTime = 0;
    std::vector<items::ItemStack> loot;
    bool discovered           = false; // flips when a living character enters WITNESS_RADIUS
    characterId discoveredBy  = 0;     // 4.1's delayed-report input: WHO flipped it (0 = anonymous)
    std::int64_t lootCurrency = 0;     // the victim's purse that drops with the body (4.7's
                                       // massacre rule zeroes it — "loot items remain");
                                       // DEVIATIONS.md carries the additive-field row
};

// Corpse entities live in WorldStore (3.3); until 7.1's save schema absorbs the
// member, the store ships standalone and is referenced from WorldContext.
class CorpseStore
{
public:
    // die() stages a death at commit time (step 10); step 11 materializes the entity.
    void stageDeath(
        characterId victim,
        const Position &pos,
        std::uint64_t deathTime,
        std::vector<items::ItemStack> loot,
        std::int64_t lootCurrency
    );

    // Step 11 (Aftermath): pending staged deaths become corpse entities.
    void materializePending();

    // Step 1 (Sweeps): decay after CORPSE_DECAY_DAYS (A-36).
    void sweepDecayed(std::uint64_t now);

    // Step 11: discovery trigger for 4.1's delayed reports. `discoverer` rides
    // the record (0 = anonymous); the justice sink hears the flip through the
    // D-SINK-1 seam (onBodyDiscovered) — this store never includes justice.
    void markDiscovered(characterId victim, characterId discoverer);

    Corpse *find(characterId victim);
    const std::map<characterId, Corpse> &all() const
    {
        return corpses_;
    }

    // Restore one saved corpse verbatim (task 7.1's load path; the id is the
    // victim's — one corpse per victim). A duplicate is a programmer error
    // (assert in debug; release logs and skips deterministically). The staged
    // pending_ list is NOT restorable: staging is same-tick state (deaths
    // stage at step 10 and materialize at step 11 — §4.4), empty at every
    // save point the runtime can take.
    void restoreCorpse(const Corpse &corpse);
    std::size_t pendingCount() const
    {
        return pending_.size();
    }
    std::size_t size() const
    {
        return corpses_.size();
    }

private:
    std::map<characterId, Corpse> corpses_; // one per victim id (R7 order)
    std::vector<Corpse> pending_;
};

// ---- the three doc death paths (design reference §Death, verbatim) ----
void onEnvironmentDeath(Character &victim, WorldContext &ctx);
void onNonconsensualPvpDeath(Character &victim, Character &killer, WorldContext &ctx);
void onNpcKill(Character &victim, Character &killer, WorldContext &ctx);

// isCriminalAct — the full gate chain, design-reference verbatim. Reads the VICTIM's
// position (the doc's argument order; evidence snapshots that territory — 3.3 pitfall).
bool isCriminalAct(
    CrimeType type, const Character &culprit, const Character &victim, const Position &position, WorldContext &ctx
);

// The one-death-one-handler routing table (3.3 pitfalls):
//   player victim -> PvP path (killer) / environment path (no killer);
//   NPC victim    -> onNpcKill (any killer; XP faucet) + murder evidence when the
//                    killer is a player; no killer -> silent removal (no evidence).
// Corpse placement is STAGED here and materialized in step 11 (§4.4); deaths apply
// immediately inside step 10 (D-TICK-1).
void die(Character &victim, characterId killerId, WorldContext &ctx);

// Player respawn: at the safe zone's kingdom spawn point; penalties already applied
// ("victim penalties stay instant"); body resets to maxHp, lastCombatTime = 0.
void respawnPlayer(Character &player, const Position &deathPosition, WorldContext &ctx);

// Step 1 (Sweeps): corpse decay expiry.
class CorpseDecaySystem final : public IStepSystem
{
public:
    void run(WorldContext &ctx) override;
};

// Step 11 (Aftermath): corpse/loot placement + discovery checks.
class AftermathSystem final : public IStepSystem
{
public:
    void run(WorldContext &ctx) override;
};

} // namespace harrowmere
