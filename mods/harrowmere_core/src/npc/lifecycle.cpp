// npc/lifecycle.cpp — Task 3.4 implementation.
#include "npc/lifecycle.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "character/character.hpp"
#include "character/species.hpp"
#include "character/status.hpp"
#include "combat/death.hpp"
#include "core/constants.hpp"
#include "core/constants_phase2.hpp"
#include "core/world_context.hpp"
#include "economy/economy.hpp"
#include "npc/lod.hpp"
#include "npc/roles.hpp"
#include "npc/state.hpp"
#include "progression/progression.hpp"
#include "world/region.hpp" // regionCenter (3.4's frontier-cell spawn, the 8.3 as-built)

namespace harrowmere
{

namespace
{

constexpr std::uint64_t kHourGameSeconds = 3600;

inline std::uint64_t satSub(
    std::uint64_t a, std::uint64_t b
)
{
    return a > b ? a - b : 0u;
}
inline std::uint64_t satAdd(
    std::uint64_t a, std::uint64_t b
)
{
    return a <= SCALE - b ? a + b : SCALE;
}

// A-37 rates per game-hour, applied on exact hourly boundaries (deterministic).
void applyHourlyDecay(
    NpcState &npc
)
{
    npc.needs.hunger = satSub(npc.needs.hunger, NEED_DECAY_HUNGER_PER_HOUR);
    npc.needs.rest   = satSub(npc.needs.rest, NEED_DECAY_REST_PER_HOUR);
    npc.needs.wealth = satSub(npc.needs.wealth, NEED_DECAY_WEALTH_PER_HOUR);
    npc.needs.social = satSub(npc.needs.social, NEED_DECAY_SOCIAL_PER_HOUR);
}

void refreshIntent(
    NpcState &npc, WorldContext &ctx
)
{
    // At need < NEED_CRISIS_THRESHOLD the NPC generates the matching intent (3.4).
    const std::uint64_t lowest = npc.needs.lowest();
    if (lowest >= NEED_CRISIS_THRESHOLD)
    {
        npc.currentIntent = NpcIntent{}; // no crisis: no intent
        return;
    }
    if (npc.needs.hunger == lowest)
        npc.currentIntent.kind = NpcIntentKind::Eat;
    else if (npc.needs.rest == lowest)
        npc.currentIntent.kind = NpcIntentKind::Rest;
    else if (npc.needs.wealth == lowest)
        npc.currentIntent.kind = NpcIntentKind::Earn;
    else
        npc.currentIntent.kind = NpcIntentKind::Socialize;

    // Targets: eat -> a NEAR/MID vendor (abstract-scored in MID); socialize -> a
    // nearby affinity target; rest/earn resolve toward home/work (settlement center).
    npc.currentIntent.target = 0;
    if (npc.currentIntent.kind == NpcIntentKind::Eat)
    {
        characterId vendor = 0;
        ctx.lod->forEachActive([&](characterId id) {
            if (vendor != 0 || !ctx.npcs->contains(id))
                return;
            if (ctx.npcs->get(id).roleKey == "vendor")
                vendor = id;
        });
        npc.currentIntent.target = vendor;
    } else if (npc.currentIntent.kind == NpcIntentKind::Socialize)
    {
        characterId friend0 = 0;
        ctx.lod->forEachActive([&](characterId id) {
            if (friend0 != 0 || id == 0 || !ctx.npcs->contains(id))
                return;
            if (npc.affinity.count(id) != 0)
                friend0 = id;
        });
        npc.currentIntent.target = friend0;
    }
}

// Needs are the only per-tick movement driver in NEAR tier; MID drifts along its
// route at walk speed (3.5's table). Straight-line drift v1 — 3.10's grid avoidance
// joins through the same call site when its pathing helper is exposed to NPC work.
void driftToward(
    Character &c, const Position &target, WorldContext &ctx
)
{
    const double dx   = target.x - c.body.position.x;
    const double dz   = target.z - c.body.position.z;
    const double dist = std::sqrt(dx * dx + dz * dz);
    if (dist <= 0.5)
        return;
    const double step = std::min(dist, NPC_WALK_SPEED_MPS * static_cast<double>(ctx.tickGameSeconds));
    c.body.position.x += dx / dist * step;
    c.body.position.z += dz / dist * step;
}

void intentTargetPosition(
    const NpcState &npc, WorldContext &ctx, Position &out
)
{
    switch (npc.currentIntent.kind)
    {
    case NpcIntentKind::Eat:
        if (npc.currentIntent.target != 0 && ctx.characters->exists(npc.currentIntent.target))
        {
            out = ctx.characters->get(npc.currentIntent.target).body.position;
            return;
        }
        out = ctx.world->settlementCenter(npc.homeTerritory); // the market/stall side of town
        return;
    case NpcIntentKind::Socialize:
        if (npc.currentIntent.target != 0 && ctx.characters->exists(npc.currentIntent.target))
        {
            out = ctx.characters->get(npc.currentIntent.target).body.position;
            return;
        }
        out = ctx.world->settlementCenter(npc.homeTerritory);
        return;
    case NpcIntentKind::Rest:
    case NpcIntentKind::Earn:
    default:
        out = ctx.world->settlementCenter(npc.homeTerritory); // home / the work spot
        return;
    }
}

namespace
{

// The daily sweep's in-day population corrections (the 8.3 as-built): the
// census is day-cached, so a sweep's own births, deaths and migrations are
// invisible to the next roll's reads — the day's migrants all read the same
// stale "rooms here" and funnel into ONE settlement (the 8.3 soak's wave
// shape: a whole region's population teleporting into the player's
// neighborhood in a single sweep). The delta rides the sweep (RelocationDelta
// in lifecycle.hpp): every population-affecting move adds its ±1 and every
// room/openings read adds the delta back onto the cached count.
std::uint32_t effectivePopulation(
    WorldContext &ctx, territoryId t, const RelocationDelta *relocations
)
{
    const std::uint32_t pop = ctx.world->territoryPopulation(t);
    if (relocations == nullptr)
    {
        return pop;
    }
    const auto it = relocations->find(t);
    if (it == relocations->end() || it->second == 0)
    {
        return pop;
    }
    if (it->second < 0 && static_cast<std::uint32_t>(-it->second) >= pop)
    {
        return 0; // the day's leavers cannot underflow the cached count
    }
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(pop) + it->second);
}

} // namespace

void naturalDeath(
    characterId id, NpcState &npc, Character &c, WorldContext &ctx, RelocationDelta *relocations
)
{
    // Aging death: silent removal — no evidence (nothing routes through the justice
    // sink), corpse without loot, onCriticalNpcDeath if the role is critical (3.6).
    ctx.corpses->stageDeath(id, c.body.position, ctx.now, {}, 0); // no loot, no purse (natural death)
    onCriticalNpcDeath(npc, ctx);
    ctx.npcs->erase(id);
    ctx.characters->remove(id);
    if (relocations != nullptr && npc.homeTerritory != world::kNoTerritory)
    {
        --(*relocations)[npc.homeTerritory]; // the sweep's own death, visible to its next reads
    }
}

speciesId pickResidentSpecies(
    territoryId territory, WorldContext &ctx
)
{
    std::vector<speciesId> residents; // id-ordered (map iteration, R7)
    for (const auto &[id, npc] : ctx.npcs->all())
    {
        if (npc.homeTerritory == territory && ctx.characters->exists(id))
        {
            residents.push_back(ctx.characters->get(id).species);
        }
    }
    if (residents.empty())
        return 1; // species 1 = human (shipped table) — dead settlement edge
    return residents[ctx.rng->range("npc_life", 0, static_cast<std::uint32_t>(residents.size()))];
}

void migrationRoll(
    NpcState &npc, Character &c, WorldContext &ctx, RelocationDelta *relocations
)
{
    if (ctx.now - npc.lastMigrationRollWorldTime < MIGRATION_ROLL_INTERVAL_DAYS * core::DAY_LENGTH_SECONDS)
        return;
    npc.lastMigrationRollWorldTime = ctx.now;

    // Choose the best-pull destination deterministically (id order, ties -> lower id).
    territoryId best       = 0;
    std::uint64_t bestPull = 0;
    ctx.world->forEachSettlementTerritory([&](territoryId t) {
        if (t == npc.homeTerritory)
            return;
        std::uint32_t minPop = 0, maxPop = 0;
        ctx.world->populationBounds(t, minPop, maxPop);
        // No template band = no rooms (the birth gate's minPop == 0 mirror;
        // the 8.3 soak caught the uncapped reading: kingdom/empire PARENT
        // territories join the settlement walk with [0, 0] bands, and the
        // "maxPop == 0 = uncapped" reading funneled thousands of residents
        // into the empty parent nodes — their bottomless openings also won
        // every destination contest, bleeding real villages below their
        // minimum and driving the eternal-birth population explosion).
        // The read is the sweep's COHERENT count (the 8.3 as-built): the
        // cached census plus the day's own relocations — the wave fills a
        // settlement to its real rooms and then moves on, instead of every
        // same-day roll reading the same stale "not full".
        if (maxPop == 0 || effectivePopulation(ctx, t, relocations) >= maxPop)
            return; // no band, no room; full = full
        const std::uint64_t pull = migrationPull(npc, c, t, ctx, relocations);
        if (best == 0 || pull > bestPull)
        { // strict >: lower id wins ties
            best     = t;
            bestPull = pull;
        }
    });
    if (best == 0 || bestPull == 0)
        return;

    const std::uint64_t pct = MIGRATION_ROLL_PCT_BASE * bestPull / SCALE;
    if (ctx.rng->pct("npc_life") >= pct)
        return;

    const territoryId origin = npc.homeTerritory; // before the move (the delta's leaver side)
    npc.homeTerritory        = best;
    npc.currentRegion        = ctx.world->regionAt(ctx.world->settlementCenter(best));
    if (npc.tier == LodTier::FAR)
    {
        c.body.position = ctx.world->settlementCenter(best); // FAR teleports (statistical tier)
        npc.migrating   = false;
    } else
    {
        npc.migrationTarget = ctx.world->settlementCenter(best);
        npc.migrating       = true; // NEAR/MID walk off-screen over the following game-hours
    }
    if (relocations != nullptr)
    {
        // The move enters the sweep's own books: the destination's next
        // same-day roll sees the arrival, the origin its departure.
        ++(*relocations)[best];
        if (origin != world::kNoTerritory && origin != best)
        {
            --(*relocations)[origin];
        }
    }
}

} // namespace

