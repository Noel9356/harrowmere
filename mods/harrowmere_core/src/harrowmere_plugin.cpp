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
//                (the bridge's registry), input and the lighting provider
//                (the bridge's sun - the manifest DEMANDS lighting +
//                pbr_direct_diffuse, so the provider is in the run; the
//                probe below stays as the degrade-if-absent guard);
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
#include "engine/kernel/mod_discovery.hpp"        // deterministic package discovery for 7.5 data overlays

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "logging_capability.hpp"
#include "scene_capability.hpp"    // engine::SceneCapability (required)
#include "lighting_capability.hpp" // engine::LightingCapability (manifest-demanded; the probe stays defensive)

#include "harrowmere_capability.hpp"

#include "core/clock.hpp"
#include "core/enums.hpp"
#include "core/log.hpp"
#include "core/rng.hpp"
#include "data/config.hpp"
#include "data/config_engine.hpp" // only the kernel-facing ConfigSource adapter shape; the context source below keeps src/data/ engine-free
#include "data/registry.hpp"
#include "data/mod_overrides.hpp"
#include "bridge/scene_bridge.hpp"
#include "bridge/terrain.hpp"          // task 5.1.2: the deterministic heightfield (heightAt is the ONE height door)
#include "bridge/scatter.hpp"          // task 5.2.1: scatter placements (data + seed)
#include "bridge/borders.hpp"          // task 5.2.2: water + territory borders
#include "bridge/atmosphere.hpp"       // task 5.1.4: sky/sun/weather on screen
#include "bridge/player_view.hpp"      // task 2.8: the step-13 PlayerView fill (the tested free function)
#include "bridge/society_view.hpp"     // task 7.3b: the society view fills (the tested free functions)
#include "bridge/store_adapters.hpp"   // the 7.2 runtime CharactersView over the stores (3.1's recipe)
#include "character/store.hpp"         // CharacterStore (2.0) - the player + every NPC record
#include "character/status.hpp"        // cooldown helpers (the sim reads them through ctx)
#include "combat/attacks.hpp"          // CombatQueue + CombatResolutionSystem (step 10)
#include "combat/consent.hpp"          // ConsentRegistry (2.7 duels/arenas)
#include "combat/death.hpp"            // CorpseStore + Aftermath/CorpseDecay systems (step 11)
#include "core/command_apply.hpp"      // PlayerCommandSystem (step 9) + CommandServices
#include "core/game_events.hpp"        // the 0.7 payload types the bus adapter forwards
#include "core/scheduler.hpp"          // the 14-step tick pipeline (3.1)
#include "core/world_context.hpp"      // WorldContext + the seam interfaces
#include "data/json.hpp"               // goods.json (the 0.4 adapter shape, D-DATA-1)
#include "economy/economy.hpp"         // EconomyStore + EconomySystem (step 7)
#include "factions/claims.hpp"         // task 6.3: the claim ledger + the abandonment sweep (step 1)
#include "factions/factions.hpp"       // task 6.1: the faction store + queries + monthly drift
#include "factions/war.hpp"            // task 6.2: the war registry + the progression tick (step 8)
#include "items/crafting.hpp"          // RecipeTable (2.4's recipes.json table)
#include "items/inventory.hpp"         // InventoryStore + ItemInstanceRegistry (2.4)
#include "justice/cases.hpp"           // runCaseTicks (step 6) + sweepExpiredEvidence (the daily backstop)
#include "justice/evidence.hpp"        // JusticeSystem (the real justice sink — 4.1 swaps the null default)
#include "justice/enforcement.hpp"     // task 4.6: the warrant response + lawless driver (step 6)
#include "justice/justice_store.hpp"   // the JusticeStore (§4.5's evidence/warrant/reputation/jail state)
#include "justice/reputation.hpp"      // the complete Reputation record (the store's map value)
#include "justice/conviction.hpp"      // isArrested (the fast-forward's exit read)
#include "justice/suspicion.hpp"       // task 4.5: the suspicion pass (step 6) + the price/A-25 seams
#include "justice/warrants.hpp"        // the complete Warrant record (the store's map value)
#include "events/engine.hpp"           // task 5.1: the event engine (step 5; the spawner/exclusion seams)
#include "events/contribution.hpp"     // task 5.3: the contribution router + the snapshot slice
#include "events/world_effects.hpp"    // task 5.5: the weather-bias pin (step 2)
#include "npc/lifecycle.hpp"           // Lifecycle/NpcTick systems (steps 1/4)
#include "npc/lod.hpp"                 // LodSystem (step 3) + the spatial source
#include "npc/perception.hpp"          // task 3.9: the perceive seam (sightings + witness candidates)
#include "npc/roles.hpp"               // RoleRegistry + RoleDutiesSystem (step 4)
#include "npc/state.hpp"               // NpcStore (3.4's sidecar records)
#include "progression/matchmaking.hpp" // task 6.5: the matchmaking sink (the SP difficulty dial)
#include "quests/quests.hpp"           // task 6.4: the quest layer (the event-engine wrappers)
#include "player/controller.hpp"
#include "world/region.hpp"
#include "world/seasons.hpp"
#include "world/spatial_grid.hpp" // 1.6's index (the LOD source + consent broadcast)
#include "world/world.hpp"
#include "world/worldgen.hpp"

namespace
{

using namespace harrowmere; // NOLINT - the plugin composition reads across the mod's namespaces

// D-JAIL-1's fast-forward burst bound (the as-built note): one ServeSentence
// click serves at most this many raw ticks of the pinned pipeline (~7 game-
// days at the 10 Hz x TIME_SCALE 60 cadence, D-TICK-1). A longer sentence
// keeps serving in real time; the next click serves the next burst. A
// runtime pacing bound, not a game rule — it names no game state.
constexpr std::uint64_t kFastForwardTickBurstMax = 100000;

// The signals ring the Justice panel mirrors (7.3b): the last N observable
// signals (4.6's Investigated/Wanted/Poster/Greeting) reach the UI's
// UiSignals view; a presentation-pacing bound, not game state.
constexpr std::size_t kSignalRingCapacity = 16;

// ---- kernel-free config seam over the component context ----------------------

// The 0.4 wrapper's ConfigSource, answered by the ComponentContext's typed
// section readers. (config_engine.hpp's adapter binds the raw kernel store;
// a mod's context exposes the same values through the section readers, so
// this adapter keeps every other TU kernel-free exactly as D-DATA-1 pins.)
class ContextConfigSource final : public data::ConfigSource
{
public:
    explicit ContextConfigSource(
        engine::ComponentContext &context
    )
        : context_(context)
    {}

    std::optional<std::int64_t> integer(
        const char *section, const char *key
    ) const override
    {
        return context_.configurationIntegerInSection(section, key);
    }

    std::optional<std::string> string(
        const char *section, const char *key
    ) const override
    {
        return context_.configurationStringInSection(section, key);
    }

private:
    engine::ComponentContext &context_;
};

// ---- published-vocabulary mappers (sim ordinals -> capability surface) -------

harrowmere::views::SeasonId publishSeason(
    world::Season season
)
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

harrowmere::views::WeatherKind publishWeather(
    world::Weather weather
)
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

harrowmere::views::TerritoryType publishTerritoryType(
    TerritoryType type
)
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
    StoreBridgeWorldView(
        const world::WorldStore &store, const world::Region *primaryRegion
    )
        : store_(store), primaryRegion_(primaryRegion)
    {}

    void setPrimaryRegion(
        const world::Region *region
    )
    {
        primaryRegion_ = region;
    }

    // Phase 5: when false, territories() returns empty — the bridge creates
    // no debug markers (territory boxes + safe-zone discs). Used when the
    // terrain mesh is present so the markers don't clutter the scene.
    void setShowMarkers(
        bool show
    ) noexcept
    {
        showMarkers_ = show;
    }

