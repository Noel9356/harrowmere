// The native-mod plugin seam of 'harrowmere_core': one exported entry point
// that hands the kernel this binary's entry table, exactly the way a module's
// module_plugin.cpp hands over its descriptor. The contract a native mod
// answers to is the kernel's mod_api.hpp C ABI (engine_query_mod_entry_table
// -> EngineModEntryTable; the module plugin descriptor's capability lists are
// package data for mods and live in mod.toml's `provides`, which the compiled
// manifest below must agree with - the kernel verifies both at startup).
//
// v0.1 (task 0.1): the mod is load-bearing but carries zero game behavior -
// it acquires its requirements, logs its lifecycle, provides the inert
// SimCapability stub and claims the world through its manifest. Every later
// task copies this wiring pattern.
//
// Phase-1 runtime bootstrap (the exit checklist's "playable" shape): this
// component is now ALSO the mod-side composition the Phase-1 cards wire
// together. The plan pins where each slice runs (common.md §4.4; there is no
// Scheduler until Phase 2's task 3.1):
//
//   construct  - acquire logging (install the HARROWMERE_LOG_* sink), scene
//                (the bridge's registry) and input; probe the optional
//                lighting provider (the bridge's sun; degrade-if-absent);
//                read the [harrowmere] config through the 0.4 wrapper seam.
//   start      - load the DataRegistry from the package dir (0.4's pinned
//                path resolution), fail startup with file+pointer+reason on
//                any data error, generate the world from the seed (1.5 - the
//                FNV-1a summary hash logs at INFO inside), seed the RNG,
//                spawn the player at the beginner safe village, and subscribe
//                the two lanes:
//                  * EngineFixedUpdateEvent - ONE runTick in the §4.4 order:
//                    the clocks advance (step 0), the seasons roll with the
//                    clocks and the weather machine runs (3.8's Phase-1
//                    slice), the queued intents apply (step 9's home,
//                    3.10's applyIntents).
//                  * EngineFrameEvent - rebuild the SimCapability view
//                    snapshots (§4.2 step 13's home) and drive the 7.2P1
//                    scene bridge (frame-coupled by design).
//   stop       - release the subscriptions (RAII), flush the log, say so.
//
// The worldgen seed: [harrowmere] seed = 0 means "random at first launch,
// then persisted" (task 0.1's config comment) - persistence is 7.1's (Phase
// 4), so a Phase-1 run with seed 0 uses the seed value 0 deterministically
// and logs a note; two runs with the same seed produce identical worldgen
// hashes either way (the exit checklist's determinism requirement).
#include "engine/kernel/component_interface.hpp"
#include "engine/kernel/component_manifest.hpp"
#include "engine/kernel/mod_api.hpp"
#include "engine/kernel/filesystem_utilities.hpp" // engine::executableDirectory (0.4's package-dir resolution)

#include <filesystem>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

#include "logging_capability.hpp"
#include "scene_capability.hpp"        // engine::SceneCapability (required)
#include "lighting_capability.hpp"     // engine::LightingCapability (optional)

#include "harrowmere_capability.hpp"

#include "core/clock.hpp"
#include "core/enums.hpp"
#include "core/log.hpp"
#include "core/rng.hpp"
#include "data/config.hpp"
#include "data/config_engine.hpp" // only the kernel-facing ConfigSource adapter shape; the context source below keeps src/data/ engine-free
#include "data/registry.hpp"
#include "bridge/scene_bridge.hpp"
#include "player/controller.hpp"
#include "world/region.hpp"
#include "world/seasons.hpp"
#include "world/world.hpp"
#include "world/worldgen.hpp"

namespace
{

using namespace harrowmere; // NOLINT - the plugin composition reads across the mod's namespaces

// ---- kernel-free config seam over the component context ----------------------

// The 0.4 wrapper's ConfigSource, answered by the ComponentContext's typed
// section readers. (config_engine.hpp's adapter binds the raw kernel store;
// a mod's context exposes the same values through the section readers, so
// this adapter keeps every other TU kernel-free exactly as D-DATA-1 pins.)
class ContextConfigSource final : public data::ConfigSource
{
public:
    explicit ContextConfigSource(engine::ComponentContext &context)
        : context_(context)
    {
    }

