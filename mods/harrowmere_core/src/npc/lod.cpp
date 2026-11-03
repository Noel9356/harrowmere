// npc/lod.cpp — Task 3.5 implementation.
#include "npc/lod.hpp"

#include <algorithm>
#include <cmath>

#include "character/character.hpp"
#include "core/constants_phase2.hpp"
#include "core/world_context.hpp"

namespace harrowmere
{

void updateNpcSimulation(
    NpcState &npc, double distance
)
{
    // Design reference §LOD, verbatim. Enter NEAR below NEAR_DISTANCE * TIER_MARGIN;
    // leave NEAR above NEAR_DISTANCE / TIER_MARGIN — the two directional tests pin
    // both comparisons (62.5/60 m entering, 40/50 m leaving).
    if (npc.tier != LodTier::NEAR && distance < NEAR_DISTANCE_M * TIER_MARGIN)
        npc.tier = LodTier::NEAR;
    else if (npc.tier == LodTier::NEAR && distance > NEAR_DISTANCE_M / TIER_MARGIN)
        npc.tier = LodTier::MID;
    else if (npc.tier != LodTier::MID && distance < MID_DISTANCE_M * TIER_MARGIN)
        npc.tier = LodTier::MID;
    else if (npc.tier == LodTier::MID && distance > MID_DISTANCE_M / TIER_MARGIN)
        npc.tier = LodTier::FAR;
}

namespace
{
inline double planarDistance(
    const Position &a, const Position &b
)
{
    const double dx = a.x - b.x;
    const double dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz); // planar x,z (R5)
}
} // namespace

void LodSystem::run(
    WorldContext &ctx
)
{
    // D-LOD-1: the anchor is the player; with no player in-world, the beginner
    // safe-zone spawn. SP has one anchor.
    const characterId playerId = ctx.world->playerCharacterId();
    const Position anchor      = (playerId != 0 && ctx.characters->exists(playerId))
                                   ? ctx.characters->get(playerId).body.position
                                   : ctx.world->beginnerSpawnPosition();

    ctx.lod->clearTick();
    const double searchRadius = MID_DISTANCE_M * TIER_MARGIN;
    spatial_.forEachCharacterInRadius(anchor, searchRadius, [&](characterId id) {
        if (id == playerId)
            return; // the anchor never tiers itself
        if (!ctx.characters->exists(id) || !ctx.npcs->contains(id))
            return;
        NpcState &npc = ctx.npcs->get(id);
        updateNpcSimulation(npc, planarDistance(anchor, ctx.characters->get(id).body.position));
        if (npc.tier == LodTier::NEAR)
            ctx.lod->activeNear.push_back(id);
        else if (npc.tier == LodTier::MID)
            ctx.lod->activeMid.push_back(id);
        // FAR: untouched per tick — daily-sweep territory only (3.5's table).
    });

    // Registration order within the step is deterministic; keep the active sets
    // sorted so every downstream pass iterates in id order (R7).
    std::sort(ctx.lod->activeNear.begin(), ctx.lod->activeNear.end());
    std::sort(ctx.lod->activeMid.begin(), ctx.lod->activeMid.end());
}

} // namespace harrowmere