    std::vector<TerritoryDesc> territories() const override
    {
        if (!showMarkers_)
            return {}; // Phase 5: suppress debug markers when terrain is active
        std::vector<TerritoryDesc> out;
        out.reserve(store_.territoryCount());
        for (const auto &[id, territory] : store_.territories())
        {
            TerritoryDesc desc;
            desc.id     = territory.id;
            desc.type   = territory.type;
            desc.center = world::regionCenter(territory.region);
            desc.safe   = territory.safe;
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
    bool showMarkers_ = true; // Phase 5: false = suppress debug markers
};

// ClockBridgeView — the runtime clock read for the bridge (dayPhase01).
class ClockBridgeView final : public bridge::ClockViewForBridge
{
public:
    explicit ClockBridgeView(
        const core::WorldClock &clock
    )
        : clock_(clock)
    {}

    double dayPhase01() const override
    {
        return clock_.dayPhase01();
    }

private:
    const core::WorldClock &clock_;
};

// SceneRegistrySink over the engine capabilities: objects land in the scene
// registry (required), the sun lands in the lighting state (manifest-demanded
// - this sink still degrades to a no-op when a trimmed build ships without
// the provider), the sky tint has no runtime engine
// surface in v1 (the renderer reads [scene].sky_radiance at start; the
// runtime seam is 7.2's Phase-2 bridge sync) - the tint is computed and
// logged-once, never silently dropped.
class EngineSceneSink final : public bridge::SceneRegistrySink
{
public:
    EngineSceneSink(
        engine::SceneCapability *scene, engine::LightingCapability *lighting
    )
        : scene_(scene), lighting_(lighting)
    {}

    Handle spawnBox(
        const Position &center, float halfWidth, float height, const std::array<float, 3> &tint
    ) override
    {
        if (scene_ == nullptr)
        {
            return NULL_HANDLE; // headless/degraded: the bridge's logic still runs
        }
        engine::SceneObjectDescriptor object;
        object.type = engine::scene_object_type::box;
        // The registry's boxes are half extents; stand the marker on the
        // ground plane (center y = height/2).
        object.position  = {static_cast<float>(center.x), height * 0.5f, static_cast<float>(center.z)};
        object.size      = {halfWidth, height * 0.5f, halfWidth};
        object.albedo    = tint;
        object.roughness = 0.9f;
        const engine::SceneCapability::ObjectHandle handle = scene_->addObject(object);
        return handle == engine::SceneCapability::invalidObject ? NULL_HANDLE : static_cast<Handle>(handle);
    }

    Handle spawnDisc(
        const Position &center, double radius, float height, const std::array<float, 3> &tint
    ) override
    {
        if (scene_ == nullptr)
        {
            return NULL_HANDLE;
        }
        // The registry has no disc primitive; D-PRES-1's thin viewer maps the
        // safe-zone disc to a flat plate (a box of the disc's radius and
        // height) - the marker reads identically at gameplay zoom.
        engine::SceneObjectDescriptor object;
        object.type      = engine::scene_object_type::box;
        object.position  = {static_cast<float>(center.x), height * 0.5f, static_cast<float>(center.z)};
        object.size      = {static_cast<float>(radius), height * 0.5f, static_cast<float>(radius)};
        object.albedo    = tint;
        object.roughness = 1.0f;
        const engine::SceneCapability::ObjectHandle handle = scene_->addObject(object);
        return handle == engine::SceneCapability::invalidObject ? NULL_HANDLE : static_cast<Handle>(handle);
    }

    void setObjectPosition(
        Handle handle, const Position &p
    ) override
    {
        if (scene_ == nullptr || handle == NULL_HANDLE)
        {
            return;
        }
        (void)scene_->setObjectTransform(
            static_cast<engine::SceneCapability::ObjectHandle>(handle),
            {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)},
            0.0f
        );
    }

    void setSunLight(
        const std::array<float, 3> &direction, float intensity
    ) override
    {
        if (lighting_ == nullptr)
        {
            return; // degraded run (no lighting provider): the sun seam is silent
        }
        engine::DirectionalLightDescription sun;
        sun.direction[0] = direction[0];
        sun.direction[1] = direction[1];
        sun.direction[2] = direction[2];
        sun.intensity    = intensity;
        lighting_->setSunLight(sun);
    }

    // Phase 5: spawn a sphere (character presence). The scene registry's
    // sphere primitive takes radius in size.x.
    Handle spawnSphere(
        const Position &center, float radius, const std::array<float, 3> &tint
    ) override
    {
        if (scene_ == nullptr)
            return NULL_HANDLE;
        engine::SceneObjectDescriptor object;
        object.type       = engine::scene_object_type::sphere;
        object.position   = {static_cast<float>(center.x), static_cast<float>(center.y), static_cast<float>(center.z)};
        object.size       = {radius, radius, radius};
        object.albedo     = tint;
        object.roughness  = 0.6f;
        const auto handle = scene_->addObject(object);
        return handle == engine::SceneCapability::invalidObject ? NULL_HANDLE : static_cast<Handle>(handle);
    }

    // Phase 5: spawn the terrain mesh. The bridge passes a flat (x, y, z)
    // triple array over a gridSteps x gridSteps grid. This override converts
    // it to a triangle mesh (vertices with normals + indices), registers it
    // via addMesh, and adds a mesh scene object that instances it.
    //
    // The input grid is TERRAIN_GRID_STEPS (512) — too dense for software
    // raster (lavapipe). We subsample to a coarser grid (64x64) by striding
    // through the input. The relief is still visible; the triangle count
    // drops from ~524K to ~8K.
    Handle spawnTerrainMesh(
        const std::vector<double> &vertices, std::uint32_t gridSteps
    ) override
    {
        if (scene_ == nullptr || gridSteps < 2 || vertices.empty())
            return NULL_HANDLE;

        // Subsample to a coarser grid for the actual mesh.
        constexpr std::uint32_t kCoarseSteps = 128;
        const std::uint32_t stride           = std::max(1u, gridSteps / kCoarseSteps);
        const std::uint32_t coarseSteps      = std::min(kCoarseSteps, (gridSteps + stride - 1) / stride);

        // Build the triangle mesh: coarseSteps x coarseSteps vertices.
        engine::SceneMeshDescriptor mesh;
        mesh.vertices.reserve(static_cast<std::size_t>(coarseSteps) * coarseSteps * 6);
        mesh.indices.reserve(static_cast<std::size_t>(coarseSteps - 1) * (coarseSteps - 1) * 6);

        // First pass: positions (subsampled).
        for (std::uint32_t row = 0; row < coarseSteps; ++row)
        {
            for (std::uint32_t col = 0; col < coarseSteps; ++col)
            {
                const std::uint32_t srcRow = std::min(row * stride, gridSteps - 1);
                const std::uint32_t srcCol = std::min(col * stride, gridSteps - 1);
                const std::size_t idx      = (static_cast<std::size_t>(srcRow) * gridSteps + srcCol) * 3;
                if (idx + 2 >= vertices.size())
                    continue;
                mesh.vertices.push_back(static_cast<float>(vertices[idx + 0])); // x
                mesh.vertices.push_back(static_cast<float>(vertices[idx + 1])); // y
                mesh.vertices.push_back(static_cast<float>(vertices[idx + 2])); // z
                mesh.vertices.push_back(0.0f);                                  // normal x (computed in second pass)
                mesh.vertices.push_back(1.0f);                                  // normal y
                mesh.vertices.push_back(0.0f);                                  // normal z
            }
        }

        // Compute normals via central difference on the coarse grid.
        for (std::uint32_t row = 0; row < coarseSteps; ++row)
        {
            for (std::uint32_t col = 0; col < coarseSteps; ++col)
            {
                const std::size_t vi = (static_cast<std::size_t>(row) * coarseSteps + col) * 6;
                if (vi + 5 >= mesh.vertices.size())
                    continue;
                auto pos = [&](int r, int c) -> std::array<float, 3> {
                    r                    = std::clamp(r, 0, static_cast<int>(coarseSteps) - 1);
                    c                    = std::clamp(c, 0, static_cast<int>(coarseSteps) - 1);
                    const std::size_t ni = (static_cast<std::size_t>(r) * coarseSteps + c) * 6;
                    return {mesh.vertices[ni], mesh.vertices[ni + 1], mesh.vertices[ni + 2]};
                };
                const auto pL  = pos(static_cast<int>(row), static_cast<int>(col) - 1);
                const auto pR  = pos(static_cast<int>(row), static_cast<int>(col) + 1);
                const auto pD  = pos(static_cast<int>(row) - 1, static_cast<int>(col));
                const auto pU  = pos(static_cast<int>(row) + 1, static_cast<int>(col));
                const float dx = (pR[1] - pL[1]) * 0.5f;
                const float dz = (pU[1] - pD[1]) * 0.5f;
                float nx = -dx, ny = 1.0f, nz = -dz;
                const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (len > 0.0f)
                {
                    nx /= len;
                    ny /= len;
                    nz /= len;
                }
                mesh.vertices[vi + 3] = nx;
                mesh.vertices[vi + 4] = ny;
                mesh.vertices[vi + 5] = nz;
            }
        }

        // Build indices: two triangles per grid cell (CCW winding).
        for (std::uint32_t row = 0; row < coarseSteps - 1; ++row)
        {
            for (std::uint32_t col = 0; col < coarseSteps - 1; ++col)
            {
                const std::uint32_t v00 = row * coarseSteps + col;
                const std::uint32_t v10 = row * coarseSteps + col + 1;
                const std::uint32_t v01 = (row + 1) * coarseSteps + col;
                const std::uint32_t v11 = (row + 1) * coarseSteps + col + 1;
                // Triangle 1: v00, v01, v10
                mesh.indices.push_back(v00);
                mesh.indices.push_back(v01);
                mesh.indices.push_back(v10);
                // Triangle 2: v10, v01, v11
                mesh.indices.push_back(v10);
                mesh.indices.push_back(v01);
                mesh.indices.push_back(v11);
            }
        }

        // Register the mesh.
        const auto meshHandle = scene_->addMesh("terrain", mesh);
        if (meshHandle == engine::SceneCapability::invalidMesh)
            return NULL_HANDLE;

        // Add a mesh scene object that instances it. The size is the
        // bounding-box half extents (the registry computes these from the
        // mesh, but we need to set them for the tracing proxy).
        engine::SceneObjectDescriptor object;
        object.type       = engine::scene_object_type::mesh;
        object.meshHandle = meshHandle;
        // The terrain spans [0, worldExtent] on x/z; y range is ~[0, 120].
        const float halfExtent = static_cast<float>(vertices.size() >= 3 ? vertices[0] : 8000.0); // rough
        object.size            = {halfExtent, 60.0f, halfExtent};                                 // proxy box
        object.albedo          = {0.20f, 0.35f, 0.12f}; // grass green (fallback if no texture)
        object.roughness       = 0.95f;
        object.textureSet      = "grass"; // PBR texture set (assets/textures/grass/)
        object.uvScale         = 64.0f;   // tile the 512px texture every 64m
        const auto handle      = scene_->addObject(object);
        return handle == engine::SceneCapability::invalidObject ? NULL_HANDLE : static_cast<Handle>(handle);
    }

    // Phase 2: remove an object (character despawn, corpse decay).
    void removeObject(
        Handle handle
    ) override
    {
        if (scene_ == nullptr || handle == NULL_HANDLE)
            return;
        scene_->removeObject(static_cast<engine::SceneCapability::ObjectHandle>(handle));
    }

    void setSkyTint(
        const std::array<float, 3> &tint
    ) override
    {
        // No runtime engine surface for sky radiance in v1 (see file header).
        // Compute-and-log-once keeps the A-117 table observable and the seam
        // honest; 7.2's Phase-2 bridge sync replaces this body.
        if (!skyTintLogged_)
        {
            HARROWMERE_LOG_INFO(
                world,
                "sky tint set to [%.3f, %.3f, %.3f] (no runtime sky surface in v1; the renderer keeps "
                "[scene].sky_radiance)",
                tint[0],
                tint[1],
                tint[2]
            );
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
class RuntimeWorldQueries; // the adapter below (the step systems hold back-pointers)

// ---- the 3.1 step systems the composition owns ------------------------------
// The Phase-1 slices move into the table as thin systems - re-registration,
// not a rewrite (the card's migration rule; the pinned functions keep their
// owners: world::updateSeasons/updateWeather, player::applyIntents).

// Step 0 (Clocks): both clocks advance one tick, then the context reads the
// fresh values (R8: the sim never reads the wall clock).
struct ClockStepSystem final : public IStepSystem
{
    core::WorldClock *clock = nullptr;
    WorldContext *ctx       = nullptr;
    std::function<void(std::uint64_t)> onWorldTime; // the adapter's bindWorldTime (set at bind)
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

// Step 9's claim slice (6.3, registered AFTER the PlayerCommandSystem): the
// local player's presence stamps over their own claims — the controller's
// AFK verdict is fresh here, so "AFK time never counts" (D-AFK-1) reads the
// truth of this tick. NPC owners are stamped by the claim sweep itself.
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

// Step 6 (Justice): 4.2's case ticks — the per-case state machine for the
// territories with the player nearby (A-75), one updateEvidence per case per
// game-hour (A-76). sweepExpiredEvidence is NOT here: the daily backstop runs
// in step 1 (the doc's own placement, DayGateJusticeSweepSystem below).
struct JusticeStepSystem final : public IStepSystem
{
    void run(
        WorldContext &ctx
    ) override
    {
        justice::runCaseTicks(ctx);
    }
};

// Step 1's justice slice: the daily global backstop sweep (4.2 — "the global
// backstop elsewhere"), day-gated like the lifecycle sweep.
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

// The 0.7 sim-event forwarder: the justice publications ride the ISimEventSink
// seam out of the kernel-free WorldContext and land on the engine bus channels
// (the UI and later phases subscribe there; 4.7/5.4 consume the facts).
class BusSimEventSink final : public ISimEventSink
{
public:
    // The bus lands in construct() (after member init) — bind() there.
    void bind(
        engine::EventBus *bus
    )
    {
        bus_ = bus;
    }

    void onCrimeRecorded(
        const CrimeRecorded &ev
    ) override
    {
        if (bus_ != nullptr)
        {
            bus_->channel<CrimeRecorded>().publish(ev);
        }
    }
    void onWarrantIssued(
        const WarrantIssued &ev
    ) override
    {
        if (bus_ != nullptr)
        {
            bus_->channel<WarrantIssued>().publish(ev);
        }
    }
    void onWarrantResolved(
        const WarrantResolved &ev
    ) override
    {
        if (bus_ != nullptr)
        {
            bus_->channel<WarrantResolved>().publish(ev);
        }
    }
    void onEventStarted(
        const EventStarted &ev
    ) override
    {
        if (bus_ != nullptr)
        {
            bus_->channel<EventStarted>().publish(ev);
        }
    }
    void onEventEnded(
        const EventEnded &ev
    ) override
    {
        if (bus_ != nullptr)
        {
            bus_->channel<EventEnded>().publish(ev);
        }
    }
    void onEventStateChanged(
        const EventStateChanged &ev
    ) override
    {
        if (bus_ != nullptr)
        {
            bus_->channel<EventStateChanged>().publish(ev);
        }
    }
    void onTerritoryChanged(
        const TerritoryChanged &ev
    ) override
    {
        // 6.2/6.3's territory facts (declarations, claims, outcome
        // transfers) — the 0.7 payload set's waiting emitter seam.
        if (bus_ != nullptr)
        {
            bus_->channel<TerritoryChanged>().publish(ev);
        }
    }
    void onPlayerSignal(
        const PlayerSignal &ev
    ) override
    {
        if (bus_ != nullptr)
        {
            bus_->channel<PlayerSignal>().publish(ev);
        }
        // Task 7.3b: the signals mirror rides to the UI's view too (the
        // Justice panel's "Recent signals" block) — the observable surface
        // is the point of the fog-of-war rule. The ring is the capability's
        // (bound once at wiring; the bus side never sees it null).
        if (signalRing_ != nullptr)
        {
            views::PlayerSignalEntry entry;
            switch (ev.kind)
            {
            case PlayerSignal::Kind::Investigated:
                entry.kind = views::PlayerSignalKind::Investigated;
                break;
            case PlayerSignal::Kind::Wanted:
                entry.kind = views::PlayerSignalKind::Wanted;
                break;
            case PlayerSignal::Kind::Poster:
                entry.kind = views::PlayerSignalKind::Poster;
                break;
            case PlayerSignal::Kind::Greeting:
                entry.kind = views::PlayerSignalKind::Greeting;
                break;
            case PlayerSignal::Kind::War:
                entry.kind = views::PlayerSignalKind::War; // 6.2's notice (D-WAR-SIGNAL-1)
                break;
            }
            entry.text = ev.text;
            signalRing_->entries.push_back(std::move(entry));
            if (signalRing_->entries.size() > kSignalRingCapacity)
            {
                signalRing_->entries.erase(signalRing_->entries.begin()); // drop the oldest
            }
        }
    }

    void bindSignalRing(
        views::UiSignals *ring
    )
    {
        signalRing_ = ring;
    }

private:
    engine::EventBus *bus_        = nullptr;
    views::UiSignals *signalRing_ = nullptr;
};

// Step 11's tail: the 1.6 spatial index mirrors the authoritative body
// positions (the LOD source, the consent broadcast and the interact gate all
// read it). Only entities that can have moved this tick are visited: the
// player, the NEAR/MID active sets, and - on a daily-sweep tick - the whole
// FAR population (aging deaths, migration teleports). FAR NPCs are otherwise
// static (their sim is statistical, 3.5's table).
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
        // nothing — it was ~90% of the sustained tick budget on a 5,000-NPC
        // world. The 8.3 soak's step-cost profile caught it; the per-tick
        // NEAR/MID branch's exists() guards keep the hot set honest, and the
        // daily prune bounds any stale grid entry to one game-day.)

        if (playerCharacterId != 0 && characters->exists(playerCharacterId))
        {
            syncOne(playerCharacterId, player->position); // the controller's transform is authoritative
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
        const bool sweepTick    = day != *lastSweepDay; // the daily sweep ran this tick (step 1)
        *lastSweepDay           = day;
        if (sweepTick)
        {
            // The daily prune (see the run() header note): deaths since
            // yesterday leave stale ledger rows; drop them, then re-sync the
            // whole population.
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
                    syncOne(id, characters->get(id).body.position); // migration teleports / natural deaths
                }
            }
        }
    }
};

// ---- the 3.1 runtime adapters -----------------------------------------------
// WorldContext is plain data over pointers (the 2.1 pinned shape); the plugin
// binds these adapters in start() and keeps them alive as members.

// IWorldQueries over the worldgen graph + the living stores. Every walk
// reuses WorldStore's D-JUR-1 implementations; populations read the stores
// (the living truth), never the stale plan.
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
        // territory_templates.json's per-TYPE blueprint (1.2's loader): the
        // band a territory of this type keeps (3.4 births / 3.6 replacement).
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

    // 3.8's shipped hooks (consumed, never redefined).
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
        // The most common resident species in the territory (3.4's bias read);
        // ties break to the lower species id (R7's deterministic order).
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
        // 1.4's lawsAt walk (D-JUR-1): the territory's effective law set's
        // cultureBias toward the self species (laws.json; negative = xenophobia).
        return world_.lawsAt(t).cultureBias.count(self) != 0 ? world_.lawsAt(t).cultureBias.at(self) : 0;
    }