    std::optional<std::int64_t> integer(const char *section, const char *key) const override
    {
        return context_.configurationIntegerInSection(section, key);
    }

    std::optional<std::string> string(const char *section, const char *key) const override
    {
        return context_.configurationStringInSection(section, key);
    }

private:
    engine::ComponentContext &context_;
};

// ---- published-vocabulary mappers (sim ordinals -> capability surface) -------

harrowmere::views::SeasonId publishSeason(world::Season season)
{
    switch (season)
    {
    case world::Season::SPRING:
        return harrowmere::views::SeasonId::Spring;
    case world::Season::SUMMER:
        return harrowmere::views::SeasonId::Summer;
    case world::Season::AUTUMN:
        return harrowmere::views::SeasonId::Autumn;
    case world::Season::WINTER:
        return harrowmere::views::SeasonId::Winter;
    case world::Season::COUNT:
        break;
    }
    return harrowmere::views::SeasonId::Spring; // unreachable; deterministic recovery (§3.4)
}

harrowmere::views::WeatherKind publishWeather(world::Weather weather)
{
    switch (weather)
    {
    case world::Weather::CLEAR:
        return harrowmere::views::WeatherKind::Clear;
    case world::Weather::RAIN:
        return harrowmere::views::WeatherKind::Rain;
    case world::Weather::STORM:
        return harrowmere::views::WeatherKind::Storm;
    case world::Weather::FOG:
        return harrowmere::views::WeatherKind::Fog;
    case world::Weather::SNOW:
        return harrowmere::views::WeatherKind::Snow;
    case world::Weather::COUNT:
        break;
    }
    return harrowmere::views::WeatherKind::Clear; // unreachable; deterministic recovery (§3.4)
}

harrowmere::views::TerritoryType publishTerritoryType(TerritoryType type)
{
    switch (type)
    {
    case TerritoryType::VILLAGE:
        return harrowmere::views::TerritoryType::Village;
    case TerritoryType::CITY:
        return harrowmere::views::TerritoryType::City;
    case TerritoryType::KINGDOM:
        return harrowmere::views::TerritoryType::Kingdom;
    case TerritoryType::EMPIRE:
        return harrowmere::views::TerritoryType::Empire;
    case TerritoryType::COUNT:
        break;
    }
    return harrowmere::views::TerritoryType::Village; // unreachable; deterministic recovery (§3.4)
}

// ---- 7.2P1 bridge adapters (the production views over the runtime state) -----

// WorldViewForBridge over the WorldStore: territories in ascending id order
// (std::map iteration, R7), each anchored at its region's center; the primary
// weather is the player-region's (the viewing bubble's) weather.
class StoreBridgeWorldView final : public bridge::WorldViewForBridge
{
public:
    StoreBridgeWorldView(const world::WorldStore &store, const world::Region *primaryRegion)
        : store_(store)
        , primaryRegion_(primaryRegion)
    {
    }

    void setPrimaryRegion(const world::Region *region)
    {
        primaryRegion_ = region;
    }

    std::vector<TerritoryDesc> territories() const override
    {
        std::vector<TerritoryDesc> out;
        out.reserve(store_.territoryCount());
        for (const auto &[id, territory] : store_.territories())
        {
            TerritoryDesc desc;
            desc.id = territory.id;
            desc.type = territory.type;
            desc.center = world::regionCenter(territory.region);
            desc.safe = territory.safe;
            out.push_back(desc);
        }
        return out;
    }

    world::Weather primaryWeather() const override
    {
        if (primaryRegion_ == nullptr)
        {
            return world::Weather::CLEAR; // deterministic fallback: the world-epoch state
        }
        return world::weatherFromKind(primaryRegion_->weather.kind);
    }

private:
    const world::WorldStore &store_;
    const world::Region *primaryRegion_;
};

class ClockBridgeView final : public bridge::ClockViewForBridge
{
public:
    explicit ClockBridgeView(const core::WorldClock &clock)
        : clock_(clock)
    {
    }