// ------------------------------------------------------------------ public --

void applyNeedsDecay(
    NpcState &npc, std::uint64_t gameSeconds
)
{
    npc.needsCarry += gameSeconds;
    while (npc.needsCarry >= kHourGameSeconds)
    {
        npc.needsCarry -= kHourGameSeconds;
        applyHourlyDecay(npc);
    }
}

void npcAutoBuy(
    Character &c, WorldContext &ctx
)
{
    // 2.3's auto-buy with the pinned priority: cheapest stat point first, then
    // levels at 20/40/60/75/90 milestones only (card 3.4).
    for (int guard = 0; guard < 64; ++guard)
    {
        StatType bestStat      = StatType::STRENGTH;
        std::uint64_t bestCost = std::numeric_limits<std::uint64_t>::max();
        bool statFound         = false;
        for (std::size_t s = 0; s < static_cast<std::size_t>(StatType::COUNT); ++s)
        {
            const StatType st = static_cast<StatType>(s);
            if (c.progression.stats[s] >= statCaps()[c.progression.level])
                continue;
            const std::uint64_t cost = statCosts()[c.progression.stats[s]];
            if (cost < bestCost)
            {
                bestCost  = cost;
                bestStat  = st;
                statFound = true;
            }
        }
        if (statFound && c.progression.availableXp >= static_cast<std::int64_t>(bestCost))
        {
            (void)buyStat(c, bestStat, ctx);
            continue;
        }
        const std::uint16_t nextLevel = static_cast<std::uint16_t>(c.progression.level + 1);
        const bool milestone          = std::find(NPC_LEVEL_MILESTONES.begin(), NPC_LEVEL_MILESTONES.end(), nextLevel)
                                     != NPC_LEVEL_MILESTONES.end();
        if (milestone && c.progression.level < MAX_LEVEL
            && c.progression.availableXp >= static_cast<std::int64_t>(levelCosts()[c.progression.level]))
        {
            (void)buyLevel(c, /*automatic=*/true);
            continue;
        }
        break;
    }
}

