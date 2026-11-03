// npc/lifecycle.hpp — Task 3.4: NPCs as living characters — the parallel NpcState
// record, natural birth/aging/death, needs driving intents, and region migration.
//
// The daily sweep runs in step 1 (card 3.4 pins "(step 1 daily)"); the per-tick
// NEAR/MID workloads run in step 4 (§4.4's Npcs step). FAR NPCs are touched ONLY
// by the daily sweep (3.5's contract).
#pragma once
#include <cstdint>
#include <map>

#include "core/scheduler.hpp"
#include "core/types.hpp"

namespace harrowmere::world
{
struct NpcSeed; // worldgen.hpp (1.5) — the population-plan row
}

namespace harrowmere
{

struct WorldContext;
struct Character;
struct NpcState;

// D-WGEN-2: a new game spawns the population plan as living NPCs (called once by
// the plugin after worldgen; tests call it after planting the plan).
void bootstrapPopulation(WorldContext &ctx);

// 3.4's spawn policy (the 8.3 soak's as-built): a newly created NPC stands at
// their home settlement's center; a frontier wanderer (kNoTerritory) stands at
// their home region's cell center (the NpcSeed's "settlement anchor / frontier
// cell" comment). createNpc leaves the body at the origin default ("placement
// is task 3.4's spawn policy") — the bootstrap, the birth path and 3.6's
// replacement spawn all call this; without it the whole world piles at (0,0)
// until a migration roll teleports it out, and the pile is the LOD/proximity
// systems' compounding cost (the 8.3 soak's step-profile finding).
void placeNpcAtSpawn(NpcState &npc, Character &c, const world::NpcSeed &seed, WorldContext &ctx);

// Step 1 (Sweeps) — the game-day boundary wrapper around lifecycleDailySweep.
class LifecycleSweepSystem final : public IStepSystem
{
public:
    void run(WorldContext &ctx) override;

private:
    bool started_               = false;
    std::uint64_t lastSweepDay_ = 0;
};

// Step 4 (Npcs) — the per-tick NEAR/MID workload wrapper around npcPerTickWork.
class NpcTickSystem final : public IStepSystem
{
public:
    void run(WorldContext &ctx) override;
};

// The daily population sweep: aging & natural deaths, child->adult promotion,
// NPC auto-buy purchases, births, migration rolls (3.4), statistical role
// production (3.6), FAR needs floor/ceiling.
void lifecycleDailySweep(WorldContext &ctx);

// The per-tick NEAR/MID workloads: needs decay (hourly cadence via a carry), need
// crisis intents, movement drift along intents at walk speed (A-57'). Never touches
// FAR (3.5's table); TestLod_FarUntouchedPerTick guards that.
void npcPerTickWork(WorldContext &ctx);

// A-37: apply the hourly decay rates for `gameSeconds` of game time (carry-safe).
void applyNeedsDecay(NpcState &npc, std::uint64_t gameSeconds);

// The daily sweep's in-day population ledger (the 8.3 as-built): the census
// is day-cached, so the sweep's own births, deaths and migrations are
// invisible to the next roll's room reads. Every population-affecting move
// adds its ±1 here; every room/openings read adds the delta onto the cached
// count (effectivePopulation). One sweep, one ledger; it dies with the day.
using RelocationDelta = std::map<territoryId, std::int64_t>;

// The migration pull for one destination, on the 0..SCALE grid:
// pull = clamp(pressure + bias + openings - homeAffinity + climatePull, 0, 3*SCALE) / 3
// pct  = MIGRATION_ROLL_PCT_BASE * pull / SCALE   (card 3.4)
// The relocations ledger is the caller sweep's coherent-count read (tests
// pass nothing and read the raw cached census, the pre-8.3 shape).
std::uint64_t migrationPull(
    const NpcState &npc,
    const Character &c,
    territoryId destination,
    WorldContext &ctx,
    RelocationDelta *relocations = nullptr
);

// NPC purchases (2.3's auto-buy, pinned priority): cheapest stat point first, then
// levels at the 20/40/60/75/90 milestones only.
void npcAutoBuy(Character &c, WorldContext &ctx);

} // namespace harrowmere