    double dayPhase01() const override
    {
        return clock_.dayPhase01();
    }

private:
    const core::WorldClock &clock_;
};

// SceneRegistrySink over the engine capabilities: objects land in the scene
// registry (required), the sun lands in the lighting state (optional -
// degrade-if-absent, common.md §4.0), the sky tint has no runtime engine
// surface in v1 (the renderer reads [scene].sky_radiance at start; the
// runtime seam is 7.2's Phase-2 bridge sync) - the tint is computed and
// logged-once, never silently dropped.
class EngineSceneSink final : public bridge::SceneRegistrySink
{
public:
    EngineSceneSink(engine::SceneCapability *scene, engine::LightingCapability *lighting)
        : scene_(scene)
        , lighting_(lighting)
    {
    }

    Handle spawnBox(const Position &center, float halfWidth, float height, const std::array<float, 3> &tint) override
    {
        if (scene_ == nullptr)
        {
            return NULL_HANDLE; // headless/degraded: the bridge's logic still runs
        }
        engine::SceneObjectDescriptor object;
        object.type = engine::scene_object_type::box;
        // The registry's boxes are half extents; stand the marker on the
        // ground plane (center y = height/2).
        object.position = {static_cast<float>(center.x), height * 0.5f, static_cast<float>(center.z)};
        object.size = {halfWidth, height * 0.5f, halfWidth};
        object.albedo = tint;
        object.roughness = 0.9f;
        const engine::SceneCapability::ObjectHandle handle = scene_->addObject(object);
        return handle == engine::SceneCapability::invalidObject ? NULL_HANDLE : static_cast<Handle>(handle);
    }

    Handle spawnDisc(const Position &center, double radius, float height, const std::array<float, 3> &tint) override
    {
        if (scene_ == nullptr)
        {
            return NULL_HANDLE;
        }
        // The registry has no disc primitive; D-PRES-1's thin viewer maps the
        // safe-zone disc to a flat plate (a box of the disc's radius and
        // height) - the marker reads identically at gameplay zoom.
        engine::SceneObjectDescriptor object;
        object.type = engine::scene_object_type::box;
        object.position = {static_cast<float>(center.x), height * 0.5f, static_cast<float>(center.z)};
        object.size = {static_cast<float>(radius), height * 0.5f, static_cast<float>(radius)};
        object.albedo = tint;
        object.roughness = 1.0f;
        const engine::SceneCapability::ObjectHandle handle = scene_->addObject(object);
        return handle == engine::SceneCapability::invalidObject ? NULL_HANDLE : static_cast<Handle>(handle);
    }

    void setObjectPosition(Handle handle, const Position &p) override
    {
        if (scene_ == nullptr || handle == NULL_HANDLE)
        {
            return;
        }
        (void)scene_->setObjectTransform(
            static_cast<engine::SceneCapability::ObjectHandle>(handle),
            {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)}, 0.0f);
    }

    void setSunLight(const std::array<float, 3> &direction, float intensity) override
    {
        if (lighting_ == nullptr)
        {
            return; // degraded run (no lighting provider): the sun seam is silent
        }
        engine::DirectionalLightDescription sun;
        sun.direction[0] = direction[0];
        sun.direction[1] = direction[1];
        sun.direction[2] = direction[2];
        sun.intensity = intensity;
        lighting_->setSunLight(sun);
    }

    void setSkyTint(const std::array<float, 3> &tint) override
    {
        // No runtime engine surface for sky radiance in v1 (see file header).
        // Compute-and-log-once keeps the A-117 table observable and the seam
        // honest; 7.2's Phase-2 bridge sync replaces this body.
        if (!skyTintLogged_)
        {
            HARROWMERE_LOG_INFO(
                world,
                "sky tint set to [%.3f, %.3f, %.3f] (no runtime sky surface in v1; the renderer keeps [scene].sky_radiance)",
                tint[0], tint[1], tint[2]);
            skyTintLogged_ = true;
        }
    }

private:
    engine::SceneCapability *scene_;
    engine::LightingCapability *lighting_;
    bool skyTintLogged_ = false;
};