std::uint64_t migrationPull(
    const NpcState &npc, const Character &c, territoryId destination, WorldContext &ctx, RelocationDelta *relocations
)
{
    // Card 3.4's pull, made countable on the 0..SCALE grid (implementation formula;
    // components per the card: needs pressure, species bias toward the destination
    // culture, destination job openings, minus home affinity; 3.8's frozen climate
    // hook adds the winter warm-destination pull).
    const std::uint64_t avgNeed  = (npc.needs.hunger + npc.needs.rest + npc.needs.wealth + npc.needs.social) / 4;
    const std::uint64_t pressure = SCALE - avgNeed;

    // 2.2's bias core: the self species' xenophobiaBias row plus the
    // destination territory's cultureBias toward that species (laws.json;
    // 1.4's lawsAt walk behind the world-queries seam — 3.9 wraps the same
    // core for perception).
    std::int64_t bias = speciesBiasToward(
        ctx.data != nullptr && ctx.data->hasSpecies(c.species) ? ctx.data->species(c.species).xenophobiaBias : 0,
        ctx.world->cultureBiasToward(destination, c.species)
    );
    if (bias > static_cast<std::int64_t>(SCALE))
        bias = static_cast<std::int64_t>(SCALE);
    if (bias < -static_cast<std::int64_t>(SCALE))
        bias = -static_cast<std::int64_t>(SCALE);

    std::uint32_t minPop = 0, maxPop = 0;
    ctx.world->populationBounds(destination, minPop, maxPop);
    // Openings mirror the roll's room check (no band = no room, full = full;
    // the sweep's coherent count — the 8.3 as-built).
    const std::uint64_t openings
        = (maxPop != 0 && effectivePopulation(ctx, destination, relocations) < maxPop) ? SCALE : 0;

    std::int64_t affinitySum = 0;
    for (const auto &[who, value] : npc.affinity)
        affinitySum += value;
    std::int64_t homeAffinity = npc.affinity.empty() ? 0 : affinitySum / static_cast<std::int64_t>(npc.affinity.size());
    if (homeAffinity > static_cast<std::int64_t>(SCALE))
        homeAffinity = static_cast<std::int64_t>(SCALE);
    if (homeAffinity < -static_cast<std::int64_t>(SCALE))
        homeAffinity = -static_cast<std::int64_t>(SCALE);

    const std::int64_t climate = ctx.world->migrationClimatePullFor(destination);
    std::int64_t numerator     = static_cast<std::int64_t>(pressure + openings) + bias - homeAffinity + climate;
    if (numerator < 0)
        numerator = 0;
    constexpr std::int64_t kMaxPull = 3 * static_cast<std::int64_t>(SCALE);
    if (numerator > kMaxPull)
        numerator = kMaxPull;
    return static_cast<std::uint64_t>(numerator) / 3; // 0..SCALE
}

