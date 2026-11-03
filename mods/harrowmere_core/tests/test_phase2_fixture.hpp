// tests/test_phase2_fixture.hpp — local test harness for the Phase-2 cards 3.1–3.7.
//
// Assembles a full WorldContext over the task-0.x/2.x stand-ins (scripts/fakes/) plus
// recording sinks, a stub world (territories/regions/population), and helpers to spawn
// NPCs. In the real repo the fakes are replaced by the delivered Phase-1 stores; the
// fixture shape (Fixture::ctx and the spawn helpers) stays identical.
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <doctest/doctest.h>

#include "character/store.hpp"
#include "combat/attacks.hpp"
#include "combat/damage.hpp"
#include "combat/death.hpp"
#include "core/constants.hpp"
#include "core/constants_phase2.hpp"
#include "core/scheduler.hpp"
#include "core/world_context.hpp"
#include "data/registry.hpp"
#include "economy/economy.hpp"
#include "items/inventory.hpp"
#include "npc/lod.hpp"
#include "npc/roles.hpp"
#include "npc/state.hpp"

namespace phase2_test
{

using namespace harrowmere;

// ------------------------------------------------------------------ stubs ----

struct RecordingJustice final : public JusticeSink
{
    std::vector<CrimeContext> crimes;
    std::vector<std::tuple<characterId, characterId, std::uint64_t>> assaults;
    bool lethalWarrant = false;
    bool warrant       = false;

    void recordCrime(
        const CrimeContext &c
    ) override
    {
        crimes.push_back(c);
    }
    void noteAssault(
        characterId culprit, characterId victim, std::uint64_t applied
    ) override
    {
        assaults.emplace_back(culprit, victim, applied);
    }
    bool hasWarrantIn(
        characterId, territoryId
    ) const override
    {
        return warrant;
    }
    bool hasLethalWarrantIn(
        characterId, territoryId
    ) const override
    {
        return lethalWarrant;
    }
};

struct RecordingEvents final : public IEventsSink
{
    struct PriceMove
    {
        regionId region;
        goodId good;
        std::uint64_t oldPrice;
        std::uint64_t newPrice;
    };
    std::vector<DamageEvent> damages;
    std::vector<PriceMove> priceMoves;
    std::vector<std::tuple<characterId, characterId, std::uint64_t>> heals;

    void onCombatDamage(
        const DamageEvent &ev
    ) override
    {
        damages.push_back(ev);
    }
    void onHeal(
        characterId healer, characterId target, std::uint64_t amount
    ) override
    {
        heals.emplace_back(healer, target, amount);
    }
    void onPriceChanged(
        regionId region, goodId good, std::uint64_t oldPrice, std::uint64_t newPrice
    ) override
    {
        priceMoves.push_back({region, good, oldPrice, newPrice});
    }
};

struct StubPriceModifier final : public IPriceModifierSink
{
    std::uint64_t modifier = SCALE;
    std::vector<std::tuple<characterId, territoryId>> calls;
    std::uint64_t sellReceiptModifier(
        characterId seller, territoryId territory
    ) const override
    {
        const_cast<StubPriceModifier *>(this)->calls.emplace_back(seller, territory);
        return modifier;
    }
};

struct StubFactions final : public IFactionQueries
{
    bool atWar = false;
    bool isAtWar(
        factionId, factionId
    ) const override
    {
        return atWar;
    }
};

struct StubWars final : public IWarRegistry
{
    bool combatant = false;
    bool isCombatant(
        factionId
    ) const override
    {
        return combatant;
    }
};

// Stub world: a handful of territories with fixed centers/regions; nearest-center
// territoryAt; population read straight from the stores. Good enough for every
// Phase-2 card's paths.
struct TerritoryDef
{
    territoryId id = 0;
    Position center{};
    regionId region      = 0;
    bool safe            = false;
    std::uint32_t minPop = 0;
    std::uint32_t maxPop = 0;
};

class FakeWorld final : public IWorldQueries
{
public:
    std::vector<TerritoryDef> territories;
    characterId playerId      = 0;
    std::uint64_t seasonYield = SCALE;
    std::int64_t climatePull  = 0;
    Position safeSpawn{5.0, 0.0, 5.0};
    std::uint32_t seedAgeYears = 25;
    std::vector<NpcSeed> plannedSeeds;
    std::int64_t fakeCultureBias = 0; // the migration-bias seam (cultureBiasToward)

    std::int64_t cultureBiasToward(
        territoryId, speciesId
    ) const override
    {
        return fakeCultureBias;
    }

    const NpcStore *npcs        = nullptr; // fixture wires these after construction
    const CharacterStore *chars = nullptr;