// ---- the runtime SimCapability (§4.2: views rebuilt per frame; submit queues) --

// The concrete capability the mod serves for "harrowmere.core". The base
// class owns the v0.1 stub storage and the accessor signatures; this subclass
// overrides the readers to return ITS rebuilt snapshots and routes submit()
// into the fixed-update lane's queue (applied next tick - §4.2 rule 2).
class RuntimeSimCapability final : public harrowmere::SimCapability
{
public:
    // --- the runtime state this capability snapshots (all owned by the mod) ---
    void bindRuntime(
        const world::WorldStore &world,
        const core::WorldClock &clock,
        const player::PlayerController &player,
        std::uint64_t tickNumber
    )
    {
        world_ = &world;
        clock_ = &clock;
        player_ = &player;
        tickNumber_ = tickNumber;
    }

    void submit(harrowmere::SimCommand command) override
    {
        if (queue_ != nullptr)
        {
            queue_->push(std::move(command));
            return;
        }
        // No runtime bound yet (a frame before start() finished): the command
        // is dropped with a WARN - §4.2 rule 2's rejection path, never a
        // crash. The UI mod cannot submit before the mod component started
        // (the resolver orders constructs), so this is a belt-and-braces arm.
        HARROWMERE_LOG_WARN(sim, "submit() before the runtime bound - command dropped");
    }

    void bindQueue(player::IntentQueue *queue)
    {
        queue_ = queue;
    }

    // --- reads: the rebuilt snapshots (§4.2 rule 1) ---
    const harrowmere::views::WorldView &world() const override
    {
        return worldView_;
    }

    const harrowmere::views::PlayerView &player() const override
    {
        return playerView_;
    }

    // rebuildViews() - the step-13 home (§4.4): runs once per frame from the
    // plugin's frame subscription, before the UI reads.
    void rebuildViews()
    {
        if (world_ == nullptr || clock_ == nullptr || player_ == nullptr)
        {
            return; // not started: keep the zero views
        }

        harrowmere::views::WorldView view{};
        const regionId regionUnderfoot = world::regionIdAt(player_->position);
        if (regionUnderfoot != world::kNoRegion)
        {
            if (const world::Region *region = world_->region(regionUnderfoot); region != nullptr)
            {
                view.currentRegion.id = static_cast<std::uint32_t>(region->id);
                view.currentRegion.manaRichness = region->manaRichness;
                view.season.season = publishSeason(world::seasonAt(clock_->worldTime));
                view.season.dayOfSeason = (clock_->worldTime % world::seasonLengthGameSeconds())
                                        / core::DAY_LENGTH_SECONDS;
                view.weather.kind = publishWeather(world::weatherFromKind(region->weather.kind));
            }
        }
        const territoryId territoryUnderfoot = world_->territoryAt(player_->position);
        view.currentTerritory.id = territoryUnderfoot;
        if (const world::Territory *territory = world_->territory(territoryUnderfoot); territory != nullptr)
        {
            view.currentTerritory.type = publishTerritoryType(territory->type);
            view.currentTerritory.safe = territory->safe;
            view.currentTerritory.lawless = territory->lawless;
            view.currentTerritory.lawSetKey = world_->lawsAt(territoryUnderfoot).key;
        }
        view.tickNumber = tickNumber_;
        worldView_ = std::move(view);

        harrowmere::views::PlayerView pv{};
        pv.position.x = player_->position.x;
        pv.position.y = player_->position.y;
        pv.position.z = player_->position.z;
        pv.worldTime = clock_->worldTime;
        pv.realTime = clock_->realTime;
        pv.afk = player_->afk;
        // cachedPower/peakPower/level/availableXp stay zero: Phase 2 fills
        // them (2.5) - the published shape does not move when they land.
        playerView_ = std::move(pv);
    }

private:
    const world::WorldStore *world_ = nullptr;
    const core::WorldClock *clock_ = nullptr;
    const player::PlayerController *player_ = nullptr;
    player::IntentQueue *queue_ = nullptr;
    std::uint64_t tickNumber_ = 0;

