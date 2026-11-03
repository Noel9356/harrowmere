// tests/soak_main.cpp — task 8.3's headless soak harness: the FULL 14-step
// tick pipeline (§4.4) driven directly by runTick, no renderer, no engine
// kernel — the exact wiring shape harrowmere_plugin.cpp assembles, mirrored
// here because the plugin's lifecycle needs the engine context this harness
// deliberately runs without ("Headless is first-class", common.md §4.4; the
// card's anti-theater rule: the soak runs the real pipeline — every step,
// every system, the real stores — never a shortcut loop).
//
// Compile modes:
//   * standalone binary (harrowmere_soak): main() below parses --game-years /
//     --seed / --data-dir and runs the nightly soak;
//   * library: the doctest binary compiles this file with
//     HARROWMERE_SOAK_NO_MAIN and test_soak.cpp / test_save_load.cpp drive
//     the same harness (the 7.1 roundtrip tests build their 30-game-day
//     world through it — the save/load paths are exercised end to end).
//
// Invariants (the card's monthly sample set): population within the template
// bands (±20% — the world's living population against the planned one,
// D-WGEN-2's band reading), bounded RSS, active events finite (≤ 1 global +
// regional cap), evidence store bounded, prices within the D-ECON-1 4x bands,
// no NaN positions, jail/warrant maps bounded, RNG streams advancing. A
// failure dumps the month index + the canonical state hash for reproduction.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>
#include <filesystem>
#include <system_error>
#include <memory>
#include <string>
#include <vector>

#include "character/appearance.hpp"
#include "character/store.hpp"
#include "combat/attacks.hpp"
#include "combat/consent.hpp"
#include "combat/damage.hpp"
#include "combat/death.hpp"
#include "core/clock.hpp"
#include "core/constants.hpp"
#include "core/constants_phase2.hpp"
#include "core/rng.hpp"
#include "core/scheduler.hpp"
#include "core/world_context.hpp"
#include "data/config.hpp"
#include "data/registry.hpp"
#include "economy/economy.hpp"
#include "events/contribution.hpp"
#include "events/difficulty.hpp"
#include "events/engine.hpp"
#include "events/rewards.hpp"
#include "events/world_effects.hpp"
#include "factions/claims.hpp"
#include "factions/factions.hpp"
#include "factions/war.hpp"
#include "items/crafting.hpp"
#include "items/inventory.hpp"
#include "items/crafting.hpp"
#include "justice/enforcement.hpp"
#include "justice/evidence.hpp"
#include "justice/reputation.hpp"
#include "justice/warrants.hpp"
#include "justice/justice_store.hpp"
#include "justice/suspicion.hpp"
#include "npc/lifecycle.hpp"
#include "npc/lod.hpp"
#include "npc/perception.hpp"
#include "npc/roles.hpp"
#include "npc/state.hpp"
#include "persist/save.hpp"
#include "persist/state_hash.hpp"
#include "player/controller.hpp"
#include "player/interactions.hpp"
#include "progression/matchmaking.hpp"
#include "progression/progression.hpp"
#include "quests/quests.hpp"
#include "world/region.hpp"
#include "world/seasons.hpp"
#include "world/spatial_grid.hpp"
#include "world/world.hpp"
#include "world/worldgen.hpp"

#include "core/command_apply.hpp" // PlayerCommandSystem + CommandServices
#include "core/game_events.hpp"
#include "justice/cases.hpp"
#include "justice/conviction.hpp"
#include "npc/relationships.hpp"
#include "world/territory.hpp"

namespace soak
{

using namespace harrowmere;

// A test/pacing bound, not a game rule (the kFastForwardTickBurstMax
// precedent): the soak's RSS ceiling. The reference machine's budget is
// 16 GB; the headless soak's working set is orders below it — 2 GB catches
// unbounded growth long before a machine suffers.
constexpr std::uint64_t SOAK_RSS_BUDGET_BYTES = 2ULL * 1024ULL * 1024ULL * 1024ULL;

// Generous bounds on the maps the card names (a playerless soak sits at or
// near zero; the bound catches unbounded growth, not honest traffic).
constexpr std::size_t SOAK_EVIDENCE_ROW_BOUND = 10000;
constexpr std::size_t SOAK_JAIL_BOUND         = 10000;
constexpr std::size_t SOAK_WARRANT_BOUND      = 10000;

// ---- the plugin's kernel-free adapters, mirrored verbatim -------------------
// (harrowmere_plugin.cpp owns the originals; they are anonymous-namespace
// members there, so the harness re-declares them here. A drift between the
// two would show as a soak/harness behavior gap — the mirrors are copied
// verbatim and named identically.)

// Step 0 (Clocks): both clocks advance one tick, then the context reads the
// fresh values (R8: the sim never reads the wall clock).
struct ClockStepSystem final : public IStepSystem
{
    core::WorldClock *clock = nullptr;
    WorldContext *ctx       = nullptr;
    std::function<void(std::uint64_t)> onWorldTime;
    std::uint64_t tickGameSeconds = 6;

    void run(
        WorldContext &
    ) override
    {
        clock->advanceTick();
        ctx->now             = clock->worldTime;
        ctx->realTimeSeconds = clock->realTime;
        ctx->tickGameSeconds = tickGameSeconds;
        if (onWorldTime)
        {
            onWorldTime(clock->worldTime);
        }
    }
};

// Step 0's season roll (3.8's pinned function, unchanged).
struct SeasonsStepSystem final : public IStepSystem
{
    world::WorldStore *world = nullptr;
    core::WorldClock *clock  = nullptr;

    void run(
        WorldContext &
    ) override
    {
        world::updateSeasons(*world, clock->worldTime);
    }
};

// Step 9's claim slice (6.3, registered AFTER the PlayerCommandSystem).
struct ClaimPresenceSystem final : public IStepSystem
{
    factions::ClaimStore *store          = nullptr;
    player::PlayerController *player     = nullptr;
    const characterId *playerCharacterId = nullptr;

    void run(
        WorldContext &ctx
    ) override
    {
        if (store == nullptr || player == nullptr || playerCharacterId == nullptr || *playerCharacterId == 0)
        {
            return;
        }
        for (const territoryId t : store->claimsOwnedBy(*playerCharacterId))
        {
            store->notePresence(t, *playerCharacterId, player->afk, ctx.realTimeSeconds);
        }
    }
};

// Step 2 (Weather): 3.8's state machine, unchanged.
struct WeatherStepSystem final : public IStepSystem
{
    world::WorldStore *world    = nullptr;
    core::WorldClock *clock     = nullptr;
    core::SeedRng *rng          = nullptr;
    std::uint64_t prevWorldTime = 0;

    void run(
        WorldContext &
    ) override
    {
        world::updateWeather(*world, prevWorldTime, clock->worldTime, *rng);
        prevWorldTime = clock->worldTime;
    }
};

// Step 6 (Justice): 4.2's case ticks.
struct JusticeStepSystem final : public IStepSystem
{
    void run(
        WorldContext &ctx
    ) override
    {
        justice::runCaseTicks(ctx);
    }
};

// Step 1's justice slice: the daily global backstop sweep (4.2), day-gated.
struct DayGateJusticeSweepSystem final : public IStepSystem
{
    std::uint64_t *lastSweepDay = nullptr;

    void run(
        WorldContext &ctx
    ) override
    {
        const std::uint64_t day = ctx.now / core::DAY_LENGTH_SECONDS;
        if (day == *lastSweepDay)
        {
            return;
        }
        *lastSweepDay = day;
        justice::sweepExpiredEvidence(ctx);
    }
};

// Step 11's tail: the 1.6 spatial index mirrors the authoritative body
// positions (the plugin's GridSyncSystem, verbatim).
struct GridSyncSystem final : public IStepSystem
{
    world::SpatialGrid *grid              = nullptr;
    character::CharacterStore *characters = nullptr;
    NpcStore *npcs                        = nullptr;
    harrowmere::LodActiveSet *lod         = nullptr;
    player::PlayerController *player      = nullptr;
    characterId playerCharacterId         = 0;
    std::uint64_t *lastSweepDay           = nullptr;
    std::map<characterId, Position> syncedPositions_;

    void syncOne(
        characterId id, const Position &p
    )
    {
        const auto it = syncedPositions_.find(id);
        if (it == syncedPositions_.end())
        {
            grid->insert(id, p);
            syncedPositions_.emplace(id, p);
            return;
        }
        if (it->second.x != p.x || it->second.y != p.y || it->second.z != p.z)
        {
            grid->move(id, it->second, p);
            it->second = p;
        }
    }

    void run(
        WorldContext &ctx
    ) override
    {
        // (The dead-id prune runs in the daily branch below: FAR deaths only
        // happen on the daily sweep, so a per-tick full-ledger scan bought
        // nothing — it measured ~90% of the sustained tick budget on a
        // 5,000-NPC world, and the 8.3 soak's step-cost profile caught it.
        // The per-tick NEAR/MID branch's exists() guards keep the hot set
        // honest; the daily prune bounds any stale grid entry to a day.)
        if (playerCharacterId != 0 && characters->exists(playerCharacterId))
        {
            syncOne(playerCharacterId, player->position);
        }
        for (characterId id : lod->activeNear)
        {
            if (characters->exists(id))
            {
                syncOne(id, characters->get(id).body.position);
            }
        }
        for (characterId id : lod->activeMid)
        {
            if (characters->exists(id))
            {
                syncOne(id, characters->get(id).body.position);
            }
        }

        const std::uint64_t day = ctx.now / core::DAY_LENGTH_SECONDS;
        const bool sweepTick    = day != *lastSweepDay;
        *lastSweepDay           = day;
        if (sweepTick)
        {
            // The daily prune (deaths since yesterday) + the full re-sync
            // (migration teleports / natural deaths).
            for (auto it = syncedPositions_.begin(); it != syncedPositions_.end();)
            {
                if (!characters->exists(it->first))
                {
                    grid->remove(it->first);
                    it = syncedPositions_.erase(it);
                } else
                {
                    ++it;
                }
            }
            for (const auto &[id, npc] : npcs->all())
            {
                if (characters->exists(id))
                {
                    syncOne(id, characters->get(id).body.position);
                }
            }
        }
    }
};

// IWorldQueries over the worldgen graph + the living stores (the plugin's
// RuntimeWorldQueries, verbatim).
class RuntimeWorldQueries final : public harrowmere::IWorldQueries
{
public:
    RuntimeWorldQueries(
        const world::WorldStore &world,
        const character::CharacterStore &characters,
        const NpcStore &npcs,
        const world::NpcSeedOut &plan
    )
        : world_(world), characters_(characters), npcs_(npcs), plan_(plan)
    {}

    territoryId territoryAt(
        const Position &p
    ) const override
    {
        return world_.territoryAt(p);
    }
    bool isInSafeZone(
        const Position &p
    ) const override
    {
        return world_.isInSafeZone(p);
    }
    regionId regionAt(
        const Position &p
    ) const override
    {
        return world::regionIdAt(p);
    }
    TerritoryInfo territoryById(
        territoryId id
    ) const override
    {
        TerritoryInfo info{};
        if (const world::Territory *t = world_.territory(id); t != nullptr)
        {
            info.id     = t->id;
            info.parent = t->parent;
            info.type   = t->type;
            info.safe   = t->safe;
        }
        return info;
    }
    territoryId territoryOfRegion(
        regionId r
    ) const override
    {
        return world_.territoryOfRegion(r);
    }

    std::uint32_t regionPopulation(
        regionId r
    ) const override
    {
        rebuildCensusIfStale();
        const auto it = regionCensus_.find(r);
        return it == regionCensus_.end() ? 0u : it->second;
    }
    std::uint32_t territoryPopulation(
        territoryId t
    ) const override
    {
        rebuildCensusIfStale();
        const auto it = homeCensus_.find(t);
        return it == homeCensus_.end() ? 0u : it->second;
    }
    void populationBounds(
        territoryId t, std::uint32_t &minOut, std::uint32_t &maxOut
    ) const override
    {
        minOut                      = 0;
        maxOut                      = 0;
        const world::Territory *ter = world_.territory(t);
        if (ter == nullptr || templates_ == nullptr)
        {
            return;
        }
        if (const data::TerritoryTypeTemplate *tmpl = templates_->byType(ter->type); tmpl != nullptr)
        {
            minOut = tmpl->populationMin;
            maxOut = tmpl->populationMax;
        }
    }

    Position settlementCenter(
        territoryId t
    ) const override
    {
        if (const world::Territory *ter = world_.territory(t); ter != nullptr && ter->region != world::kNoRegion)
        {
            return world::regionCenter(ter->region);
        }
        return Position{};
    }
    Position safeZoneSpawnFor(
        const Position &
    ) const override
    {
        return genSpawn_;
    }
    Position beginnerSpawnPosition() const override
    {
        return genSpawn_;
    }
    characterId playerCharacterId() const override
    {
        return characters_.playerId();
    }

    std::uint64_t seasonYieldScale(
        regionId r
    ) const override
    {
        if (const world::Region *region = world_.region(r); region != nullptr)
        {
            return world::cropYieldMultiplier(
                world::seasonAt(currentWorldTime_), static_cast<world::Weather>(region->weather.kind)
            );
        }
        return SCALE;
    }
    std::int64_t migrationClimatePullFor(
        regionId r
    ) const override
    {
        if (const world::Region *region = world_.region(r); region != nullptr)
        {
            return static_cast<std::int64_t>(
                world::migrationClimatePull(world::seasonAt(currentWorldTime_), region->climate)
            );
        }
        return 0;
    }

    void forEachSettlementTerritory(
        const std::function<void(territoryId)> &fn
    ) const override
    {
        for (const auto &[id, territory] : world_.territories())
        {
            if (territory.type == TerritoryType::VILLAGE || territory.type == TerritoryType::CITY
                || territory.type == TerritoryType::KINGDOM || territory.type == TerritoryType::EMPIRE)
            {
                fn(id);
            }
        }
    }
    void forEachPlannedSeed(
        const std::function<void(NpcSeed &)> &fn
    ) const override
    {
        for (NpcSeed seed : plan_)
        {
            fn(seed);
        }
    }
    speciesId dominantSpeciesOf(
        territoryId t
    ) const override
    {
        std::map<speciesId, std::uint32_t> counts;
        for (const auto &[id, npc] : npcs_.all())
        {
            if (npc.homeTerritory == t && characters_.exists(id))
            {
                ++counts[characters_.get(id).species];
            }
        }
        speciesId best  = 0;
        std::uint32_t n = 0;
        for (const auto &[species, count] : counts)
        {
            if (count > n)
            {
                best = species;
                n    = count;
            }
        }
        return best;
    }
    std::int64_t cultureBiasToward(
        territoryId t, speciesId self
    ) const override
    {
        return world_.lawsAt(t).cultureBias.count(self) != 0 ? world_.lawsAt(t).cultureBias.at(self) : 0;
    }

    bool makeNpcSeed(
        NpcSeed &out, territoryId home, speciesId species, const char *roleKey
    ) override
    {
        out.species       = species;
        out.homeTerritory = home;
        out.homeRegion    = homeRegionOf(home);
        out.roleKey       = roleKey;
        out.ageYears      = std::string(roleKey) == CHILD_ROLE_KEY ? 0u : kAdultSeedYears_;
        out.level         = 1;
        out.givenName     = "npc_" + std::to_string(++nameCounter_);
        return true;
    }

    void bindTemplates(
        const data::TerritoryTemplateSet &templates
    )
    {
        templates_ = &templates;
    }
    void bindSpawn(
        Position spawn
    )
    {
        genSpawn_ = spawn;
    }
    void bindWorldTime(
        std::uint64_t worldTime
    )
    {
        currentWorldTime_ = worldTime;
    }

    // The dump's reconciliation read: the census's own build stamp (the day
    // it was built, the rows it saw, the living sum it bucketed) and the
    // world time the day gate reads.
    struct CensusDebug
    {
        std::uint64_t day = 0;
        std::size_t rows  = 0;
        std::size_t sum   = 0;
    };
    [[nodiscard]] CensusDebug censusDebug() const
    {
        return debugLastRebuild_;
    }
    [[nodiscard]] std::map<territoryId, std::uint32_t> censusSnapshot() const
    {
        return homeCensus_;
    }
    [[nodiscard]] std::uint64_t censusWorldTime() const
    {
        return currentWorldTime_;
    }

private:
    regionId homeRegionOf(
        territoryId t
    ) const
    {
        if (const world::Territory *ter = world_.territory(t); ter != nullptr)
        {
            return ter->region;
        }
        return world::kNoRegion;
    }

    const world::WorldStore &world_;
    const character::CharacterStore &characters_;
    const NpcStore &npcs_;
    const world::NpcSeedOut &plan_;
    const data::TerritoryTemplateSet *templates_ = nullptr;
    Position genSpawn_{};
    std::uint64_t currentWorldTime_ = 0;
    int nameCounter_                = 0;
    std::uint32_t kAdultSeedYears_  = 20;

    // ---- the day-cached home/region census (8.3's soak found the cliff) ----
    // territoryPopulation/regionPopulation are O(living population) reads,
    // and the daily sweep's consumers (migration room checks, the birth gate,
    // the economy's population snapshot) call them per territory/region — an
    // O(N x settlements) pass per day that went quadratic on the first
    // monthly migration roll (every NPC's first roll lands in the same few
    // days). The census rebuilds ONCE per game-day: the consumers are all
    // daily decisions, and a same-day migration/birth lands in tomorrow's
    // census (deterministic; the honest cadence of every reader).
    mutable std::map<territoryId, std::uint32_t> homeCensus_; // homeTerritory -> living residents
    mutable std::map<regionId, std::uint32_t> regionCensus_;  // currentRegion -> living residents
    mutable std::uint64_t censusDay_ = 0;                     // the census's game-day
    mutable CensusDebug debugLastRebuild_{};                  // the dump's reconciliation read