    territoryId territoryAt(
        const Position &p
    ) const override
    {
        territoryId best = 0;
        double bestD     = 1e300;
        for (const auto &t : territories)
        {
            const double dx = t.center.x - p.x;
            const double dz = t.center.z - p.z;
            const double d  = dx * dx + dz * dz;
            if (best == 0 || d < bestD)
            {
                best  = t.id;
                bestD = d;
            }
        }
        return best;
    }
    bool isInSafeZone(
        const Position &p
    ) const override
    {
        const territoryId t = territoryAt(p);
        for (const auto &td : territories)
            if (td.id == t)
                return td.safe;
        return false;
    }
    regionId regionAt(
        const Position &p
    ) const override
    {
        const territoryId t = territoryAt(p);
        for (const auto &td : territories)
            if (td.id == t)
                return td.region;
        return 0;
    }
    TerritoryInfo territoryById(
        territoryId id
    ) const override
    {
        for (const auto &td : territories)
            if (td.id == id)
            {
                TerritoryInfo info;
                info.id   = td.id;
                info.type = TerritoryType::VILLAGE;
                info.safe = td.safe;
                return info;
            }
        return TerritoryInfo{};
    }
    territoryId territoryOfRegion(
        regionId r
    ) const override
    {
        for (const auto &td : territories)
            if (td.region == r)
                return td.id;
        return 0;
    }
    std::uint32_t regionPopulation(
        regionId r
    ) const override
    {
        std::uint32_t n = 0;
        if (npcs == nullptr)
            return 0;
        for (const auto &[id, npc] : npcs->all())
        {
            if (npc.currentRegion == r && chars != nullptr && chars->exists(id))
                ++n;
        }
        return n;
    }
    std::uint32_t territoryPopulation(
        territoryId t
    ) const override
    {
        std::uint32_t n = 0;
        if (npcs == nullptr)
            return 0;
        for (const auto &[id, npc] : npcs->all())
        {
            if (npc.homeTerritory == t && chars != nullptr && chars->exists(id))
                ++n;
        }
        return n;
    }
    void populationBounds(
        territoryId t, std::uint32_t &minOut, std::uint32_t &maxOut
    ) const override
    {
        minOut = 0;
        maxOut = 0;
        for (const auto &td : territories)
            if (td.id == t)
            {
                minOut = td.minPop;
                maxOut = td.maxPop;
            }
    }
    Position settlementCenter(
        territoryId t
    ) const override
    {
        for (const auto &td : territories)
            if (td.id == t)
                return td.center;
        return Position{};
    }
    Position safeZoneSpawnFor(
        const Position &
    ) const override
    {
        return safeSpawn;
    }
    Position beginnerSpawnPosition() const override
    {
        return safeSpawn;
    }
    characterId playerCharacterId() const override
    {
        return playerId;
    }
    std::uint64_t seasonYieldScale(
        regionId
    ) const override
    {
        return seasonYield;
    }
    std::int64_t migrationClimatePullFor(
        regionId
    ) const override
    {
        return climatePull;
    }
    void forEachSettlementTerritory(
        const std::function<void(territoryId)> &fn
    ) const override
    {
        for (const auto &td : territories)
            fn(td.id);
    }
    void forEachPlannedSeed(
        const std::function<void(NpcSeed &)> &fn
    ) const override
    {
        for (NpcSeed seed : plannedSeeds)
            fn(seed);
    }
    speciesId dominantSpeciesOf(
        territoryId t
    ) const override
    {
        const auto it = dominant_.find(t);
        return it == dominant_.end() ? static_cast<speciesId>(1) : it->second;
    }
    bool makeNpcSeed(
        NpcSeed &out, territoryId home, speciesId species, const char *roleKey
    ) override
    {
        out.species       = species;
        out.homeTerritory = home;
        out.roleKey       = roleKey;
        out.ageYears      = (roleKey == std::string(CHILD_ROLE_KEY)) ? 0u : seedAgeYears;
        for (const TerritoryDef &td : territories)
            if (td.id == home)
                out.homeRegion = td.region; // the spawn anchor (3.4's placement reads it)
        out.givenName = "npc_" + std::to_string(++nameCounter_);
        return true;
    }