    harrowmere::views::WorldView worldView_{};
    harrowmere::views::PlayerView playerView_{};
};

// ---- the mod component ----------------------------------------------------------

class HarrowmereCoreMod final : public engine::ModInterface
{
public:
    HarrowmereCoreMod()
    {
        // The compiled manifest must agree with mod.toml on identity and on
        // `provides`; the requirement wiring is package data (the kernel
        // resolves the graph from mod.toml alone). Keep the two files in
        // lockstep - a drift is a precise startup error, by design.
        manifest_.name                 = "harrowmere_core";
        manifest_.kind                 = engine::ComponentKind::SharedLibraryMod;
        manifest_.version              = "0.1.0";
        manifest_.providedCapabilities = {"harrowmere.core"};
        manifest_.requiredCapabilities = {"logging", "scene", "input"};
        manifest_.optionalCapabilities = {"physics", "audio", "animation", "asset", "lighting"};
        manifest_.requiredPermissions  = {"filesystem.write"};
        manifest_.knownConfigKeys      = {
            "harrowmere.tick_rate",
            "harrowmere.time_scale",
            "harrowmere.autosave_game_hours",
            "harrowmere.seed",
        };
    }

    const engine::ComponentManifest &manifest() const override
    {
        return manifest_;
    }

    void construct(engine::ComponentContext &context) override
    {
        // The logging capability is the graph's root dependency; every later
        // system logs through the wrapper task 0.5 pins on top of this sink.
        logging_ = &context.acquireCapability<engine::LoggerCapability>();
        harrowmere::log::install(logging_);

        // The scene registry (required): the 7.2P1 bridge's sink.
        scene_ = &context.acquireCapability<engine::SceneCapability>();

        // The event bus: the two lane subscriptions bind here (start()).
        bus_ = &context.eventBus();

        // The optional lighting provider (the bridge's sun): declared in
        // optional_capabilities, probed here - an absent provider degrades
        // the sun seam, never the run (common.md §4.0).
        if (context.isCapabilityAvailable("lighting"))
        {
            lighting_ = static_cast<engine::LightingCapability *>(context.acquireCapability("lighting"));
        }
        // The [harrowmere] tunables (0.4's wrapper; missing key -> the pinned
        // package default). Read once in construct - the layer stack is
        // merged before components construct.
        const ContextConfigSource source(context);
        config_ = data::GameConfig::load(source);
    }