    void rebuildCensusIfStale() const
    {
        const std::uint64_t day = currentWorldTime_ / core::DAY_LENGTH_SECONDS;
        if (censusDay_ == day && !homeCensus_.empty())
        {
            return;
        }
        homeCensus_.clear();
        regionCensus_.clear();
        std::size_t rows = 0, sum = 0;
        for (const auto &[npcId, npc] : npcs_.all())
        {
            ++rows;
            if (!characters_.exists(npcId))
            {
                continue;
            }
            ++homeCensus_[npc.homeTerritory];
            ++regionCensus_[npc.currentRegion];
            ++sum;
        }
        censusDay_        = day;
        debugLastRebuild_ = {day, rows, sum}; // the dump's reconciliation read
    }
};

// IBagSource over the 2.4 player-side bag store.
class RuntimeBagSource final : public harrowmere::IBagSource
{
public:
    explicit RuntimeBagSource(
        items::InventoryStore &bags
    )
        : bags_(bags)
    {}
    items::Inventory *bagOf(
        characterId id
    ) override
    {
        return &bags_.bag(id);
    }

private:
    items::InventoryStore &bags_;
};

// ILodSpatialSource over 1.6's grid.
class RuntimeLodSource final : public harrowmere::ILodSpatialSource
{
public:
    explicit RuntimeLodSource(
        const world::SpatialGrid &grid
    )
        : grid_(grid)
    {}
    void forEachCharacterInRadius(
        const Position &center, double radiusMeters, const std::function<void(characterId)> &fn
    ) const override
    {
        for (characterId id : grid_.idsWithinRadius(center, radiusMeters))
        {
            fn(id);
        }
    }

private:
    const world::SpatialGrid &grid_;
};

// ---- the headless runtime -----------------------------------------------------

// A development-only per-step cost probe (8.3's budget discipline): the soak
// harness registers its systems through registerProfiled(); when the env
// HARROWMERE_SOAK_STEP_PROFILE=1 the shim accumulates each system's wall
// cost and printStepProfile() reports it. Test tooling only — never part of
// the game targets, and the shim is transparent when the env is unset.
struct StepProfileShim final : public IStepSystem
{
    IStepSystem *inner = nullptr;
    std::string name;
    static std::map<std::string, double> &micros()
    {
        static std::map<std::string, double> table;
        return table;
    }
    void run(
        WorldContext &ctx
    ) override
    {
        const auto start = std::chrono::steady_clock::now();
        inner->run(ctx);
        micros()[name] += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    }
};

inline bool stepProfilingEnabled()
{
    const char *env = std::getenv("HARROWMERE_SOAK_STEP_PROFILE");
    return env != nullptr && env[0] == '1';
}

struct SoakRuntime
{
    // content + clocks
    data::DataRegistry registry;
    RecipeTable recipes; // items/crafting.hpp — harrowmere-scope (the plugin spells it unqualified too)
    quests::QuestTable quests;
    core::WorldClock clock{};
    core::SeedRng rng{};

    // world + stores (the plugin's member set)
    std::unique_ptr<world::WorldStore> world;
    world::WorldGenResult genResult{};
    world::NpcSeedOut npcPlan;
    character::CharacterStore characters{};
    NpcStore npcs{};
    EconomyStore economy{};
    CorpseStore corpses{};
    RoleRegistry roles{};
    items::InventoryStore bags{};
    items::ItemInstanceRegistry itemInstances{};
    combat::ConsentRegistry consents{};
    world::SpatialGrid grid{};

    // the seams (real implementations, the plugin's binding set)
    justice::JusticeSystem justiceSystem{};
    justice::JusticeStore justiceStore{};
    npc::PerceptionSink perceptionSink{};
    character::AppearanceHistory appearanceHistory{};
    justice::SuspicionPriceModifier suspicionPriceModifier{};
    justice::SuspicionSignalSink suspicionSignalSink{};
    NullUiSignalSink signals{};
    factions::FactionStore factionStore{};
    factions::FactionQueries factionQueries{};
    factions::FactionMonthlySystem factionMonthlySystem{};
    factions::WarStore warStore{};
    factions::WarTickSystem warTickSystem{};
    factions::ClaimStore claimStore{};
    factions::ClaimSweepSystem claimSweepSystem{};
    progression::MatchmakingSystem matchmakingSystem{};
    events::EventEngine eventEngine{};
    events::ContributionRouter contributionRouter{};
    events::ContributionTickSystem contributionTickSystem{};
    events::WorldEffectsTickSystem worldEffectsTickSystem{};
    harrowmere::LodActiveSet lod{};
    CombatQueue combatQueue{};

    // adapters
    std::unique_ptr<RuntimeWorldQueries> worldQueries;
    std::unique_ptr<RuntimeBagSource> bagSource;
    std::unique_ptr<RuntimeLodSource> lodSource;
    WorldContext simCtx{};

    // step systems
    ClockStepSystem clockStepSystem{};
    SeasonsStepSystem seasonsStepSystem{};
    WeatherStepSystem weatherStepSystem{};
    GridSyncSystem gridSyncSystem{};
    JusticeStepSystem justiceStepSystem{};
    justice::SuspicionTickSystem suspicionTickSystem{};
    justice::EnforcementStepSystem enforcementStepSystem{};
    events::EventTickSystem eventTickSystem{};
    events::EventSweepSystem eventSweepSystem{};
    DayGateJusticeSweepSystem justiceSweepSystem{};
    ClaimPresenceSystem claimPresenceSystem{};
    std::unique_ptr<LifecycleSweepSystem> lifecycleSystem;
    std::unique_ptr<NpcTickSystem> npcTickSystem;
    std::unique_ptr<RoleDutiesSystem> roleDutiesSystem;
    std::unique_ptr<LodSystem> lodSystem;
    std::unique_ptr<EconomySystem> economySystem;
    std::unique_ptr<PlayerCommandSystem> playerSystem;
    std::unique_ptr<CombatResolutionSystem> combatSystem;
    std::unique_ptr<CorpseDecaySystem> corpseDecaySystem;
    std::unique_ptr<AftermathSystem> aftermathSystem;

