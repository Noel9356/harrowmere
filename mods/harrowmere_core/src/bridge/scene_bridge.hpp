// Task 7.2P1 - Static scene bridge (Phase-1 slice of card 7.2).
//
// A pure bridge: reads the static world (territories, safe zones, day phase,
// weather) and maps it into the engine's scene registry once per
// EngineFrameEvent. No gameplay logic, no sim-state mutation
// (D-ARCH-3, common.md section 4.4 step 13).
//
// What ships in this slice:
//   * One tall thin box per territory center, height by type
//     (village 5 m / city 10 m / kingdom 15 m / empire 20 m - task spec).
//   * A flat tinted disc per safe zone.
//   * Sun direction + intensity from WorldClock::dayPhase01(), repushed only
//     on a >= 1% phase delta (avoids per-frame registry traffic).
//   * Sky radiance tint per weather state, table A-117.
//
// What does NOT ship here (Phase 2, card 7.2): characters, corpses, loot,
// animation, presentation-radius culling. A-118 is declared at the seam so
// the Phase-2 bridge extends this contract rather than rewriting it.
//
// Subscriptions: the plugin (harrowmere_plugin.cpp) subscribes this bridge to
// EngineFrameEvent with a priority LOWER than harrowmere_ui's overlay
// subscription so the UI draws on top (common.md section 4.0: "higher
// dispatches first"). The bridge exposes onFrame() for the subscription
// handler to call; it does not depend on the engine's EventBus type, which
// keeps it unit-testable without engine headers.
//
// Constants: A-117 (sky tints), A-118 (presentation radius, seam-only),
// A-119 (chunk budget). Territory-marker heights/width and the disc geometry
// are pinned in the 7.2P1 task spec. Per DoD section 1.5 every numeric
// constant belongs in core/constants.hpp (append-only per task 0.2); they
// are kept local to this header so the 7.2P1 deliverable is drop-in and
// does not modify an existing constants.hpp. Merge on integrate.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include "core/types.hpp"    // Position, territoryId, regionId (task 0.2)
#include "core/enums.hpp"    // TerritoryType (task 1.1)
#include "world/seasons.hpp" // WeatherState (task 3.8)