    void start() override
    {
        // The pinned startup line: TestBootstrap_GameAssemblyBoots (task 0.1)
        // asserts this exact text in the log of a headless boot.
        HARROWMERE_LOG_INFO(sim, "harrowmere_core 0.1.0 up");

        // ---- the Phase-1 runtime bootstrap ---------------------------------
        // Package content directory (0.4's pinned resolution):
        //   <exe_dir>/<mod_directory>/harrowmere_core/data/
        const std::string packageDir = config_.coreDataDirectory(engine::executableDirectory().string());
        data::DataErrors errors;
        if (!registry_.load(packageDir, errors) || !errors.empty())
        {
            // Fail startup with file + pointer + expected (the plan's hard
            // rule: never a mid-sim crash). The first error carries all three.
            std::string message = "game data failed validation";
            if (!errors.empty())
            {
                message = errors.front().toString();
            }
            HARROWMERE_LOG_ERROR(sim, "%s", message.c_str());
            throw std::runtime_error("harrowmere_core: " + message);
        }
        HARROWMERE_LOG_INFO(
            sim,
            "game data validated: %zu species, %zu items, %zu law sets, package=%s",
            registry_.speciesCount(),
            registry_.itemCount(),
            registry_.lawSetCount(),
            packageDir.c_str());

        // World seed: 0 = random-at-first-launch-and-persisted (task 7.1,
        // Phase 4). Until persistence lands, seed 0 stays the deterministic
        // seed 0 - two runs with the same config must agree byte-for-byte.
        std::uint64_t seed = config_.seed();
        if (seed == 0)
        {
            HARROWMERE_LOG_INFO(
                sim,
                "seed=0: the first-launch random pick arrives with save persistence (7.1); using the deterministic seed 0");
        }

        // The world from the seed (1.5): regions, tree, safe zones, faction
        // seeds, the population PLAN, and the FNV-1a summary hash (logged at
        // INFO inside - the exit checklist's determinism evidence).
        world_ = std::make_unique<world::WorldStore>(registry_.defaultLaws());
        world::NpcSeedOut npcs;
        genResult_ = world::generateWorld(seed, registry_, *world_, npcs);
        npcPlan_ = std::move(npcs); // consumed verbatim by 3.4's spawn path (Phase 2)

        // Clocks and RNG (0.3): the fixed-update lane drives both.
        clock_.timeScale = config_.timeScale();
        clock_.tickRate = config_.tickRate();
        rng_.seedAll(seed);

        // The two lane subscriptions (the fixed-update lane IS the sim's
        // heartbeat, D-ARCH-3; the frame lane carries the view rebuild and
        // the 7.2P1 bridge).
        subscribeLanes();

        // The player spawns at the beginner kingdom's safe village (1.5
        // stage 4; the design rule: safe zones are spawn points).
        player_.position = genResult_.playerSpawn;

        // The runtime capability: views snapshot this state; submit() queues.
        sim_.bindQueue(&intentQueue_);

        // The 7.2P1 scene bridge over the runtime state.
        bridgeWorld_ = std::make_unique<StoreBridgeWorldView>(*world_, world_->region(world::regionIdAt(player_.position)));
        bridgeClock_ = std::make_unique<ClockBridgeView>(clock_);
        bridgeSink_ = std::make_unique<EngineSceneSink>(scene_, lighting_);
        bridge_ = std::make_unique<bridge::SceneBridge>(*bridgeWorld_, *bridgeClock_, *bridgeSink_);

        HARROWMERE_LOG_INFO(
            sim,
            "world ready: player spawn (%.1f, %.1f, %.1f) in territory %llu; fixed-update lane bound",
            player_.position.x,
            player_.position.y,
            player_.position.z,
            static_cast<unsigned long long>(genResult_.beginnerTerritory));
    }

    void stop() override
    {
        // Reverse of start: subscriptions first (no in-flight dispatch lands
        // on tearing-down state), then the world.
        fixedUpdateSubscription_.release();
        frameSubscription_.release();
        bridge_.reset();
        bridgeSink_.reset();
        bridgeClock_.reset();
        bridgeWorld_.reset();
        sim_.bindQueue(nullptr);
        npcPlan_.clear();
        genResult_ = world::WorldGenResult{};
        world_.reset();
        HARROWMERE_LOG_INFO(sim, "harrowmere_core stopping (state flushed)");
        harrowmere::log::uninstall();
    }

    // The pull-based provider hook: the kernel asks at acquisition time,
    // every time - nothing is registered anywhere between the phases.
    void *capabilityPointerFor(const std::string &capabilityName) override
    {
        if (capabilityName == harrowmere::SimCapability::capabilityName)
        {
            return &sim_;
        }
        return nullptr;
    }

    // ---- the two lane subscriptions ------------------------------------------
    //
    // The bus pointer landed in construct (every component's context carries
    // the kernel-owned bus; it outlives the mod). start() calls this once the
    // world exists so no dispatch can land on half-built state.

