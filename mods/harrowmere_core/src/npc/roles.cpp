// npc/roles.cpp — Task 3.6 implementation.
#include "npc/roles.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "character/character.hpp"
#include "character/species.hpp"
#include "core/constants.hpp"
#include "core/constants_phase2.hpp"
#include "core/math.hpp"
#include "core/world_context.hpp"
#include "justice/suspicion.hpp" // 4.5's service refusal (the healer refuses SHUNNED)
#include "data/registry.hpp"
#include "economy/economy.hpp"
#include "npc/lod.hpp"
#include "npc/lifecycle.hpp" // placeNpcAtSpawn (3.4's spawn policy, the 8.3 as-built)
#include "npc/state.hpp"
#include "progression/power.hpp"

namespace harrowmere
{

namespace
{

const data::RoleData *roleData(
    const std::string &key, WorldContext &ctx
)
{
    return ctx.data->role(key);
}

std::uint64_t rolePriority(
    const std::string &key, WorldContext &ctx
)
{
    const data::RoleData *r = roleData(key, ctx);
    return r != nullptr ? r->priority : 0;
}

bool speciesMatchesSuitability(
    const data::RoleData &role, speciesId species, WorldContext &ctx
)
{
    if (role.suitableSpecies.empty())
        return true; // default: any sapient
    const data::SpeciesData *row = ctx.data->hasSpecies(species) ? &ctx.data->species(species) : nullptr;
    if (row == nullptr)
        return false;
    for (const std::string &key : role.suitableSpecies)
    {
        if (key == row->key)
            return true;
    }
    return false;
}

bool isEligibleCandidate(
    const Character &c,
    const NpcState &npc,
    const data::RoleData &role,
    const std::string &roleKey,
    territoryId territory,
    WorldContext &ctx
)
{
    if (npc.roleKey == roleKey)
        return false; // already in the seat
    if (npc.homeTerritory != territory)
        return false; // per-territory (pitfall)
    if (npc.ageYears(ctx.now) < ADULT_YEARS)
        return false; // A-44: children never
    if (npc.roleKey == CHILD_ROLE_KEY)
        return false;
    if (!isSapient(*ctx.data, c.species))
        return false; // animals never
    if (!speciesMatchesSuitability(role, c.species, ctx))
        return false;
    return rolePriority(npc.roleKey, ctx) < role.priority; // roleless ("") or lower priority
}

void captainRollup(
    territoryId territory, WorldContext &ctx
)
{
    // The captain holds the territory guard-power rollup (4.6 reads it in Phase 3).
    std::uint64_t power = 0;
    for (const auto &[id, npc] : ctx.npcs->all())
    {
        if (npc.homeTerritory != territory || npc.roleKey != "guard")
            continue;
        if (!ctx.characters->exists(id))
            continue;
        if (ctx.itemInstances == nullptr)
            continue; // instance registry not wired (power needs equipped-id lookups)
        power += getCurrentPower(ctx.characters->get(id), *ctx.data, *ctx.itemInstances);
    }
    ctx.roles->setGuardPower(territory, power);
}

// ---- duties (mechanical v1; executed at each role's duty slot) ----

void guardDuty(
    Character &c, WorldContext &ctx
)
{
    // Patrol waypoints around the territory center: orbit the settlement center on
    // a deterministic schedule derived from the world clock (R8: no wall time).
    const Position center = ctx.world->settlementCenter(ctx.world->territoryAt(c.body.position));
    const double angle    = static_cast<double>((ctx.now / 30) % 8) * (3.14159265358979323846 / 4.0);
    Position target       = center;
    target.x += 20.0 * std::cos(angle);
    target.z += 20.0 * std::sin(angle);
    const double dx   = target.x - c.body.position.x;
    const double dz   = target.z - c.body.position.z;
    const double dist = std::sqrt(dx * dx + dz * dz);
    if (dist > 0.5)
    {
        const double step = std::min(dist, NPC_WALK_SPEED_MPS * static_cast<double>(ctx.tickGameSeconds));
        c.body.position.x += dx / dist * step;
        c.body.position.z += dz / dist * step;
    }
}

void healerDuty(
    characterId healerId, WorldContext &ctx
)
{
    // Heals NEAR injured; 5.3's HEALING validation joins when events land (the sink
    // hook is already in place).
    for (const characterId id : ctx.lod->activeNear)
    {
        if (!ctx.characters->exists(id))
            continue;
        Character &patient = ctx.characters->get(id);
        if (patient.body.currentHp >= patient.body.maxHp)
            continue;
        // 4.5's services refusal: the healer refuses a SHUNNED patient (the
        // provider's own trust memory decides; the tier read is the pinned
        // effect — "healer refuses SHUNNED").
        if (ctx.npcs->contains(healerId) && justice::serviceRefused(ctx.npcs->get(healerId), id))
            continue;
        const std::uint64_t room   = patient.body.maxHp - patient.body.currentHp;
        const std::uint64_t amount = HEALER_HEAL_PER_DUTY < room ? HEALER_HEAL_PER_DUTY : room;
        patient.body.currentHp += amount; // heals clamp in their own path (3.2 pitfall)
        if (patient.eventId != 0)
            ctx.events->onHeal(healerId, id, amount);
        return; // one patient per duty slot (mechanical v1)
    }
}

void laborerDuty(
    Character &c, WorldContext &ctx
)
{
    // The wage path (3.7's Done-when names it): paid from territory wealth, never
    // minted — currency mutations go through buy/sell/craft/wage paths only.
    const territoryId terr   = ctx.world->territoryOfRegion(ctx.world->regionAt(c.body.position));
    const std::uint64_t wage = ctx.economy->takeWealth(terr, WAGE_PER_DUTY_HOUR);
    if (wage > 0)
        c.progression.currency += static_cast<std::int64_t>(wage);
}

void nobleDuty(
    const NpcState &noble, WorldContext &ctx
)
{
    // Territory wealth accrual: the noble collects the tax slice transactions parked.
    ctx.economy->collectTaxes(noble.homeTerritory);
}

void farmerProduceDaily(
    const NpcState &farmer, WorldContext &ctx
)
{
    // Food into the regional stock per season yield (3.8's A-52 through the hook).
    const goodId grain = ctx.economy->goodIdForKey("grain");
    if (grain == 0 || farmer.currentRegion == 0)
        return;
    const std::uint64_t yield  = ctx.world->seasonYieldScale(farmer.currentRegion);
    const std::uint64_t output = static_cast<std::uint64_t>(mulDiv128(FARMER_OUTPUT_PER_GAME_DAY, yield, SCALE, SCALE));
    ctx.economy->addSupply(farmer.currentRegion, grain, output);
}

} // namespace

// ---------------------------------------------------------- replacement flow --

void onCriticalNpcDeath(
    const NpcState &dead, WorldContext &ctx
)
{
    const data::RoleData *role = roleData(dead.roleKey, ctx);
    if (role == nullptr || !role->critical || dead.roleKey.empty())
        return;
    if (ctx.roles->hasOpenSeat(dead.homeTerritory, dead.roleKey))
        return; // one open seat per role/territory
    ctx.roles->markOpenSeat(dead.homeTerritory, dead.roleKey);

    const territoryId territory = dead.homeTerritory;
    ctx.scheduler->scheduleAt(
        ctx.now + REPLACEMENT_DELAY, TickStep::Npcs, [territory, roleKey = dead.roleKey](WorldContext &c) {
            if (eligibleNpcExists(roleKey, territory, c))
            {
                assignRole(selectEligibleNpc(roleKey, territory, c), roleKey, c);
            } else
            {
                spawnBackupNpc(roleKey, territory, c);
            }
        }
    );
}

bool eligibleNpcExists(
    const std::string &role, territoryId territory, WorldContext &ctx
)
{
    const data::RoleData *r = roleData(role, ctx);
    if (r == nullptr)
        return false;
    for (const auto &[id, npc] : ctx.npcs->all())
    {
        if (!ctx.characters->exists(id))
            continue;
        if (isEligibleCandidate(ctx.characters->get(id), npc, *r, role, territory, ctx))
            return true;
    }
    return false;
}

characterId selectEligibleNpc(
    const std::string &role, territoryId territory, WorldContext &ctx
)
{
    const data::RoleData *r = roleData(role, ctx);
    if (r == nullptr)
        return 0;
    characterId best           = 0;
    std::uint64_t bestPriority = 0;
    std::uint32_t bestAge      = 0;
    for (const auto &[id, npc] : ctx.npcs->all())
    {
        if (!ctx.characters->exists(id))
            continue;
        if (!isEligibleCandidate(ctx.characters->get(id), npc, *r, role, territory, ctx))
            continue;
        const std::uint64_t priority = rolePriority(npc.roleKey, ctx); // highest first
        const std::uint32_t age      = npc.ageYears(ctx.now);          // then oldest
        const bool better            = best == 0 || priority > bestPriority
                                    || (priority == bestPriority && (age > bestAge || (age == bestAge && id < best)));
        if (better)
        {
            best         = id;
            bestPriority = priority;
            bestAge      = age;
        }
    }
    return best; // deterministic under repeated runs (R7)
}

void assignRole(
    characterId npcId, const std::string &role, WorldContext &ctx
)
{
    const data::RoleData *r = roleData(role, ctx);
    if (r == nullptr || !ctx.npcs->contains(npcId) || !ctx.characters->exists(npcId))
        return;
    NpcState &npc         = ctx.npcs->get(npcId);
    npc.roleKey           = role;
    npc.integrity         = r->integrityMax > r->integrityMin
                              ? r->integrityMin + ctx.rng->range("npc_life", r->integrityMin, r->integrityMax + 1)
                              : r->integrityMin;
    npc.nextDutyWorldTime = ctx.now; // first duty slot immediately
    ctx.roles->clearOpenSeat(npc.homeTerritory, role);
}

void spawnBackupNpc(
    const std::string &role, territoryId territory, WorldContext &ctx
)
{
    std::uint32_t minPop = 0;
    std::uint32_t maxPop = 0;
    ctx.world->populationBounds(territory, minPop, maxPop);
    if (ctx.world->territoryPopulation(territory) < minPop)
        return; // seat stays open until births refill

    // 1.5's seed path for one NPC; the frontier pool defaults to the territory's
    // dominant species (species 1 = human in the shipped table, A-127).
    NpcSeed seed;
    if (!ctx.world->makeNpcSeed(seed, territory, /*species=*/1, role.c_str()))
        return;

    const characterId id    = ctx.characters->createNpc(seed);
    NpcState &npc           = ctx.npcs->create(id);
    npc.name                = seed.givenName;
    npc.birthWorldTime      = ctx.now - static_cast<std::uint64_t>(seed.ageYears) * YEAR_GAME_SECONDS;
    npc.roleKey             = role;
    npc.homeTerritory       = territory;
    npc.currentRegion       = ctx.world->regionAt(ctx.world->settlementCenter(territory));
    npc.tier                = LodTier::FAR;
    npc.migrated            = true; // the 3.6 card's marker
    const data::RoleData *r = roleData(role, ctx);
    npc.integrity           = (r != nullptr && r->integrityMax > r->integrityMin)
                                ? r->integrityMin + ctx.rng->range("npc_life", r->integrityMin, r->integrityMax + 1)
                                : (r != nullptr ? r->integrityMin : 0);
    npc.nextDutyWorldTime   = ctx.now;
    placeNpcAtSpawn(npc, ctx.characters->get(id), seed, ctx); // 3.4's spawn policy (the 8.3 as-built)
    if (r != nullptr && r->critical)
        ctx.roles->clearOpenSeat(territory, role);
}

void retryPendingSeats(
    territoryId territory, WorldContext &ctx
)
{
    // Births refilled the territory: any open seat retries the fill path on the
    // spot (deterministic, same rules as the scheduled replacement).
    const auto &seats = ctx.roles->openSeatsOf(territory);
    const std::set<std::string> snapshot(seats.begin(), seats.end()); // copy: filling clears entries
    for (const std::string &role : snapshot)
    {
        if (eligibleNpcExists(role, territory, ctx))
        {
            assignRole(selectEligibleNpc(role, territory, ctx), role, ctx);
        } else
        {
            spawnBackupNpc(role, territory, ctx);
        }
    }
}

// ---------------------------------------------------- duties & daily sweeps --

void RoleDutiesSystem::run(
    WorldContext &ctx
)
{
    // Hourly role duties for the NEAR/MID active set only (3.5's tier table);
    // FAR NPCs work purely in the daily statistical sweep.
    ctx.lod->forEachActive([&](characterId id) {
        if (!ctx.npcs->contains(id) || !ctx.characters->exists(id))
            return;
        NpcState &npc = ctx.npcs->get(id);
        if (npc.roleKey.empty())
            return;
        if (npc.nextDutyWorldTime > ctx.now)
            return;
        npc.nextDutyWorldTime = ctx.now + 3600; // the next game-hour slot
        Character &c          = ctx.characters->get(id);
        if (npc.roleKey == "guard")
            guardDuty(c, ctx);
        else if (npc.roleKey == "healer")
            healerDuty(id, ctx);
        else if (npc.roleKey == "laborer")
            laborerDuty(c, ctx);
        else if (npc.roleKey == "captain")
            captainRollup(npc.homeTerritory, ctx);
        else if (npc.roleKey == "noble")
            nobleDuty(npc, ctx);
        // vendor: prices come from 3.7's hourly recompute — no mechanical duty v1.
        // militia_veteran: the 4.7 engagement pool joins in Phase 3.
    });
}

void rolesDailySweep(
    WorldContext &ctx
)
{
    // Statistical production for the WHOLE population (FAR included): farmers
    // produce food; nobles collect the day's taxes; captains refresh rollups.
    // Full-map iteration is a daily-sweep privilege — per-tick paths never do this.
    for (auto &[id, npc] : ctx.npcs->all())
    {
        if (npc.roleKey == "farmer")
            farmerProduceDaily(npc, ctx);
    }
    for (auto &[id, npc] : ctx.npcs->all())
    {
        if (npc.roleKey == "noble")
            nobleDuty(npc, ctx);
    }
    for (auto &[id, npc] : ctx.npcs->all())
    {
        if (npc.roleKey == "captain")
            captainRollup(npc.homeTerritory, ctx);
    }
}

} // namespace harrowmere