    std::map<territoryId, speciesId> dominant_;

private:
    int nameCounter_ = 0;
};

// ----------------------------------------------------------- registry seed ----

// The shipped data directory (test_registry.cpp's resolution order: env,
// baked define, source-tree walk).
inline std::filesystem::path resolveDataDir()
{
    if (const char *env = std::getenv("HARROWMERE_TEST_DATA"))
    {
        if (env[0] != '\0')
            return std::filesystem::path(env);
    }
#ifdef HARROWMERE_CORE_DATA_DIR
    return std::filesystem::path(HARROWMERE_CORE_DATA_DIR);
#endif
    std::filesystem::path probe = std::filesystem::current_path();
    for (int i = 0; i < 6; ++i)
    {
        const std::filesystem::path candidate = probe / "mods" / "harrowmere_core" / "data";
        if (std::filesystem::exists(candidate / "species.json"))
            return candidate;
        probe = probe.parent_path();
    }
    return std::filesystem::path("mods") / "harrowmere_core" / "data";
}

// One committed registry for the whole binary run: the SHIPPED data (the
// snapshot is immutable once committed; DataRegistry is non-copyable, so a
// static owns it by pointer). Species 0 (player) is sapient in species.json —
// isCriminalAct and the assault hook read it; the wolf/goo_beast rows are the
// 2.2 card's pinned non-sapient / damage-reduction rows; role priorities are
// 3.6's table. No test invents rows: the tests are the data's customers.
inline DataRegistry &shippedRegistry()
{
    static DataRegistry *registry = [] {
        auto r = std::make_unique<DataRegistry>();
        std::vector<data::DataError> errors;
        REQUIRE_MESSAGE(r->load(resolveDataDir().string(), errors), "shipped data registry load failed");
        return r.release();
    }();
    return *registry;
}

inline speciesId speciesIdByKey(
    const char *key
)
{
    const data::SpeciesData *row = shippedRegistry().speciesByKey(key);
    REQUIRE_MESSAGE(row != nullptr, (std::string("shipped species.json is missing key: ") + key).c_str());
    return row->id;
}

// ---------------------------------------------------------------- fixture ----

struct Fixture
{
    CharacterStore characters;
    NpcStore npcs;
    EconomyStore economy;
    CorpseStore corpses;
    RoleRegistry roles;
    items::ItemInstanceRegistry itemInstances; // 2.4's instance-id space (power/equip paths)
    LodActiveSet lod;
    CombatQueue combatQueue;
    SeedRng rng;
    Scheduler scheduler;
    RecordingJustice justice;
    IPerceptionSink perception; // seam stub: perceiveWithin returns {}
    RecordingEvents events;
    StubPriceModifier priceModifier;
    NullUiSignalSink signals;
    NullBagSource bags;
    StubFactions factions;
    StubWars wars;
    FakeWorld world;
    WorldContext ctx;

    Fixture()
        : ctx{0,
              0,
              6,
              &world,
              &factions,
              &wars,
              &justice,
              &perception,
              &events,
              &priceModifier,
              &signals,
              &bags,
              &characters,
              &npcs,
              &economy,
              &corpses,
              &roles,
              &shippedRegistry(),
              &itemInstances,
              &scheduler,
              &lod,
              &combatQueue,
              &rng}
    {
        world.npcs  = &npcs;
        world.chars = &characters;
        // The plugin's 0.4 data adapter loads goods.json at boot and calls
        // registerGood per row; the fixture mirrors those 8 rows verbatim so
        // economy paths (farmer production, prices) run for real. perCapitaNeed
        // rides the SCALE grid (goods.json's float x SCALE).
        economy.registerGood(1, "grain", 2, 20000);
        economy.registerGood(2, "meat", 5, 5000);
        economy.registerGood(3, "wood", 3, 2000);
        economy.registerGood(4, "iron", 12, 500);
        economy.registerGood(5, "cloth", 6, 1000);
        economy.registerGood(6, "tools", 20, 200);
        economy.registerGood(7, "medicine", 15, 500);
        economy.registerGood(8, "mana_tonic", 250, 0);
    }

    void addTerritory(
        territoryId id,
        double x,
        double z,
        regionId region,
        bool safe,
        std::uint32_t minPop = 0,
        std::uint32_t maxPop = 0
    )
    {
        TerritoryDef t;
        t.id     = id;
        t.center = Position{x, 0.0, z};
        t.region = region;
        t.safe   = safe;
        t.minPop = minPop;
        t.maxPop = maxPop;
        world.territories.push_back(t);
    }
};

// ---------------------------------------------------------------- helpers ----

// Position equality (the doc struct carries no comparison operators).
inline bool samePos(
    const Position &a, const Position &b
)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

// Spawns an NPC through the store + parallel NpcState (the 3.4 pairing), adult by
// default, with a body the combat paths can chew on.
inline characterId spawnNpc(
    Fixture &f, speciesId species, territoryId home, regionId region, std::uint32_t ageYears, const char *roleKey = ""
)
{
    NpcSeed seed;
    seed.species         = species;
    seed.homeTerritory   = home;
    seed.homeRegion      = region;
    seed.roleKey         = roleKey;
    seed.ageYears        = ageYears;
    seed.givenName       = "spawn";
    const characterId id = f.characters.createNpc(seed);
    NpcState &npc        = f.npcs.create(id);
    npc.name             = seed.givenName;
    npc.birthWorldTime   = f.ctx.now - static_cast<std::uint64_t>(ageYears) * YEAR_GAME_SECONDS;
    npc.roleKey          = roleKey;
    npc.homeTerritory    = home;
    npc.currentRegion    = region;
    npc.tier             = FAR;
    Character &c         = f.characters.get(id);
    c.body.maxHp         = 100;
    c.body.currentHp     = 100;
    c.body.position      = f.world.settlementCenter(home); // 3.4's placement: home settlement
    return id;
}

inline characterId spawnPlayer(
    Fixture &f
)
{
    const characterId id = f.characters.createPlayer(PlayerCreation{"tester", {}, ""});
    f.world.playerId     = id;
    return id;
}

} // namespace phase2_test