    void subscribeLanes()
    {
        if (bus_ == nullptr)
        {
            return;
        }
        engine::EventChannel<engine::EngineFixedUpdateEvent> &fixedChannel =
            bus_->channel<engine::EngineFixedUpdateEvent>();
        fixedUpdateSubscription_ = engine::EventSubscription(
            bus_,
            typeid(engine::EngineFixedUpdateEvent),
            fixedChannel.subscribe<HarrowmereCoreMod, &HarrowmereCoreMod::onFixedUpdate>(this));

        engine::EventChannel<engine::EngineFrameEvent> &frameChannel = bus_->channel<engine::EngineFrameEvent>();
        frameSubscription_ = engine::EventSubscription(
            bus_,
            typeid(engine::EngineFrameEvent),
            frameChannel.subscribe<HarrowmereCoreMod, &HarrowmereCoreMod::onFrame>(this));
    }

private:
    // ONE runTick: the §4.4 order without the Scheduler (Phase 2's 3.1 turns
    // these three calls into step registrations - re-registration, not a
    // rewrite; 3.8's and 3.10's DEVIATIONS.md rows pin exactly this shape).
    void onFixedUpdate(const engine::EngineFixedUpdateEvent &event)
    {
        // Step 0 (clocks): both clocks advance one tick - worldTime by
        // timeScale/tickRate game seconds (integer, remainder carried), real
        // time by the same lane.
        clock_.advanceTick();
        ++tickNumber_;

        // Steps 0/2 (3.8's Phase-1 slice): the season roll rides the clocks,
        // then the weather machine - exactly tickSeasonsAndWeather's order.
        world::tickSeasonsAndWeather(clock_, *world_, rng_);

        // Step 9 (player): drain the queue, integrate the motion state, run
        // the AFK verdict (3.10's applyIntents).
        player::applyIntents(player_, intentQueue_, clock_, event.deltaSeconds);

        // The bridge's primary-weather anchor follows the player.
        if (bridgeWorld_ != nullptr)
        {
            bridgeWorld_->setPrimaryRegion(world_->region(world::regionIdAt(player_.position)));
        }
    }

    // Step 13's home (§4.4): rebuild the views once per frame, then let the
    // bridge push the world into the scene registry (frame-coupled by design;
    // D-ARCH-3 keeps the sim itself on the fixed lane).
    void onFrame(const engine::EngineFrameEvent & /*event*/)
    {
        sim_.bindRuntime(*world_, clock_, player_, tickNumber_);
        sim_.rebuildViews();
        if (bridge_ != nullptr)
        {
            bridge_->onFrame();
        }
    }

    engine::ComponentManifest manifest_;
    engine::LoggerCapability *logging_ = nullptr;
    engine::SceneCapability *scene_ = nullptr;
    engine::LightingCapability *lighting_ = nullptr;
    engine::EventBus *bus_ = nullptr;

    data::GameConfig config_ = data::GameConfig::withValues(10, 60, 6, 0, "mods");
    data::DataRegistry registry_;

    core::WorldClock clock_{};
    core::SeedRng rng_{};
    std::unique_ptr<world::WorldStore> world_;
    world::WorldGenResult genResult_{};
    world::NpcSeedOut npcPlan_;

    player::PlayerController player_{};
    player::IntentQueue intentQueue_;
    std::uint64_t tickNumber_ = 0;

    RuntimeSimCapability sim_{};

    std::unique_ptr<StoreBridgeWorldView> bridgeWorld_;
    std::unique_ptr<ClockBridgeView> bridgeClock_;
    std::unique_ptr<EngineSceneSink> bridgeSink_;
    std::unique_ptr<bridge::SceneBridge> bridge_;

    engine::EventSubscription fixedUpdateSubscription_;
    engine::EventSubscription frameSubscription_;
};

engine::ModInterface *createHarrowmereCoreMod()
{
    return new HarrowmereCoreMod();
}

void destroyHarrowmereCoreMod(engine::ModInterface *mod)
{
    delete mod;
}

const EngineModEntryTable harrowmereCoreEntryTable{
    .interfaceVersion = engine::engineModInterfaceVersion,
    .createMod        = &createHarrowmereCoreMod,
    .destroyMod       = &destroyHarrowmereCoreMod,
};

} // namespace

// The one exported symbol of a shared library mod (mod_api.hpp). The kernel
// resolves it by name after dlopen and never links the mod's import library.
extern "C" const EngineModEntryTable *engine_query_mod_entry_table()
{
    return &harrowmereCoreEntryTable;
}