    bool makeNpcSeed(
        NpcSeed &out, territoryId home, speciesId species, const char *roleKey
    ) override
    {
        // 1.5's single-NPC seed path (3.4 births, 3.6 backup migrants): a
        // deterministic adult of the requested species/role at the settlement.
        out.species       = species;
        out.homeTerritory = home;
        out.homeRegion    = homeRegionOf(home);
        out.roleKey       = roleKey;
        out.ageYears      = std::string(roleKey) == CHILD_ROLE_KEY ? 0u : kAdultSeedYears_;
        out.level         = 1;
        out.givenName     = "npc_" + std::to_string(++nameCounter_);
        return true;
    }

    // -- runtime wiring the plugin refreshes per phase --
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
    const data::TerritoryTemplateSet *templates_ = nullptr; // bound at start (population bands)
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

    void rebuildCensusIfStale() const
    {
        const std::uint64_t day = currentWorldTime_ / core::DAY_LENGTH_SECONDS;
        if (censusDay_ == day && !homeCensus_.empty())
        {
            return;
        }
        homeCensus_.clear();
        regionCensus_.clear();
        for (const auto &[npcId, npc] : npcs_.all())
        {
            if (!characters_.exists(npcId))
            {
                continue;
            }
            ++homeCensus_[npc.homeTerritory];
            ++regionCensus_[npc.currentRegion];
        }
        censusDay_ = day;
    }
};

// IBagSource over the 2.4 player-side bag store (3.3's corpse-loot read).
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