namespace harrowmere {
namespace bridge {

// ---- Pinned constants (Appendix A rows; see file-header note on placement) -

// A-117: sky tint per weather state. CLEAR is the base; RAIN/STORM/SNOW are
// derived by multiply (rain x0.7, storm x0.45, snow x1.2 blue-shift).
// Linear-RGB, components in [0, 1].
inline constexpr std::array<float, 3> SKY_TINT_CLEAR = {0.22f, 0.30f, 0.48f}; // A-117
inline constexpr float SKY_TINT_RAIN_FACTOR  = 0.7f;  // A-117
inline constexpr float SKY_TINT_STORM_FACTOR = 0.45f; // A-117
inline constexpr float SKY_TINT_SNOW_FACTOR  = 1.2f;  // A-117

// A-118: render never sees FAR NPCs. Phase-2 bridge consumes this for NPC
// culling; declared here so the seam is visible from day one. Unused in P1.
inline constexpr double PRESENTATION_RADIUS_M = 300.0; // A-118

// A-119: scene-registry rebuild budget - creations chunked per frame.
inline constexpr std::uint32_t SCENE_CREATE_CHUNK_PER_FRAME = 32; // A-119

// Sun phase update threshold: a >= 1% dayPhase01 delta triggers a
// set_sun_light call. Pinned in task 7.2P1 spec ("updated on >= 1% phase
// change").
inline constexpr double SUN_PHASE_UPDATE_THRESHOLD = 0.01; // task 7.2P1 spec

// Territory marker geometry: one tall thin box per territory center, height
// by type. Pinned in task 7.2P1 spec.
inline constexpr float TERRITORY_MARKER_HALF_WIDTH       = 1.0f;  // thin (7.2P1 spec)
inline constexpr float TERRITORY_MARKER_HEIGHT_VILLAGE   = 5.0f;  // 7.2P1 spec
inline constexpr float TERRITORY_MARKER_HEIGHT_CITY      = 10.0f; // 7.2P1 spec
inline constexpr float TERRITORY_MARKER_HEIGHT_KINGDOM   = 15.0f; // 7.2P1 spec
inline constexpr float TERRITORY_MARKER_HEIGHT_EMPIRE    = 20.0f; // 7.2P1 spec

// Safe-zone disc geometry: flat tinted disc per safe zone. Radius matches
// D-SHELTER-1's shelter radius (50 m of a building marker) so the disc
// visually marks the sheltered area of the safe zone.
inline constexpr double SAFE_ZONE_DISC_RADIUS = 50.0; // D-SHELTER-1 (App A.6)
inline constexpr float  SAFE_ZONE_DISC_HEIGHT = 0.1f; // flat (7.2P1 spec)

// Presentation tints (Linear-RGB) for territory markers by type and for the
// safe-zone disc. These are 7.2P1 presentation values NOT yet in Appendix A;
// they should be added via the deviation protocol (section 1.4) or made
// data-driven (data/*.json) on integrate. Height is the primary visual
// indicator; tints are secondary identity.
inline constexpr std::array<float, 3> TINT_VILLAGE = {0.85f, 0.75f, 0.55f}; // 7.2P1 presentation
inline constexpr std::array<float, 3> TINT_CITY    = {0.75f, 0.70f, 0.90f}; // 7.2P1 presentation
inline constexpr std::array<float, 3> TINT_KINGDOM = {0.90f, 0.65f, 0.55f}; // 7.2P1 presentation
inline constexpr std::array<float, 3> TINT_EMPIRE  = {0.95f, 0.60f, 0.40f}; // 7.2P1 presentation
inline constexpr std::array<float, 3> TINT_SAFE    = {0.30f, 0.85f, 0.40f}; // 7.2P1 presentation

// ---- Bridge-side interfaces (decouple the bridge from engine & full world) -

// Scene-registry operations the bridge drives. The engine's SceneCapability
// (acquired via ComponentContext in the plugin) is wrapped by an adapter
// that implements this interface; tests provide a fake. Method names mirror
// the engine's scene_registry vocabulary (spawn_object / set_object_position
// / set_sun_light) per the 7.2P1 spec.
class SceneRegistrySink {
public:
    using Handle = std::uint64_t;
    static constexpr Handle NULL_HANDLE = 0;

    virtual ~SceneRegistrySink() = default;

    // Spawn a box (territory marker) centered at `center` with the given
    // half-width (X/Z), height (Y), and tint. Returns a stable handle.
    virtual Handle spawnBox(const Position& center,
                            float halfWidth,
                            float height,
                            const std::array<float, 3>& tint) = 0;

    // Spawn a flat disc (safe-zone marker) centered at `center` with the
    // given radius, height, and tint. Returns a stable handle.
    virtual Handle spawnDisc(const Position& center,
                             double radius,
                             float height,
                             const std::array<float, 3>& tint) = 0;

    // Update an existing object's position (used when territories move in
    // later phases; P1 calls this only if a territory's center changes
    // between frames, which it does not in P1 - declared for the seam).
    virtual void setObjectPosition(Handle h, const Position& p) = 0;

    // Sun light: direction is a unit vector pointing in the direction light
    // travels (from sun toward scene); intensity is in [0, 1].
    virtual void setSunLight(const std::array<float, 3>& direction,
                             float intensity) = 0;

    // Sky radiance tint (Linear-RGB).
    virtual void setSkyTint(const std::array<float, 3>& tint) = 0;
};

// World view: the territories to render plus the primary weather (drives
// sky tint). The production adapter wraps WorldStore (task 1.4) and computes
// territory centers from the region grid (A-40). Tests provide a fake.
class WorldViewForBridge {
public:
    struct TerritoryDesc {
        territoryId          id;       // R10: opaque id, never reused
        TerritoryType        type;     // drives marker height/tint
        Position             center;   // territory center in world space
        bool                 safe;     // true -> also spawn a disc
    };