    Scheduler scheduler{};
    std::vector<std::unique_ptr<StepProfileShim>> stepShims; // profiling only
    player::PlayerController player{};
    player::IntentQueue intentQueue;
    std::uint64_t worldSeed           = 0;
    characterId playerCharacterId     = 0;
    std::uint64_t tickNumber          = 0;
    std::uint64_t lastSweepDay        = 0;
    std::uint64_t lastJusticeSweepDay = 0;
    bool wired                        = false;
};

inline void registerProfiled(
    SoakRuntime &rt, TickStep step, IStepSystem *system, const std::string &name
)
{
    if (stepProfilingEnabled())
    {
        auto shim   = std::make_unique<StepProfileShim>();
        shim->inner = system;
        shim->name  = name;
        rt.scheduler.registerSystem(step, shim.get());
        rt.stepShims.push_back(std::move(shim));
    } else
    {
        rt.scheduler.registerSystem(step, system);
    }
}

inline void printStepProfile()
{
    if (!stepProfilingEnabled())
    {
        return;
    }
    std::printf("soak step profile (cumulative microseconds):\n");
    std::vector<std::pair<std::string, double>> rows(
        StepProfileShim::micros().begin(), StepProfileShim::micros().end()
    );
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    for (const auto &[name, us] : rows)
    {
        std::printf("  %-28s %12.0f\n", name.c_str(), us);
    }
}

// goods.json -> EconomyStore::registerGood (the plugin's loadGoods, verbatim
// modulo the data-dir join).
inline bool loadGoods(
    SoakRuntime &rt, const std::string &dataDir, std::string &error
)
{
    const auto loaded = data::loadJsonFile(dataDir + "/goods.json");
    if (loaded.isErr())
    {
        error = "goods.json failed to parse: " + loaded.error().toString();
        return false;
    }
    const data::Json &rows = loaded.value();
    if (!rows.is_array())
    {
        error = "goods.json must be an array of goods";
        return false;
    }
    for (const auto &row : rows)
    {
        if (!row.contains("id") || !row.contains("key") || !row.contains("baseValue") || !row.contains("perCapitaNeed"))
        {
            error = "goods.json row missing id/key/baseValue/perCapitaNeed";
            return false;
        }
        const double perCapitaNeed = row["perCapitaNeed"].get<double>();
        rt.economy.registerGood(
            row["id"].get<goodId>(),
            row["key"].get<std::string>().c_str(),
            row["baseValue"].get<std::uint64_t>(),
            static_cast<std::uint64_t>(perCapitaNeed * static_cast<double>(SCALE))
        );
    }
    return true;
}

// The shared wiring half: adapters, context bindings, the pinned step table
// (the plugin's wireSimulation minus the new-game population half — that is
// newGame()'s; loading restores the population instead).
inline bool wireAll(
    SoakRuntime &rt, std::string &error
)
{
    if (rt.wired)
    {
        return true;
    }
    if (!rt.world)
    {
        error = "wireAll: the world store does not exist yet (load content first)";
        return false;
    }

    rt.worldQueries = std::make_unique<RuntimeWorldQueries>(*rt.world, rt.characters, rt.npcs, rt.npcPlan);
    rt.worldQueries->bindTemplates(rt.registry.territoryTemplates());

    rt.perceptionSink.bind(&rt.grid, &rt.characters, &rt.npcs, &rt.registry);
    rt.perceptionSink.bindAppearanceHistory(&rt.appearanceHistory);

    rt.bagSource              = std::make_unique<RuntimeBagSource>(rt.bags);
    rt.lodSource              = std::make_unique<RuntimeLodSource>(rt.grid);
    rt.simCtx                 = WorldContext{};
    rt.simCtx.now             = rt.clock.worldTime;
    rt.simCtx.realTimeSeconds = rt.clock.realTime;
    rt.simCtx.tickGameSeconds = rt.clock.timeScale / (rt.clock.tickRate > 0 ? rt.clock.tickRate : 10);
    rt.simCtx.world           = rt.worldQueries.get();
    rt.simCtx.factions        = &rt.factionQueries;
    rt.simCtx.wars            = &rt.warStore;
    rt.simCtx.justice         = &rt.justiceSystem;
    rt.justiceSystem.bind(&rt.simCtx);
    rt.simCtx.perception = &rt.perceptionSink;
    rt.simCtx.events     = &rt.contributionRouter;
    rt.suspicionPriceModifier.bind(&rt.simCtx);
    rt.suspicionSignalSink.bind(&rt.simCtx);
    rt.simCtx.priceModifier     = &rt.suspicionPriceModifier;
    rt.simCtx.suspicion         = &rt.suspicionSignalSink;
    rt.simCtx.signals           = &rt.signals;
    rt.simCtx.bags              = rt.bagSource.get();
    rt.simCtx.characters        = &rt.characters;
    rt.simCtx.npcs              = &rt.npcs;
    rt.simCtx.economy           = &rt.economy;
    rt.simCtx.corpses           = &rt.corpses;
    rt.simCtx.roles             = &rt.roles;
    rt.simCtx.data              = &rt.registry;
    rt.simCtx.itemInstances     = &rt.itemInstances;
    rt.simCtx.scheduler         = &rt.scheduler;
    rt.simCtx.lod               = &rt.lod;
    rt.simCtx.combatQueue       = &rt.combatQueue;
    rt.simCtx.rng               = &rt.rng;
    rt.simCtx.appearanceHistory = &rt.appearanceHistory;
    rt.simCtx.worldStore        = rt.world.get();
    rt.simCtx.justiceStore      = &rt.justiceStore;
    rt.simCtx.simEvents         = nullptr; // headless: the 0.7 publications drop deterministically (D-SINK-1)
    rt.simCtx.playerActions     = &rt.player.actions;
    rt.simCtx.grid              = &rt.grid;
    rt.simCtx.eventSpawner      = &rt.eventEngine;
    rt.simCtx.eventExclusion    = &rt.eventEngine;
    rt.simCtx.eventEffects      = &rt.eventEngine;
    rt.eventEngine.bind(&rt.registry.eventTemplates());
    rt.eventEngine.bindContext(&rt.simCtx);
    rt.contributionRouter.bind(&rt.eventEngine, &rt.registry.eventTemplates(), &rt.simCtx);
    rt.simCtx.factionStore = &rt.factionStore;
    rt.simCtx.warStore     = &rt.warStore;
    rt.simCtx.claims       = &rt.claimStore;

    // The step table (§4.4; registration order within a step = execution
    // order — the plugin's exact registration sequence).
    rt.clockStepSystem.clock       = &rt.clock;
    rt.clockStepSystem.ctx         = &rt.simCtx;
    RuntimeWorldQueries *queries   = rt.worldQueries.get();
    rt.clockStepSystem.onWorldTime = [queries, &rt](std::uint64_t worldTime) {
        queries->bindWorldTime(worldTime);
        rt.perceptionSink.bindWorldTime(worldTime);
    };
    rt.clockStepSystem.tickGameSeconds  = rt.simCtx.tickGameSeconds;
    rt.seasonsStepSystem.world          = rt.world.get();
    rt.seasonsStepSystem.clock          = &rt.clock;
    rt.weatherStepSystem.world          = rt.world.get();
    rt.weatherStepSystem.clock          = &rt.clock;
    rt.weatherStepSystem.rng            = &rt.rng;
    rt.worldEffectsTickSystem.engine    = &rt.eventEngine;
    rt.gridSyncSystem.grid              = &rt.grid;
    rt.gridSyncSystem.characters        = &rt.characters;
    rt.gridSyncSystem.npcs              = &rt.npcs;
    rt.gridSyncSystem.lod               = &rt.lod;
    rt.gridSyncSystem.player            = &rt.player;
    rt.gridSyncSystem.playerCharacterId = rt.playerCharacterId;
    rt.gridSyncSystem.lastSweepDay      = &rt.lastSweepDay;
    rt.justiceSweepSystem.lastSweepDay  = &rt.lastJusticeSweepDay;
    registerProfiled(rt, TickStep::Clocks, &rt.clockStepSystem, "Clocks.clockStep");
    registerProfiled(rt, TickStep::Sweeps, &rt.seasonsStepSystem, "Sweeps.seasons");
    registerProfiled(rt, TickStep::Weather, &rt.weatherStepSystem, "Weather.weatherMachine");
    registerProfiled(rt, TickStep::Weather, &rt.worldEffectsTickSystem, "Weather.worldEffects");
    rt.lifecycleSystem  = std::make_unique<LifecycleSweepSystem>();
    rt.npcTickSystem    = std::make_unique<NpcTickSystem>();
    rt.roleDutiesSystem = std::make_unique<RoleDutiesSystem>();
    rt.lodSystem        = std::make_unique<LodSystem>(*rt.lodSource);
    rt.economySystem    = std::make_unique<EconomySystem>();
    rt.playerSystem     = std::make_unique<PlayerCommandSystem>(
        CommandServices{
            &rt.intentQueue,
            &rt.player,
            &rt.characters,
            &rt.bags,
            &rt.itemInstances,
            &rt.recipes,
            &rt.consents,
            &rt.grid,
            &rt.clock,
            rt.clock.tickRate > 0 ? 1.0 / static_cast<double>(rt.clock.tickRate) : 0.1,
            &rt.eventEngine,
            &rt.factionStore,
            0
        },
        player::nullInteractionResolver(),
        player::InteractionHandlers::empty()
    );
    rt.combatSystem                          = std::make_unique<CombatResolutionSystem>();
    rt.corpseDecaySystem                     = std::make_unique<CorpseDecaySystem>();
    rt.aftermathSystem                       = std::make_unique<AftermathSystem>();
    rt.factionMonthlySystem.store            = &rt.factionStore;
    rt.claimSweepSystem.store                = &rt.claimStore;
    rt.warTickSystem.store                   = &rt.warStore;
    rt.claimPresenceSystem.store             = &rt.claimStore;
    rt.claimPresenceSystem.player            = &rt.player;
    rt.claimPresenceSystem.playerCharacterId = &rt.playerCharacterId;
    registerProfiled(rt, TickStep::Sweeps, rt.lifecycleSystem.get(), "Sweeps.lifecycle");
    registerProfiled(rt, TickStep::Sweeps, &rt.justiceSweepSystem, "Sweeps.justiceBackstop");
    registerProfiled(rt, TickStep::Sweeps, &rt.eventSweepSystem, "Sweeps.eventCleanup");
    registerProfiled(rt, TickStep::Sweeps, &rt.factionMonthlySystem, "Sweeps.factionDrift");
    registerProfiled(rt, TickStep::Sweeps, &rt.claimSweepSystem, "Sweeps.claims");
    registerProfiled(rt, TickStep::Events, &rt.eventTickSystem, "Events.engineTick");
    registerProfiled(rt, TickStep::Events, &rt.contributionTickSystem, "Events.contribution");
    registerProfiled(rt, TickStep::Lod, rt.lodSystem.get(), "Lod.tiers");
    registerProfiled(rt, TickStep::Npcs, rt.npcTickSystem.get(), "Npcs.tick");
    registerProfiled(rt, TickStep::Npcs, rt.roleDutiesSystem.get(), "Npcs.roleDuties");
    registerProfiled(rt, TickStep::Justice, &rt.justiceStepSystem, "Justice.caseTicks");
    registerProfiled(rt, TickStep::Justice, &rt.suspicionTickSystem, "Justice.suspicion");
    registerProfiled(rt, TickStep::Justice, &rt.enforcementStepSystem, "Justice.enforcement");
    registerProfiled(rt, TickStep::Economy, rt.economySystem.get(), "Economy.hourDay");
    registerProfiled(rt, TickStep::Macro, &rt.warTickSystem, "Macro.wars");
    registerProfiled(rt, TickStep::Player, rt.playerSystem.get(), "Player.commands");
    registerProfiled(rt, TickStep::Player, &rt.claimPresenceSystem, "Player.claimPresence");
    registerProfiled(rt, TickStep::Combat, rt.combatSystem.get(), "Combat.resolution");
    registerProfiled(rt, TickStep::Aftermath, rt.corpseDecaySystem.get(), "Aftermath.corpseDecay");
    registerProfiled(rt, TickStep::Aftermath, rt.aftermathSystem.get(), "Aftermath.loot");
    registerProfiled(rt, TickStep::Aftermath, &rt.gridSyncSystem, "Aftermath.gridSync");

    rt.contributionTickSystem.engine = &rt.eventEngine;
    rt.eventTickSystem.engine        = &rt.eventEngine;
    rt.eventSweepSystem.engine       = &rt.eventEngine;

    rt.wired = true;
    return true;
}

// Load the package content (both paths run this first).
inline bool loadContent(
    SoakRuntime &rt, const std::string &dataDir, std::string &error
)
{
    data::DataErrors errors;
    if (!rt.registry.load(dataDir, errors) || !errors.empty())
    {
        error = errors.empty() ? "unknown registry error" : errors.front().toString();
        return false;
    }
    if (!loadGoods(rt, dataDir, error))
    {
        return false;
    }
    if (std::vector<data::DataError> recipeErrors;
        !rt.recipes.load(dataDir + "/recipes.json", rt.registry, recipeErrors) || !recipeErrors.empty())
    {
        error = recipeErrors.empty() ? "unknown recipes.json error" : recipeErrors.front().toString();
        return false;
    }
    if (std::vector<data::DataError> questErrors;
        !quests::loadQuests(dataDir + "/quests.json", rt.quests, questErrors) || !questErrors.empty())
    {
        error = questErrors.empty() ? "unknown quests.json error" : questErrors.front().toString();
        return false;
    }
    for (const auto &[questId, quest] : rt.quests.rows())
    {
        (void)questId;
        if (rt.registry.eventTemplates().byKey(quest.eventTemplateKey) == nullptr)
        {
            error = "quest '" + quest.key + "' binds unknown template '" + quest.eventTemplateKey + "'";
            return false;
        }
    }
    return true;
}

// A new game from the seed: worldgen, the population plan as living NPCs,
// the faction adoption, the wiring. (The plugin's start()+wireSimulation
// new-game half.)
inline bool newGame(
    SoakRuntime &rt, std::uint64_t seed, const std::string &dataDir, std::string &error
)
{
    if (!loadContent(rt, dataDir, error))
    {
        return false;
    }

    rt.world = std::make_unique<world::WorldStore>(rt.registry.defaultLaws());
    world::NpcSeedOut npcs;
    rt.genResult = world::generateWorld(seed, rt.registry, *rt.world, npcs);
    rt.npcPlan   = std::move(npcs);

    rt.worldSeed       = seed;
    rt.clock.timeScale = 60; // D-TIME-1's default (config layer owns it in the runtime)
    rt.clock.tickRate  = 10; // D-TICK-1's default
    // A-129: the world epoch opens at 10:00 game time (mid-morning daylight).
    rt.clock.worldTime = 10u * 3600u;
    rt.rng.seedAll(seed);

    // The store adoption FIRST (the 2.8 rule), then the player.
    rt.characters.adoptWorldGen(rt.genResult.nextCharacterId, rt.genResult.playerSpawn);
    rt.playerCharacterId = rt.characters.createPlayer(PlayerCreation{"wanderer", {}, ""});

    if (!wireAll(rt, error))
    {
        return false;
    }

    // The plan becomes living NPCs, then every character joins the grid.
    rt.simCtx.now = rt.clock.worldTime;
    bootstrapPopulation(rt.simCtx);
    for (const auto &[id, character] : rt.characters.all())
    {
        rt.grid.insert(id, character.body.position);
    }

    // The faction adoption + the live adapters.
    rt.factionStore.adoptWorldGen(rt.genResult.factions, rt.simCtx);
    rt.factionQueries.bind(&rt.factionStore, &rt.simCtx);
    rt.matchmakingSystem.bind(&rt.simCtx);
    rt.simCtx.factions    = &rt.factionQueries;
    rt.simCtx.matchmaking = &rt.matchmakingSystem;
    rt.warStore.bindFactions(&rt.factionStore);
    rt.warStore.bindContext(&rt.simCtx);

    rt.player.position                                    = rt.genResult.playerSpawn;
    rt.characters.get(rt.playerCharacterId).body.position = rt.player.position;
    rt.weatherStepSystem.prevWorldTime                    = rt.clock.worldTime;
    return true;
}

// The load path: content, an empty wiring over empty stores, then the save.
inline bool loadGame(
    SoakRuntime &rt, const std::string &savePath, const std::string &dataDir, std::string &error
)
{
    if (!loadContent(rt, dataDir, error))
    {
        return false;
    }
    rt.world = std::make_unique<world::WorldStore>(rt.registry.defaultLaws());
    if (!wireAll(rt, error))
    {
        return false;
    }

    persist::LoadTargets targets;
    targets.seed          = 0;
    targets.clock         = &rt.clock;
    targets.rng           = &rt.rng;
    targets.characters    = &rt.characters;
    targets.npcs          = &rt.npcs;
    targets.world         = rt.world.get();
    targets.justice       = &rt.justiceStore;
    targets.events        = &rt.eventEngine;
    targets.economy       = &rt.economy;
    targets.factions      = &rt.factionStore;
    targets.wars          = &rt.warStore;
    targets.claims        = &rt.claimStore;
    targets.corpses       = &rt.corpses;
    targets.itemInstances = &rt.itemInstances;
    targets.bags          = &rt.bags;
    targets.appearance    = &rt.appearanceHistory;
    targets.roles         = &rt.roles;
    targets.consents      = &rt.consents;
    targets.combatQueue   = &rt.combatQueue;
    targets.registry      = &rt.registry;
    persist::RuntimeState restoredRuntime;
    targets.runtime = &restoredRuntime;

    if (!persist::readSaveFile(savePath, targets, error))
    {
        return false;
    }
    rt.worldSeed = targets.seed;

    // The runtime pacing gates ride the save (7.1): a loaded world resumes
    // every cadence stamp exactly, so the re-saved file is byte-identical
    // and the tick phasing matches the world that was saved.
    rt.tickNumber                      = restoredRuntime.tickNumber;
    rt.lastSweepDay                    = restoredRuntime.gridLastSweepDay;
    rt.lastJusticeSweepDay             = restoredRuntime.justiceSweepLastDay;
    rt.weatherStepSystem.prevWorldTime = restoredRuntime.weatherPrevWorldTime;
    if (rt.economySystem != nullptr)
    {
        rt.economySystem->restoreRuntimeGates(
            {restoredRuntime.economyStarted, restoredRuntime.economyLastHour, restoredRuntime.economyLastDay}
        );
    }
    rt.suspicionTickSystem.restoreLastRunHour(restoredRuntime.suspicionLastRunHour);
    rt.contributionTickSystem.restoreSnapshotAt(std::move(restoredRuntime.contributionSnapshotAt));
    rt.contributionTickSystem.restoreNextWalkAt(restoredRuntime.contributionNextWalkAt);
    rt.enforcementStepSystem.restoreResponded(std::move(restoredRuntime.enforcementResponded));

    // Post-load wiring refreshes: the context clock, the adapter bindings,
    // the faction adapters, the day gates, the grid over the restored
    // positions.
    rt.simCtx.now             = rt.clock.worldTime;
    rt.simCtx.realTimeSeconds = rt.clock.realTime;
    rt.factionQueries.bind(&rt.factionStore, &rt.simCtx);
    rt.matchmakingSystem.bind(&rt.simCtx);
    rt.warStore.bindFactions(&rt.factionStore);
    rt.warStore.bindContext(&rt.simCtx);
    rt.worldQueries->bindSpawn(rt.characters.playerSpawn());
    rt.worldQueries->bindWorldTime(rt.clock.worldTime);
    rt.perceptionSink.bindWorldTime(rt.clock.worldTime);
    // (weatherStepSystem.prevWorldTime came off the save's runtime block
    // above — overwriting it with the current world time here would shift
    // the weather cadence by the saved world's age.)

    for (const auto &[id, character] : rt.characters.all())
    {
        rt.grid.insert(id, character.body.position);
    }
    if (rt.characters.playerCount() > 0)
    {
        rt.playerCharacterId                = rt.characters.playerId();
        rt.gridSyncSystem.playerCharacterId = rt.playerCharacterId;
        rt.player.position                  = rt.characters.get(rt.playerCharacterId).body.position;
        // The AFK clock re-derives per session (controller.hpp's contract):
        // the load IS the session start, so the stamp arms here.
        rt.player.lastIntentRealTime = rt.clock.realTime;
    }
    return true;
}

// ONE runTick (the plugin's onFixedUpdate minus the fast-forward burst, which
// no soak input arms).
inline void runTick(
    SoakRuntime &rt
)
{
    ++rt.tickNumber;
    rt.scheduler.runTick(rt.simCtx);
}

inline void runTicks(
    SoakRuntime &rt, std::uint64_t ticks
)
{
    for (std::uint64_t i = 0; i < ticks; ++i)
    {
        runTick(rt);
    }
}

constexpr std::uint64_t kTickGameSeconds = 6; // 10 Hz x TIME_SCALE 60 (D-TICK-1)
constexpr std::uint64_t kTicksPerGameDay = core::DAY_LENGTH_SECONDS / kTickGameSeconds;

inline void runGameDays(
    SoakRuntime &rt, std::uint64_t days
)
{
    runTicks(rt, days * kTicksPerGameDay);
}

// ---- the save/load + hash surface (the 7.1 tests drive these) ------------------

inline persist::RuntimeState runtimeStateOf(
    const SoakRuntime &rt
)
{
    persist::RuntimeState runtime;
    runtime.tickNumber           = rt.tickNumber;
    runtime.gridLastSweepDay     = rt.lastSweepDay;
    runtime.justiceSweepLastDay  = rt.lastJusticeSweepDay;
    runtime.weatherPrevWorldTime = rt.weatherStepSystem.prevWorldTime;
    if (rt.economySystem != nullptr)
    {
        const auto gates        = rt.economySystem->runtimeGates();
        runtime.economyStarted  = gates.started;
        runtime.economyLastHour = gates.lastHour;
        runtime.economyLastDay  = gates.lastDay;
    }
    runtime.suspicionLastRunHour   = rt.suspicionTickSystem.lastRunHour();
    runtime.contributionSnapshotAt = rt.contributionTickSystem.snapshotAt();
    runtime.contributionNextWalkAt = rt.contributionTickSystem.nextWalkAt();
    runtime.enforcementResponded   = rt.enforcementStepSystem.responded();
    runtime.playerPosition         = rt.player.position;
    runtime.playerPositionValid    = rt.playerCharacterId != 0;
    return runtime;
}

inline persist::SaveSources saveSourcesOf(
    const SoakRuntime &rt, const persist::RuntimeState &runtime
)
{
    persist::SaveSources sources;
    sources.seed          = rt.worldSeed;
    sources.clock         = &rt.clock;
    sources.rng           = &rt.rng;
    sources.characters    = &rt.characters;
    sources.npcs          = &rt.npcs;
    sources.world         = rt.world.get();
    sources.justice       = &rt.justiceStore;
    sources.events        = &rt.eventEngine;
    sources.economy       = &rt.economy;
    sources.factions      = &rt.factionStore;
    sources.wars          = &rt.warStore;
    sources.claims        = &rt.claimStore;
    sources.corpses       = &rt.corpses;
    sources.itemInstances = &rt.itemInstances;
    sources.bags          = &rt.bags;
    sources.appearance    = &rt.appearanceHistory;
    sources.roles         = &rt.roles;
    sources.consents      = &rt.consents;
    sources.combatQueue   = &rt.combatQueue;
    sources.runtime       = &runtime;
    return sources;
}

inline bool saveToFile(
    const SoakRuntime &rt, const std::string &path, std::string &error
)
{
    const persist::RuntimeState runtime = runtimeStateOf(rt);
    const persist::SaveSources sources  = saveSourcesOf(rt, runtime);
    return persist::writeSaveFile(path, sources, error);
}

inline std::uint64_t stateHash(
    const SoakRuntime &rt
)
{
    const persist::RuntimeState runtime = runtimeStateOf(rt);
    const persist::SaveSources sources  = saveSourcesOf(rt, runtime);
    return persist::canonicalStateHash(sources);
}

// ---- the monthly invariants ----------------------------------------------------

struct SoakSample
{
    std::uint64_t month           = 0;
    std::uint32_t population      = 0;
    std::size_t activeEvents      = 0;
    std::size_t evidenceRows      = 0;
    std::size_t warrants          = 0;
    std::size_t jailed            = 0;
    std::uint32_t outOfBand       = 0; // diagnostic: settlements outside their template band
    std::size_t eventParticipants = 0; // diagnostic: Σ active events' participant rows
    std::size_t affinityRows      = 0; // diagnostic: Σ npc.affinity rows (3.9's meeting book)
    std::size_t trustRows         = 0; // diagnostic: Σ trust.perCharacterScore rows (4.5)
    std::size_t seenRows          = 0; // diagnostic: Σ trust.lastSeen rows (4.5's stranger memory)
    std::size_t corpses           = 0; // diagnostic: the corpse store's rows
    std::size_t factionMembers    = 0; // diagnostic: Σ faction membership rows
    std::size_t gridRows          = 0; // diagnostic: the spatial grid's occupied cells
    std::size_t lodActive         = 0; // diagnostic: the last tick's NEAR+MID active set
    std::uint64_t rssBytes        = 0;
    std::uint64_t stateHash       = 0;
    std::string failure; // empty = the month passed
};

inline std::uint64_t currentRssBytes()
{
    // Linux: /proc/self/status VmRSS. Any other platform: 0 (the check
    // degrades to the store-bound invariants).
    std::FILE *status = std::fopen("/proc/self/status", "r");
    if (status == nullptr)
    {
        return 0;
    }
    char line[256];
    std::uint64_t kb = 0;
    while (std::fgets(line, sizeof(line), status) != nullptr)
    {
        if (std::strncmp(line, "VmRSS:", 6) == 0)
        {
            const char *p = line + 6;
            while (*p == ' ' || *p == '\t')
            {
                ++p;
            }
            kb = std::strtoull(p, nullptr, 10);
            break;
        }
    }
    std::fclose(status);
    return kb * 1024ULL;
}

// One monthly invariant sweep. Returns the sample; `failure` carries the
// violated invariants (month index + detail — the card's dump). When
// `populationDump` is non-null (the --dump-population probe) every
// settlement's living census + template band lands there first.
inline SoakSample sampleInvariants(
    const SoakRuntime &rt,
    std::uint64_t month,
    const std::vector<std::pair<std::string, std::uint64_t>> &prevRngState,
    std::FILE *populationDump = nullptr,
    std::FILE *eventsDump     = nullptr
)
{
    SoakSample sample;
    sample.month        = month;
    sample.population   = static_cast<std::uint32_t>(rt.characters.population());
    sample.activeEvents = rt.eventEngine.activeCount();
    for (const auto &[evId, ev] : rt.eventEngine.active())
    {
        (void)evId;
        sample.eventParticipants += ev.participants.size();
    }
    // The growth diagnostics (the 8.3 soak's compounding-cost read): the
    // per-NPC meeting books and the store sizes the hot systems walk.
    for (const auto &[npcId, npc] : rt.npcs.all())
    {
        (void)npcId;
        sample.affinityRows += npc.affinity.size();
        sample.trustRows += npc.trust.perCharacterScore.size();
        sample.seenRows += npc.trust.lastSeen.size();
    }
    sample.corpses        = rt.corpses.size();
    sample.factionMembers = rt.factionStore.snapshot().membership.size();
    sample.lodActive      = rt.lod.activeNear.size() + rt.lod.activeMid.size();
    if (eventsDump != nullptr)
    {
        // The active-event book (the QA probe: what is running, how big, how
        // old — the 8.3 soak's event-shape read).
        std::fprintf(
            eventsDump,
            "  events: %zu active, %zu participant rows (worldTime %llu)\n",
            sample.activeEvents,
            sample.eventParticipants,
            static_cast<unsigned long long>(rt.worldQueries->censusWorldTime())
        );
        // The LOD crowd read (the 8.3 soak's compounding-cost probe): the
        // tier histogram and the anchor-radius occupancy the tier pass and
        // the NEAR workloads scale with.
        std::size_t tierNear = 0, tierMid = 0, tierFar = 0;
        for (const auto &[npcId, npc] : rt.npcs.all())
        {
            (void)npcId;
            if (npc.tier == LodTier::NEAR)
            {
                ++tierNear;
            } else if (npc.tier == LodTier::MID)
            {
                ++tierMid;
            } else
            {
                ++tierFar;
            }
        }
        const Position anchor                   = rt.worldQueries->beginnerSpawnPosition();
        const std::vector<characterId> inRadius = rt.grid.idsWithinRadius(anchor, MID_DISTANCE_M * TIER_MARGIN);
        std::fprintf(
            eventsDump,
            "  lod: NEAR %zu, MID %zu, FAR %zu; within MID*MARGIN of the anchor: %zu npcs\n",
            tierNear,
            tierMid,
            tierFar,
            inRadius.size()
        );
        // The position-clustering read: the authoritative body positions
        // bucketed to 100 m — a pile-up shows as one bucket eating the world.
        {
            std::map<long long, std::size_t> buckets;
            for (const auto &[id, c] : rt.characters.all())
            {
                (void)id;
                const long long bx = std::llround(c.body.position.x / 100.0);
                const long long bz = std::llround(c.body.position.z / 100.0);
                ++buckets[bx * 1'000'000 + bz]; // bucket coords stay ±200 on the A-40 plane
            }
            std::fprintf(
                eventsDump,
                "  position buckets (100 m): %zu distinct; anchor (%.0f, %.0f)\n",
                buckets.size(),
                anchor.x,
                anchor.z
            );
            std::size_t shownBuckets = 0;
            for (const auto &[key, count] : buckets)
            {
                if (shownBuckets++ >= 6)
                {
                    break;
                }
                std::fprintf(
                    eventsDump, "    bucket (%lld, %lld): %zu npcs\n", key / 1'000'000, key % 1'000'000, count
                );
            }
        }
        for (const auto &[evId, ev] : rt.eventEngine.active())
        {
            const events::EventTemplate *tpl = rt.registry.eventTemplates().byId(ev.templateId);
            std::fprintf(
                eventsDump,
                "    event %-6llu tpl %-8llu %-16s region %-4u depth %u state %-4llu started day %-6llu "
                "maxDur %u d, participants %zu, targets %zu, snapshots %zu, difficulty %u\n",
                static_cast<unsigned long long>(evId),
                static_cast<unsigned long long>(ev.templateId),
                tpl != nullptr ? tpl->key.c_str() : "?",
                static_cast<unsigned>(ev.region),
                static_cast<unsigned>(ev.depth),
                static_cast<unsigned long long>(ev.currentState.stateId),
                static_cast<unsigned long long>(ev.startedAt / 86400ULL),
                tpl != nullptr ? tpl->maxDurationDays : 0u,
                ev.participants.size(),
                ev.targetIds.size(),
                ev.currentState.powerSnapshots.size(),
                ev.currentState.difficulty
            );
        }
    }
    sample.stateHash = stateHash(rt);

    // (1) population within the template bands (±20%) — the card's claim is
    // the WORLD's: the living population stays inside ±20% of the template
    // bands, and the world's template band is the settlement rows' own sum
    // (D-WGEN-2's data-driven reading: the shipped template ranges produce
    // the planned population, and their sum is the seed-independent anchor —
    // a loaded world re-derives it from the same territories + templates, no
    // worldgen artifact needed). Settlement-level drift is real sim dynamics
    // the invariant must survive (a war month drains the belligerents' home
    // settlements by design; 8.4's balance pass owns the per-settlement
    // curves), so the per-settlement census rides the --dump-population probe
    // and the monthly `oob` count, never the verdict.
    {
        std::uint64_t templateMinSum = 0;
        std::uint64_t templateMaxSum = 0;
        rt.worldQueries->forEachSettlementTerritory([&](territoryId t) {
            std::uint32_t minPop = 0, maxPop = 0;
            rt.worldQueries->populationBounds(t, minPop, maxPop);
            templateMinSum += minPop;
            templateMaxSum += maxPop;
            if (minPop == 0 && maxPop == 0)
            {
                return; // no template band (degenerate data row)
            }
            const std::uint32_t living = rt.worldQueries->territoryPopulation(t);
            const bool out             = living < static_cast<std::uint32_t>(static_cast<double>(minPop) * 0.8)
                                      || living > static_cast<std::uint32_t>(static_cast<double>(maxPop) * 1.2) + 1;
            if (out)
            {
                sample.outOfBand += 1;
            }
            if (populationDump != nullptr)
            {
                std::fprintf(
                    populationDump,
                    "  territory %-6u band [%u, %u] living %u%s\n",
                    static_cast<unsigned>(t),
                    minPop,
                    maxPop,
                    living,
                    out ? "  <-- OUT" : ""
                );
            }
        });
        if (populationDump != nullptr)
        {
            // The census reconciliation (the dump's job: where do the world's
            // characters live?): the store sizes, the settlement-census sum
            // and the homes the settlement walk does not visit.
            std::size_t npcRows = 0, missingCharacter = 0, settlementHomes = 0;
            std::map<territoryId, std::size_t> otherHomes;
            for (const auto &[npcId, npc] : rt.npcs.all())
            {
                (void)npcId;
                ++npcRows;
                if (!rt.characters.exists(npcId))
                {
                    ++missingCharacter;
                    continue;
                }
                bool isSettlement = false;
                rt.worldQueries->forEachSettlementTerritory([&](territoryId s) {
                    if (s == npc.homeTerritory)
                    {
                        isSettlement = true;
                    }
                });
                if (isSettlement)
                {
                    ++settlementHomes;
                } else
                {
                    ++otherHomes[npc.homeTerritory];
                }
            }
            std::size_t otherHomeNpcs = 0;
            for (const auto &[home, n] : otherHomes)
            {
                (void)home;
                otherHomeNpcs += n;
            }
            std::fprintf(
                populationDump,
                "  census: characters %zu, npc rows %zu (no-character %zu), settlement homes %zu, other homes %zu rows "
                "/ %zu npcs\n"
                "  census build: worldTime %llu (day %llu), built at day %llu, rows %zu, bucketed sum %zu\n",
                rt.characters.all().size(),
                npcRows,
                missingCharacter,
                settlementHomes,
                otherHomes.size(),
                otherHomeNpcs,
                static_cast<unsigned long long>(rt.worldQueries->censusWorldTime()),
                static_cast<unsigned long long>(rt.worldQueries->censusWorldTime() / 86400ULL),
                static_cast<unsigned long long>(rt.worldQueries->censusDebug().day),
                rt.worldQueries->censusDebug().rows,
                rt.worldQueries->censusDebug().sum
            );
            for (const auto &[home, n] : otherHomes)
            {
                std::fprintf(populationDump, "    home %-8llu : %zu npcs\n", static_cast<unsigned long long>(home), n);
            }
            const auto raw = rt.worldQueries->censusSnapshot();
            std::fprintf(populationDump, "  census map: %zu keys, bucketed sum %zu\n", raw.size(), [&] {
                std::size_t s = 0;
                for (const auto &[k, v] : raw)
                {
                    (void)k;
                    s += v;
                }
                return s;
            }());
            std::size_t shown = 0;
            for (auto it = raw.rbegin(); it != raw.rend() && shown < 70; ++it, ++shown)
            {
                std::fprintf(
                    populationDump, "    census[%llu] = %u\n", static_cast<unsigned long long>(it->first), it->second
                );
            }
        }
        if (templateMinSum == 0 && templateMaxSum == 0)
        {
            sample.failure = "population band: no settlement template rows (content broken)";
            return sample;
        }
        const std::uint64_t low  = static_cast<std::uint64_t>(static_cast<double>(templateMinSum) * 0.8);
        const std::uint64_t high = static_cast<std::uint64_t>(static_cast<double>(templateMaxSum) * 1.2) + 1;
        if (sample.population < low || sample.population > high)
        {
            std::ostringstream detail;
            detail << "population " << sample.population << " outside [" << low << ", " << high
                   << "] (settlement template sums [" << templateMinSum << ", " << templateMaxSum << "])";
            sample.failure = "population band: " + detail.str();
            return sample;
        }
    }

    // (2) no unbounded memory (RSS budget).
    sample.rssBytes = currentRssBytes();
    if (sample.rssBytes > SOAK_RSS_BUDGET_BYTES)
    {
        std::ostringstream detail;
        detail << "RSS " << (sample.rssBytes / (1024 * 1024)) << " MiB exceeds the "
               << (SOAK_RSS_BUDGET_BYTES / (1024 * 1024)) << " MiB budget";
        sample.failure = detail.str();
        return sample;
    }

    // (3) active events finite: at most one global + one per region, never a
    // duplicate region.
    {
        std::set<regionId> regions;
        std::size_t globalCount = 0;
        for (const auto &[id, ev] : rt.eventEngine.active())
        {
            (void)id;
            if (ev.region == world::kNoRegion || ev.region == 0)
            {
                // region 0 IS a valid grid region id; the doc's global scale
                // is regionId 0 per the engine's own convention (Event::region
                // doc: "0 = global-scale").
                ++globalCount;
            } else
            {
                if (!regions.insert(ev.region).second)
                {
                    sample.failure = "events: two active events share region " + std::to_string(ev.region);
                    return sample;
                }
            }
        }
        if (globalCount > 1)
        {
            sample.failure = "events: " + std::to_string(globalCount) + " global events active (cap 1)";
            return sample;
        }
        sample.activeEvents = rt.eventEngine.activeCount();
    }

    // (4) evidence store bounded (the sweeps work).
    {
        std::size_t rows = 0;
        for (const auto &[territory, list] : rt.justiceStore.evidenceStore)
        {
            (void)territory;
            rows += list.size();
        }
        sample.evidenceRows = rows;
        if (rows > SOAK_EVIDENCE_ROW_BOUND)
        {
            sample.failure = "evidence store " + std::to_string(rows) + " rows exceeds the bound";
            return sample;
        }
    }

    // (5) prices within the D-ECON-1 4x bands.
    {
        const auto snap = rt.economy.snapshot();
        for (const auto &[region, goods] : snap.prices)
        {
            (void)region;
            for (const auto &[good, price] : goods)
            {
                const std::uint64_t base = rt.economy.baseValueOf(good);
                if (base == 0)
                {
                    continue;
                }
                if (price < base / 4 || price > base * 4)
                {
                    std::ostringstream detail;
                    detail << "price for good " << good << " is " << price << " outside [" << base / 4 << ", "
                           << base * 4 << "]";
                    sample.failure = "price band: " + detail.str();
                    return sample;
                }
            }
        }
    }

    // (6) no NaN positions.
    for (const auto &[id, c] : rt.characters.all())
    {
        (void)id;
        if (!std::isfinite(c.body.position.x) || !std::isfinite(c.body.position.y) || !std::isfinite(c.body.position.z))
        {
            sample.failure = "NaN position on character " + std::to_string(c.id);
            return sample;
        }
    }

    // (7) jail/warrant maps bounded.
    sample.warrants = rt.justiceStore.warrants.size();
    sample.jailed   = rt.justiceStore.jailUntil.size();
    if (sample.warrants > SOAK_WARRANT_BOUND)
    {
        sample.failure = "warrant map " + std::to_string(sample.warrants) + " rows exceeds the bound";
        return sample;
    }
    if (sample.jailed > SOAK_JAIL_BOUND)
    {
        sample.failure = "jail map " + std::to_string(sample.jailed) + " rows exceeds the bound";
        return sample;
    }

    // (8) RNG streams advancing.
    {
        const auto now = rt.rng.state();
        if (now == prevRngState)
        {
            sample.failure = "RNG stream states unchanged across a game-month";
            return sample;
        }
    }

    return sample;
}

// ---- task 8.4's balance instrumentation -----------------------------------------
//
// The card's instrumented measurements over the SAME real pipeline the soak
// drives (never a second sim): (a) the XP income per A-6 stage band -> the
// A-120 days-per-10-levels curve, (b) the level distribution (no
// runtime-reachable peak stage before the age bands allow — worldgen starts
// every NPC at level <= STARTING_LEVEL_MAX, so any peak-stage row inside a
// soak window is runtime XP growth), (c) price stability + the wealth Gini,
// (f) the event-difficulty equilibrium (successRate hovers 3000-7000 — the
// doc's regulation loop works). (d)'s scripted crime probe and (e)'s
// bounty-vs-penalty sweep close the set below. Everything here is READ-ONLY
// over the stores except the crime probe, whose harness actions are named at
// the probe.

// The A-6 stage bands: early 1-25, middle 26-50, late 51-75, peak 76-100.
inline constexpr std::uint64_t BAL_BAND_START[4] = {1, 26, 51, 76};

inline std::size_t balBandOf(
    std::uint64_t level
)
{
    if (level >= BAL_BAND_START[3])
        return 3;
    if (level >= BAL_BAND_START[2])
        return 2;
    if (level >= BAL_BAND_START[1])
        return 1;
    return 0;
}

// Σ levelCosts[L .. L+9]: the XP to advance from level L to L+10 (the A-120
// curve's cost side over A-7/A-8's precomputed table; levelCosts[i] prices
// the advance i -> i+1).
inline std::uint64_t balTenLevelCost(
    std::uint64_t fromLevel
)
{
    const std::vector<std::uint64_t> &costs = levelCosts();
    std::uint64_t sum                       = 0;
    for (std::uint64_t i = fromLevel; i < fromLevel + 10 && i < costs.size(); ++i)
    {
        sum += costs[i];
    }
    return sum;
}

// The Gini coefficient over signed purses (currency is i64; conviction debts
// included — the doc's wealth). Exact integer accumulation in __int128 (R3's
// discipline: Σ(2i+1−n)·x overflows i64 on a 10k-row × 10^9-currency
// population), then one long-double divide for the unit-free ratio.
inline double balGini(
    std::vector<std::int64_t> values
)
{
    if (values.size() < 2)
    {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    // R3's spelling: the __extension__ typedef exempts the alias from
    // -Wpedantic (core/math.cpp's precedent).
    __extension__ typedef __int128 bal_wide_i128;
    bal_wide_i128 sum      = 0;
    bal_wide_i128 weighted = 0; // Σ (2i + 1 − n) · x_i over the ascending sort
    const bal_wide_i128 n  = static_cast<bal_wide_i128>(values.size());
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        const bal_wide_i128 x = static_cast<bal_wide_i128>(values[i]);
        sum += x;
        weighted += (2 * static_cast<bal_wide_i128>(i) + 1 - n) * x;
    }
    if (sum <= 0)
    {
        return 0.0; // an all-debt population has no concentration to measure
    }
    return static_cast<double>(
        static_cast<long double>(weighted) / (static_cast<long double>(n) * static_cast<long double>(sum))
    );
}

// The p-quantile of a sorted-copy sample (nearest-rank, deterministic).
inline double balQuantile(
    std::vector<std::uint64_t> values, double frac
)
{
    if (values.empty())
    {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t idx = static_cast<std::size_t>(
        std::min<double>(std::max<double>(frac, 0.0), 1.0) * static_cast<double>(values.size() - 1) + 0.5
    );
    return static_cast<double>(values[idx]);
}

struct BalanceTracker
{
    // The previous boundary's per-character XP book (the income deltas read
    // totalXp, which only ever grows — R4 — so the delta is the month's
    // faucet sum: event rewards, kill XP, consumable XP; the auto-buy's
    // spending never touches it).
    std::map<characterId, std::uint64_t> prevTotalXp;
    // (f) pooled across the run: the DRAINED per-attempt book (the loop's
    // actual post-attempt outputs) and the live active-book snapshots.
    std::vector<std::uint32_t> eventSuccessSamples;
    std::vector<std::uint32_t> eventLiveSamples;
    std::uint64_t eventAttemptTotal = 0;
};

struct BalanceSample
{
    std::uint64_t month = 0;
    // (a) the income cohort per band (alive at BOTH boundaries, bucketed by
    // this boundary's band).
    std::uint64_t xpDelta[4]    = {};
    std::uint32_t incomeNpcs[4] = {};
    // (b) the full living-NPC level histogram + the peak-stage read.
    std::uint32_t levelRows[4]  = {};
    std::uint16_t maxNpcLevel   = 0;
    std::uint32_t peakStageRows = 0;
    // (c) wealth + prices (the SCALE-grid price/base ratio sample).
    double gini                 = 0.0;
    std::uint32_t wealthCohort  = 0;
    std::uint64_t priceSamples  = 0;
    std::uint64_t priceInHalf2x = 0; // ratios inside [base/2, base*2]
    double priceMedian          = 0.0;
    double priceP95             = 0.0;
    // (f) this month's active-event read (also pooled into the tracker).
    std::size_t eventsActive    = 0;
    std::uint32_t eventSrMin    = 0;
    std::uint32_t eventSrMax    = 0;
    std::uint64_t eventAttempts = 0;
};

// One monthly balance sweep over the living stores (the soak's
// sampleInvariants sibling — the same boundary, the balance reads).
// `poolEvents` folds this boundary's active-event states into the tracker's
// equilibrium sample pool; the balance run samples the pool DAILY (see the
// monthly loop) so short-lived events cannot slip between monthly reads and
// passes false here to avoid double-counting the boundary day.
inline BalanceSample sampleBalance(
    const SoakRuntime &rt, std::uint64_t month, BalanceTracker &tracker, bool poolEvents
)
{
    BalanceSample s;
    s.month = month;

    // (a)+(b)+(c): one walk over the living NPC population (dead characters
    // are REMOVED from the store by the lifecycle sweep — the walk is the
    // living population by construction).
    std::vector<std::int64_t> purses;
    for (const auto &[id, c] : rt.characters.all())
    {
        (void)id;
        if (!rt.npcs.contains(c.id))
        {
            continue; // the player is not population
        }
        const NpcState &npc       = rt.npcs.get(c.id);
        const std::uint64_t level = c.progression.level;
        const std::size_t band    = balBandOf(level);
        s.levelRows[band] += 1;
        if (level >= BAL_BAND_START[3])
        {
            s.peakStageRows += 1;
        }
        if (level > s.maxNpcLevel)
        {
            s.maxNpcLevel = static_cast<std::uint16_t>(level);
        }

        const auto prev = tracker.prevTotalXp.find(c.id);
        if (prev != tracker.prevTotalXp.end() && c.progression.totalXp >= prev->second)
        {
            s.xpDelta[band] += c.progression.totalXp - prev->second;
            s.incomeNpcs[band] += 1;
        }

        if (npc.ageYears(rt.clock.worldTime) >= ADULT_YEARS)
        {
            purses.push_back(c.progression.currency); // (c) the wealth cohort: adults
        }
    }
    tracker.prevTotalXp.clear();
    for (const auto &[id, c] : rt.characters.all())
    {
        (void)id;
        if (rt.npcs.contains(c.id))
        {
            tracker.prevTotalXp.emplace(c.id, c.progression.totalXp);
        }
    }
    s.gini         = balGini(purses);
    s.wealthCohort = static_cast<std::uint32_t>(purses.size());

    // (c) prices: every region-good's live price against its base value, on
    // the SCALE grid (the D-ECON-1 clamp bounds the ratio to [1/4, 4] — the
    // stability read asks how much of that band the simulation actually
    // uses).
    {
        std::vector<std::uint64_t> ratios;
        const auto snap = rt.economy.snapshot();
        for (const auto &[region, goods] : snap.prices)
        {
            (void)region;
            for (const auto &[good, price] : goods)
            {
                const std::uint64_t base = rt.economy.baseValueOf(good);
                if (base == 0)
                {
                    continue;
                }
                ratios.push_back(price * SCALE / base);
            }
        }
        s.priceSamples = ratios.size();
        for (const std::uint64_t r : ratios)
        {
            if (r >= SCALE / 2 && r <= 2 * SCALE)
            {
                s.priceInHalf2x += 1;
            }
        }
        s.priceMedian = balQuantile(ratios, 0.5) / static_cast<double>(SCALE);
        s.priceP95    = balQuantile(ratios, 0.95) / static_cast<double>(SCALE);
    }

    // (f) the active-event difficulty read; every sample pools into the
    // tracker when pooling is on (one per active event per sample).
    s.eventsActive = rt.eventEngine.activeCount();
    for (const auto &[evId, ev] : rt.eventEngine.active())
    {
        (void)evId;
        const std::uint32_t sr = ev.currentState.successRate;
        if (s.eventSrMin == 0 || sr < s.eventSrMin)
        {
            s.eventSrMin = sr;
        }
        if (sr > s.eventSrMax)
        {
            s.eventSrMax = sr;
        }
        s.eventAttempts += ev.currentState.attempts;
        if (poolEvents)
        {
            tracker.eventSuccessSamples.push_back(sr);
        }
    }
    tracker.eventAttemptTotal += s.eventAttempts;
    return s;
}

// The (f) event read: the engine's per-attempt sample book drains into the
// equilibrium pool (the post-attempt successRates — the loop's actual
// outputs), plus every active event's current state (the live book). A
// state's successRate moves at its attempt's conclusion (D-DIFF-1) and ended
// events leave the book the same tick (the per-tick sweep), so the drained
// per-attempt book IS the honest equilibrium population.
inline void sampleEventEquilibrium(
    const SoakRuntime &rt, BalanceTracker &tracker
)
{
    for (const std::uint32_t sr : events::drainDifficultySamples())
    {
        tracker.eventSuccessSamples.push_back(sr);
        tracker.eventAttemptTotal += 1;
    }
    for (const auto &[evId, ev] : rt.eventEngine.active())
    {
        (void)evId;
        tracker.eventLiveSamples.push_back(ev.currentState.successRate);
    }
}

// ---- the (a) faucet probe: one real kill through the death path ------------------
//
// The playerless world's NPC XP income is structurally zero (no paying event
// spawns headless, nobody dies — the report carries the read), so the A-120
// curve's income side is the adventurer's faucet model. This probe verifies
// the model's one sim-side constant through the REAL death path: a player
// kill pays A-35's 0.8 x the A-34 loss, i.e. 1.6 x levelCosts[victim.level]
// exactly. The measured row anchors the report's curve composition.
struct KillXpProbe
{
    bool ran                  = false;
    std::uint64_t victimLevel = 0;
    std::uint64_t expectedXp  = 0;
    std::uint64_t gainedXp    = 0;
};

inline KillXpProbe runKillXpProbe(
    SoakRuntime &rt
)
{
    KillXpProbe out;
    // Any living, adult, sapient, non-invulnerable NPC (id-ascending).
    characterId victim = 0;
    for (const auto &[id, c] : rt.characters.all())
    {
        (void)id;
        if (!rt.npcs.contains(c.id) || c.species == 0)
        {
            continue;
        }
        const NpcState &n = rt.npcs.get(c.id);
        if (n.invulnerable || !npc::canTestify(c, &n, rt.clock.worldTime, rt.registry))
        {
            continue;
        }
        victim = c.id;
        break;
    }
    if (victim == 0)
    {
        return out;
    }
    const Character &vc = rt.characters.get(victim);
    out.victimLevel     = vc.progression.level;
    out.expectedXp      = NPC_VICTIM_PENALTY_FACTOR * levelCosts()[out.victimLevel] / SCALE * NPC_KILL_XP_GAIN / SCALE;

    // Stand next to the victim (the kill is a real step-10 application; the
    // player's own grid entry self-heals at the next daily sync).
    Character &player    = rt.characters.get(rt.playerCharacterId);
    player.body.position = vc.body.position;
    rt.player.position   = player.body.position;

    const std::uint64_t before = player.progression.totalXp;
    (void)applyDamage(DamageEvent{rt.playerCharacterId, victim, 100000, DamageType::Melee}, rt.simCtx);
    runTicks(rt, 2); // the death materializes at step 11
    out.gainedXp = rt.characters.get(rt.playerCharacterId).progression.totalXp - before;
    out.ran      = true;

    rt.justiceStore.jailUntil.erase(rt.playerCharacterId); // the kill may have recorded a case
    return out;
}

inline void printBalanceLine(
    const BalanceSample &s, double monthDays
)
{
    std::printf(
        "  balance: income/day e %.1f (n %u), m %.1f (n %u), l %.1f (n %u), p %.1f (n %u)"
        " | levels e/m/l/p %u/%u/%u/%u max %u peak %u"
        " | gini %.3f (n %u) | prices %zu med %.2f p95 %.2f inHalf2x %.1f%%"
        " | events %zu sr %u-%u attempts %llu\n",
        monthDays > 0 && s.incomeNpcs[0] > 0 ? static_cast<double>(s.xpDelta[0]) / (s.incomeNpcs[0] * monthDays) : 0.0,
        s.incomeNpcs[0],
        monthDays > 0 && s.incomeNpcs[1] > 0 ? static_cast<double>(s.xpDelta[1]) / (s.incomeNpcs[1] * monthDays) : 0.0,
        s.incomeNpcs[1],
        monthDays > 0 && s.incomeNpcs[2] > 0 ? static_cast<double>(s.xpDelta[2]) / (s.incomeNpcs[2] * monthDays) : 0.0,
        s.incomeNpcs[2],
        monthDays > 0 && s.incomeNpcs[3] > 0 ? static_cast<double>(s.xpDelta[3]) / (s.incomeNpcs[3] * monthDays) : 0.0,
        s.incomeNpcs[3],
        s.levelRows[0],
        s.levelRows[1],
        s.levelRows[2],
        s.levelRows[3],
        static_cast<unsigned>(s.maxNpcLevel),
        s.peakStageRows,
        s.gini,
        s.wealthCohort,
        s.priceSamples,
        s.priceMedian,
        s.priceP95,
        s.priceSamples > 0 ? 100.0 * static_cast<double>(s.priceInHalf2x) / static_cast<double>(s.priceSamples) : 0.0,
        s.eventsActive,
        s.eventSrMin,
        s.eventSrMax,
        static_cast<unsigned long long>(s.eventAttempts)
    );
}

// ---- the scripted player-crime probe (the card's (d)) ----------------------------
//
// Identical player-crime trials in settlements of every shipped law set; the
// outcome split IS the "convictions per 100 player-crimes by territory law
// set" measurement. The trial walks the REAL pipeline: the punch is step 10's
// applyDamage (the same entry the combat tests drive), the case build is
// 4.2's case ticks over 4.1's real evidence, the conviction is 4.3's
// surrender (the doc's visible way out for a wanted player).
//
// Harness actions, named (the headless equivalents of walking and serving):
//   * the player teleports between settlements and stands 1 m from the
//     victim (a level-1 stroll across a 4 km continent would dominate the
//     probe's cost without touching what it measures);
//   * co-presence sightings record through 3.9's public store seam under the
//     PINNED rule (observer within A-55's 15 m, culprit unmasked) — the
//     NEAR-tier co-presence driver itself never shipped (no call site; see
//     the balance report's known-notes), so without this action no witness
//     can ever know a stranger's identity and every law set would measure
//     the same structural zero;
//   * the thin arm teleports the victim + two witnesses to an isolated spot
//     inside the same region cell (balThinTrialsAt — the walk out of town);
//   * the ramp series stamps an escalating notoriety floor (the store stamp
//     conviction's addNotoriety performs — balRampSeries isolates the damper);
//   * the exemption spot-check installs the xenophobic law set on a default
//     village when the variance roll drew none (runExemptionSpotCheck — the
//     law set is the subject, the pointer restored after);
//   * each trial ends with the conviction's jail stamp cleared and the
//     player healed: the probe measures classification rates, not 800-day
//     sentences of idle jail (D-JAIL-1's catch-up would spend ~11.7M ticks
//     per assault trial re-proving the pacing, not the justice math);
//   * the series carries the repeat-offender ramp (notoriety is per
//     culprit, global — every conviction lowers every later threshold); the
//     report splits the series into halves so the ramp is visible.
//
// Trials with no qualifying victim in the crowd count separately
// (noWitness) — a zero-witness assault cannot convict in ANY law set, so
// counting it in the denominator would measure crowd density, not laws.

struct CrimeTrials
{
    std::uint32_t committed     = 0; // trials whose OWN crime type produced a case
    std::uint32_t noWitness     = 0; // no qualifying victim/crowd near the center
    std::uint32_t convicted     = 0; // own-type warrant -> surrender -> 4.3's CONVICTED
    std::uint32_t wanted        = 0; // own-type warrant issued, surrender refused (a probe bug — loud)
    std::uint32_t investigating = 0; // own-type case crossed half the threshold (4.6's shadowing) but not the threshold
    std::uint32_t below         = 0; // own-type case recorded, still UNKNOWN at the trial's read
    std::uint32_t exempt        = 0; // the hard gate refused the own-type row (or no crime landed)
};

// Trials are TYPED: the as-built damage order records evidence before death
// (3.2's pinned order), so a kill produces the killing blow's ASSAULT case
// AND the death's MURDER case — the murder column must read the MURDER
// case's own outcome, never the blow's, or every law set would measure its
// assault threshold under a murder label.

struct LawSetTrials
{
    std::string lawKey;
    std::uint32_t settlements = 0;
    CrimeTrials assault;
    CrimeTrials murder;
    CrimeTrials thin; // the two-witness arm (the thresholds' operating read — see balThinTrialsAt)
};

inline std::size_t balPlayerEvidenceRows(
    const SoakRuntime &rt, CrimeType type
)
{
    std::size_t rows = 0;
    for (const auto &[territory, cases] : rt.justiceStore.evidenceStore)
    {
        (void)territory;
        for (const justice::Evidence &e : cases)
        {
            if (e.culpritId == rt.playerCharacterId && e.type == type)
            {
                ++rows;
            }
        }
    }
    return rows;
}

// The crime types of the player's LIVE warrants in one territory (the
// warrant's evidence's type — a warrant exists per case that crossed its
// law set's threshold AND identified).
inline std::set<CrimeType> balPlayerWarrantTypes(
    SoakRuntime &rt, territoryId territory
)
{
    std::set<CrimeType> types;
    for (const auto &[warrantId, w] : rt.justiceStore.warrants)
    {
        (void)warrantId;
        if (w.culpritId != rt.playerCharacterId || w.territory != territory)
        {
            continue;
        }
        if (const justice::Evidence *e = justice::findEvidence(rt.justiceStore, w.evidenceId); e != nullptr)
        {
            types.insert(e->type);
        }
    }
    return types;
}

// One trial: position the player next to the victim, let the crowd see them
// (the pinned 3.9 sighting rule), commit the crime through step 10, run the
// case tick + enforcement, then surrender if the warrant came. `murder`
// selects the trial's own crime type (see CrimeTrials' note).
//
// `resetNotoriety` (a named harness action): the player's notoriety row is
// zeroed before the trial. Notoriety is per culprit and GLOBAL (every
// conviction lowers every later threshold everywhere), so an unreset 40-trial
// series would measure the repeat-offender ramp, not the law sets — the ramp
// gets its own dedicated series (balRampSeries) instead.
inline void balRunTrial(
    SoakRuntime &rt,
    territoryId territory,
    characterId victimId,
    bool murder,
    CrimeTrials &out,
    bool resetNotoriety = true
)
{
    const CrimeType ownType = murder ? CrimeType::MURDER : CrimeType::ASSAULT;
    const Position stand    = rt.characters.get(victimId).body.position;
    Position playerPos      = stand;
    playerPos.x += 1.0;
    Character &player    = rt.characters.get(rt.playerCharacterId);
    player.body.position = playerPos;
    rt.player.position   = playerPos;
    if (resetNotoriety)
    {
        rt.justiceStore.reputations[rt.playerCharacterId].notoriety = 0;
    }

    // The co-presence driver action: every qualifying observer within
    // A-55's IDENTIFY_RANGE records the player's sighting (unmasked), the
    // pinned 3.9 rule the NEAR-tier driver would run every tick.
    const std::uint64_t version = rt.perceptionSink.culpritAppearanceVersionNow(rt.playerCharacterId);
    for (const characterId observer : rt.grid.idsWithinRadius(playerPos, static_cast<double>(IDENTIFY_RANGE_M)))
    {
        if (observer == rt.playerCharacterId || !rt.npcs.contains(observer) || !rt.characters.exists(observer))
        {
            continue;
        }
        rt.perceptionSink.store().recordSighting(
            observer, npc::Sighting{rt.playerCharacterId, version, rt.clock.worldTime}, /*culpritMasked=*/false
        );
    }

    const std::size_t rowsBefore = balPlayerEvidenceRows(rt, ownType);
    (void)applyDamage(
        DamageEvent{rt.playerCharacterId, victimId, murder ? 100000ull : 5ull, DamageType::Melee}, rt.simCtx
    );
    runTicks(rt, 2); // step 6's case tick evaluates the fresh case; step 6's enforcement answers

    const std::size_t rowsAfter = balPlayerEvidenceRows(rt, ownType);
    if (rowsAfter == rowsBefore)
    {
        out.exempt += 1; // the exemption hard gate refused the own-type row — no case exists
    } else
    {
        out.committed += 1;
        const std::set<CrimeType> warrants = balPlayerWarrantTypes(rt, territory);
        if (warrants.count(ownType) != 0)
        {
            if (justice::surrender(rt.playerCharacterId, territory, rt.simCtx))
            {
                out.convicted += 1;
            } else
            {
                out.wanted += 1; // the doc's surrender refused — loud, never silent
            }
        } else
        {
            // The own-type case's stage at the read: INVESTIGATING (A-72's
            // half-threshold — 4.6's shadowing signal fired) or still
            // UNKNOWN (the score has not reached half the threshold).
            bool investigating = false;
            for (const auto &[caseTerritory, cases] : rt.justiceStore.evidenceStore)
            {
                (void)caseTerritory;
                for (const justice::Evidence &e : cases)
                {
                    if (e.culpritId == rt.playerCharacterId && e.type == ownType
                        && e.status == EvidenceStatus::INVESTIGATING)
                    {
                        investigating = true;
                    }
                }
            }
            if (investigating)
            {
                out.investigating += 1;
            } else
            {
                out.below += 1;
            }
        }
    }

    // The jail-skip + heal (named above): the sentence math is 4.3's tests'
    // job; the probe's next trial needs a free, whole player.
    rt.justiceStore.jailUntil.erase(rt.playerCharacterId);
    player.body.currentHp = player.body.maxHp;
    runTicks(rt, 20); // the spacing beat between trials
}

// The settlements of one law set, id-ascending (R7), up to `perSet` rows.
inline std::vector<territoryId> balSettlementsOfLaw(
    const SoakRuntime &rt, const std::string &lawKey, std::uint32_t perSet
)
{
    std::vector<territoryId> found;
    for (const auto &[id, t] : rt.world->territories())
    {
        if ((t.type == TerritoryType::VILLAGE || t.type == TerritoryType::CITY) && t.region != world::kNoRegion
            && t.laws != nullptr && t.laws->key == lawKey)
        {
            found.push_back(id);
            if (found.size() >= perSet)
            {
                break;
            }
        }
    }
    return found;
}

// The qualifying victim standing in the DENSEST witness crowd within
// `radiusM` of `center` — the probe's "punch into the market crowd" pick.
// After months of MID drift a settlement's NPCs scatter; the victim picked by
// bare distance stood alone with their own report (every village case scored
// below even the investigate bar — a crowd-density read, not a law-set read).
// For each candidate the qualifier count within a WITNESS_RADIUS disk is the
// score that matters; ties break toward the center, then by id (R7).
inline characterId balPickVictim(
    const SoakRuntime &rt, const Position &center, double radiusM
)
{
    const std::vector<characterId> around = rt.grid.idsWithinRadius(center, radiusM);
    std::vector<characterId> candidates;
    for (const characterId id : around)
    {
        if (id == rt.playerCharacterId || !rt.npcs.contains(id) || !rt.characters.exists(id))
        {
            continue;
        }
        const Character &c = rt.characters.get(id);
        const NpcState &n  = rt.npcs.get(id);
        if (n.invulnerable || !npc::canTestify(c, &n, rt.clock.worldTime, rt.registry))
        {
            continue;
        }
        candidates.push_back(id);
    }
    characterId best      = 0;
    std::size_t bestCrowd = 0;
    double bestDistanceSq = 0.0;
    for (const characterId id : candidates)
    {
        const Position &p   = rt.characters.get(id).body.position;
        const auto crowdIds = rt.grid.idsWithinRadius(p, static_cast<double>(WITNESS_RADIUS_M));
        std::size_t crowd   = 0;
        for (const characterId w : crowdIds)
        {
            if (w == rt.playerCharacterId || !rt.npcs.contains(w) || !rt.characters.exists(w))
            {
                continue;
            }
            const NpcState &wn = rt.npcs.get(w);
            if (wn.invulnerable || !npc::canTestify(rt.characters.get(w), &wn, rt.clock.worldTime, rt.registry))
            {
                continue;
            }
            crowd += 1;
        }
        const double dx         = p.x - center.x;
        const double dz         = p.z - center.z;
        const double distanceSq = dx * dx + dz * dz;
        if (crowd > bestCrowd || (crowd == bestCrowd && best != 0 && distanceSq < bestDistanceSq))
        {
            best           = id;
            bestCrowd      = crowd;
            bestDistanceSq = distanceSq;
        }
    }
    return best;
}

// ---- the thin-crowd arm -----------------------------------------------------------
//
// The crowd arm above saturates: a settlement crowd's aggregate report weight
// (clamped at SCALE) crosses every shipped conviction threshold (4000-8000)
// in one case tick, so every law set reads 100% and the measurement reduces
// to crowd density. The thresholds' OPERATING read is the single-witness
// scene: the victim and exactly ONE qualifying witness stand 150-400 m out
// of the settlement center (inside the same 1024 m region cell, so
// territoryAt still files the case under the settlement's law set; outside
// the center crowd's A-60 30 m witness radius), the headless equivalent of
// punching a farmer behind the barn with one friend watching. One report
// (roleBaseWeight 1200-9000 by role and the integrity draw) against
// thresholds 6000-7000 — the band where the law sets actually differentiate
// (a 6600-weight case convicts under default/xenophobic, not tolerant_city).

// The qualifying NPCs within `radiusM` of `center` (id-ascending, R7) — the
// thin scene's casting pool. The radius spans the whole 1024 m region cell:
// earlier arms' out-stationed trios stay castable (they are teleported to
// the new scene anyway), so repeated probes never starve the pool.
inline std::vector<characterId> balQualifyingCast(
    const SoakRuntime &rt, const Position &center, double radiusM
)
{
    std::vector<characterId> cast;
    for (const characterId id : rt.grid.idsWithinRadius(center, radiusM))
    {
        if (id == rt.playerCharacterId || !rt.npcs.contains(id) || !rt.characters.exists(id))
        {
            continue;
        }
        const Character &c = rt.characters.get(id);
        const NpcState &n  = rt.npcs.get(id);
        if (n.invulnerable || !npc::canTestify(c, &n, rt.clock.worldTime, rt.registry))
        {
            continue;
        }
        cast.push_back(id);
    }
    return cast;
}

// A scene spot 150-400 m out of `center` with no qualifying NPC within 31 m
// (just outside the A-60 witness radius — a stray would add a report and
// mute the single-witness read). Eight compass directions x six radii,
// scanned in order (R7); falls back to the bare 200 m north spot when every
// candidate is busy.
inline Position balThinSceneSpot(
    const SoakRuntime &rt, const Position &center
)
{
    static constexpr double kRadii[6] = {150.0, 200.0, 250.0, 300.0, 350.0, 400.0};
    for (const double r : kRadii)
    {
        for (std::uint32_t dir = 0; dir < 8; ++dir)
        {
            static constexpr double kCos[8] = {1, 0.7071, 0, -0.7071, -1, -0.7071, 0, 0.7071};
            static constexpr double kSin[8] = {0, 0.7071, 1, 0.7071, 0, -0.7071, -1, -0.7071};
            const Position spot{center.x + kCos[dir] * r, 0.0, center.z + kSin[dir] * r};
            bool busy = false;
            for (const characterId id : rt.grid.idsWithinRadius(spot, 31.0))
            {
                if (id == rt.playerCharacterId || !rt.npcs.contains(id) || !rt.characters.exists(id))
                {
                    continue;
                }
                const Character &c = rt.characters.get(id);
                const NpcState &n  = rt.npcs.get(id);
                if (npc::canTestify(c, &n, rt.clock.worldTime, rt.registry))
                {
                    busy = true; // a stray witness would contaminate the single-witness read
                    break;
                }
            }
            if (!busy)
            {
                return spot;
            }
        }
    }
    return Position{center.x + 200.0, 0.0, center.z};
}

// One thin-arm settlement: `trials` single-witness assault trials, casting
// the victim and the one witness from the region cell's qualifying pool
// (id-ascending rotation so no villager takes every punch), teleporting the
// pair to the isolated spot (a named harness action — the walk out of town).
// The trial itself is balRunTrial verbatim: real punch, real case tick, real
// surrender. `resetNotoriety` passes through (the ramp series stamps its own
// floor and needs the trial to leave it standing).
inline void balThinTrialsAt(
    SoakRuntime &rt, territoryId territory, std::uint32_t trials, CrimeTrials &out, bool resetNotoriety = true
)
{
    const world::Territory *ter = rt.world->territory(territory);
    if (ter == nullptr || ter->region == world::kNoRegion)
    {
        return;
    }
    const Position center               = world::regionCenter(ter->region);
    const std::vector<characterId> cast = balQualifyingCast(rt, center, 400.0);
    if (cast.size() < 2)
    {
        out.noWitness += trials; // no castable pair — the settlement is too thin to stage the scene
        return;
    }
    for (std::uint32_t i = 0; i < trials; ++i)
    {
        const std::size_t base     = (static_cast<std::size_t>(i) * 2) % (cast.size() - 1);
        const characterId victimId = cast[base];
        const characterId witnessA = cast[base + 1];
        const Position spot        = balThinSceneSpot(rt, center);
        const Position victimSpot  = spot;
        const Position aSpot{spot.x - 3.0, 0.0, spot.z + 2.0};
        rt.grid.move(victimId, rt.characters.get(victimId).body.position, victimSpot);
        rt.grid.move(witnessA, rt.characters.get(witnessA).body.position, aSpot);
        rt.characters.get(victimId).body.position = victimSpot;
        rt.characters.get(witnessA).body.position = aSpot;

        balRunTrial(rt, territory, victimId, /*murder=*/false, out, resetNotoriety);
    }
}

inline void balTrialsAt(
    SoakRuntime &rt, territoryId territory, std::uint32_t assaultTrials, std::uint32_t murderTrials, LawSetTrials &row
)
{
    const world::Territory *ter = rt.world->territory(territory);
    if (ter == nullptr || ter->region == world::kNoRegion)
    {
        return;
    }
    const Position center = world::regionCenter(ter->region);

    for (std::uint32_t i = 0; i < assaultTrials; ++i)
    {
        const characterId victim = balPickVictim(rt, center, 40.0);
        if (victim == 0)
        {
            row.assault.noWitness += 1;
            continue;
        }
        balRunTrial(rt, territory, victim, /*murder=*/false, row.assault); // the villager punch
    }
    for (std::uint32_t i = 0; i < murderTrials; ++i)
    {
        const characterId victim = balPickVictim(rt, center, 40.0);
        if (victim == 0)
        {
            row.murder.noWitness += 1;
            continue;
        }
        balRunTrial(rt, territory, victim, /*murder=*/true, row.murder); // the kill
    }
}

// The repeat-offender ramp, measured as a controlled response: the SAME
// single-witness scene repeated with the player's notoriety floor escalating
// per trial (the store stamp conviction's addNotoriety performs — written
// directly so the series isolates the caseThreshold damper from whatever
// notoriety the earlier probes' convictions left in the store; a real
// conviction mid-series only stacks on top). The damper — threshold x
// SCALE / (SCALE + notoriety) — can only widen convictions as the floor
// climbs: an assault conviction adds 3000 notoriety, so 10 escalations
// (0..27000) cut the effective threshold to ~27% of its face value. The
// first-half vs second-half conviction split IS the measurement (the rate
// may only rise).
struct RampSeries
{
    std::uint32_t firstHalfN          = 0;
    std::uint32_t firstHalfConvicted  = 0;
    std::uint32_t secondHalfN         = 0;
    std::uint32_t secondHalfConvicted = 0;
    std::uint32_t firstHalfBelow      = 0; // cases not even at the investigate bar early
    std::uint32_t secondHalfBelow     = 0;
    std::uint32_t investigating       = 0;
    std::uint32_t exempt              = 0;
    std::uint32_t noWitness           = 0;
};

inline RampSeries balRampSeries(
    SoakRuntime &rt, territoryId territory, std::uint32_t trials
)
{
    RampSeries out;
    const world::Territory *ter = rt.world->territory(territory);
    if (ter == nullptr || ter->region == world::kNoRegion)
    {
        return out;
    }
    for (std::uint32_t i = 0; i < trials; ++i)
    {
        // The escalating floor: ASSAULT_NOTORIETY_GAIN (3000, the crime's own
        // notorietyGain) per prior conviction, stamped straight into the
        // reputation row (the decay is step-based per game-day — a 22-tick
        // trial cannot erode it).
        rt.justiceStore.reputations[rt.playerCharacterId].notoriety = static_cast<std::int32_t>(i * 3000);
        CrimeTrials one;
        balThinTrialsAt(rt, territory, 1, one, /*resetNotoriety=*/false);
        const bool convicted = one.convicted > 0;
        out.investigating += one.investigating;
        out.exempt += one.exempt;
        out.noWitness += one.noWitness;
        if (i < trials / 2)
        {
            out.firstHalfN += 1;
            out.firstHalfConvicted += convicted ? 1u : 0u;
            out.firstHalfBelow += one.below;
        } else
        {
            out.secondHalfN += 1;
            out.secondHalfConvicted += convicted ? 1u : 0u;
            out.secondHalfBelow += one.below;
        }
    }
    return out;
}

// The (d) probe over every shipped law set present in this world. Returns
// one row per law key (id-ascending first-seen order), each with the
// settlement count it ran at.
inline std::vector<LawSetTrials> runCrimeProbe(
    SoakRuntime &rt,
    std::uint32_t settlementsPerLaw,
    std::uint32_t assaultTrials,
    std::uint32_t murderTrials,
    std::uint32_t thinTrials
)
{
    // The law keys present among settlements, first-seen order (R7 map walk).
    std::vector<std::string> lawKeys;
    for (const auto &[id, t] : rt.world->territories())
    {
        (void)id;
        if ((t.type == TerritoryType::VILLAGE || t.type == TerritoryType::CITY) && t.region != world::kNoRegion
            && t.laws != nullptr && std::find(lawKeys.begin(), lawKeys.end(), t.laws->key) == lawKeys.end())
        {
            lawKeys.push_back(t.laws->key);
        }
    }

    std::vector<LawSetTrials> rows;
    for (const std::string &key : lawKeys)
    {
        LawSetTrials row;
        row.lawKey                                 = key;
        const std::vector<territoryId> settlements = balSettlementsOfLaw(rt, key, settlementsPerLaw);
        row.settlements                            = static_cast<std::uint32_t>(settlements.size());
        for (const territoryId t : settlements)
        {
            balTrialsAt(rt, t, assaultTrials, murderTrials, row);
            balThinTrialsAt(rt, t, thinTrials, row.thin);
        }
        rows.push_back(row);
    }
    return rows;
}

// The (d) species-conflict spot-check: MURDER of a beastkin victim inside a
// xenophobic_village must refuse the evidence row outright (the hard gate),
// while the same kill on a human victim records. When the seed's 10%-per-
// kingdom variance roll drew no xenophobic settlement, the check installs
// the shipped law set on one default-law village instead (a named harness
// action — the law SET is the test subject, not the worldgen draw; the
// original laws pointer is restored at the end). Returns false when the
// world lacks the pieces (no settlement to stage it on / no living
// beastkin) — the caller reports the skip.
inline bool runExemptionSpotCheck(
    SoakRuntime &rt, bool &beastkinRefused, bool &humanRecorded
)
{
    beastkinRefused = false;
    humanRecorded   = false;

    // The staging settlement: a worldgen-drawn xenophobic village when the
    // variance roll made one, else the law installed on a default village.
    bool installed    = false;
    territoryId stage = 0;
    if (const std::vector<territoryId> xeno = balSettlementsOfLaw(rt, "xenophobic_village", 1); !xeno.empty())
    {
        stage = xeno[0];
    } else if (const std::vector<territoryId> homes = balSettlementsOfLaw(rt, "default", 1); !homes.empty())
    {
        world::Territory *home       = rt.world->territory(homes[0]);
        const data::LawSet *xenoLaws = rt.registry.lawSet("xenophobic_village");
        if (home == nullptr || xenoLaws == nullptr)
        {
            return false;
        }
        home->laws = xenoLaws;
        stage      = homes[0];
        installed  = true;
    } else
    {
        return false;
    }
    world::Territory *ter = rt.world->territory(stage);
    if (ter == nullptr || ter->region == world::kNoRegion)
    {
        return false;
    }
    const data::LawSet *originalLaws = ter->laws;
    const Position center            = world::regionCenter(ter->region);

    // A living beastkin anywhere in the world (id-ascending); the species id
    // resolves through the data table (species ids are data-assigned).
    speciesId beastkinSpecies = 0;
    for (const data::SpeciesData &row : rt.registry.speciesTable())
    {
        if (row.key == "beastkin")
        {
            beastkinSpecies = row.id;
            break;
        }
    }
    characterId beastkin = 0;
    for (const auto &[id, c] : rt.characters.all())
    {
        (void)id;
        if (c.species == beastkinSpecies && beastkinSpecies != 0 && rt.npcs.contains(c.id) && c.body.currentHp > 0)
        {
            beastkin = c.id;
            break;
        }
    }
    if (beastkin == 0)
    {
        return false;
    }

    // Bring the victim to the village (the harness action: worldgen's
    // species mixes never settle beastkin inside a xenophobic village — the
    // A-127 rows are the doc's own conflict shape — so the probe moves the
    // specimen to the law instead of the law to the specimen).
    Character &bk         = rt.characters.get(beastkin);
    const Position bkFrom = bk.body.position;
    bk.body.position      = center;
    rt.grid.move(beastkin, bkFrom, center);

    // The beastkin kill: the MURDER hard gate must refuse the row entirely —
    // the observable is the store (no NEW MURDER-typed evidence row for the
    // player; the killing blow's ASSAULT row is the as-built "evidence before
    // death" order, a separate crime the exemption does not cover).
    CrimeTrials scratch;
    const std::size_t murderBefore = balPlayerEvidenceRows(rt, CrimeType::MURDER);
    balRunTrial(rt, stage, beastkin, /*murder=*/true, scratch);
    beastkinRefused = balPlayerEvidenceRows(rt, CrimeType::MURDER) == murderBefore;
    rt.justiceStore.jailUntil.erase(rt.playerCharacterId);

    // The human control in the same village: the MURDER row must appear. The
    // pick EXCLUDES the exemption's species (the gate keys the VICTIM's
    // species — a mixed village holds beastkin/taur too, whose murder the
    // same law set would refuse again) and scans the whole region cell (the
    // earlier probes' out-stationed villagers are still castable).
    characterId human = 0;
    {
        const data::LawSet &stageLaws = *ter->laws;
        for (const characterId id : balQualifyingCast(rt, center, 400.0))
        {
            const Character &c  = rt.characters.get(id);
            const auto exemptIt = stageLaws.exemptions.find(c.species);
            if (exemptIt != stageLaws.exemptions.end() && exemptIt->second[static_cast<std::size_t>(CrimeType::MURDER)])
            {
                continue; // the same hard gate would refuse this victim — not a control
            }
            human = id;
            break;
        }
    }
    if (human != 0)
    {
        const std::size_t murderRowsBefore = balPlayerEvidenceRows(rt, CrimeType::MURDER);
        CrimeTrials control;
        balRunTrial(rt, stage, human, /*murder=*/true, control);
        humanRecorded = balPlayerEvidenceRows(rt, CrimeType::MURDER) > murderRowsBefore;
    }

    // The install is undone — the probe leaves the world's law map exactly
    // as the variance roll made it (the only durable writes are the trials'
    // own cases, the same residue every other probe leaves).
    if (installed)
    {
        ter->laws = originalLaws;
    }
    return true;
}

// (e) the bounty-vs-penalty invariant over the shipped crime rows: the
// D-BOUNTY-1 bounty (min cap, severity x 5) stays below the conviction's
// currency leg (the fine, severity x 10 — FINE_CURRENCY_PER_POINT is twice
// the BOUNTY_CURRENCY_PER_POINT rate) for every severity, with the XP debt
// and the jail term stacked on top. Pure table math — no sim. Prints one row
// per crime type for the report and returns false on any violation.
inline bool balBountyBelowPenalty(
    const SoakRuntime &rt
)
{
    bool all          = true;
    const auto &costs = levelCosts();
    std::printf(
        "bounty vs penalty (e): D-BOUNTY-1 min(%llu, severity x %llu) < fine severity x %llu (+ 2x levelCost XP debt + "
        "jail)\n",
        static_cast<unsigned long long>(BOUNTY_CAP),
        static_cast<unsigned long long>(BOUNTY_CURRENCY_PER_POINT),
        static_cast<unsigned long long>(FINE_CURRENCY_PER_POINT)
    );
    for (std::uint8_t t = 0; t < static_cast<std::uint8_t>(CrimeType::COUNT); ++t)
    {
        const CrimeType type       = static_cast<CrimeType>(t);
        const data::CrimeData &row = rt.registry.crime(type);
        const std::uint64_t bounty
            = std::min<std::uint64_t>(BOUNTY_CAP, static_cast<std::uint64_t>(row.severity) * BOUNTY_CURRENCY_PER_POINT);
        const std::uint64_t fine     = static_cast<std::uint64_t>(row.severity) * FINE_CURRENCY_PER_POINT;
        const std::uint64_t debtAtL1 = CONVICTION_PENALTY_FACTOR * costs[1] / SCALE; // zone 1x, rep 0 (the floor)
        const bool holds             = bounty < fine + debtAtL1;
        all                          = all && holds;
        std::printf(
            "  %-11s severity %-5u bounty %-6llu fine %-6llu l1XPdebt >= %-5llu %s\n",
            std::string(data::crimeTypeKey(type)).c_str(),
            row.severity,
            static_cast<unsigned long long>(bounty),
            static_cast<unsigned long long>(fine),
            static_cast<unsigned long long>(debtAtL1),
            holds ? "ok" : "VIOLATED"
        );
    }
    return all;
}

// ---- Phase-4 acceptance scenario presets (task 8.5) ---------------------------
// These presets prepare deterministic, real-store worlds for walkthrough
// cases. The walkthrough remains player-driven after setup; the presets never
// mutate save files or bypass the simulation's public subsystem APIs.
struct AcceptanceWar
{
    factionId attacker  = 0;
    factionId defender  = 0;
    std::uint64_t warId = 0;
    regionId region     = world::kNoRegion;
};

inline bool syncScenarioClocks(
    SoakRuntime &rt
)
{
    rt.simCtx.now             = rt.clock.worldTime;
    rt.simCtx.realTimeSeconds = rt.clock.realTime;
    return rt.simCtx.worldStore != nullptr && rt.simCtx.characters != nullptr;
}

inline bool createAndClaimFrontier(
    SoakRuntime &rt, bool safe, territoryId &outTerritory, std::string &error
)
{
    if (!syncScenarioClocks(rt) || rt.world == nullptr)
    {
        error = "scenario setup requires a wired world";
        return false;
    }

    regionId frontier = world::kNoRegion;
    for (const auto &[region, row] : rt.world->regions())
    {
        (void)row;
        if (rt.world->territoryOfRegion(region) == 0)
        {
            frontier = region;
            break;
        }
    }
    if (frontier == world::kNoRegion)
    {
        error = "worldgen produced no unclaimed frontier region for the scenario";
        return false;
    }

    const territoryId nextId = rt.world->territories().empty() ? 1 : rt.world->territories().rbegin()->first + 1;
    world::Territory territory{};
    territory.id     = nextId;
    territory.region = frontier;
    territory.safe   = safe;
    territory.type   = TerritoryType::VILLAGE;
    rt.world->addTerritory(territory);
    rt.world->setRegionTerritory(frontier, nextId);

    const Position destination                            = world::regionCenter(frontier);
    const Position previous                               = rt.characters.get(rt.playerCharacterId).body.position;
    rt.player.position                                    = destination;
    rt.characters.get(rt.playerCharacterId).body.position = destination;
    rt.grid.move(rt.playerCharacterId, previous, destination);
    rt.characters.get(rt.playerCharacterId).progression.currency = 1'000'000;

    const factions::ClaimResult result = rt.claimStore.claim(nextId, rt.playerCharacterId, rt.simCtx);
    if (result.isErr())
    {
        error = "frontier claim refused by ClaimStore (error " + std::to_string(static_cast<int>(result.error())) + ")";
        return false;
    }
    outTerritory = nextId;
    return true;
}

inline bool prepareAcceptanceWar(
    SoakRuntime &rt,
    AcceptanceWar &out,
    std::string &error,
    bool massacre              = false,
    regionId regionOverride    = world::kNoRegion,
    factionId defenderOverride = 0
)
{
    if (!syncScenarioClocks(rt))
    {
        error = "scenario setup requires a wired world";
        return false;
    }

    factionId attacker = 0;
    factionId defender = defenderOverride;
    for (const auto &[id, faction] : rt.factionStore.all())
    {
        if (faction.members.empty())
            continue;
        if (id != defender && attacker == 0)
            attacker = id;
        if (defender == 0 && id != attacker)
            defender = id;
    }
    if (attacker == 0 || defender == 0 || attacker == defender)
    {
        error = "worldgen did not produce two distinct populated factions";
        return false;
    }

    const factions::Faction *attackerRow = rt.factionStore.find(attacker);
    const factions::Faction *defenderRow = rt.factionStore.find(defender);
    if (attackerRow == nullptr || defenderRow == nullptr || defenderRow->members.empty())
    {
        error = "acceptance war faction membership is incomplete";
        return false;
    }
    characterId attackerPowerMember = 0;
    for (const characterId member : attackerRow->members)
    {
        if (rt.characters.exists(member) && rt.characters.get(member).body.currentHp != 0)
        {
            attackerPowerMember = member;
            break;
        }
    }
    if (attackerPowerMember == 0)
    {
        error = "acceptance attacker has no living members";
        return false;
    }
    std::uint64_t defenderPower = 0;
    for (const characterId member : defenderRow->members)
    {
        if (rt.characters.exists(member) && rt.characters.get(member).body.currentHp != 0)
        {
            rt.characters.get(member).cachedPower = 1'000ULL;
            defenderPower += 1'000ULL;
        }
    }
    if (defenderPower == 0)
    {
        error = "acceptance defender has no living members";
        return false;
    }
    for (const characterId member : attackerRow->members)
    {
        if (rt.characters.exists(member))
            rt.characters.get(member).cachedPower = member == attackerPowerMember ? defenderPower * 2ULL : 0ULL;
    }

    regionId targetRegion = regionOverride;
    if (targetRegion == world::kNoRegion)
    {
        // Prefer a territory titled by the defender so the outcome exercises
        // the actual title-transfer path; otherwise use the attacker's seat.
        for (const auto &[territoryIdValue, territory] : rt.world->territories())
        {
            (void)territoryIdValue;
            bool heldByDefender = false;
            for (const auto &[holder, socialClass] : territory.titles)
            {
                (void)socialClass;
                if (rt.factionStore.factionOfCharacter(holder) == defender)
                {
                    heldByDefender = true;
                    break;
                }
            }
            if (heldByDefender)
            {
                targetRegion = territory.region;
                break;
            }
        }
    }
    if (targetRegion == world::kNoRegion && !rt.world->regions().empty())
        targetRegion = rt.world->regions().begin()->first;

    factions::CasusBelli cb{};
    cb.kind   = massacre ? factions::CasusBelliKind::Massacre : factions::CasusBelliKind::PowerRatio;
    cb.region = targetRegion;
    if (massacre)
    {
        bool markerMade = false;
        for (const auto &[territoryIdValue, territory] : rt.world->territories())
        {
            for (const auto &[holder, socialClass] : territory.titles)
            {
                (void)socialClass;
                if (rt.factionStore.factionOfCharacter(holder) == attacker)
                {
                    rt.warStore.registerCasusBelli(territoryIdValue, *defenderRow->members.begin(), defender);
                    markerMade = true;
                    break;
                }
            }
            if (markerMade)
                break;
        }
        if (!markerMade)
        {
            // The declared PowerRatio fallback remains a valid war preset if
            // this generated map has no attacker-held title to host a marker.
            cb.kind = factions::CasusBelliKind::PowerRatio;
        }
    }

    const factions::WarResult declared = rt.warStore.declareWar(attacker, defender, cb, rt.simCtx);
    if (declared.isErr())
    {
        error = "war declaration refused (error " + std::to_string(static_cast<int>(declared.error())) + ")";
        return false;
    }
    const factions::War *war = rt.warStore.activeWarBetween(attacker, defender);
    if (war == nullptr)
    {
        error = "WarStore accepted declaration but no active war row was published";
        return false;
    }
    out = AcceptanceWar{attacker, defender, war->id, war->cb.region};
    return true;
}

inline bool activateAcceptanceWar(
    SoakRuntime &rt, const AcceptanceWar &war, std::string &error
)
{
    const factions::War *row = rt.warStore.find(war.warId);
    if (row == nullptr || !row->active)
    {
        error = "acceptance war disappeared before its notice ended";
        return false;
    }
    rt.clock.worldTime = std::max(rt.clock.worldTime, row->noticeEndsAt);
    rt.simCtx.now      = rt.clock.worldTime;
    if (!factions::warEffectivelyActive(*row, rt.simCtx.now))
    {
        error = "war did not cross the notice boundary";
        return false;
    }
    return true;
}

inline bool prepareMidManhunt(
    SoakRuntime &rt, std::string &error
)
{
    if (!syncScenarioClocks(rt))
    {
        error = "scenario setup requires a wired world";
        return false;
    }
    const regionId region       = world::regionIdAt(rt.player.position);
    const territoryId territory = rt.world->territoryAt(rt.player.position);
    if (territory == 0)
    {
        error = "player is not standing in a mapped territory";
        return false;
    }
    justice::Evidence evidence{};
    evidence.id        = rt.justiceStore.nextEvidenceId();
    evidence.culpritId = rt.playerCharacterId;
    evidence.territory = territory;
    evidence.worldTime = rt.simCtx.now;
    evidence.type      = CrimeType::MURDER;
    evidence.status    = EvidenceStatus::INVESTIGATING;
    rt.justiceStore.evidenceStore[territory].push_back(evidence);
    justice::Evidence &stored = rt.justiceStore.evidenceStore[territory].back();
    justice::issueWarrant(stored, rt.simCtx);
    if (rt.justiceStore.warrants.empty())
    {
        error = "warrant issuance did not create a warrant";
        return false;
    }
    const justice::Warrant &warrant = rt.justiceStore.warrants.rbegin()->second;
    const ManhuntSeed seed{warrant.id, rt.playerCharacterId, territory, region};
    if (!rt.eventEngine.spawnEvent("MANHUNT", seed))
    {
        error = "MANHUNT event template refused the acceptance seed";
        return false;
    }
    return true;
}

inline bool runAcceptanceScenario(
    SoakRuntime &rt,
    const std::string &scenario,
    const std::string &savePath,
    const std::string &dataDir,
    std::string &error
)
{
    if (scenario == "A01" || scenario == "A02" || scenario == "A03" || scenario == "A04" || scenario == "A05"
        || scenario == "A06" || scenario == "A07" || scenario == "A08" || scenario == "A09" || scenario == "A10"
        || scenario == "A11" || scenario == "A12" || scenario == "A13" || scenario == "A14" || scenario == "A15"
        || scenario == "A16" || scenario == "A17" || scenario == "A18" || scenario == "A37" || scenario == "A38")
    {
        // These are player-executed interaction cases. A fresh deterministic
        // world is the correct spawn preset; the paired unit fixture is the
        // automated rule backstop named in acceptance_walkthrough.md.
        return true;
    }

    if (scenario == "A19" || scenario == "A20" || scenario == "A21" || scenario == "A22" || scenario == "A23"
        || scenario == "A24" || scenario == "A25" || scenario == "A26" || scenario == "war-notice"
        || scenario == "war-active" || scenario == "massacre-war")
    {
        AcceptanceWar war{};
        territoryId claimed = 0;
        if (scenario == "A22" || scenario == "A24")
        {
            if (!createAndClaimFrontier(rt, false, claimed, error))
                return false;
        }
        if (scenario == "A24")
        {
            const auto founded
                = rt.factionStore.foundPlayerFaction(rt.playerCharacterId, "Acceptance Test Company", rt.simCtx);
            if (founded.isErr())
            {
                error = "could not found player faction for claim-transfer preset";
                return false;
            }
            const factionId playerFaction = rt.factionStore.factionOfCharacter(rt.playerCharacterId);
            rt.characters.get(rt.playerCharacterId).cachedPower = 1'000ULL;
            if (!prepareAcceptanceWar(rt, war, error, false, rt.world->territory(claimed)->region, playerFaction))
                return false;
        } else
        {
            const bool massacre   = scenario == "A19" || scenario == "massacre-war";
            const regionId region = claimed != 0 ? rt.world->territory(claimed)->region : world::kNoRegion;
            if (!prepareAcceptanceWar(rt, war, error, massacre, region))
                return false;
        }
        if (scenario == "A19" || scenario == "A20" || scenario == "A21" || scenario == "war-notice"
            || scenario == "massacre-war")
        {
            const auto *row = rt.warStore.find(war.warId);
            std::printf(
                "acceptance %s: war=%llu noticeEndsAt=%llu cb=%u active=%s\n",
                scenario.c_str(),
                static_cast<unsigned long long>(war.warId),
                static_cast<unsigned long long>(row->noticeEndsAt),
                static_cast<unsigned>(row->cb.kind),
                "false (notice phase)"
            );
            return true;
        }
        if (!activateAcceptanceWar(rt, war, error))
            return false;
        if (scenario == "A23" || scenario == "A24" || scenario == "A25" || scenario == "A26")
        {
            rt.warStore.endWar(war.warId, factions::WarOutcome::AttackerWon, rt.simCtx);
            const auto *ended = rt.warStore.find(war.warId);
            if (ended == nullptr || ended->active)
            {
                error = "war outcome did not end the acceptance war";
                return false;
            }
            if (scenario == "A24")
            {
                const world::Territory *transferred = rt.world->territory(claimed);
                if (transferred == nullptr || transferred->owner == rt.playerCharacterId)
                {
                    error = "player-owned territory did not transfer to the winner";
                    return false;
                }
            }
            if (scenario == "A26")
            {
                factions::CasusBelli cb{};
                cb.kind = factions::CasusBelliKind::PowerRatio;
                if (!rt.warStore.declareWar(war.attacker, war.defender, cb, rt.simCtx).isErr())
                {
                    error = "peace treaty did not block immediate re-declaration";
                    return false;
                }
            }
            std::printf(
                "acceptance %s: war=%llu ended outcome=%u region=%u\n",
                scenario.c_str(),
                static_cast<unsigned long long>(war.warId),
                static_cast<unsigned>(ended->outcome),
                static_cast<unsigned>(war.region)
            );
            return true;
        }
        // A22 is the exact protection transition at the notice boundary.
        if (scenario == "A22" || scenario == "war-active")
        {
            if (claimed != 0 && rt.claimStore.isProtectedFromDestruction(claimed, rt.simCtx))
            {
                error = "claimed territory remained protected after the war became active";
                return false;
            }
            std::printf(
                "acceptance %s: war active at %llu; protection suspended=%s\n",
                scenario.c_str(),
                static_cast<unsigned long long>(rt.simCtx.now),
                claimed != 0 ? "true" : "not applicable"
            );
            return true;
        }
        return true;
    }

    if (scenario == "A27" || scenario == "A28" || scenario == "A29" || scenario == "A30" || scenario == "A31"
        || scenario == "A32" || scenario == "A33" || scenario == "abandoned-claim")
    {
        territoryId claimed = 0;
        const bool safe     = scenario == "A29";
        if (!createAndClaimFrontier(rt, safe, claimed, error))
            return false;
        if (scenario == "A29" && !rt.world->territory(claimed)->safe)
        {
            error = "safe-zone flag did not survive claiming";
            return false;
        }
        if (scenario == "A28")
        {
            data::LawSet custom = rt.registry.defaultLaws();
            custom.key          = "acceptance_charter";
            if (rt.claimStore.installLaws(claimed, rt.playerCharacterId, custom, rt.simCtx).isErr())
            {
                error = "custom-law installation was refused";
                return false;
            }
            return rt.world->lawsAt(claimed).key == "acceptance_charter";
        }
        if (scenario == "A30")
        {
            // Exactly 29 validated minutes: this must not satisfy the
            // 30-minute activity threshold (A-114).
            for (std::uint64_t second = 30; second <= 29 * 60; second += 30)
                rt.claimStore.notePresence(claimed, rt.playerCharacterId, false, second);
        } else if (scenario == "A31")
        {
            // Mirror the mixed ClaimStore regression: 20 validated active
            // minutes followed by 40 AFK minutes. If AFK seconds were wrongly
            // credited, this 60-minute wall-clock stint would reset the timer.
            for (std::uint64_t second = 30; second <= 20 * 60; second += 30)
                rt.claimStore.notePresence(claimed, rt.playerCharacterId, false, second);
            for (std::uint64_t second = 20 * 60 + 30; second <= 60 * 60; second += 30)
                rt.claimStore.notePresence(claimed, rt.playerCharacterId, true, second);
        }
        if (scenario == "A30" || scenario == "A31" || scenario == "A32" || scenario == "A33"
            || scenario == "abandoned-claim")
        {
            rt.clock.realTime         = 14ULL * 24ULL * 60ULL * 60ULL;
            rt.simCtx.realTimeSeconds = rt.clock.realTime;
            rt.claimStore.tick(rt.simCtx);
            if (!rt.claimStore.isAbandoned(claimed))
            {
                error = "territory did not reach the 14-real-day abandoned state";
                return false;
            }
            if (scenario == "A33")
            {
                world::CompactRecord record{};
                record.evidenceId = 1;
                record.worldTime  = rt.simCtx.now;
                record.crimeType  = static_cast<std::uint32_t>(CrimeType::MURDER);
                record.culprit    = rt.playerCharacterId;
                rt.world->territory(claimed)->archive.push_back(record);
                if (rt.claimStore.dismantleAbandoned(claimed, rt.playerCharacterId, rt.simCtx).isErr())
                {
                    error = "dismantle refused an abandoned player claim";
                    return false;
                }
                const world::Territory *after = rt.world->territory(claimed);
                if (after == nullptr || after->owner != 0 || after->archive.size() != 1)
                {
                    error = "dismantle failed to clear ownership or preserve archive";
                    return false;
                }
            }
            std::printf(
                "acceptance %s: territory=%llu abandoned=true realTime=%llu\n",
                scenario.c_str(),
                static_cast<unsigned long long>(claimed),
                static_cast<unsigned long long>(rt.clock.realTime)
            );
        }
        return true;
    }

    if (scenario == "A34")
    {
        const regionId region = world::regionIdAt(rt.player.position);
        if (rt.eventEngine.startEventByKey("bandit_raid", region, 0, nullptr, rt.simCtx) == 0)
        {
            error = "bandit_raid event template refused the scenario";
            return false;
        }
        return true;
    }
    if (scenario == "A36")
    {
        const regionId region       = world::regionIdAt(rt.player.position);
        const std::uint64_t eventId = rt.eventEngine.startEventByKey("bandit_raid", region, 0, nullptr, rt.simCtx);
        if (eventId == 0)
        {
            error = "bandit_raid event template refused the reward fixture";
            return false;
        }
        events::Event *event             = rt.eventEngine.findMutable(eventId);
        const events::EventTemplate *tpl = event != nullptr && rt.eventEngine.templates() != nullptr
                                             ? rt.eventEngine.templates()->byId(event->templateId)
                                             : nullptr;
        if (event == nullptr || tpl == nullptr || !tpl->hasRewardTable || tpl->rewards.currencyPerContribution == 0)
        {
            error = "reward fixture could not resolve a paying event template";
            return false;
        }

        std::vector<characterId> contributors;
        contributors.reserve(3);
        for (const world::NpcSeed &seed : rt.npcPlan)
        {
            if (!rt.characters.exists(seed.id) || !rt.npcs.contains(seed.id))
                continue;
            const Character &candidate = rt.characters.get(seed.id);
            if (candidate.body.currentHp == 0 || candidate.eventId != 0)
                continue;
            contributors.push_back(seed.id);
            if (contributors.size() == 3)
                break;
        }
        if (contributors.size() != 3)
        {
            error = "worldgen did not provide three available NPC reward contributors";
            return false;
        }
        std::sort(contributors.begin(), contributors.end());
        for (const characterId id : contributors)
        {
            rt.eventEngine.join(id, eventId, rt.simCtx);
            event->actionAmounts[id][static_cast<std::size_t>(ActionType::DAMAGE)] = 5'000;
        }
        // A small fixture difficulty makes the computed pool 12,001 points:
        // three equal 4,000-point contributions plus one flooring remainder.
        event->currentState.difficulty = 2;
        const std::uint64_t pool       = events::rewardPool(*event, *tpl);
        if (contributors[0] >= contributors[1] || contributors[1] >= contributors[2] || pool != 12'001)
        {
            error = "reward fixture did not produce the pinned equal-share pool";
            return false;
        }

        std::array<std::int64_t, 3> before{};
        for (std::size_t i = 0; i < contributors.size(); ++i)
            before[i] = rt.characters.get(contributors[i]).progression.currency;
        rt.eventEngine.finishEvent(eventId, true, rt.simCtx); // real lifecycle payout point

        const std::uint64_t baseShare = pool / contributors.size();
        const std::uint64_t remainder = pool % contributors.size();
        std::uint64_t totalPaid       = 0;
        for (std::size_t i = 0; i < contributors.size(); ++i)
        {
            const std::uint64_t expectedPoints   = baseShare + (i == 0 ? remainder : 0);
            const std::int64_t actualCurrency    = rt.characters.get(contributors[i]).progression.currency - before[i];
            const std::uint64_t expectedCurrency = expectedPoints * tpl->rewards.currencyPerContribution;
            if (actualCurrency < 0 || static_cast<std::uint64_t>(actualCurrency) != expectedCurrency)
            {
                error = "reward payout did not give the integer remainder to the lowest character id";
                return false;
            }
            totalPaid += static_cast<std::uint64_t>(actualCurrency);
        }
        if (totalPaid != pool * tpl->rewards.currencyPerContribution || rt.eventEngine.find(eventId) == nullptr)
        {
            error = "reward payout pool invariant or event-resolution lifecycle failed";
            return false;
        }
        std::printf(
            "acceptance A36: event=%llu contributors=%llu pool=%llu currencyPaid=%llu lowestIdRemainder=%llu\n",
            static_cast<unsigned long long>(eventId),
            static_cast<unsigned long long>(contributors.size()),
            static_cast<unsigned long long>(pool),
            static_cast<unsigned long long>(totalPaid),
            static_cast<unsigned long long>(remainder)
        );
        return true;
    }
    if (scenario == "A35")
    {
        AcceptanceWar war{};
        if (!prepareAcceptanceWar(rt, war, error) || !activateAcceptanceWar(rt, war, error))
            return false;
        rt.warStore.tick(rt.simCtx); // the real war step spawns a dual-target BATTLE event
        return true;
    }
    if (scenario == "A39" || scenario == "mid-manhunt")
    {
        if (!prepareMidManhunt(rt, error))
            return false;
        if (!savePath.empty() && !saveToFile(rt, savePath, error))
            return false;
        return true;
    }
    if (scenario == "A40" || scenario == "campaign-acceptance")
    {
        const std::filesystem::path tempSave
            = std::filesystem::temp_directory_path() / "harrowmere_acceptance_A40.json";
        const std::string firstSave = tempSave.string();
        std::error_code ec;
        std::filesystem::remove(tempSave, ec);
        runGameDays(rt, 10);
        if (!saveToFile(rt, firstSave, error))
            return false;
        SoakRuntime resumed;
        if (!loadGame(resumed, firstSave, dataDir, error))
        {
            error = "campaign acceptance load failed: " + error;
            std::filesystem::remove(tempSave, ec);
            return false;
        }
        runGameDays(resumed, 10);

        // The final ten-day segment must end in an actual player faction
        // territory being transferred by a war, not merely a war record
        // being created and ended. Create the claim and found the faction
        // through the same runtime APIs the player uses.
        territoryId claimedTerritory = 0;
        if (!createAndClaimFrontier(resumed, false, claimedTerritory, error))
        {
            std::filesystem::remove(tempSave, ec);
            return false;
        }
        const auto founded = resumed.factionStore.foundPlayerFaction(
            resumed.playerCharacterId, "Acceptance Campaign Company", resumed.simCtx
        );
        if (founded.isErr())
        {
            error = "campaign acceptance could not found the player faction for the war transfer";
            std::filesystem::remove(tempSave, ec);
            return false;
        }
        const factionId playerFaction = resumed.factionStore.factionOfCharacter(resumed.playerCharacterId);
        AcceptanceWar war{};
        if (playerFaction == 0
            || !prepareAcceptanceWar(
                resumed, war, error, false, resumed.world->territory(claimedTerritory)->region, playerFaction
            )
            || !activateAcceptanceWar(resumed, war, error))
        {
            if (error.empty())
                error = "campaign acceptance could not establish the player-side war";
            std::filesystem::remove(tempSave, ec);
            return false;
        }
        resumed.warStore.endWar(war.warId, factions::WarOutcome::AttackerWon, resumed.simCtx);
        const world::Territory *transferred = resumed.world->territory(claimedTerritory);
        if (transferred == nullptr || transferred->owner == 0 || transferred->owner == resumed.playerCharacterId
            || resumed.factionStore.factionOfCharacter(transferred->owner) != war.attacker
            || resumed.claimStore.ownerOf(claimedTerritory, resumed.simCtx) != transferred->owner)
        {
            error = "campaign war failed to transfer the player claim and rewire its owner to the winning faction";
            std::filesystem::remove(tempSave, ec);
            return false;
        }
        const std::string finalPath = savePath.empty() ? firstSave : savePath;
        if (!saveToFile(resumed, finalPath, error))
        {
            std::filesystem::remove(tempSave, ec);
            return false;
        }
        SoakRuntime finalLoad;
        if (!loadGame(finalLoad, finalPath, dataDir, error))
        {
            std::filesystem::remove(tempSave, ec);
            return false;
        }
        const std::uint64_t expected = stateHash(resumed);
        const std::uint64_t actual   = stateHash(finalLoad);
        if (expected != actual)
        {
            error = "final campaign save/load hash mismatch";
            std::filesystem::remove(tempSave, ec);
            return false;
        }
        std::filesystem::remove(tempSave, ec);
        std::printf(
            "acceptance %s: 10 days + save/load + 10 days + territory transfer + save/load hash=%016llx\n",
            scenario.c_str(),
            static_cast<unsigned long long>(actual)
        );
        return true;
    }

    error = "unknown acceptance scenario: " + scenario;
    return false;
}

inline void listAcceptanceScenarios()
{
    static const char *const names[]
        = {"A01",
            "A02",
            "A03",
            "A04",
            "A05",
            "A06",
            "A07",
            "A08",
            "A09",
            "A10",
            "A11",
            "A12",
            "A13",
            "A14",
            "A15",
            "A16",
            "A17",
            "A18",
            "A19",
            "A20",
            "A21",
            "A22",
            "A23",
            "A24",
            "A25",
            "A26",
            "A27",
            "A28",
            "A29",
            "A30",
            "A31",
            "A32",
            "A33",
            "A34",
            "A35",
            "A36",
            "A37",
            "A38",
            "A39",
            "A40"};
    for (const char *name : names)
        std::printf("%s\n", name);
    std::printf("aliases: war-notice war-active massacre-war abandoned-claim mid-manhunt campaign-acceptance\n");
}

} // namespace soak

#ifndef HARROWMERE_SOAK_NO_MAIN

// ---- the standalone nightly soak ------------------------------------------------

#include <chrono>
#include <cstdlib>
#include <iostream>

namespace
{

std::filesystem::path resolveDataDir(
    int argc, char **argv
)
{
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--data-dir") == 0)
        {
            return std::filesystem::path(argv[i + 1]);
        }
    }
    std::filesystem::path probe = std::filesystem::current_path();
    for (int i = 0; i < 6; ++i)
    {
        const std::filesystem::path candidate = probe / "mods" / "harrowmere_core" / "data";
        if (std::filesystem::exists(candidate / "species.json"))
        {
            return candidate;
        }
        probe = probe.parent_path();
    }
    return std::filesystem::path("mods") / "harrowmere_core" / "data";
}

} // namespace

int main(
    int argc, char **argv
)
{
    std::uint64_t gameYears    = 3; // the nightly default (A.12: 3 game-years)
    std::uint64_t gameMonths   = 0; // 0 = derive from game-years (fine-grained runs/tests)
    std::uint64_t rawTicks     = 0; // 0 = run months; >0 = a raw tick probe (profiles)
    std::uint64_t seed         = 20261102;
    bool dumpPopulation        = false;
    bool dumpEvents            = false;
    bool balanceMode           = false;   // --balance: task 8.4's instrumented run (the same soak + the balance reads)
    const char *checkpointPath = nullptr; // --checkpoint: a month-boundary save after every sample
    const char *resumePath     = nullptr; // --resume: continue a checkpointed run (the tick count picks the month up)
    const char *scenarioName   = nullptr; // --scenario A01..A40: deterministic acceptance fixture setup
    const char *scenarioSave   = nullptr; // --scenario-save: optional snapshot after preset setup
    bool listScenarios         = false;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--game-years") == 0)
        {
            gameYears = std::strtoull(argv[i + 1], nullptr, 10);
        } else if (std::strcmp(argv[i], "--game-months") == 0)
        {
            gameMonths = std::strtoull(argv[i + 1], nullptr, 10);
        } else if (std::strcmp(argv[i], "--ticks") == 0)
        {
            rawTicks = std::strtoull(argv[i + 1], nullptr, 10);
        } else if (std::strcmp(argv[i], "--seed") == 0)
        {
            seed = std::strtoull(argv[i + 1], nullptr, 10);
        } else if (std::strcmp(argv[i], "--checkpoint") == 0)
        {
            checkpointPath = argv[i + 1];
        } else if (std::strcmp(argv[i], "--resume") == 0)
        {
            resumePath = argv[i + 1];
        } else if (std::strcmp(argv[i], "--scenario") == 0)
        {
            scenarioName = argv[i + 1];
        } else if (std::strcmp(argv[i], "--scenario-save") == 0)
        {
            scenarioSave = argv[i + 1];
        }
    }
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--list-scenarios") == 0)
        {
            listScenarios = true;
        } else if (std::strcmp(argv[i], "--dump-population") == 0)
        {
            dumpPopulation = true; // the QA probe: every settlement's census + band per month
        } else if (std::strcmp(argv[i], "--dump-events") == 0)
        {
            dumpEvents = true; // the QA probe: the active-event book per month
        } else if (std::strcmp(argv[i], "--balance") == 0)
        {
            balanceMode = true; // the 8.4 balance pass: the soak plus the card's (a)-(f) measurements
        }
    }
    if (listScenarios)
    {
        soak::listAcceptanceScenarios();
        return 0;
    }
    if (balanceMode && gameMonths == 0 && gameYears == 3)
    {
        gameYears = 1; // the balance default: one game-year (the PR-soak window; --game-months overrides)
    }

    const std::filesystem::path dataDir = resolveDataDir(argc, argv);
    const std::uint64_t totalMonths     = gameMonths != 0 ? gameMonths : gameYears * 12;
    const std::uint64_t ticksPerMonth   = soak::kTicksPerGameDay * 365 / 12; // A-38's year; the month is its twelfth

    std::printf(
        "soak: seed=%llu game-years=%llu (%llu months, %llu ticks/month) data=%s\n",
        static_cast<unsigned long long>(seed),
        static_cast<unsigned long long>(gameYears),
        static_cast<unsigned long long>(totalMonths),
        static_cast<unsigned long long>(ticksPerMonth),
        dataDir.string().c_str()
    );

    soak::SoakRuntime rt;
    std::string error;
    std::uint64_t startMonth = 0;
    const auto start         = std::chrono::steady_clock::now();
    if (resumePath != nullptr)
    {
        // The checkpointed continuation: the save carries the clocks, the RNG
        // streams and every store (7.1), so the run resumes its exact random
        // sequence and cadence stamps. The resumed hash prints for the log —
        // equal to the checkpoint month's line (the bit-exact continuation,
        // 7.1's contract, visible in the nightly output).
        if (!soak::loadGame(rt, resumePath, dataDir.string(), error))
        {
            std::fprintf(stderr, "soak: resume failed: %s\n", error.c_str());
            return 1;
        }
        startMonth = rt.tickNumber / ticksPerMonth;
        std::printf(
            "soak: resumed from %s at tick %llu (month %llu/%llu, hash %016llx)\n",
            resumePath,
            static_cast<unsigned long long>(rt.tickNumber),
            static_cast<unsigned long long>(startMonth),
            static_cast<unsigned long long>(totalMonths),
            static_cast<unsigned long long>(soak::stateHash(rt))
        );
    } else if (!soak::newGame(rt, seed, dataDir.string(), error))
    {
        std::fprintf(stderr, "soak: new game failed: %s\n", error.c_str());
        return 1;
    }

    if (scenarioName != nullptr)
    {
        const std::string savePath = scenarioSave != nullptr ? scenarioSave : "";
        if (!soak::runAcceptanceScenario(rt, scenarioName, savePath, dataDir.string(), error))
        {
            std::fprintf(stderr, "acceptance scenario %s failed: %s\n", scenarioName, error.c_str());
            return 2;
        }
        if (scenarioSave != nullptr && std::strcmp(scenarioName, "A39") != 0
            && std::strcmp(scenarioName, "mid-manhunt") != 0 && std::strcmp(scenarioName, "A40") != 0
            && std::strcmp(scenarioName, "campaign-acceptance") != 0)
        {
            if (!soak::saveToFile(rt, scenarioSave, error))
            {
                std::fprintf(stderr, "acceptance scenario save failed: %s\n", error.c_str());
                return 2;
            }
        }
        std::printf(
            "acceptance scenario %s ready; state hash=%016llx\n",
            scenarioName,
            static_cast<unsigned long long>(soak::stateHash(rt))
        );
        return 0;
    }

    if (rawTicks != 0)
    {
        // A raw tick probe: run N pipeline ticks, report the step profile and
        // the tick rate. No invariant sampling (the months path owns that).
        const auto probeStart = std::chrono::steady_clock::now();
        soak::runTicks(rt, rawTicks);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - probeStart).count();
        soak::printStepProfile();
        std::printf(
            "probe: %llu ticks in %.2f s (%.0f ticks/s)\n",
            static_cast<unsigned long long>(rawTicks),
            seconds,
            static_cast<double>(rawTicks) / seconds
        );
        return 0;
    }

    std::vector<std::pair<std::string, std::uint64_t>> prevRng = rt.rng.state();
    soak::BalanceTracker tracker;
    std::vector<soak::BalanceSample> balanceSamples;
    const double monthDays = static_cast<double>(ticksPerMonth * soak::kTickGameSeconds)
                           / static_cast<double>(harrowmere::core::DAY_LENGTH_SECONDS);
    for (std::uint64_t month = startMonth + 1; month <= totalMonths; ++month)
    {
        if (balanceMode)
        {
            // Day-chunked months: the equilibrium pool samples daily (see
            // sampleEventEquilibrium — battles live one game-day, a monthly
            // read would miss most of their lives). kTicksPerGameDay does
            // not divide the month evenly; the remainder rides the last
            // chunk unchanged.
            const std::uint64_t ticksPerDay = soak::kTicksPerGameDay;
            const std::uint64_t wholeDays   = ticksPerMonth / ticksPerDay;
            for (std::uint64_t day = 0; day < wholeDays; ++day)
            {
                soak::runTicks(rt, ticksPerDay);
                soak::sampleEventEquilibrium(rt, tracker);
            }
            soak::runTicks(rt, ticksPerMonth - wholeDays * ticksPerDay);
        } else
        {
            soak::runTicks(rt, ticksPerMonth);
        }
        if (dumpPopulation)
        {
            std::printf(
                "month %llu population dump (settlement census vs template band):\n",
                static_cast<unsigned long long>(month)
            );
        }
        if (dumpEvents)
        {
            std::printf("month %llu event dump (the active-event book):\n", static_cast<unsigned long long>(month));
        }
        const soak::SoakSample sample = soak::sampleInvariants(
            rt, month, prevRng, dumpPopulation ? stdout : nullptr, dumpEvents ? stdout : nullptr
        );
        prevRng = rt.rng.state();
        if (!sample.failure.empty())
        {
            std::fprintf(
                stderr,
                "SOAK FAILURE month=%llu hash=%016llx population=%u activeEvents=%zu evidenceRows=%zu\n"
                "  %s\n",
                static_cast<unsigned long long>(sample.month),
                static_cast<unsigned long long>(sample.stateHash),
                sample.population,
                sample.activeEvents,
                sample.evidenceRows,
                sample.failure.c_str()
            );
            return 1;
        }
        if (checkpointPath != nullptr && !soak::saveToFile(rt, checkpointPath, error))
        {
            std::fprintf(stderr, "soak: checkpoint write failed: %s\n", error.c_str());
            return 1;
        }
        std::printf(
            "soak: month %llu/%llu ok (pop %u, oob %u, events %zu/%zu rows, evidence %zu, "
            "affn %zu, trust %zu, seen %zu, corpses %zu, fmembers %zu, lodActive %zu, RSS %llu MiB, hash %016llx)\n",
            static_cast<unsigned long long>(month),
            static_cast<unsigned long long>(totalMonths),
            sample.population,
            sample.outOfBand,
            sample.activeEvents,
            sample.eventParticipants,
            sample.evidenceRows,
            sample.affinityRows,
            sample.trustRows,
            sample.seenRows,
            sample.corpses,
            sample.factionMembers,
            sample.lodActive,
            static_cast<unsigned long long>(sample.rssBytes / (1024 * 1024)),
            static_cast<unsigned long long>(sample.stateHash)
        );
        if (balanceMode)
        {
            balanceSamples.push_back(soak::sampleBalance(rt, month, tracker, /*poolEvents=*/false));
            soak::printBalanceLine(balanceSamples.back(), monthDays);
        }
        std::fflush(stdout);
    }

    if (balanceMode)
    {
        // ---- the 8.4 report blocks (the same order the balance report cites) ----

        // (a) the A-120 curve: pooled per-band income -> days per 10 levels at
        // the card's four anchors (early 1-25 / middle 26-50 / late 51-75 /
        // peak 76+; a thin cohort prints, never asserts).
        std::printf(
            "A-120 leveling curve (pooled %llu months):\n", static_cast<unsigned long long>(balanceSamples.size())
        );
        for (const std::uint64_t anchor : {1ull, 25ull, 50ull, 75ull})
        {
            const std::size_t band = soak::balBandOf(anchor);
            std::uint64_t delta    = 0;
            double npcDays         = 0.0;
            for (const soak::BalanceSample &s : balanceSamples)
            {
                delta += s.xpDelta[band];
                npcDays += static_cast<double>(s.incomeNpcs[band]) * monthDays;
            }
            const double incomePerDay = npcDays > 0 ? static_cast<double>(delta) / npcDays : 0.0;
            const double days
                = incomePerDay > 0 ? static_cast<double>(soak::balTenLevelCost(anchor)) / incomePerDay : 0.0;
            std::printf(
                "  L%-2llu -> L%-2llu: %llu XP over 10 levels @ %.1f XP/day (band %zu, %.0f npc-days) = %.1f game-days"
                " (A-120 target ~15 early, doubling per band)%s\n",
                static_cast<unsigned long long>(anchor),
                static_cast<unsigned long long>(anchor + 10),
                static_cast<unsigned long long>(soak::balTenLevelCost(anchor)),
                incomePerDay,
                band,
                npcDays,
                days,
                npcDays < 100.0 ? "  [thin cohort]" : ""
            );
        }

        // (a) the faucet probe: one real kill pays 1.6 x levelCosts[victim.level].
        {
            const soak::KillXpProbe kill = soak::runKillXpProbe(rt);
            if (kill.ran)
            {
                std::printf(
                    "  kill faucet (measured): player kill of a level %llu victim paid %llu XP"
                    " (A-34/A-35 formula: %llu) %s\n",
                    static_cast<unsigned long long>(kill.victimLevel),
                    static_cast<unsigned long long>(kill.gainedXp),
                    static_cast<unsigned long long>(kill.expectedXp),
                    kill.gainedXp == kill.expectedXp ? "MATCH" : "MISMATCH"
                );
            }
        }

        // (b) the peak-stage verdict: worldgen starts every NPC at
        // STARTING_LEVEL_MAX (10) or below; a peak-stage row inside a soak
        // window would be runtime XP growth the age bands never allowed.
        {
            std::uint16_t maxLevel = 0;
            std::uint32_t peak     = 0;
            for (const soak::BalanceSample &s : balanceSamples)
            {
                maxLevel = std::max(maxLevel, s.maxNpcLevel);
                peak     = std::max(peak, s.peakStageRows);
            }
            std::printf(
                "power distribution (b): max NPC level %u across the run, peak-stage rows %u (worldgen band <= %u)\n",
                static_cast<unsigned>(maxLevel),
                peak,
                static_cast<unsigned>(harrowmere::world::STARTING_LEVEL_MAX)
            );
        }

        // (e) the bounty-vs-penalty invariant sweep (pure table math).
        const bool bountyOk = soak::balBountyBelowPenalty(rt);

        // (d) the scripted crime probe: convictions per 100 player-crimes by
        // territory law set — the crowd arm (saturated: what a market punch
        // costs) plus the two-witness thin arm (the thresholds' operating
        // read), and the repeat-offender ramp is per series — the halves
        // split keeps it visible.
        std::printf(
            "crime probe (d): convictions per 100 player-crimes, by law set (sightings scripted at the 3.9 seam):\n"
        );
        const std::vector<soak::LawSetTrials> probe = soak::runCrimeProbe(rt, 2, 12, 8, 8);
        for (const soak::LawSetTrials &row : probe)
        {
            const double rate = [](const soak::CrimeTrials &t) {
                return t.committed > 0 ? 100.0 * static_cast<double>(t.convicted) / static_cast<double>(t.committed)
                                       : -1.0;
            }(row.assault);
            const double mRate = [](const soak::CrimeTrials &t) {
                return t.committed > 0 ? 100.0 * static_cast<double>(t.convicted) / static_cast<double>(t.committed)
                                       : -1.0;
            }(row.murder);
            const double tRate = [](const soak::CrimeTrials &t) {
                return t.committed > 0 ? 100.0 * static_cast<double>(t.convicted) / static_cast<double>(t.committed)
                                       : -1.0;
            }(row.thin);
            std::printf(
                "  %-18s settlements %u | assault %u/%u convicted %.0f%% (investigating %u, below %u, no-witness %u, "
                "exempt %u)"
                " | murder %u/%u convicted %.0f%% (investigating %u, below %u, no-witness %u, exempt %u)"
                " | thin(1w) %u/%u convicted %.0f%% (investigating %u, below %u, no-witness %u, exempt %u)\n",
                row.lawKey.c_str(),
                row.settlements,
                row.assault.convicted,
                row.assault.committed,
                rate,
                row.assault.investigating,
                row.assault.below,
                row.assault.noWitness,
                row.assault.exempt,
                row.murder.convicted,
                row.murder.committed,
                mRate,
                row.murder.investigating,
                row.murder.below,
                row.murder.noWitness,
                row.murder.exempt,
                row.thin.convicted,
                row.thin.committed,
                tRate,
                row.thin.investigating,
                row.thin.below,
                row.thin.noWitness,
                row.thin.exempt
            );
        }

        // The (d) species-conflict spot-check (the exemption hard gate).
        {
            bool refused   = false;
            bool recorded  = false;
            const bool ran = soak::runExemptionSpotCheck(rt, refused, recorded);
            std::printf(
                "  exemption spot-check: %s\n",
                ran ? (refused && recorded
                              ? "beastkin murder in the xenophobic village refused the row; the human control recorded"
                              : "RAN BUT DID NOT HOLD — VIOLATED")
                    : "skipped (no xenophobic settlement or no living beastkin this seed)"
            );
            if (ran && !(refused && recorded))
            {
                std::printf("    refused=%d recorded=%d\n", refused ? 1 : 0, recorded ? 1 : 0);
            }
        }

        // The repeat-offender ramp (the notoriety damper), measured in one
        // default-law settlement as the controlled two-witness response: the
        // notoriety floor escalates by the assault's own notorietyGain (3000)
        // per trial; the conviction rate may only rise.
        {
            const auto homes = soak::balSettlementsOfLaw(rt, "default", 1);
            if (!homes.empty())
            {
                const soak::RampSeries ramp = soak::balRampSeries(rt, homes[0], 10);
                std::printf(
                    "  repeat-offender ramp: first half %u/%u convicted (below %u), second half %u/%u convicted (below "
                    "%u)"
                    " | investigating %u, exempt %u, no-witness %u"
                    " — threshold x SCALE/(SCALE+notoriety), floor 0..27000 in the assault's own 3000 steps\n",
                    ramp.firstHalfConvicted,
                    ramp.firstHalfN,
                    ramp.firstHalfBelow,
                    ramp.secondHalfConvicted,
                    ramp.secondHalfN,
                    ramp.secondHalfBelow,
                    ramp.investigating,
                    ramp.exempt,
                    ramp.noWitness
                );
            }
        }

        // (f) the pooled event-difficulty equilibrium read: the drained
        // per-attempt book (the loop's outputs) and the live-book snapshots.
        {
            const auto bandStats = [](std::vector<std::uint32_t> samples) {
                std::vector<std::uint64_t> sorted(samples.begin(), samples.end());
                std::size_t inBand = 0;
                for (const std::uint64_t v : sorted)
                {
                    if (v >= 3000 && v <= 7000)
                    {
                        inBand += 1;
                    }
                }
                char buf[160];
                if (sorted.empty())
                {
                    std::snprintf(buf, sizeof(buf), "no samples");
                } else
                {
                    std::snprintf(
                        buf,
                        sizeof(buf),
                        "n %zu median %.0f p10 %.0f p90 %.0f, in [3000, 7000]: %.1f%%",
                        sorted.size(),
                        soak::balQuantile(sorted, 0.5),
                        soak::balQuantile(sorted, 0.10),
                        soak::balQuantile(sorted, 0.90),
                        100.0 * static_cast<double>(inBand) / static_cast<double>(sorted.size())
                    );
                }
                return std::string(buf);
            };
            std::printf(
                "event equilibrium (f): attempts %llu, drained (post-attempt) %s | live book %s\n",
                static_cast<unsigned long long>(tracker.eventAttemptTotal),
                bandStats(tracker.eventSuccessSamples).c_str(),
                bandStats(tracker.eventLiveSamples).c_str()
            );
        }

        std::printf(
            "balance: %s\n",
            bountyOk ? "GREEN (see docs/balance_report.md for the full read)" : "FAILED the bounty-vs-penalty sweep"
        );
        std::fflush(stdout);
        return bountyOk ? 0 : 1;
    }

    soak::printStepProfile();
    const auto elapsed   = std::chrono::steady_clock::now() - start;
    const double seconds = std::chrono::duration<double>(elapsed).count();
    std::printf(
        "soak: GREEN — %llu game-months this process, %llu ticks in %.1f s (%.0f ticks/s)\n",
        static_cast<unsigned long long>(totalMonths - startMonth),
        static_cast<unsigned long long>((totalMonths - startMonth) * ticksPerMonth),
        seconds,
        static_cast<double>((totalMonths - startMonth) * ticksPerMonth) / seconds
    );
    return 0;
}

#endif // HARROWMERE_SOAK_NO_MAIN
