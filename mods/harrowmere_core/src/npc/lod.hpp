// npc/lod.hpp — Task 3.5: three cost tiers with the design reference's exact
// hysteresis so nobody flickers at tier boundaries.
//
// The active-set rebuild at step 3 is O(nearby cells) via 1.6's grid — FAR NPCs are
// touched only by the daily sweep (3.4). D-LOD-1: the tier anchor is the player
// character; with no player in-world the anchor is the beginner safe-zone spawn.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include "core/scheduler.hpp"
#include "npc/state.hpp"

namespace harrowmere
{

struct WorldContext;
struct Character;

// 1.6's spatial grid, seen from the LOD system: O(nearby cells) candidate lookup.
// The default answers nothing — a trimmed assembly degrades to an empty active set,
// never to a full-population scan (3.5's rule).
class ILodSpatialSource
{
public:
    virtual ~ILodSpatialSource() = default;
    virtual void forEachCharacterInRadius(
        const Position & /*center*/, double /*radiusMeters*/, const std::function<void(characterId)> & /*fn*/
    ) const
    {}
};

// Design reference §LOD, verbatim (A-45/A-46/A-47: 50 m, 200 m, 1.25 margin).
// The margin comparisons are asymmetric on purpose: `< NEAR*MARGIN` to enter,
// `> NEAR/MARGIN` to leave — flipping one is the classic bug (3.5 pitfalls).
void updateNpcSimulation(NpcState &npc, double distance);

// Step-3 output consumed by step-4 work: NPCs in the NEAR/MID bands of the anchor.
// `farTouches` exists for TestLod_FarUntouchedPerTick — any per-tick path that
// touches a FAR-tier NPC increments it.
class LodActiveSet
{
public:
    std::vector<characterId> activeNear;
    std::vector<characterId> activeMid;
    std::uint64_t farTouches = 0;

    void clearTick()
    {
        activeNear.clear();
        activeMid.clear();
    }

    template <typename Fn>
    void forEachActive(
        Fn &&fn
    ) const
    {
        for (const characterId id : activeNear)
            fn(id);
        for (const characterId id : activeMid)
            fn(id);
    }
};

// Step 3 — tier re-evaluation with hysteresis (§4.4's Lod step).
class LodSystem final : public IStepSystem
{
public:
    explicit LodSystem(
        const ILodSpatialSource &spatialSource
    )
        : spatial_(spatialSource)
    {}
    void run(WorldContext &ctx) override;

private:
    const ILodSpatialSource &spatial_;
};

} // namespace harrowmere
