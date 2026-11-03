// combat/death.cpp — Task 3.3 implementation (design reference §Death verbatim).
#include "combat/death.hpp"

#include <algorithm>
#include <cassert>

#include "character/species.hpp"
#include "character/status.hpp"
#include "combat/justice_sink.hpp"
#include "core/constants.hpp"
#include "core/constants_phase2.hpp"
#include "core/math.hpp"
#include "core/world_context.hpp"
#include "npc/lod.hpp"
#include "npc/roles.hpp"
#include "npc/state.hpp"
#include "progression/progression.hpp"
#include "progression/power.hpp"

namespace harrowmere
{

// ---------------------------------------------------------------- CorpseStore --

void CorpseStore::stageDeath(
    characterId victim,
    const Position &pos,
    std::uint64_t deathTime,
    std::vector<items::ItemStack> loot,
    std::int64_t lootCurrency
)
{
    Corpse c;
    c.victim       = victim;
    c.pos          = pos;
    c.deathTime    = deathTime;
    c.loot         = std::move(loot);
    c.lootCurrency = lootCurrency;
    c.discovered   = false;
    pending_.push_back(std::move(c));
}

void CorpseStore::materializePending()
{
    for (Corpse &c : pending_)
        corpses_[c.victim] = std::move(c);
    pending_.clear();
}

void CorpseStore::sweepDecayed(
    std::uint64_t now
)
{
    const std::uint64_t expiry = CORPSE_DECAY_DAYS * core::DAY_LENGTH_SECONDS;
    for (auto it = corpses_.begin(); it != corpses_.end();)
    {
        if (now - it->second.deathTime > expiry)
            it = corpses_.erase(it);
        else
            ++it;
    }
}

void CorpseStore::markDiscovered(
    characterId victim, characterId discoverer
)
{
    auto it = corpses_.find(victim);
    if (it != corpses_.end())
    {
        it->second.discovered   = true;
        it->second.discoveredBy = discoverer; // 4.1's delayed-report input
    }
}

Corpse *CorpseStore::find(
    characterId victim
)
{
    auto it = corpses_.find(victim);
    return it == corpses_.end() ? nullptr : &it->second;
}

void CorpseStore::restoreCorpse(
    const Corpse &corpse
)
{
    // One corpse per victim id; a duplicate means a corrupted save slipped
    // past the load validation (assert in debug; release skips the row).
    assert(corpses_.find(corpse.victim) == corpses_.end() && "CorpseStore::restoreCorpse: duplicate victim id");
    if (corpse.victim == 0 || corpses_.find(corpse.victim) != corpses_.end())
    {
        return;
    }
    corpses_.emplace(corpse.victim, corpse);
}

// ------------------------------------------------- the three doc death paths --

void onEnvironmentDeath(
    Character &victim, WorldContext & /*ctx*/
)
{
    // A-32: debt = 0.5x the victim's next level cost, clamped via applyXpDelta (R1/R4).
    const std::int64_t debt
        = static_cast<std::int64_t>(ENVIRONMENT_DEATH_PENALTY_FACTOR * levelCosts()[victim.progression.level] / SCALE);
    victim.progression.availableXp = applyXpDelta(victim.progression.availableXp, -debt);
}

void onNonconsensualPvpDeath(
    Character &victim, Character &killer, WorldContext &ctx
)
{
    // A-33: a full level, instant ("victim penalties stay instant").
    const std::int64_t loss
        = static_cast<std::int64_t>(PVP_VICTIM_PENALTY_FACTOR * levelCosts()[victim.progression.level] / SCALE);
    victim.progression.availableXp = applyXpDelta(victim.progression.availableXp, -loss);
    // die()'s routing table (3.3 pitfalls): only a PLAYER culprit generates evidence —
    // a player killed by an NPC takes the same instant penalty, silently.
    if (!ctx.characters->isPlayer(killer.id))
        return;
    if (isCriminalAct(CrimeType::MURDER, killer, victim, victim.body.position, ctx))
    {
        CrimeContext crime;
        crime.type      = CrimeType::MURDER;
        crime.culprit   = killer.id;
        crime.victim    = victim.id;
        crime.position  = victim.body.position; // victim's position (doc argument order)
        crime.worldTime = ctx.now;
        crime.witnesses = ctx.perception->perceiveWithin(victim.body.position, WITNESS_RADIUS_M);
        ctx.justice->recordCrime(crime);
    }
}

void onNpcKill(
    Character &victim, Character &killer, WorldContext & /*ctx*/
)
{
    // A-34/A-35: monsters are the XP faucet; NPCs gain XP dropped by players they
    // kill (design rule) — this handler runs for any killer species.
    const std::uint64_t loss       = NPC_VICTIM_PENALTY_FACTOR * levelCosts()[victim.progression.level] / SCALE;
    victim.progression.availableXp = applyXpDelta(victim.progression.availableXp, -static_cast<std::int64_t>(loss));
    const std::uint64_t gain       = loss * NPC_KILL_XP_GAIN / SCALE;
    killer.progression.totalXp += gain; // never decreases (R4)
    killer.progression.availableXp = applyXpDelta(killer.progression.availableXp, static_cast<std::int64_t>(gain));
    markPowerDirty(killer);
}

bool isCriminalAct(
    CrimeType type, const Character &culprit, const Character &victim, const Position &position, WorldContext &ctx
)
{
    if (consentsToPvp(victim, culprit))
        return false; // duels/arenas
    if (!isSapient(*ctx.data, victim.species))
        return false; // hunting deer creates no crime
    if (ctx.world->isInSafeZone(position))
        return true; // sapient kill in a safe zone: always criminal
    if (ctx.factions->isAtWar(culprit.faction, victim.faction))
        return false;
    const territoryId terr = ctx.world->territoryAt(position);
    if (type == CrimeType::MURDER)
        return !ctx.justice->hasLethalWarrantIn(victim.id, terr);
    return !ctx.justice->hasWarrantIn(victim.id, terr);
}

// ------------------------------------------------------------- die() routing --

namespace
{

std::vector<items::ItemStack> collectLoot(
    const Character &victim, const NpcState *npc, WorldContext &ctx
)
{
    std::vector<items::ItemStack> loot;
    for (const std::uint64_t itemId : victim.equippedItems)
        loot.push_back(items::ItemStack{itemId, 1});
    if (npc != nullptr)
    {
        for (const auto &stack : npc->inventory.stacks)
            loot.push_back(items::ItemStack{stack.first, stack.second});
    } else if (items::Inventory *bag = ctx.bags->bagOf(victim.id); bag != nullptr)
    {
        for (const auto &stack : bag->stacks)
            loot.push_back(items::ItemStack{stack.first, stack.second});
    }
    return loot;
}

void clearBelongings(
    Character &victim, NpcState *npc, WorldContext &ctx
)
{
    victim.equippedItems.clear();
    if (npc != nullptr)
    {
        npc->inventory.stacks.clear();
        npc->inventory.equipped.clear();
    } else if (items::Inventory *bag = ctx.bags->bagOf(victim.id); bag != nullptr)
    {
        bag->stacks.clear();
        bag->equipped.clear();
    }
}

} // namespace

void die(
    Character &victim, characterId killerId, WorldContext &ctx
)
{
    if (victim.body.currentHp != 0)
        return; // one death, one handler
    assert(victim.progression.availableXp >= -MAX_XP && "XP debt clamp invariant (R4)");

    Character *killer  = (killerId != 0 && ctx.characters->exists(killerId)) ? &ctx.characters->get(killerId) : nullptr;
    NpcState *npcState = ctx.npcs->contains(victim.id) ? &ctx.npcs->get(victim.id) : nullptr;
    const Position deathPosition = victim.body.position;

    if (ctx.characters->isPlayer(victim.id))
    {
        if (killer != nullptr)
            onNonconsensualPvpDeath(victim, *killer, ctx);
        else
            onEnvironmentDeath(victim, ctx);
    } else
    {
        if (killer != nullptr)
        {
            onNpcKill(victim, *killer, ctx);
            // Murder evidence for sapient NPC victims (guards!) — from
            // onNonconsensualPvpDeath's logic via the isCriminalAct routing here:
            // only player culprits generate evidence (design rule).
            if (ctx.characters->isPlayer(killerId) && isSapient(*ctx.data, victim.species)
                && !consentsToPvp(victim, *killer)
                && isCriminalAct(CrimeType::MURDER, *killer, victim, deathPosition, ctx))
            {
                CrimeContext crime;
                crime.type      = CrimeType::MURDER;
                crime.culprit   = killer->id;
                crime.victim    = victim.id;
                crime.position  = deathPosition;
                crime.worldTime = ctx.now;
                crime.witnesses = ctx.perception->perceiveWithin(deathPosition, WITNESS_RADIUS_M);
                ctx.justice->recordCrime(crime);
            }
        }
        // No killer: natural-cause-style silent removal — no evidence, no XP.
    }

    // 4.7's village-slaughter tally runs BEFORE the loot staging so the
    // marker the fifth kill raises already zeroes that corpse's loot currency
    // ("corpse loot currency zeroed (loot items remain)"). The sink call is
    // the D-SINK-1 door — combat/death never includes justice internals.
    if (killer != nullptr && npcState != nullptr && isSapient(*ctx.data, victim.species))
    {
        ctx.justice->onNpcKilled(victim.id, killer->id);
    }

    // Loot first (v1: everything drops), then the corpse is staged for step 11.
    const bool killed                  = killer != nullptr;
    std::vector<items::ItemStack> loot = killed ? collectLoot(victim, npcState, ctx) : std::vector<items::ItemStack>{};
    // The victim's purse drops with the body; 4.7's massacre rule zeroes it
    // (the sink answers 0 when the marker is active — "loot items remain").
    const std::int64_t lootCurrency
        = killed ? ctx.justice->massacreLootCurrency(killer->id, deathPosition, victim.progression.currency) : 0;
    clearBelongings(victim, npcState, ctx);
    ctx.corpses->stageDeath(victim.id, deathPosition, ctx.now, std::move(loot), lootCurrency);

    // Critical-role deaths keep settlements functional (3.6) — the NpcState is still
    // alive here; the replacement flow captures the ROLE KEY and territory, never a
    // reference to the dead NPC (3.3/3.6 pitfalls).
    if (npcState != nullptr)
        onCriticalNpcDeath(*npcState, ctx);

    if (ctx.characters->isPlayer(victim.id))
    {
        respawnPlayer(victim, deathPosition, ctx);
        markPowerDirty(victim); // everything was unequipped into the corpse
    } else
    {
        ctx.npcs->erase(victim.id);
        ctx.characters->remove(victim.id);
    }
}

void respawnPlayer(
    Character &player, const Position &deathPosition, WorldContext &ctx
)
{
    player.body.currentHp = player.body.maxHp;
    player.lastCombatTime = 0;
    player.body.position  = ctx.world->safeZoneSpawnFor(deathPosition);
}

// ------------------------------------------------------------------ systems --

void CorpseDecaySystem::run(
    WorldContext &ctx
)
{
    ctx.corpses->sweepDecayed(ctx.now);
}

void AftermathSystem::run(
    WorldContext &ctx
)
{
    // Step 11 handles only corpse/spawn placement (§4.4; D-TICK-1 keeps deaths in
    // step 10). Staged deaths become entities; undiscovered corpses flip their
    // discovered flag when a living character enters WITNESS_RADIUS (A-60) — the
    // delayed-report trigger for 4.1. Every character within that radius of the
    // anchor is in the active set (it covers the whole MID band), so scanning the
    // active set + the player is exact — no full-population pass (3.5's rule).
    // The FIRST character in that scan order is the discoverer (deterministic:
    // player, then NEAR, then MID ascending); the flip hears justice through the
    // sink seam (D-SINK-1 — this file never includes justice internals).
    ctx.corpses->materializePending();
    for (auto &[victimId, corpse] : ctx.corpses->all())
    {
        if (corpse.discovered)
            continue;
        characterId discoverer  = 0;
        const auto withinRadius = [&](characterId who) {
            if (discoverer != 0 || !ctx.characters->exists(who))
                return;
            const Character &c = ctx.characters->get(who);
            const double dx    = c.body.position.x - corpse.pos.x;
            const double dz    = c.body.position.z - corpse.pos.z; // planar (R5)
            if (dx * dx + dz * dz <= WITNESS_RADIUS_M * WITNESS_RADIUS_M)
                discoverer = who;
        };
        const characterId player = ctx.world->playerCharacterId();
        if (player != 0)
            withinRadius(player);
        if (discoverer == 0)
            for (const characterId id : ctx.lod->activeNear)
                withinRadius(id);
        if (discoverer == 0)
            for (const characterId id : ctx.lod->activeMid)
                withinRadius(id);
        if (discoverer != 0)
        {
            ctx.corpses->markDiscovered(victimId, discoverer);
            ctx.justice->onBodyDiscovered(victimId, discoverer); // 4.1's delayed report
        }
    }
}

} // namespace harrowmere