void lifecycleDailySweep(
    WorldContext &ctx
)
{
    rolesDailySweep(ctx); // 3.6: statistical production (farmer), taxes (noble), rollups

    // The sweep's own population ledger (the 8.3 as-built): the census is
    // day-cached, so the day's deaths, migrations and births are invisible
    // to the next roll's room reads — the coherent count is the cached census
    // plus this delta. One sweep, one ledger; it dies with the day.
    RelocationDelta relocations;

    // Aging, natural deaths, child->adult, auto-buy, FAR needs floor/ceiling.
    std::vector<characterId> ids; // snapshot: the sweep erases (map order, R7)
    ids.reserve(ctx.npcs->all().size());
    for (const auto &[id, npc] : ctx.npcs->all())
        ids.push_back(id);

    for (const characterId id : ids)
    {
        if (!ctx.npcs->contains(id) || !ctx.characters->exists(id))
            continue;
        NpcState &npc = ctx.npcs->get(id);
        Character &c  = ctx.characters->get(id);

        if (npc.ageYears(ctx.now) >= effectiveLifespan(*ctx.data, c))
        {
            naturalDeath(id, npc, c, ctx, &relocations);
            continue;
        }
        if (npc.roleKey == CHILD_ROLE_KEY && npc.ageYears(ctx.now) >= ADULT_YEARS)
        {
            npc.roleKey = ""; // adult: roleless until the role market (3.6) assigns
        }
        // FAR statistical needs: a day of decay applied in one step, clamped to
        // the 0..SCALE band ("needs floor/ceiling" — 3.5's FAR row).
        if (npc.tier == LodTier::FAR)
        {
            npc.needs.hunger = satSub(npc.needs.hunger, NEED_DECAY_HUNGER_PER_HOUR * 24);
            npc.needs.rest   = satSub(npc.needs.rest, NEED_DECAY_REST_PER_HOUR * 24);
            npc.needs.wealth = satSub(npc.needs.wealth, NEED_DECAY_WEALTH_PER_HOUR * 24);
            npc.needs.social = satSub(npc.needs.social, NEED_DECAY_SOCIAL_PER_HOUR * 24);
        }
        // NPCs gain XP only from kills (3.3) and event rewards (5.x) — never here.
        npcAutoBuy(c, ctx);
        migrationRoll(npc, c, ctx, &relocations);
    }

    // Births: population below the template minimum and food stock positive.
    // The gate reads the sweep's coherent count and every birth enters the
    // ledger — the day's births cannot over-fill the settlement's minimum.
    ctx.world->forEachSettlementTerritory([&](territoryId t) {
        std::uint32_t minPop = 0, maxPop = 0;
        ctx.world->populationBounds(t, minPop, maxPop);
        if (minPop == 0 || effectivePopulation(ctx, t, &relocations) >= minPop)
            return;

        const Position center = ctx.world->settlementCenter(t);
        const regionId region = ctx.world->regionAt(center);
        const goodId grain    = ctx.economy->goodIdForKey("grain");
        if (grain != 0)
        {
            const RegionStock *stock = ctx.economy->findStock(region, grain);
            if (stock == nullptr || stock->supply == 0)
                return; // no food, no births
        }

        NpcSeed seed;
        if (!ctx.world->makeNpcSeed(seed, t, pickResidentSpecies(t, ctx), CHILD_ROLE_KEY))
            return;
        const characterId id = ctx.characters->createNpc(seed);
        NpcState &npc        = ctx.npcs->create(id);
        npc.name             = seed.givenName;
        npc.birthWorldTime   = ctx.now;        // age 0; species from the residents pool
        npc.roleKey          = CHILD_ROLE_KEY; // until ADULT_YEARS (A-44)
        npc.homeTerritory    = t;
        npc.currentRegion    = region;
        npc.tier             = LodTier::FAR;
        seed.homeTerritory   = t; // the newborn's home IS the birth settlement
        seed.homeRegion      = region;
        placeNpcAtSpawn(npc, ctx.characters->get(id), seed, ctx); // born at home (the 8.3 as-built)
        ++relocations[t]; // the birth enters the sweep's ledger (the 8.3 as-built)

        retryPendingSeats(t, ctx); // a birth may refill an open critical seat (3.6)
    });
}