// ILodSpatialSource over 1.6's grid (step 3 is O(nearby cells), never O(n)).
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
        world_      = &world;
        clock_      = &clock;
        player_     = &player;
        tickNumber_ = tickNumber;
    }

    void submit(
        harrowmere::SimCommand command
    ) override
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

    void bindQueue(
        player::IntentQueue *queue
    )
    {
        queue_ = queue;
    }

    // The signals ring the BusSimEventSink appends into (7.3b's observable
    // surface — the Justice panel's mirror). The ring is the view: it fills
    // as the signals fire, not per rebuild.
    harrowmere::views::UiSignals &signalRing()
    {
        return signalsView_;
    }

    // Task 7.3b: the society fills' sources (the justice/events state lives
    // in the context + the engine; the template table names the events).
    // Bound once at wiring, before the first frame rebuild.
    void bindSocietySources(
        WorldContext *ctx, events::EventEngine *engine, const events::EventTemplateTable *templates
    )
    {
        societyCtx_     = ctx;
        eventEngine_    = engine;
        eventTemplates_ = templates;
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

    // Task 7.3b: the society snapshots (the whitelist + the events + the
    // signals ring) — the Justice/Events/Map panels' data paths go live.
    const harrowmere::views::JusticeView &justice() const override
    {
        return justiceView_;
    }

    const harrowmere::views::EventsView &events() const override
    {
        return eventsView_;
    }

    const harrowmere::views::UiSignals &signals() const override
    {
        return signalsView_;
    }

    // bindRuntime — the frame-coupled state the snapshots read. Task 2.8 adds
    // the player's STORE sources (the CharacterStore + the creation record's
    // registry home): the fill reads through the store, never a cached
    // Character* (§4.2 rule 1's lifetime rule).
    void bindRuntime(
        const world::WorldStore &world,
        const core::WorldClock &clock,
        const player::PlayerController &player,
        std::uint64_t tickNumber,
        const character::CharacterStore &characters,
        const data::DataRegistry &data
    )
    {
        world_      = &world;
        clock_      = &clock;
        player_     = &player;
        tickNumber_ = tickNumber;
        characters_ = &characters;
        data_       = &data;
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
                view.currentRegion.id           = static_cast<std::uint32_t>(region->id);
                view.currentRegion.manaRichness = region->manaRichness;
                view.season.season              = publishSeason(world::seasonAt(clock_->worldTime));
                view.season.dayOfSeason
                    = (clock_->worldTime % world::seasonLengthGameSeconds()) / core::DAY_LENGTH_SECONDS;
                view.weather.kind = publishWeather(world::weatherFromKind(region->weather.kind));
            }
        }
        const territoryId territoryUnderfoot = world_->territoryAt(player_->position);
        view.currentTerritory.id             = territoryUnderfoot;
        if (const world::Territory *territory = world_->territory(territoryUnderfoot); territory != nullptr)
        {
            view.currentTerritory.type      = publishTerritoryType(territory->type);
            view.currentTerritory.safe      = territory->safe;
            view.currentTerritory.lawless   = territory->lawless;
            view.currentTerritory.lawSetKey = world_->lawsAt(territoryUnderfoot).key;
        }
        view.tickNumber = tickNumber_;
        worldView_      = std::move(view);

        // Task 2.8: the PlayerView fill is ONE tested free function
        // (bridge/player_view.hpp); rebuildViews is its thin caller - the
        // snapshot reads through the store (const refs fetched fresh this
        // frame), never a cached Character* (§4.2 rule 1). No player in-world
        // (main menu / pre-start) keeps the zero view: a field with no data
        // keeps its zero, never a made-up value.
        if (characters_ == nullptr || data_ == nullptr)
        {
            harrowmere::views::PlayerView pv{}; // not wired: the Phase-1 slice only
            pv.position.x = player_->position.x;
            pv.position.y = player_->position.y;
            pv.position.z = player_->position.z;
            pv.worldTime  = clock_->worldTime;
            pv.realTime   = clock_->realTime;
            pv.afk        = player_->afk;
            playerView_   = std::move(pv);
            return;
        }
        if (characters_->playerCount() == 0)
        {
            playerView_ = harrowmere::views::PlayerView{}; // no player: the zero view (never a made-up field)
            return;
        }
        const characterId playerId = characters_->playerId();
        playerView_                = bridge::buildPlayerView(
            characters_->get(playerId),
            characters_->playerCreation(),
            *data_,
            player_->position,
            clock_->worldTime,
            clock_->realTime,
            player_->afk
        );

        // Task 7.3b: the society snapshots (the Justice/Events/Map panels'
        // data paths) — the same tested free functions the whitelist test
        // exercises; rebuildViews is the thin caller. The signals ring is
        // the live capture (the sink appends as the signals fire); the
        // zero-value rule holds: no justice state, no made-up rows.
        if (societyCtx_ != nullptr)
        {
            if (societyCtx_->worldStore != nullptr)
            {
                worldView_.territories = bridge::buildTerritoryTree(*societyCtx_->worldStore);
            }
            justiceView_ = bridge::buildJusticeView(*societyCtx_, playerId);
            if (eventEngine_ != nullptr && eventTemplates_ != nullptr)
            {
                eventsView_ = bridge::buildEventsView(*eventEngine_, *eventTemplates_, playerId);
            }
        }
    }