    virtual ~WorldViewForBridge() = default;

    // Territories sorted by id ascending (deterministic iteration, R7).
    virtual std::vector<TerritoryDesc> territories() const = 0;

    // Primary weather at the viewing region (camera/player region). Drives
    // the global sky tint. The kind is task 3.8's Weather enum (the ordinal
    // Region::weather.kind carries; weatherFromKind() decodes it).
    virtual world::Weather primaryWeather() const = 0;
};

// Clock view: dayPhase01 in [0, 1) within the game day (0 = midnight,
// 0.25 = sunrise, 0.5 = noon, 0.75 = sunset). Production adapter wraps
// WorldClock (task 0.3).
class ClockViewForBridge {
public:
    virtual ~ClockViewForBridge() = default;
    virtual double dayPhase01() const = 0;
};

// ---- SceneBridge -----------------------------------------------------------

class SceneBridge {
public:
    SceneBridge(WorldViewForBridge& world,
                ClockViewForBridge& clock,
                SceneRegistrySink& sink);
    ~SceneBridge();

    SceneBridge(const SceneBridge&)            = delete;
    SceneBridge& operator=(const SceneBridge&) = delete;

    // Called once per EngineFrameEvent by the plugin's subscription handler.
    // Pure bridge: reads world/clock state, pushes scene objects, never
    // mutates sim state.
    void onFrame();

    // ---- test hooks (zero overhead in release; used by the pinned tests) --
    // Total live scene objects this bridge has created (boxes + discs).
    std::size_t spawnedObjectCount() const;
    // Territories not yet turned into scene objects (waiting for a chunk
    // slot). Asserts the A-119 batching invariant.
    std::size_t pendingCreationCount() const;
    // True iff the last onFrame() pushed a fresh set_sun_light call.
    bool sunWasUpdatedOnLastFrame()    const { return sun_updated_last_frame_; }
    // True iff the last onFrame() pushed a fresh set_sky_tint call.
    bool skyTintWasUpdatedOnLastFrame() const { return sky_tint_updated_last_frame_; }

private:
    void buildPendingQueueIfNeeded();
    void processPendingCreations();        // chunked <= SCENE_CREATE_CHUNK_PER_FRAME
    void updateSunIfPhaseChanged();        // >= SUN_PHASE_UPDATE_THRESHOLD delta
    void updateSkyTintIfWeatherChanged();  // per A-117 table

    float                territoryMarkerHeight(TerritoryType t) const;
    std::array<float, 3> territoryMarkerTint(TerritoryType t) const;
    std::array<float, 3> skyTintFor(world::Weather w) const;

    WorldViewForBridge& world_;
    ClockViewForBridge& clock_;
    SceneRegistrySink&  sink_;

    // R7: deterministic iteration via std::map (territoryId-sorted).
    // One box per territory, one disc per safe zone. Kept across frames so
    // a second onFrame() does not re-spawn.
    std::map<territoryId, SceneRegistrySink::Handle> territory_boxes_;
    std::map<territoryId, SceneRegistrySink::Handle> safe_zone_discs_;

    // Pending creation tasks (built once from world_.territories(), drained
    // in chunks). Ordering is deterministic: territories in id order, and
    // for each territory the box task precedes its disc task.
    struct CreationTask {
        territoryId id;
        bool        is_disc;   // false = box, true = disc
    };
    std::vector<CreationTask> pending_creations_;
    bool pending_queue_built_ = false;

    // Sun update hysteresis: -1.0 forces the first frame to push.
    double last_sun_phase_ = -1.0;
    // Weather-state hysteresis: COUNT forces the first frame to push.
    world::Weather last_sky_weather_ = world::Weather::COUNT;

    bool sun_updated_last_frame_      = false;
    bool sky_tint_updated_last_frame_ = false;
};

} // namespace bridge
} // namespace harrowmere