void npcPerTickWork(
    WorldContext &ctx
)
{
    // NEAR: needs decay, intents, movement along intents, (combat participation
    // joins via the step-10 queue). MID: needs decay, abstract economics, position
    // drift along the route at walk speed. FAR is never iterated here.
    ctx.lod->forEachActive([&](characterId id) {
        if (!ctx.npcs->contains(id) || !ctx.characters->exists(id))
            return;
        NpcState &npc = ctx.npcs->get(id);
        Character &c  = ctx.characters->get(id);
        if (npc.tier == LodTier::FAR)
        {
            ++ctx.lod->farTouches; // TestLod_FarUntouchedPerTick: must stay 0
            return;
        }
        applyNeedsDecay(npc, ctx.tickGameSeconds);
        refreshIntent(npc, ctx);
        if (npc.migrating)
        {
            driftToward(c, npc.migrationTarget, ctx);
            const double dx = npc.migrationTarget.x - c.body.position.x;
            const double dz = npc.migrationTarget.z - c.body.position.z;
            if (dx * dx + dz * dz <= 1.0)
                npc.migrating = false;
        } else
        {
            Position target{};
            intentTargetPosition(npc, ctx, target);
            driftToward(c, target, ctx);
        }
    });
}

void placeNpcAtSpawn(
    NpcState &npc, Character &c, const world::NpcSeed &seed, WorldContext &ctx
)
{
    // 3.4's spawn policy (the 8.3 as-built): home settlement center; the
    // frontier's home region cell for wanderers. The read is pure data (the
    // world's own layout), so the placement is seed-deterministic (R6) and
    // the LOD tier stays FAR (the caller's default) — the daily sweep's
    // statistical work owns FAR NPCs until a migration or the player's
    // proximity promotes them.
    if (seed.homeTerritory != world::kNoTerritory)
    {
        c.body.position = ctx.world->settlementCenter(seed.homeTerritory);
    } else if (seed.homeRegion != world::kNoRegion)
    {
        c.body.position = world::regionCenter(seed.homeRegion);
    }
    (void)npc; // the callers own currentRegion (their reads predate this helper)
}