private:
    const world::WorldStore *world_              = nullptr;
    const core::WorldClock *clock_               = nullptr;
    const player::PlayerController *player_      = nullptr;
    const character::CharacterStore *characters_ = nullptr; // task 2.8: the fill's store source
    const data::DataRegistry *data_              = nullptr; // task 2.8: the fill's registry source
    player::IntentQueue *queue_                  = nullptr;
    std::uint64_t tickNumber_                    = 0;

    // Task 7.3b: the society fills' sources + their snapshots.
    WorldContext *societyCtx_                         = nullptr;
    events::EventEngine *eventEngine_                 = nullptr;
    const events::EventTemplateTable *eventTemplates_ = nullptr;

    harrowmere::views::WorldView worldView_{};
    harrowmere::views::PlayerView playerView_{};
    harrowmere::views::JusticeView justiceView_{};
    harrowmere::views::EventsView eventsView_{};
    harrowmere::views::UiSignals signalsView_{}; // the sink's live ring (the same object)
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
        // Required capabilities MUST mirror mod.toml's `capabilities`: the
        // kernel's demand closure is seeded by the PACKAGE manifest for mods,
        // and these two names are what bring the lighting provider into the
        // run (see mod.toml for the full why). Lockstep is the file's own
        // convention - keep the two files identical on every edit.
        manifest_.requiredCapabilities = {"logging", "scene", "input", "lighting", "pbr_direct_diffuse"};
        manifest_.optionalCapabilities = {"physics", "audio", "animation", "asset"};
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

    void construct(
        engine::ComponentContext &context
    ) override
    {
        // The logging capability is the graph's root dependency; every later
        // system logs through the wrapper task 0.5 pins on top of this sink.
        logging_ = &context.acquireCapability<engine::LoggerCapability>();
        harrowmere::log::install(logging_);

        // The scene registry (required): the 7.2P1 bridge's sink.
        scene_ = &context.acquireCapability<engine::SceneCapability>();

        // The event bus: the two lane subscriptions bind here (start()).
        bus_ = &context.eventBus();
        busSimEvents_.bind(bus_); // the 0.7 justice publications forward here (4.x)

        // The lighting provider (the bridge's sun): the manifest demands
        // "lighting" + "pbr_direct_diffuse" (see mod.toml - optional could
        // never pull the provider, kernel demand-closure semantics), so the
        // probe succeeds in every assembly that ships the game's module set.
        // The probe remains the degrade-if-absent guard: a custom assembly
        // that trims the pbr module loses the sun, never the run.
        if (context.isCapabilityAvailable("lighting"))
        {
            lighting_ = static_cast<engine::LightingCapability *>(context.acquireCapability("lighting"));
        }
        // Match the kernel's disabled-mod filtering during our later data
        // package pass. ComponentContext exposes the assembly's [engine]
        // array as a typed read; retain the copied ids rather than this
        // construction-only context.
        disabledModIds_ = context.configurationStringArrayInSection("engine", "disabled_mods");

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
        const std::filesystem::path executableDir = engine::executableDirectory();
        const std::filesystem::path coreDataDir(config_.coreDataDirectory(executableDir.string()));
        const std::filesystem::path modsDirectory   = executableDir / config_.modDirectory();
        const std::vector<std::string> disabledMods = disabledModIds_;

        // Data packages are discovered by the same manifest rule as the
        // kernel. The base package is always first; active external packages
        // are merged by manifest id ascending (D-MOD-2), never filesystem
        // iteration order. Script and native packages may both ship data/.
        std::string discoveryReport;
        const std::vector<engine::DiscoveredModPackage> discovered
            = engine::discoverModPackages(modsDirectory, disabledMods, discoveryReport);
        if (!discoveryReport.empty())
        {
            HARROWMERE_LOG_WARN(sim, "mod discovery reported package issues:\n%s", discoveryReport.c_str());
        }
        std::vector<data::DataModPackage> dataPackages;
        dataPackages.reserve(discovered.size());
        for (const engine::DiscoveredModPackage &package : discovered)
        {
            if (package.manifest.id == "harrowmere_core")
                continue;
            if (!std::filesystem::is_directory(package.manifest.directory / "data"))
                continue;
            dataPackages.push_back({package.manifest.id, package.manifest.directory});
        }

#ifdef _WIN32
        const auto processId = static_cast<unsigned long long>(_getpid());
#else
        const auto processId = static_cast<unsigned long long>(getpid());
#endif
        const std::size_t activeDataModCount = dataPackages.size();
        mergedDataDirectory_
            = std::filesystem::temp_directory_path() / ("harrowmere_data_" + std::to_string(processId));
        data::DataErrors errors;
        if (!data::materializeMergedData(coreDataDir, std::move(dataPackages), mergedDataDirectory_, errors)
            || !registry_.load(mergedDataDirectory_.string(), errors) || !errors.empty())
        {
            // Fail startup with file + pointer + expected. No partially
            // merged snapshot is committed, and the source packages remain
            // untouched; report the precise row from the staging/overlay path.
            std::string message = "game data failed validation";
            if (!errors.empty())
                message = errors.front().toString();
            HARROWMERE_LOG_ERROR(sim, "%s", message.c_str());
            std::error_code cleanupError;
            std::filesystem::remove_all(mergedDataDirectory_, cleanupError);
            mergedDataDirectory_.clear();
            throw std::runtime_error("harrowmere_core: " + message);
        }
        dataDirectory_ = mergedDataDirectory_.string();
        HARROWMERE_LOG_INFO(
            sim,
            "game data validated: %zu species, %zu items, %zu law sets, core=%s, active_data_mods=%zu",
            registry_.speciesCount(),
            registry_.itemCount(),
            registry_.lawSetCount(),
            coreDataDir.string().c_str(),
            activeDataModCount
        );

        // World seed: 0 = random-at-first-launch-and-persisted (task 7.1,
        // Phase 4). Until persistence lands, seed 0 stays the deterministic
        // seed 0 - two runs with the same config must agree byte-for-byte.
        std::uint64_t seed = config_.seed();
        if (seed == 0)
        {
            HARROWMERE_LOG_INFO(
                sim,
                "seed=0: the first-launch random pick arrives with save persistence (7.1); using the deterministic "
                "seed 0"
            );
        }

        // The world from the seed (1.5): regions, tree, safe zones, faction
        // seeds, the population PLAN, and the FNV-1a summary hash (logged at
        // INFO inside - the exit checklist's determinism evidence).
        world_ = std::make_unique<world::WorldStore>(registry_.defaultLaws());
        world::NpcSeedOut npcs;
        genResult_ = world::generateWorld(seed, registry_, *world_, npcs);
        npcPlan_   = std::move(npcs); // consumed verbatim by 3.4's spawn path (Phase 2)

        // Clocks and RNG (0.3): the fixed-update lane drives both.
        clock_.timeScale = config_.timeScale();
        clock_.tickRate  = config_.tickRate();
        // The world epoch opens at 10:00 game time, not midnight: the 7.2P1
        // sun seam maps midnight to intensity 0 (computeSun clamps the night
        // to zero), so a 0-seeded worldTime staged the whole first session in
        // the dark - the first Windows run rendered exactly that (black
        // ground, 2026-10-30). Mid-morning spawns in full daylight with long
        // shadows; the constant is deterministic (R6: every peer seeds the
        // same value), and 7.1's saves persist worldTime from Phase 4 on.
        clock_.worldTime = 10u * 3600u;
        rng_.seedAll(seed);

        // The two lane subscriptions (the fixed-update lane IS the sim's
        // heartbeat, D-ARCH-3; the frame lane carries the view rebuild and
        // the 7.2P1 bridge).
        subscribeLanes();

        // The player spawns at the beginner kingdom's safe village (1.5
        // stage 4; the design rule: safe zones are spawn points).
        player_.position = genResult_.playerSpawn;

        // ---- the runtime capability: views snapshot this state; submit() queues --
        sim_.bindQueue(&intentQueue_);
        // Task 7.3b: the society fills' sources + the signals ring (the
        // Justice/Events/Map panels' data paths; the sink mirrors the
        // observable signals into the capability's UiSignals view).
        sim_.bindSocietySources(&simCtx_, &eventEngine_, &registry_.eventTemplates());
        busSimEvents_.bindSignalRing(&sim_.signalRing());

        // The 7.2P1 scene bridge over the runtime state.
        bridgeWorld_
            = std::make_unique<StoreBridgeWorldView>(*world_, world_->region(world::regionIdAt(player_.position)));
        bridgeClock_ = std::make_unique<ClockBridgeView>(clock_);
        bridgeSink_  = std::make_unique<EngineSceneSink>(scene_, lighting_);
        // The 7.2 store view (the recipe in store_adapters.hpp): characters +
        // corpses + the NPC tier reads, over the stores this mod owns.
        storeCharacterView_ = std::make_unique<bridge::StoreCharacterView>(characters_, registry_, player_.position);
        storeCharacterView_->setCorpseProvider([this] {
            // The present, un-decayed corpses (3.3's discovery flip rides
            // the record; 4.1's delayed reports read the same rows).
            std::vector<bridge::CharactersViewForBridge::CorpseDesc> out;
            for (const auto &[victim, corpse] : corpses_.all())
            {
                if (simCtx_.now - corpse.deathTime < CORPSE_DECAY_DAYS * core::DAY_LENGTH_SECONDS)
                {
                    out.push_back({victim, corpse.pos, corpse.discovered});
                }
            }
            return out;
        });
        storeCharacterView_->setNpcTierProvider(
            [this](characterId id) -> std::uint8_t {
                return npcs_.contains(id) ? static_cast<std::uint8_t>(npcs_.get(id).tier)
                                          : static_cast<std::uint8_t>(bridge::kTierFar);
            },
            [this](characterId id) {
                return npcs_.contains(id)
                    && (npcs_.get(id).migrating || npcs_.get(id).currentIntent.kind != NpcIntentKind::None);
            }
        );
        // ---- Phase 5: build the terrain field from the worldgen seed + climate grid ----
        // D-IMG-1: height is presentation. The TerrainField is built once from
        // the seed + region climate grid (1.6.1's output) and passed to the
        // bridge. The bridge publishes its meshGrid() once on the first frame
        // via sink_.spawnTerrainMesh() — the production adapter converts it to
        // a triangle mesh and registers it with the scene registry.
        std::vector<world::Climate> climatesVec(static_cast<std::size_t>(REGION_COUNT), world::Climate::TEMPERATE);
        for (const auto &[id, region] : world_->regions())
        {
            if (id < REGION_COUNT)
                climatesVec[id] = region.climate;
        }
        terrainField_ = std::make_unique<bridge::TerrainField>(seed, climatesVec);

        // Phase 5: build the scatter field (vegetation, rocks, settlement props).
        // D-IMG-2: scatter is data + seed. The ScatterField is built from the
        // same seed + climate grid + terrain field + scatter tables.
        bridge::ScatterTables scatterTables;
        if (bridge::loadScatterTables(std::filesystem::path(dataDirectory_), scatterTables))
        {
            scatterField_ = std::make_unique<bridge::ScatterField>(seed, climatesVec, *terrainField_, scatterTables);
        }

        // Phase 5: build the borders field (water + territory ribbons).
        bordersField_ = std::make_unique<bridge::BordersField>();
        bordersField_->setTerrain(terrainField_.get());
        bordersField_->setWaterExtent(harrowmere::TERRAIN_WORLD_EXTENT_M);
        // Populate border ribbons from the territory tree.
        for (const auto &[id, territory] : world_->territories())
        {
            if (territory.type == TerritoryType::KINGDOM || territory.type == TerritoryType::EMPIRE)
                continue;
            bridge::BorderRibbon r;
            r.id          = id;
            r.type        = territory.type;
            r.owner       = 0;
            r.safe        = territory.safe;
            const auto rc = world::regionCenter(territory.region);
            r.polygon     = bridge::regionBoxPolygon(rc, static_cast<double>(REGION_SIZE_M) * 0.5);
            bordersField_->upsertRibbon(r);
        }

        // Phase 5: when terrain is present, suppress the Phase-1 debug markers
        // (territory boxes + safe-zone discs). They were designed for the flat-
        // plane debug view; with the terrain mesh they clutter the scene. The
        // territory data is still available to the sim through WorldStore — only
        // the bridge's visual markers are suppressed.
        if (terrainField_)
        {
            bridgeWorld_->setShowMarkers(false);
        }
        bridge_ = std::make_unique<bridge::SceneBridge>(
            *bridgeWorld_, *bridgeClock_, *bridgeSink_, storeCharacterView_.get(), terrainField_.get()
        );

        //        // Phase 5: publish scatter placements as scene objects. Each placement
        //        // becomes a box/sphere/cone primitive at the terrain height. The cap
        //        // is A-152 (12,000) but the scene registry's max_objects is 128
        //        // (A-130) — publish only the nearest N to the player so the registry
        //        // doesn't overflow.
        //        if (scatterField_ && scene_ != nullptr)
        //        {
        //            const auto &placements = scatterField_->placements();
        //            const auto playerPos = player_.position;
        //            // Sort by distance to player, take the nearest 40 (the registry
        //            // has 128 slots, ~69 used by Phase-1 markers, leaving ~59).
        //            std::vector<std::pair<double, std::size_t>> byDist;
        //            byDist.reserve(placements.size());
        //            for (std::size_t i = 0; i < placements.size(); ++i)
        //            {
        //                const auto &p = placements[i];
        //                const double dx = p.position.x - playerPos.x;
        //                const double dz = p.position.z - playerPos.z;
        //                byDist.emplace_back(dx * dx + dz * dz, i);
        //            }
        //            std::sort(byDist.begin(), byDist.end());
        //            const std::size_t scatterCap = 0; // disabled: dark scatter objects looked like floating black
        //            cubes for (std::size_t i = 0; i < scatterCap; ++i)
        //            {
        //                const auto &p = placements[byDist[i].second];
        //                // Natural colors per prop type: trees dark green, boulders
        //                // gray, houses warm brown, shrubs olive, dead trees dark brown.
        //                std::array<float, 3> propColor = {0.2f, 0.35f, 0.12f}; // default dark green
        //                if (p.propIndex < 5)
        //                {
        //                    switch (p.propIndex)
        //                    {
        //                        case 0: propColor = {0.12f, 0.30f, 0.08f}; break; // pine: dark forest green
        //                        case 1: propColor = {0.38f, 0.36f, 0.32f}; break; // boulder: granite gray
        //                        case 2: propColor = {0.42f, 0.28f, 0.15f}; break; // house: warm timber brown
        //                        case 3: propColor = {0.25f, 0.38f, 0.15f}; break; // shrub: olive green
        //                        case 4: propColor = {0.30f, 0.22f, 0.12f}; break; // dead_tree: dark brown
        //                    }
        //                }
        //                bridgeSink_->spawnBox(
        //                    p.position,
        //                    static_cast<float>(p.scaleM * 0.5),
        //                    static_cast<float>(p.scaleM),
        //                    propColor
        //                );
        //            }
        // ---- the 3.1 sim wiring: stores, adapters, context, steps ----------
        wireSimulation(seed);

        HARROWMERE_LOG_INFO(
            sim,
            "world ready: player spawn (%.1f, %.1f, %.1f) in territory %llu; population %zu; fixed-update lane bound",
            player_.position.x,
            player_.position.y,
            player_.position.z,
            static_cast<unsigned long long>(genResult_.beginnerTerritory),
            characters_.population()
        );
    }

    void stop() override
    {
        // Reverse of start: subscriptions first (no in-flight dispatch lands
        // on tearing-down state), then the systems, then the stores.
        fixedUpdateSubscription_.release();
        frameSubscription_.release();
        playerSystem_.reset();
        lifecycleSystem_.reset();
        npcTickSystem_.reset();
        roleDutiesSystem_.reset();
        lodSystem_.reset();
        economySystem_.reset();
        combatSystem_.reset();
        corpseDecaySystem_.reset();
        aftermathSystem_.reset();
        storeCharacterView_.reset();
        lodSource_.reset();
        bagSource_.reset();
        worldQueries_.reset();
        bridge_.reset();
        bridgeSink_.reset();
        bridgeClock_.reset();
        bridgeWorld_.reset();
        sim_.bindQueue(nullptr);
        npcPlan_.clear();
        genResult_ = world::WorldGenResult{};
        world_.reset();
        if (!mergedDataDirectory_.empty())
        {
            std::error_code cleanupError;
            std::filesystem::remove_all(mergedDataDirectory_, cleanupError);
            mergedDataDirectory_.clear();
            dataDirectory_.clear();
        }
        HARROWMERE_LOG_INFO(sim, "harrowmere_core stopping (state flushed)");
        harrowmere::log::uninstall();
    }

    // The pull-based provider hook: the kernel asks at acquisition time,
    // every time - nothing is registered anywhere between the phases.
    void *capabilityPointerFor(
        const std::string &capabilityName
    ) override
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
        engine::EventChannel<engine::EngineFixedUpdateEvent> &fixedChannel
            = bus_->channel<engine::EngineFixedUpdateEvent>();
        fixedUpdateSubscription_ = engine::EventSubscription(
            bus_,
            typeid(engine::EngineFixedUpdateEvent),
            fixedChannel.subscribe<HarrowmereCoreMod, &HarrowmereCoreMod::onFixedUpdate>(this)
        );

        engine::EventChannel<engine::EngineFrameEvent> &frameChannel = bus_->channel<engine::EngineFrameEvent>();
        frameSubscription_                                           = engine::EventSubscription(
            bus_,
            typeid(engine::EngineFrameEvent),
            frameChannel.subscribe<HarrowmereCoreMod, &HarrowmereCoreMod::onFrame>(this)
        );
    }

private:
    // ---- 3.1's wiring: the WorldContext over the stores, the player as a
    // Character (D-STORE-1: one unified store), the population plan as living
    // NPCs (D-WGEN-2 via 3.4's bootstrapPopulation), and every system at its
    // pinned step (§4.4's table; registration order within a step is the
    // execution order).
    void wireSimulation(
        std::uint64_t /*seed*/
    )
    {
        // Task 2.8 (the playtest-review fix): adopt worldgen's character-id
        // counter and spawn point BEFORE the first create - the store's own
        // contract ("adoption happens before the first create, never mid-run").
        // Without this the id generator draws from its FIRST_ID default, the
        // player takes id 1, and the first plan seed (worldgen assigns plan ids
        // from 1 in strict plan order) collides: every new game logged
        // `createNpc rejected: character 1 already exists` and spawned the
        // world one person short. Plan ids ARE identity (R10: never reused) -
        // the player moves past the plan (adoptWorldGen hands plan_end + 1),
        // never the plan around the player. This also gives the sim-side
        // player record its real spawn position (createPlayer stamps
        // body.position = playerSpawn_; the zero default previously rode until
        // the first controller sync overwrote it).
        characters_.adoptWorldGen(genResult_.nextCharacterId, genResult_.playerSpawn);

        // The player IS a Character (species 0, level 1, A-41 starter purse):
        // the doc struct mirrors the controller's transform each tick.
        playerCharacterId_ = characters_.createPlayer(PlayerCreation{"wanderer", {}, ""});

        // Goods (3.7): the plugin's 0.4 adapter loads goods.json into the
        // economy's table (perCapitaNeed rides the SCALE grid).
        loadGoods();

        // Recipes (2.4): loud startup like every 0.4 load.
        if (std::vector<data::DataError> recipeErrors;
            !recipes_.load((std::filesystem::path(dataDirectory_) / "recipes.json").string(), registry_, recipeErrors)
            || !recipeErrors.empty())
        {
            const std::string detail
                = recipeErrors.empty() ? "unknown recipes.json error" : recipeErrors.front().toString();
            throw std::runtime_error("harrowmere_core: recipes.json failed: " + detail);
        }

        // Quests (6.4): loud startup like every 0.4 load. The rows bind the
        // event-templates stage (the cross-check the loader leaves to the
        // boot order: an unknown template key is a startup error — a quest
        // that cannot start is broken content). The content checklist rides
        // as WARNINGS (the editorial rule); the giver dialog that hands the
        // quests out is Phase 4's claim surface (6.4's Parallel row).
        if (std::vector<data::DataError> questErrors;
            !quests::loadQuests((std::filesystem::path(dataDirectory_) / "quests.json").string(), quests_, questErrors)
            || !questErrors.empty())
        {
            const std::string detail
                = questErrors.empty() ? "unknown quests.json error" : questErrors.front().toString();
            throw std::runtime_error("harrowmere_core: quests.json failed: " + detail);
        }
        for (const auto &[questId, quest] : quests_.rows())
        {
            (void)questId;
            if (registry_.eventTemplates().byKey(quest.eventTemplateKey) == nullptr)
            {
                throw std::runtime_error(
                    "harrowmere_core: quest '" + quest.key + "' binds unknown template '" + quest.eventTemplateKey + "'"
                );
            }
            std::vector<std::string> warnings;
            quests::questChecklistWarnings(quest, registry_.eventTemplates().byKey(quest.eventTemplateKey), warnings);
            for (const std::string &warning : warnings)
            {
                HARROWMERE_LOG_WARN(sim, "%s", warning.c_str());
            }
        }

        // The adapters + the context (plain data; pointers bound once - the
        // stores outlive the pipeline).
        worldQueries_ = std::make_unique<RuntimeWorldQueries>(*world_, characters_, npcs_, npcPlan_);
        worldQueries_->bindTemplates(registry_.territoryTemplates());
        worldQueries_->bindSpawn(genResult_.playerSpawn);

        // Task 3.9: the perception seam goes live - sightings + witness
        // candidates over 1.6's grid and 2.0's store (the phase file's "the
        // perceive→report interface (stubbed empty in Phase 2)" is now real;
        // 4.1 consumes it, no justice-side reimplementation). The per-tick
        // worldTime rides the clock step's existing bind hook below.
        perceptionSink_.bind(&grid_, &characters_, &npcs_, &registry_);
        perceptionSink_.bindAppearanceHistory(&appearanceHistory_);

        bagSource_              = std::make_unique<RuntimeBagSource>(bags_);
        lodSource_              = std::make_unique<RuntimeLodSource>(grid_);
        simCtx_                 = WorldContext{};
        simCtx_.now             = clock_.worldTime;
        simCtx_.realTimeSeconds = clock_.realTime;
        simCtx_.tickGameSeconds = config_.timeScale() / config_.tickRate();
        simCtx_.world           = worldQueries_.get();
        simCtx_.factions        = &factionQueries_;
        simCtx_.wars            = &warStore_;      // 6.2: the real registry (was the null casus-belli stub)
        simCtx_.justice         = &justiceSystem_; // task 4.1: the real justice sink (was the null default)
        justiceSystem_.bind(&simCtx_);
        simCtx_.perception = &perceptionSink_;     // task 3.9: the real perceive seam (was the empty stub)
        simCtx_.events     = &contributionRouter_; // task 5.3: the real contribution router (was the null stub)
        // Task 4.5: the real suspicion seams (the 3.7 sell receipts read the
        // vendor tiers; the 2.6 A-25 signal lands in the observers' memories).
        suspicionPriceModifier_.bind(&simCtx_);
        suspicionSignalSink_.bind(&simCtx_);
        simCtx_.priceModifier = &suspicionPriceModifier_;
        simCtx_.suspicion     = &suspicionSignalSink_;
        simCtx_.signals       = &signals_;
        simCtx_.bags          = bagSource_.get();
        simCtx_.characters    = &characters_;
        simCtx_.npcs          = &npcs_;
        simCtx_.economy       = &economy_;
        simCtx_.corpses       = &corpses_;
        simCtx_.roles         = &roles_;
        simCtx_.data          = &registry_;
        simCtx_.itemInstances = &itemInstances_;
        simCtx_.scheduler     = &scheduler_;
        simCtx_.lod           = &lod_;
        simCtx_.combatQueue   = &combatQueue_;
        simCtx_.rng           = &rng_;
        // The 2.6 appearance history (task 3.9 binds it as the appearance-VERSION
        // source; 2.6's A-25 frequent-change tracking goes live with it - the
        // suspicion seam itself stays null until 4.5 lands, D-SINK-1). Bound
        // here with the rest of the context: wireSimulation is the one place
        // simCtx_ is rebuilt, so every seam binding lives in this block.
        simCtx_.appearanceHistory = &appearanceHistory_;
        // Task 4.x: the justice state + the doc walks + the 0.7 publication
        // seam + the player's act surface (4.1's scorePhysical marker read).
        simCtx_.worldStore    = world_.get();
        simCtx_.justiceStore  = &justiceStore_;
        simCtx_.simEvents     = &busSimEvents_;
        simCtx_.playerActions = &player_.actions;
        // Tasks 4.5-4.7/5.1: the spatial grid (SpawnTargets inserts the
        // spawned ids) + the event engine as the spawner/exclusion provider
        // (the MANHUNT spawn and 4.7's smuggling rule ride the seams).
        simCtx_.grid           = &grid_;
        simCtx_.eventSpawner   = &eventEngine_;
        simCtx_.eventExclusion = &eventEngine_;
        // Task 5.5: the engine also serves the world-effect reads (the
        // economy's PriceBias consumer; the weather pin's region listing).
        simCtx_.eventEffects = &eventEngine_;
        eventEngine_.bind(&registry_.eventTemplates());
        eventEngine_.bindContext(&simCtx_);
        // Task 5.3: the real contribution router replaces the Phase-2 null
        // sink (the damage/heal hooks record; the snapshot slice rides
        // step 5 below).
        contributionRouter_.bind(&eventEngine_, &registry_.eventTemplates(), &simCtx_);

        // The population plan becomes living NPCs (D-WGEN-2; 3.4's spawn path),
        // then every character joins the spatial index at its body position.
        bootstrapPopulation(simCtx_);
        for (const auto &[id, character] : characters_.all())
        {
            grid_.insert(id, character.body.position);
        }

        // Task 6.1: the faction adoption — the stage-5 seeds become the
        // runtime Faction records, every NPC joins their kingdom court (the
        // frontier wanderers the free company), and the live adapter replaces
        // the live IFactionQueries (6.2: isAtWar reads the war registry
        // bound below). Task 6.5: the real matchmaking sink binds (the null
        // default kept every event's template difficulty until now).
        factionStore_.adoptWorldGen(genResult_.factions, simCtx_);
        factionQueries_.bind(&factionStore_, &simCtx_);
        matchmakingSystem_.bind(&simCtx_);
        simCtx_.factions    = &factionQueries_;
        simCtx_.matchmaking = &matchmakingSystem_;
        // Tasks 6.2/6.3: the war registry + the claim ledger bind (the
        // real IWarRegistry replaced the null stub at the context line
        // above; the concrete members serve the cross-store reads — the
        // claim gates, the tolerance court walk, the outcome rewiring).
        warStore_.bindFactions(&factionStore_);
        warStore_.bindContext(&simCtx_);
        simCtx_.factionStore = &factionStore_;
        simCtx_.warStore     = &warStore_;
        simCtx_.claims       = &claimStore_;

        // The step table (§4.4; registration order within a step = execution
        // order). The Phase-1 slices move in here - re-registration, not a
        // rewrite (3.8's/3.10's DEVIATIONS.md rows pin exactly this shape).
        clockStepSystem_.clock       = &clock_;
        clockStepSystem_.ctx         = &simCtx_;
        RuntimeWorldQueries *queries = worldQueries_.get();
        clockStepSystem_.onWorldTime = [queries, this](std::uint64_t worldTime) {
            queries->bindWorldTime(worldTime);
            perceptionSink_.bindWorldTime(worldTime); // task 3.9: age/sighting reads stay on the sim clock (R8)
        };
        clockStepSystem_.tickGameSeconds = simCtx_.tickGameSeconds;
        seasonsStepSystem_.world         = world_.get();
        seasonsStepSystem_.clock         = &clock_;
        weatherStepSystem_.world         = world_.get();
        weatherStepSystem_.clock         = &clock_;
        weatherStepSystem_.rng           = &rng_;
        weatherStepSystem_.prevWorldTime = clock_.worldTime;
        worldEffectsTickSystem_.engine   = &eventEngine_; // 5.5: the weather-bias pin rides step 2
        // The step-5 engine systems bind their engine here (the pinned §4.4
        // table: step 5 IS the event engine tick — without these the systems
        // register but no-op, and events never progress or resolve; the 8.3
        // soak harness exposed the gap).
        eventTickSystem_.engine           = &eventEngine_;
        eventSweepSystem_.engine          = &eventEngine_;
        contributionTickSystem_.engine    = &eventEngine_;
        gridSyncSystem_.grid              = &grid_;
        gridSyncSystem_.characters        = &characters_;
        gridSyncSystem_.npcs              = &npcs_;
        gridSyncSystem_.lod               = &lod_;
        gridSyncSystem_.player            = &player_;
        gridSyncSystem_.playerCharacterId = playerCharacterId_;
        gridSyncSystem_.lastSweepDay      = &lastSweepDay_;
        justiceSweepSystem_.lastSweepDay  = &lastJusticeSweepDay_;
        scheduler_.registerSystem(TickStep::Clocks, &clockStepSystem_);
        scheduler_.registerSystem(TickStep::Sweeps, &seasonsStepSystem_);
        scheduler_.registerSystem(TickStep::Weather, &weatherStepSystem_);
        scheduler_.registerSystem(TickStep::Weather, &worldEffectsTickSystem_); // step 2, after the machine (5.5)
        lifecycleSystem_  = std::make_unique<LifecycleSweepSystem>();
        npcTickSystem_    = std::make_unique<NpcTickSystem>();
        roleDutiesSystem_ = std::make_unique<RoleDutiesSystem>();
        lodSystem_        = std::make_unique<LodSystem>(*lodSource_);
        economySystem_    = std::make_unique<EconomySystem>();
        playerSystem_     = std::make_unique<PlayerCommandSystem>(
            CommandServices{
                &intentQueue_,
                &player_,
                &characters_,
                &bags_,
                &itemInstances_,
                &recipes_,
                &consents_,
                &grid_,
                &clock_,
                config_.tickRate() > 0 ? 1.0 / static_cast<double>(config_.tickRate()) : 0.1,
                &eventEngine_,  // 5.1: the Join/Leave owner (7.3b's Events panel paths)
                &factionStore_, // 6.1: Found/Dissolve/SetRelation
                0               // D-JAIL-1's fast-forward stamp (ServeSentence sets it)
            },
            player::nullInteractionResolver(),
            player::InteractionHandlers::empty()
        );
        combatSystem_                          = std::make_unique<CombatResolutionSystem>();
        corpseDecaySystem_                     = std::make_unique<CorpseDecaySystem>();
        aftermathSystem_                       = std::make_unique<AftermathSystem>();
        factionMonthlySystem_.store            = &factionStore_; // 6.1: the monthly drift
        claimSweepSystem_.store                = &claimStore_;   // 6.3: the abandonment sweep
        warTickSystem_.store                   = &warStore_;     // 6.2: the war progression
        claimPresenceSystem_.store             = &claimStore_;   // 6.3: the player's presence stamps
        claimPresenceSystem_.player            = &player_;
        claimPresenceSystem_.playerCharacterId = &playerCharacterId_;
        scheduler_.registerSystem(TickStep::Sweeps, lifecycleSystem_.get()); // step 1: the daily sweep
        scheduler_.registerSystem(TickStep::Sweeps, &justiceSweepSystem_);   // step 1: the justice backstop (4.2)
        scheduler_.registerSystem(TickStep::Sweeps, &eventSweepSystem_);     // step 1: the event cleanup (5.1)
        scheduler_.registerSystem(
            TickStep::Sweeps, &factionMonthlySystem_
        ); // step 1: the faction drift (6.1; lazy month gate)
        scheduler_.registerSystem(
            TickStep::Sweeps, &claimSweepSystem_
        ); // step 1: the claim abandonment sweep (6.3; after the drift)
        scheduler_.registerSystem(TickStep::Events, &eventTickSystem_);        // step 5: the event engine (5.1)
        scheduler_.registerSystem(TickStep::Events, &contributionTickSystem_); // step 5: the snapshot cadence (5.3)
        scheduler_.registerSystem(TickStep::Lod, lodSystem_.get());            // step 3: the active set
        scheduler_.registerSystem(TickStep::Npcs, npcTickSystem_.get());       // step 4: needs/intents/movement
        scheduler_.registerSystem(TickStep::Npcs, roleDutiesSystem_.get());    // step 4: the hourly duty slot
        scheduler_.registerSystem(TickStep::Justice, &justiceStepSystem_);     // step 6: the case ticks (4.2)
        scheduler_.registerSystem(TickStep::Justice, &suspicionTickSystem_);   // step 6: the suspicion pass (4.5)
        scheduler_.registerSystem(TickStep::Justice, &enforcementStepSystem_); // step 6: warrants + lawless (4.6)
        scheduler_.registerSystem(TickStep::Economy, economySystem_.get());
        scheduler_.registerSystem(TickStep::Macro, &warTickSystem_); // step 8: the war progression (6.2)
        scheduler_.registerSystem(TickStep::Player, playerSystem_.get());
        scheduler_.registerSystem(
            TickStep::Player, &claimPresenceSystem_
        ); // step 9, after the controller (the fresh AFK verdict feeds the stamps, 6.3)
        scheduler_.registerSystem(TickStep::Combat, combatSystem_.get());
        scheduler_.registerSystem(TickStep::Aftermath, corpseDecaySystem_.get());
        scheduler_.registerSystem(TickStep::Aftermath, aftermathSystem_.get());
        scheduler_.registerSystem(TickStep::Aftermath, &gridSyncSystem_);

        // The controller's transform seeds the player Character's body.
        characters_.get(playerCharacterId_).body.position = player_.position;
    }

    // goods.json -> EconomyStore::registerGood (the 0.4 adapter shape; loud on
    // schema errors like every data load).
    void loadGoods()
    {
        const auto loaded = data::loadJsonFile((std::filesystem::path(dataDirectory_) / "goods.json").string());
        if (loaded.isErr())
        {
            throw std::runtime_error("harrowmere_core: goods.json failed to parse");
        }
        const data::Json &rows = loaded.value();
        if (!rows.is_array())
        {
            throw std::runtime_error("harrowmere_core: goods.json must be an array of goods");
        }
        for (const auto &row : rows)
        {
            if (!row.contains("id") || !row.contains("key") || !row.contains("baseValue")
                || !row.contains("perCapitaNeed"))
            {
                throw std::runtime_error("harrowmere_core: goods.json row missing id/key/baseValue/perCapitaNeed");
            }
            const double perCapitaNeed = row["perCapitaNeed"].get<double>();
            economy_.registerGood(
                row["id"].get<goodId>(),
                row["key"].get<std::string>().c_str(),
                row["baseValue"].get<std::uint64_t>(),
                static_cast<std::uint64_t>(perCapitaNeed * static_cast<double>(SCALE))
            );
        }
    }

    // ONE runTick: the Scheduler owns the §4.4 order now (D-TICK-1: one
    // EngineFixedUpdateEvent dispatch == one runTick; the engine owns the
    // accumulator and the catch-up clamp).
    void onFixedUpdate(
        const engine::EngineFixedUpdateEvent & /*event*/
    )
    {
        ++tickNumber_;
        scheduler_.runTick(simCtx_);

        // Task 7.3b — D-JAIL-1's fast-forward: ServeSentence stamps the
        // sentence end on the step-9 services; the pinned pipeline runs at
        // max speed until it. The stamp drains (one click, one burst) and a
        // stale stamp can never wedge the run: the loop exits when the
        // sentence ends (isArrested's lazy purge reads the advanced clock)
        // or at the burst bound — a leftover sentence keeps serving in real
        // time and the next click serves the next burst (the as-built note,
        // DEVIATIONS row; a 1801-day sentence needs 26M raw ticks, which no
        // single click may spend).
        if (playerSystem_ != nullptr)
        {
            CommandServices &services = playerSystem_->services();
            const std::uint64_t until = services.fastForwardUntil;
            services.fastForwardUntil = 0;
            if (until != 0 && simCtx_.justiceStore != nullptr && simCtx_.now < until
                && justice::isArrested(*simCtx_.justiceStore, playerCharacterId_, simCtx_.now))
            {
                std::uint64_t ticks = 0;
                while (simCtx_.now < until && ticks < kFastForwardTickBurstMax
                       && justice::isArrested(*simCtx_.justiceStore, playerCharacterId_, simCtx_.now))
                {
                    ++ticks;
                    ++tickNumber_;
                    scheduler_.runTick(simCtx_);
                }
                HARROWMERE_LOG_INFO(
                    sim,
                    "fast-forward served %llu ticks (sentence %s)",
                    static_cast<unsigned long long>(ticks),
                    justice::isArrested(*simCtx_.justiceStore, playerCharacterId_, simCtx_.now) ? "continues" : "served"
                );
            }
        }

        // The bridge's primary-weather anchor follows the player.
        if (bridgeWorld_ != nullptr)
        {
            bridgeWorld_->setPrimaryRegion(world_->region(world::regionIdAt(player_.position)));
        }
    }

    // Step 13's home (§4.4): rebuild the views once per frame, then let the
    // bridge push the world into the scene registry (frame-coupled by design;
    // D-ARCH-3 keeps the sim itself on the fixed lane).
    void onFrame(
        const engine::EngineFrameEvent & /*event*/
    )
    {
        sim_.bindRuntime(*world_, clock_, player_, tickNumber_, characters_, registry_);
        sim_.rebuildViews();
        if (bridge_ != nullptr)
        {
            bridge_->onFrame();
        }
    }

    engine::ComponentManifest manifest_;
    engine::LoggerCapability *logging_    = nullptr;
    engine::SceneCapability *scene_       = nullptr;
    engine::LightingCapability *lighting_ = nullptr;
    engine::EventBus *bus_                = nullptr;

    data::GameConfig config_ = data::GameConfig::withValues(10, 60, 6, 0, "mods");
    std::vector<std::string> disabledModIds_;
    data::DataRegistry registry_;
    std::filesystem::path mergedDataDirectory_; // per-process staging area; package sources are never modified
    std::string dataDirectory_;                 // merged view consumed by goods/recipes/quests loaders

    core::WorldClock clock_{};
    core::SeedRng rng_{};
    std::unique_ptr<world::WorldStore> world_;
    world::WorldGenResult genResult_{};
    world::NpcSeedOut npcPlan_;

    // ---- the Phase-2 sim stores (§4.5: all sim state lives here) ------------
    character::CharacterStore characters_{};
    NpcStore npcs_{}; // 3.4's sidecar records (state.hpp, harrowmere scope)
    EconomyStore economy_{};
    CorpseStore corpses_{};
    RoleRegistry roles_{};
    items::InventoryStore bags_{};
    items::ItemInstanceRegistry itemInstances_{};
    RecipeTable recipes_{};
    quests::QuestTable quests_{}; // 6.4's loaded rows (the bound templates live in registry_)
    combat::ConsentRegistry consents_{};
    world::SpatialGrid grid_{};

    // ---- the seams (D-SINK-1 null defaults; 4.1/5.3 swap the real ones) -----
    justice::JusticeSystem justiceSystem_{}; // task 4.1: the real justice sink (was NullJusticeSink)
    justice::JusticeStore justiceStore_{};   // 4.x: the evidence/warrant/reputation/jail state (§4.5)
    BusSimEventSink busSimEvents_{};         // the 0.7 publications -> the kernel bus channels
    npc::PerceptionSink perceptionSink_{};   // task 3.9: the real perceive seam (store + grid + version source)
    character::AppearanceHistory appearanceHistory_{}; // 2.6's stamps; 3.9's appearance-version source
    // Task 5.3: the real events sink (ContributionRouter, declared with the
    // event engine below) replaced the Phase-2 NullEventsSink.
    // Task 4.5: the real suspicion seams (were the null defaults — receipts
    // read SCALE and the A-25 signal dropped; D-SINK-1).
    justice::SuspicionPriceModifier suspicionPriceModifier_{};
    justice::SuspicionSignalSink suspicionSignalSink_{};
    NullUiSignalSink signals_{};
    factions::FactionStore factionStore_{};     // task 6.1: the real faction store (was the null queries only)
    factions::FactionQueries factionQueries_{}; // task 6.1: the real IFactionQueries adapter (was NullFactionQueries)
    factions::FactionMonthlySystem factionMonthlySystem_{}; // 6.1: the monthly drift (step 1's faction slice)
    factions::WarStore warStore_{};                         // 6.2: the real war registry (replaced the NullWarRegistry
                                                            // casus-belli stub) — also the run's IWarRegistry face
    factions::WarTickSystem warTickSystem_{};               // 6.2: the war progression (step 8, Macro)
    factions::ClaimStore claimStore_{};                     // 6.3: the claim ledger (the presence/abandonment state)
    factions::ClaimSweepSystem claimSweepSystem_{};         // 6.3: the abandonment sweep (step 1, after the drift)
    ClaimPresenceSystem claimPresenceSystem_{};             // 6.3: the player's presence stamps (step 9, after the
                                                            // controller's fresh AFK verdict)
    progression::MatchmakingSystem matchmakingSystem_{};    // 6.5: the real IMatchmakingSink (was the null default)
    // Task 5.1: the event engine (the IEventSpawner + IEventExclusionSink
    // provider; templates come from the registry's committed stage). Task 5.3
    // swaps the Phase-2 null events sink for the real contribution router;
    // task 5.5's weather-bias slice rides step 2.
    events::EventEngine eventEngine_{};
    events::ContributionRouter contributionRouter_{};
    events::ContributionTickSystem contributionTickSystem_{}; // step 5, after the engine tick (5.3)
    events::WorldEffectsTickSystem worldEffectsTickSystem_{}; // step 2, after the weather machine (5.5)
    harrowmere::LodActiveSet lod_{};
    CombatQueue combatQueue_{};

    // ---- the adapters + the context -----------------------------------------
    std::unique_ptr<RuntimeWorldQueries> worldQueries_;
    std::unique_ptr<RuntimeBagSource> bagSource_;
    std::unique_ptr<RuntimeLodSource> lodSource_;
    WorldContext simCtx_{};

    // ---- the 14-step pipeline (3.1): systems owned here, registered once ----
    Scheduler scheduler_{};
    player::PlayerController player_{};
    player::IntentQueue intentQueue_;
    std::unique_ptr<PlayerCommandSystem> playerSystem_;
    std::unique_ptr<LifecycleSweepSystem> lifecycleSystem_;
    ClockStepSystem clockStepSystem_{};
    SeasonsStepSystem seasonsStepSystem_{};
    WeatherStepSystem weatherStepSystem_{};
    GridSyncSystem gridSyncSystem_{};
    JusticeStepSystem justiceStepSystem_{};                  // step 6 (4.2's case ticks)
    justice::SuspicionTickSystem suspicionTickSystem_{};     // step 6, after the case ticks (4.5)
    justice::EnforcementStepSystem enforcementStepSystem_{}; // step 6, after the suspicion ticks (4.6)
    events::EventTickSystem eventTickSystem_{};              // step 5 (5.1's engine tick)
    events::EventSweepSystem eventSweepSystem_{};            // step 1 (5.1's ended-event cleanup)
    DayGateJusticeSweepSystem justiceSweepSystem_{};         // step 1 (4.2's daily backstop)
    std::unique_ptr<NpcTickSystem> npcTickSystem_;
    std::unique_ptr<RoleDutiesSystem> roleDutiesSystem_;
    std::unique_ptr<LodSystem> lodSystem_;
    std::unique_ptr<EconomySystem> economySystem_;
    std::unique_ptr<CombatResolutionSystem> combatSystem_;
    std::unique_ptr<CorpseDecaySystem> corpseDecaySystem_;
    std::unique_ptr<AftermathSystem> aftermathSystem_;
    characterId playerCharacterId_     = 0;
    std::uint64_t tickNumber_          = 0;
    std::uint64_t lastSweepDay_        = 0; // the grid reconciliation's day gate (step 11 tail)
    std::uint64_t lastJusticeSweepDay_ = 0; // the justice backstop's day gate (step 1, 4.2)

    RuntimeSimCapability sim_{};

    std::unique_ptr<bridge::StoreCharacterView> storeCharacterView_;
    std::unique_ptr<StoreBridgeWorldView> bridgeWorld_;
    std::unique_ptr<ClockBridgeView> bridgeClock_;
    std::unique_ptr<EngineSceneSink> bridgeSink_;
    std::unique_ptr<bridge::SceneBridge> bridge_;
    // Phase 5: the presentation fields (terrain, scatter, borders). Built once
    // from the worldgen seed + climate grid; the bridge consumes them.
    std::unique_ptr<bridge::TerrainField> terrainField_;
    std::unique_ptr<bridge::ScatterField> scatterField_;
    std::unique_ptr<bridge::BordersField> bordersField_;

    engine::EventSubscription fixedUpdateSubscription_;
    engine::EventSubscription frameSubscription_;
};

engine::ModInterface *createHarrowmereCoreMod()
{
    return new HarrowmereCoreMod();
}

void destroyHarrowmereCoreMod(
    engine::ModInterface *mod
)
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