void bootstrapPopulation(
    WorldContext &ctx
)
{
    // D-WGEN-2: the population plan becomes living NPCs (one seed -> one Character
    // + one NpcState). Deterministic seed order (1.5's plan order).
    ctx.world->forEachPlannedSeed([&](NpcSeed &seed) {
        const characterId id = ctx.characters->createNpc(seed);
        NpcState &npc        = ctx.npcs->create(id);
        npc.name             = seed.givenName;
        npc.birthWorldTime   = ctx.now - static_cast<std::uint64_t>(seed.ageYears) * YEAR_GAME_SECONDS;
        npc.roleKey          = seed.roleKey;
        npc.homeTerritory    = seed.homeTerritory;
        npc.currentRegion = seed.homeRegion != 0 ? seed.homeRegion
                                                 : ctx.world->regionAt(ctx.world->settlementCenter(seed.homeTerritory));
        npc.tier          = LodTier::FAR;
        npc.integrity     = seed.integrity; // the worldgen draw (1.5's pinned draw 4) — the 8.4
                                            // probe found every living witness at 0, which zeroes
                                            // the doc's witness weight (role x integrity x (1-affinity))
                                            // and with it every witness case in the living world;
                                            // DEVIATIONS.md row 2026-11-02/8.4
        placeNpcAtSpawn(npc, ctx.characters->get(id), seed, ctx); // 3.4's spawn policy (the 8.3 as-built)
    });
}

// ------------------------------------------------------------------ systems --

void LifecycleSweepSystem::run(
    WorldContext &ctx
)
{
    const std::uint64_t day = ctx.now / core::DAY_LENGTH_SECONDS;
    if (!started_)
    {
        started_      = true;
        lastSweepDay_ = day;
        return; // the world's first tick is day 0's sweep by definition
    }
    if (day != lastSweepDay_)
    {
        lastSweepDay_ = day;
        lifecycleDailySweep(ctx);
    }
}

void NpcTickSystem::run(
    WorldContext &ctx
)
{
    npcPerTickWork(ctx);
}

} // namespace harrowmere
