// Task 7.2 - Simulation <-> scene bridge (Phase-2 full card; extends the
// 7.2P1 static slice in place).
//
// A pure bridge: reads the world (territories, safe zones, day phase,
// weather, characters, corpses) and maps it into the engine's scene registry
// once per EngineFrameEvent. No gameplay logic, no sim-state mutation
// (D-ARCH-3, common.md section 4.4 step 13).
//
// What ships in the Phase-1 slice (kept verbatim, tests untouched):
//   * One tall thin box per territory center, height by type
//     (village 5 m / city 10 m / kingdom 15 m / empire 20 m - task spec).
//   * A flat tinted disc per safe zone.
//   * Sun direction + intensity from WorldClock::dayPhase01(), repushed only
//     on a >= 1% phase delta (avoids per-frame registry traffic).
//   * Sky radiance tint per weather state, table A-117.
//
// What Phase 2 (this card) adds:
//   * ONE visual per PRESENT character (within A-118's presentation radius of
//     the LOD anchor): a sphere r = 0.8 tinted by species with a faction ring
//     color. Corpses: a flattened gray box at the death position. Territory
//     jail point: a small cage box at the territory jail offset.
//   * The FAR rule: the renderer never sees FAR-statistical NPCs. A FAR-tier
//     character carries NO presence object; a FAR character currently MOVING
//     (migration walk/teleport) shows a transient box 0.6^3 "movement
//     marker" so long-range motion is legible. Reading note (the card's
//     parenthetical compresses two rules): presence spheres are the NEAR/MID
//     tier rule ("sphere r=0.8 for sapient NEAR/MID presence" - every present
//     character gets exactly one visual, sapient or not, per the exit
//     checklist's "one visual per present character"; species color carries
//     the distinction), and the box 0.6^3 markers are the FAR-movement
//     exception ("only characters moving get markers"). Static FAR
//     characters render nothing - "far NPCs never render".
//   * Object churn through handles: map<characterId, handle> /
//     map<victimId, handle>; despawn beyond the presentation radius; create
//     chunking at A-119's 32/frame shared with the static queue so a
//     teleport's rebuild never spikes a frame.
//   * Position sync is batched: set_object_position per moved object per
//     frame; the scene registry coalesces the edits into one GPU refresh.
//
// Subscriptions: the plugin (harrowmere_plugin.cpp) subscribes this bridge to
// EngineFrameEvent with a priority LOWER than harrowmere_ui's overlay
// subscription so the UI draws on top (common.md section 4.0: "higher
// dispatches first"). The bridge exposes onFrame() for the subscription
// handler to call; it does not depend on the engine's EventBus type, which
// keeps it unit-testable without engine headers.
//
// Constants: A-117 (sky tints), A-118 (presentation radius), A-119 (chunk
// budget). Territory-marker heights/width, the disc geometry, the character
// sphere/box geometry and the jail-cage offset are pinned in the 7.2/7.2P1
// task specs. Per DoD section 1.5 every numeric constant belongs in
// core/constants.hpp (append-only per task 0.2); they are kept local to this
// header so the deliverable is drop-in and does not modify an existing
// constants.hpp (the 7.2P1 precedent). Merge on integrate.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

#include "core/types.hpp"     // Position, territoryId, regionId, characterId, speciesId, factionId (task 0.2)
#include "core/enums.hpp"     // TerritoryType (task 1.1)
#include "world/seasons.hpp"  // WeatherState (task 3.8)
#include "bridge/terrain.hpp" // TerrainField (task 5.1.2) — optional, null = Phase-1 flat ground

namespace engine
{
// The rev-34 character-customisation module's published capability (the
// pointer arrives from the plugin's acquire; the header stays out of this
// one - the bridge only stores and forwards the pointer).
class CharacterCustomisationCapability;
}

namespace harrowmere
{
namespace bridge
{
class CharacterLab; // the rev-34 lab (character_lab.hpp) - the bridge owns one

// The LOD tier ordinals the character view carries (npc/state.hpp's unscoped
// LodTier reads NEAR=0, MID=1, FAR=2; the bridge carries the plain integer
// so this header stays independent of the NPC store).
inline constexpr std::uint8_t kTierNear = 0;
inline constexpr std::uint8_t kTierMid  = 1;
inline constexpr std::uint8_t kTierFar  = 2;

// ---- Pinned constants (Appendix A rows; see file-header note on placement) -

// A-117: sky tint per weather state. CLEAR is the base; RAIN/STORM/SNOW are
// derived by multiply (rain x0.7, storm x0.45, snow x1.2 blue-shift).
// Linear-RGB, components in [0, 1].
inline constexpr std::array<float, 3> SKY_TINT_CLEAR = {0.22f, 0.30f, 0.48f}; // A-117
inline constexpr float SKY_TINT_RAIN_FACTOR          = 0.7f;                  // A-117
inline constexpr float SKY_TINT_STORM_FACTOR         = 0.45f;                 // A-117
inline constexpr float SKY_TINT_SNOW_FACTOR          = 1.2f;                  // A-117

// A-118: render never sees FAR NPCs (beyond this radius nothing exists for
// the renderer; the character view guarantees the cut, the bridge re-checks).
inline constexpr double PRESENTATION_RADIUS_M = 300.0; // A-118

// A-119: scene-registry rebuild budget - creations chunked per frame
// (shared by the static queue and the character/corpse queues).
inline constexpr std::uint32_t SCENE_CREATE_CHUNK_PER_FRAME = 32; // A-119

// Sun phase update threshold: a >= 1% dayPhase01 delta triggers a
// set_sun_light call. Pinned in task 7.2P1 spec ("updated on >= 1% phase
// change").
inline constexpr double SUN_PHASE_UPDATE_THRESHOLD = 0.01; // task 7.2P1 spec

// Territory marker geometry: one tall thin box per territory center, height
// by type. Pinned in task 7.2P1 spec.
inline constexpr float TERRITORY_MARKER_HALF_WIDTH     = 1.0f;  // thin (7.2P1 spec)
inline constexpr float TERRITORY_MARKER_HEIGHT_VILLAGE = 5.0f;  // 7.2P1 spec
inline constexpr float TERRITORY_MARKER_HEIGHT_CITY    = 10.0f; // 7.2P1 spec
inline constexpr float TERRITORY_MARKER_HEIGHT_KINGDOM = 15.0f; // 7.2P1 spec
inline constexpr float TERRITORY_MARKER_HEIGHT_EMPIRE  = 20.0f; // 7.2P1 spec

// Safe-zone disc geometry: flat tinted disc per safe zone. Radius matches
// D-SHELTER-1's shelter radius (50 m of a building marker) so the disc
// visually marks the sheltered area of the safe zone.
inline constexpr double SAFE_ZONE_DISC_RADIUS = 50.0; // D-SHELTER-1 (App A.6)
inline constexpr float SAFE_ZONE_DISC_HEIGHT  = 0.1f; // flat (7.2P1 spec)

// ---- Phase-2 character/corpse geometry (task 7.2 spec) -----------------------

// Character presence: one sphere of radius 0.8 per present character,
// resting on the ground plane (center y = radius).
inline constexpr float CHARACTER_SPHERE_RADIUS = 0.8f; // task 7.2 spec

// rev 28 - the organic near body: NEAR/MID characters within this radius of
// the LOD anchor present as the procedural rounded humanoid (kind 3, four
// objects: body + legs + head + hair meshes - the per-part randomized look);
// the rest keep the kind-1 sphere + head LOD. The radius bounds the object
// cost of the crowd (4-5 objects each) to the villagers a street-level
// camera can actually tell apart, while the 300 m presentation radius
// (A-118) stays sphere-priced.
inline constexpr double CHARACTER_DETAIL_RADIUS_M = 42.0;

// The organic mesh vocabulary's variant/scale names (registered in
// registerPropMeshVocabulary from prop_mesh_data_cc0.h). Three body builds
// (slim / regular / stout), two baked scales each (lg 1.80 m, sm 1.58 m) -
// per-NPC height variation without scaling (mesh instances translate +
// rotateY only, the renderer's contract).
inline constexpr const char *kNpcBodyVariants[3][2] = {
    {"npc_body_a_lg", "npc_body_a_sm"},
    {"npc_body_b_lg", "npc_body_b_sm"},
    {"npc_body_c_lg", "npc_body_c_sm"},
};
inline constexpr const char *kNpcLegsVariants[3][2] = {
    {"npc_legs_a_lg", "npc_legs_a_sm"},
    {"npc_legs_b_lg", "npc_legs_b_sm"},
    {"npc_legs_c_lg", "npc_legs_c_sm"},
};
// rev 28 - the baked stride poses (the walk gait). The generator
// (scripts/generate_organic_meshes.py) emits each variant+scale in THREE
// poses: the neutral rest (the tables above) plus _wa/_wb, whole meshes
// with the legs swung around the hip line (knees bent, feet re-planted) and
// the arms counter-swinging around the shoulders (the renderer's mesh
// instances translate + rotateY only, so the gait is a mesh swap, not a
// skeletal animation). Index [pose][variant][scale]:
// pose 0 = walk A (right leg forward), 1 = walk B (mirrored).
inline constexpr const char *kNpcBodyWalkVariants[2][3][2] = {
    {{"npc_body_a_lg_wa", "npc_body_a_sm_wa"},
     {"npc_body_b_lg_wa", "npc_body_b_sm_wa"},
     {"npc_body_c_lg_wa", "npc_body_c_sm_wa"}},
    {{"npc_body_a_lg_wb", "npc_body_a_sm_wb"},
     {"npc_body_b_lg_wb", "npc_body_b_sm_wb"},
     {"npc_body_c_lg_wb", "npc_body_c_sm_wb"}},
};
inline constexpr const char *kNpcLegsWalkVariants[2][3][2] = {
    {{"npc_legs_a_lg_wa", "npc_legs_a_sm_wa"},
     {"npc_legs_b_lg_wa", "npc_legs_b_sm_wa"},
     {"npc_legs_c_lg_wa", "npc_legs_c_sm_wa"}},
    {{"npc_legs_a_lg_wb", "npc_legs_a_sm_wb"},
     {"npc_legs_b_lg_wb", "npc_legs_b_sm_wb"},
     {"npc_legs_c_lg_wb", "npc_legs_c_sm_wb"}},
};
// The gait phase mirrors the A-154 walk bob exactly (character_visuals.cpp:
// phase = traveled / A-57 walk speed * WALK_BOB_HZ), so the stride poses
// swap in step with the bob instead of against it. One stride pose per bob
// half-cycle (0.909 m at the 4 m/s reference walk - a human step length).
inline constexpr double NPC_GAIT_REFERENCE_SPEED_MPS = 4.0; // A-57 mirror
// The pure bucket derivation (test-pinned): pose 0 (rest) when the
// character is not in motion (the sim's moving intent OR this frame's
// measured displacement - the render loop outruns the fixed tick, so a
// displacement-only test would flicker), otherwise walk A/B alternating
// on the bob phase. Exposed for TestBridge_GaitPoseBucket.
std::uint8_t gaitPoseBucket(double traveledDistanceM, bool inMotion) noexcept;
inline constexpr const char *kNpcHeadVariants[3][2] = {
    {"npc_head_a_lg", "npc_head_a_sm"},
    {"npc_head_b_lg", "npc_head_b_sm"},
    {"npc_head_c_lg", "npc_head_c_sm"},
};
// rev 28 - the hair-cap meshes (a rounded shell over the ellipsoid head,
// three styles: short crop / crop + back volume / full cap + long back).
// They replace the analytic hair box, which read as a brick on the rounded
// heads (the owner's "made out of blocks" verdict).
inline constexpr const char *kNpcHairVariants[3][2] = {
    {"npc_hair_a_lg", "npc_hair_a_sm"},
    {"npc_hair_b_lg", "npc_hair_b_sm"},
    {"npc_hair_c_lg", "npc_hair_c_sm"},
};
// The organic mesh sets' total heights (the generator bakes lg 1.80 m / sm
// 1.58 m); the sphere-LOD cross-fade reads them.
inline constexpr float kNpcMeshHeight[2] = {1.80f, 1.58f};
// The organic model's texture sets: smooth plaster reads as woven cloth on
// the torso, the skin set keeps the head's face tone.
inline constexpr float NPC_MESH_UV_SCALE = 0.9f;

// FAR-movement markers: a 0.6^3 box (half extents 0.3) at the mover's
// position - transient, gray.
inline constexpr float FAR_MARKER_BOX_HALF_EXTENT = 0.3f; // task 7.2 spec (0.6^3 box)

// Corpses: a flattened box, gray. Footprint ~= a fallen body (1.2 x 0.5 m
// half extents), height thin. Presentation-only shape (the card pins
// "flattened box, gray"; the dimensions are the 7.2 presentation default,
// same protocol as 7.2P1's disc geometry).
inline constexpr float CORPSE_BOX_HALF_X = 0.6f;
inline constexpr float CORPSE_BOX_HALF_Y = 0.1f; // flattened
inline constexpr float CORPSE_BOX_HALF_Z = 0.25f;

// Jail cage: a small box at the territory's jail offset (the card's "jail
// point -> cage box at the territory jail offset"). The offset is a
// presentation constant (the sim has no jail geometry); 4 m from the
// territory center keeps it inside the settlement but clear of the marker.
//
// Scope note: v1 places the cage only at SAFE-ZONE territories - the jail
// that exists in Phase 2's playable loop is the safe zone's (2.0's spawn
// rule: "safe zones are spawn points"; 4.3's respawn/serving anchors there).
// A cage per every territory would spend A-130's 128-object budget on
// static markers before a single character renders. When 4.3 lands its
// per-territory jailUntil map the adapter extends the set - this file's
// syncJailCages is the only touch point.
inline constexpr float JAIL_CAGE_HALF_EXTENT = 1.0f;
inline constexpr float JAIL_CAGE_OFFSET_M    = 4.0f;

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

// Phase-2 presentation tints: corpses are gray ("corpses flatten and gray"),
// FAR-movement markers a dim gray, the jail cage a cold iron gray.
inline constexpr std::array<float, 3> TINT_CORPSE     = {0.55f, 0.55f, 0.55f}; // task 7.2 ("gray")
inline constexpr std::array<float, 3> TINT_FAR_MARKER = {0.70f, 0.70f, 0.72f};
inline constexpr std::array<float, 3> TINT_JAIL_CAGE  = {0.45f, 0.47f, 0.50f};

// The v1 species color table (task 7.2: "colored by species (species.json
// appearance -> v1 color table)"). Keyed by the speciesId the character
// carries; unknown species fall back to the neutral row. Presentation-only
// palette (the card's v1 table; promote via the deviation protocol if a row
// needs tuning). Species ids per data/species.json: 0 player, 1 human,
// 2 beastkin, 3 taur, 4 elf, 5 vampire, 6 dragon, 7 goo_beast.
struct SpeciesColorRow
{
    speciesId species;
    std::array<float, 3> color;
};

inline constexpr std::array<SpeciesColorRow, 8> SPECIES_COLORS{{
    {0, {0.90f, 0.85f, 0.75f}}, // player: warm parchment
    {1, {0.85f, 0.72f, 0.58f}}, // human: skin-tone earth
    {2, {0.75f, 0.55f, 0.35f}}, // beastkin: fur amber
    {3, {0.65f, 0.50f, 0.45f}}, // taur: hide brown
    {4, {0.60f, 0.80f, 0.60f}}, // elf: spring green
    {5, {0.60f, 0.30f, 0.35f}}, // vampire: dusk red
    {6, {0.85f, 0.35f, 0.25f}}, // dragon: fire scale
    {7, {0.40f, 0.75f, 0.70f}}, // goo_beast: brine teal
}};
inline constexpr std::array<float, 3> SPECIES_COLOR_FALLBACK = {0.80f, 0.80f, 0.80f};

// The faction ring color table (task 7.2: "faction ring color"). V1 ships a
// small deterministic palette: faction id -> color by index; faction 0
// (factionless) renders NO ring (the ring is the faction's identity).
//
// The scene registry has no ring primitive, so the ring is a SECOND sphere
// (a thin shell of radius 0.95 behind the 0.8 body - visible as an edge).
// It is optional at the seam: spawnRing's default is a no-op, the bridge
// tolerates a NULL_HANDLE ring, and the production adapter spawns rings only
// while the registry has headroom (A-130's max_objects budget: the 69
// Phase-1 static objects + the cage/disc/marker set come first). The exit
// checklist's "one visual per present character" counts BODIES; the ring is
// decoration a degraded sink drops first.
inline constexpr std::array<float, 3> FACTION_RING_COLOR_FALLBACK = {0.95f, 0.90f, 0.55f};
inline constexpr float FACTION_RING_RADIUS                        = 0.95f; // shell radius (body 0.8)
// The v1 faction palette (8 slots by id; ids wrap through the table).
inline constexpr std::array<std::array<float, 3>, 8> FACTION_RING_COLORS{{
    {0.90f, 0.30f, 0.30f}, // 1: crimson
    {0.30f, 0.45f, 0.90f}, // 2: cobalt
    {0.25f, 0.70f, 0.35f}, // 3: verdant
    {0.90f, 0.75f, 0.25f}, // 4: gold
    {0.70f, 0.35f, 0.85f}, // 5: violet
    {0.30f, 0.80f, 0.80f}, // 6: cyan
    {0.95f, 0.55f, 0.20f}, // 7: ember
    {0.75f, 0.75f, 0.78f}, // 8: iron
}};

// ---- Bridge-side interfaces (decouple the bridge from engine & full world) -

// Scene-registry operations the bridge drives. The engine's SceneCapability
// (acquired via ComponentContext in the plugin) is wrapped by an adapter
// that implements this interface; tests provide a fake. Method names mirror
// the engine's scene_registry vocabulary (spawn_object / set_object_position
// / set_sun_light) per the 7.2P1 spec.
//
// Phase 2 adds spawnSphere + removeObject with DEFAULT implementations: the
// Phase-1 fakes (test_scene_bridge.cpp) predate them and must keep compiling
// unmodified (the exit checklist's "the Phase-1 suite still passes
// unmodified"). The production adapter overrides both; the defaults are the
// degrade-if-absent contract (a sink without spheres degrades characters to
// no-ops, never a crash).
class SceneRegistrySink
{
public:
    using Handle                        = std::uint64_t;
    static constexpr Handle NULL_HANDLE = 0;

    virtual ~SceneRegistrySink() = default;

    // Spawn a box (territory marker) centered at `center` with the given
    // half-width (X/Z), height (Y), and tint. Returns a stable handle.
    virtual Handle spawnBox(const Position &center, float halfWidth, float height, const std::array<float, 3> &tint)
        = 0;

    // Spawn a flat disc (safe-zone marker) centered at `center` with the
    // given radius, height, and tint. Returns a stable handle.
    virtual Handle spawnDisc(const Position &center, double radius, float height, const std::array<float, 3> &tint) = 0;

    // Phase 2: spawn a sphere (character presence) at `center` with the given
    // radius and tint. Default no-op (NULL_HANDLE) - the Phase-1 contract.
    virtual Handle spawnSphere(
        const Position &center, float radius, const std::array<float, 3> &tint
    )
    {
        (void)center;
        (void)radius;
        (void)tint;
        return NULL_HANDLE;
    }

    // Phase 2: the faction ring shell (a second sphere the sink may drop -
    // see the ring-table comment). Default no-op; a NULL_HANDLE ring is
    // tolerated and never re-spawned.
    virtual Handle spawnRing(
        const Position &center, float radius, const std::array<float, 3> &tint
    )
    {
        (void)center;
        (void)radius;
        (void)tint;
        return NULL_HANDLE;
    }

    // Phase 2: remove one object (character despawn beyond the presentation
    // radius, corpse decay). Default no-op - the Phase-1 contract; the
    // production adapter forwards to SceneCapability::removeObject.
    virtual void removeObject(
        Handle /*handle*/
    )
    {}

    // Phase 5 (task 5.1.2 integration): publish the terrain mesh as the
    // scene's ground claim, replacing scene.toml's flat ground plane. The
    // vertices are a flat (x, y, z) triple array in row-major order over a
    // TERRAIN_GRID_STEPS x TERRAIN_GRID_STEPS grid (A-148). Default no-op -
    // the Phase-1/Phase-2 fakes predate terrain and must keep compiling
    // unmodified; the production adapter forwards to the scene capability's
    // mesh publish (or logs until the engine grows the seam). Called ONCE
    // per TerrainField (the mesh is static; a frame never re-publishes).
    virtual Handle spawnTerrainMesh(
        const std::vector<double> & /*vertices*/, std::uint32_t /*gridSteps*/
    )
    {
        return NULL_HANDLE;
    }

    // Update an existing object's position (used when territories move in
    // later phases; P1 calls this only if a territory's center changes
    // between frames, which it does not in P1 - declared for the seam).
    virtual void setObjectPosition(Handle h, const Position &p) = 0;

    // Sun light: direction is a unit vector pointing in the direction light
    // travels (from sun toward scene); intensity is in [0, 1].
    virtual void setSunLight(const std::array<float, 3> &direction, float intensity) = 0;

    // Sky radiance tint (Linear-RGB).
    virtual void setSkyTint(const std::array<float, 3> &tint) = 0;

    // The runtime environment state (the graphics-uplift round): the
    // atmosphere module's per-frame sky gradient, fog, exposure and moon,
    // pushed as one state the production adapter forwards into the scene
    // registry's environment override (the renderer merges it over the
    // static [scene] base). Default no-op - the Phase-1/2 fakes predate the
    // atmosphere and must keep compiling unmodified.
    virtual void setEnvironmentState(const struct SceneEnvironmentState & /*state*/) {}

    // ---- the graphics-uplift spawn vocabulary -------------------------------
    // Textured spawns: the prop compositions reference the mod's texture
    // sets (the pastel palette's painted tiles) and carry an emissive
    // strength (the lit windows). Defaults DELEGATE to the untextured
    // spawns, so the Phase-1/2 fakes keep compiling AND keep counting the
    // calls their assertions read (the production adapter overrides these).
    virtual Handle spawnBoxTextured(
        const Position &center, float halfWidth, float height, const std::array<float, 3> &tint,
        const char * /*textureSet*/, float /*uvScale*/, float /*emissiveStrength*/
    )
    {
        return spawnBox(center, halfWidth, height, tint);
    }
    // rev 30: the per-axis variant - walls, panels and slabs need rectangular
    // plans (a 9.2 x 0.6 wall footprint is impossible with the square
    // halfWidth above, which is how the first GI chamber rendered as four
    // overlapping room-filling slabs). Defaults DELEGATE to the square
    // spawnBoxTextured (the Phase-1/2 fakes keep compiling; the production
    // adapter overrides this).
    virtual Handle spawnBoxExtents(
        const Position &center, float halfExtentX, float halfExtentY, float /*halfExtentZ*/,
        const std::array<float, 3> &tint, const char *textureSet, float uvScale, float emissiveStrength
    )
    {
        // The square fallback can only honor one plan axis; the default
        // delegates (the fakes never render the chamber).
        (void)halfExtentY;
        return spawnBoxTextured(center, halfExtentX, halfExtentY, tint, textureSet, uvScale, emissiveStrength);
    }
    virtual Handle spawnSphereTextured(
        const Position &center, float radius, const std::array<float, 3> &tint, const char * /*textureSet*/, float /*uvScale*/
    )
    {
        return spawnSphere(center, radius, tint);
    }

    // The A-151 water plane: one flat mesh the size of the world patch at
    // the sea level (mesh primitives are skipped by the analytic tracer, so
    // the water never casts). Default no-op - the fakes predate water.
    virtual Handle spawnWaterPlane(double /*extentM*/)
    {
        return NULL_HANDLE;
    }

    // ---- the prop-mesh vocabulary (rev 24, the proper-tree-models round) ----
    //
    // Trees as one box + one sphere read as lollipops (the owner's defect).
    // Registered triangle meshes - a branching trunk, a layered canopy -
    // rasterize their real vertices. The registry caps registered meshes
    // (scene_capability.hpp maxMeshes), so the mod registers a FIXED
    // vocabulary (trunk + canopy per species, one small variant each) and
    // instances it per placement.
    //
    // registerPropMesh: vertices are interleaved position xyz + normal xyz
    // (meshFloatsPerVertex), indices u32, authored in METERS with the base
    // at y = 0 (mesh instances translate + rotateY only - the renderer's
    // scale contract). Returns the mesh handle (0 = refused; the caller
    // keeps the analytic fallback). Default no-op - the fakes predate the
    // vocabulary and must keep compiling.
    virtual Handle registerPropMesh(
        const char * /*name*/, const std::vector<float> & /*vertices*/, const std::vector<std::uint32_t> & /*indices*/
    )
    {
        return NULL_HANDLE;
    }

    // One mesh instance at `center` (the mesh's base rides on center.y),
    // yawed rotationY radians, tinted (the A-189 tinted-photo mix pulls the
    // texture set's hue toward the tint) and textured from `textureSet`.
    // Default no-op (the degrade-if-absent contract).
    virtual Handle spawnPropMeshInstance(
        const char * /*meshName*/, const Position & /*center*/, float /*rotationY*/,
        const std::array<float, 3> & /*tint*/, const char * /*textureSet*/, float /*uvScale*/
    )
    {
        return NULL_HANDLE;
    }

    // rev 34: the same spawn with a per-axis instance scale (meters per
    // authored meter) - the character-assembly parts instance one vocabulary
    // at per-part proportions (the parameter bindings' axis scales). The
    // default delegates to the unscaled spawn so every existing fake and
    // adapter keeps compiling; the production adapter overrides it.
    virtual Handle spawnPropMeshInstanceScaled(
        const char *meshName, const Position &center, float rotationY,
        const std::array<float, 3> &tint, const char *textureSet, float uvScale,
        const std::array<float, 3> & /*scale*/
    )
    {
        return spawnPropMeshInstance(meshName, center, rotationY, tint, textureSet, uvScale);
    }

    // rev 26: move an object WITH its yaw. The default delegates to
    // setObjectPosition (yaw-drop) so the Phase-1/2 fakes keep compiling and
    // keep counting the calls their assertions read; the production adapter
    // forwards to SceneCapability::setObjectTransform, which carries the
    // rotation. The blocky character bodies (mesh instances) ride this every
    // frame: the old position-only path would have reset every villager's
    // facing to 0 on the first move.
    virtual void setObjectPose(Handle h, const Position &p, float /*rotationY*/)
    {
        setObjectPosition(h, p);
    }

    // ---- the village lamp lights (rev 24) ----------------------------------
    // The lamp posts stood as emissive boxes only - the night ground around
    // them stayed black (the spawn_night capture read as a black field with
    // two glowing village slabs). These are the REAL point lights the pbr
    // and relight passes evaluate (LightingCapability::addPointLight), one
    // push per frame from the bridge. intensity 0 lamps are legal - the
    // day frame pushes intensity 0 rather than nothing, so the adapter can
    // keep its handle bookkeeping trivial.
    struct LampLight
    {
        Position position;
        std::array<float, 3> color{1.0f, 0.72f, 0.38f};
        float intensity = 0.0f;
        float radius    = 16.0f;
    };
    virtual void pushPointLights(
        const std::vector<LampLight> & /*lights*/
    )
    {}
};

// The mod-side environment state (the bridge's vocabulary; the production
// adapter maps it onto the engine's SceneEnvironmentOverride). All colors
// Linear-RGB. sunIntensity <= 0 keeps the lighting provider's value; the
// moon direction's length is ignored (normalized at push), intensity 0 = no
// moon term.
struct SceneEnvironmentState
{
    std::array<float, 3> skyZenith{0.0f, 0.0f, 0.0f};
    std::array<float, 3> skyHorizon{0.0f, 0.0f, 0.0f};
    std::array<float, 3> skyGround{0.0f, 0.0f, 0.0f};
    float fogDensity                = 0.0f;
    std::array<float, 3> fogTint{0.7f, 0.7f, 0.7f};
    float exposure                  = -1.0f;
    std::array<float, 3> moonDirection{0.0f, 0.0f, 0.0f};
    float moonIntensity             = 0.0f;
    std::array<float, 3> sunColor{1.0f, 0.97f, 0.90f};
    float sunIntensity              = -1.0f;
    // rev 26: the live sun direction (towards the scene) - the visible sun
    // disk tracks the light the pbr module actually applies. Zero = keep
    // the static [scene].sun direction.
    std::array<float, 3> sunDirection{0.0f, 0.0f, 0.0f};
};

// World view: the territories to render plus the primary weather (drives
// sky tint). The production adapter wraps WorldStore (task 1.4) and computes
// territory centers from the region grid (A-40). Tests provide a fake.
class WorldViewForBridge
{
public:
    struct TerritoryDesc
    {
        territoryId id;     // R10: opaque id, never reused
        TerritoryType type; // drives marker height/tint
        Position center;    // territory center in world space
        bool safe;          // true -> also spawn a disc
    };

    virtual ~WorldViewForBridge() = default;

    // Territories sorted by id ascending (deterministic iteration, R7).
    virtual std::vector<TerritoryDesc> territories() const = 0;

    // Primary weather at the viewing region (camera/player region). Drives
    // the global sky tint. The kind is task 3.8's Weather enum (the ordinal
    // Region::weather.kind carries; weatherFromKind() decodes it).
    virtual world::Weather primaryWeather() const = 0;

    // The season index (0..3, task 3.8's season state). Default 0 (spring) -
    // the Phase-1/2 fakes predate seasons and must keep compiling unmodified.
    virtual std::uint32_t seasonIndex() const { return 0; }
};

// Clock view: dayPhase01 in [0, 1) within the game day (0 = midnight,
// 0.25 = sunrise, 0.5 = noon, 0.75 = sunset). Production adapter wraps
// WorldClock (task 0.3).
class ClockViewForBridge
{
public:
    virtual ~ClockViewForBridge()     = default;
    virtual double dayPhase01() const = 0;
};

// ---- Phase 2: the character/corpse view --------------------------------------

// CharactersViewForBridge: the PRESENT population (the bridge's share of the
// step-13 publish). "Present" is the view's contract: every returned record
// is within PRESENTATION_RADIUS_M (A-118) of the LOD anchor (D-LOD-1: the
// player; the beginner safe-zone spawn with no player in-world). The bridge
// re-checks the radius against anchor() (defense in depth - a stale adapter
// can never leak a FAR NPC into the renderer) and owns every shape policy.
//
// The production adapter (store_adapters.hpp) walks CharacterStore/NpcStore/
// CorpseStore with the planar distance against the anchor; tests provide
// fakes. Distance compares on the x/z plane (R5: distances are planar).
class CharactersViewForBridge
{
public:
    struct CharacterDesc
    {
        characterId id    = 0;
        speciesId species = 0;
        factionId faction = 0; // 0 = factionless (no ring)
        Position position{};
        std::uint8_t tier = kTierFar; // the kTier* ordinals above
        bool sapient      = false;
        bool moving       = false; // NEAR intent walk / MID drift / FAR migration
    };

    struct CorpseDesc
    {
        characterId victim = 0;
        Position position{};
        bool discovered = false; // 3.3's discovery flip (4.1's delayed reports)
    };

    virtual ~CharactersViewForBridge() = default;

    // The LOD anchor position (D-LOD-1): the player's transform, or the
    // beginner safe-zone spawn with no player in-world. The bridge's radius
    // re-check measures against this.
    virtual Position anchor() const = 0;

    // Present characters, id-ascending (deterministic iteration, R7). The
    // list is radius-cut by the view; the bridge's re-check is a no-op on
    // honest adapters.
    virtual std::vector<CharacterDesc> presentCharacters() const = 0;

    // Live corpses within the presentation radius, id-ascending (R7).
    virtual std::vector<CorpseDesc> presentCorpses() const = 0;
};

// ---- SceneBridge -----------------------------------------------------------

class SceneBridge
{
public:
    // `characters` may be null (the Phase-1 static run: no population yet) -
    // the feature degrades, never the run (common.md section 4.0). The
    // default keeps every Phase-1 call site and test compiling unchanged.
    SceneBridge(WorldViewForBridge &world, ClockViewForBridge &clock, SceneRegistrySink &sink);
    SceneBridge(
        WorldViewForBridge &world,
        ClockViewForBridge &clock,
        SceneRegistrySink &sink,
        CharactersViewForBridge *characters
    );
    // Phase 5 (task 5.1.2 integration): the terrain field is the bridge's
    // ground claim. When present, the bridge publishes its meshGrid() once
    // on the first frame via sink_.spawnTerrainMesh(); when null, the bridge
    // falls back to scene.toml's flat ground plane (the Phase-1 contract).
    // The TerrainField is NOT owned by the bridge (the plugin owns it for
    // the run's lifetime); the bridge holds a raw pointer.
    SceneBridge(
        WorldViewForBridge &world,
        ClockViewForBridge &clock,
        SceneRegistrySink &sink,
        CharactersViewForBridge *characters,
        const TerrainField *terrain
    );
    ~SceneBridge();

    SceneBridge(const SceneBridge &)            = delete;
    SceneBridge &operator=(const SceneBridge &) = delete;

    // Called once per EngineFrameEvent by the plugin's subscription handler.
    // Pure bridge: reads world/clock/character state, pushes scene objects,
    // never mutates sim state.
    void onFrame();

    // The graphics-uplift wiring: attach the scatter field (the settlement
    // + vegetation placements) and the borders field (the water plane). Both
    // are optional (null = the Phase-2 contract); neither is owned.
    void attachScatterField(const class ScatterField *scatter) noexcept
    {
        scatter_ = scatter;
    }
    void attachBordersField(const class BordersField *borders) noexcept
    {
        borders_ = borders;
    }

    // ---- test hooks (zero overhead in release; used by the pinned tests) --
    // Total live scene objects this bridge has created (boxes + discs).
    std::size_t spawnedObjectCount() const;
    // Territories not yet turned into scene objects (waiting for a chunk
    // slot). Asserts the A-119 batching invariant.
    std::size_t pendingCreationCount() const;
    // True iff the last onFrame() pushed a fresh set_sun_light call.
    bool sunWasUpdatedOnLastFrame() const
    {
        return sun_updated_last_frame_;
    }
    // True iff the last onFrame() pushed a fresh set_sky_tint call.
    bool skyTintWasUpdatedOnLastFrame() const
    {
        return sky_tint_updated_last_frame_;
    }
    // Phase 5: true iff the terrain mesh has been published (spawnTerrainMesh
    // called once on the first frame a TerrainField is present). Exposed for
    // the integration test that asserts the publish fires exactly once.
    bool terrainPublished() const noexcept
    {
        return terrain_published_;
    }

    // ---- the GI validation chamber (rev 30) ---------------------------------
    // The owner's "create a small room to test UE5's Lumen-like GI" - the
    // Cornell-box move: ONE sealed room, ONE deterministic camera, and every
    // GI phenomenon readable in a single frame (colored bleed, soft corner
    // occlusion, emissive-through-GI, the point light's shadow gradient).
    // The site is set by the plugin's staging (the gi_room capture scenario);
    // a run without it never spawns the chamber (the world's object budget
    // and every pinned capture stay untouched).
    struct GiChamberSite
    {
        double x = 0.0;       // plan center, world X
        double z = 0.0;       // plan center, world Z
        double groundY = 0.0; // the terrain height at (x, z) - the floor's top
    };
    void setGiChamberSite(const GiChamberSite &site);
    bool giChamberSpawned() const noexcept
    {
        return gi_chamber_spawned_;
    }

    // ---- the character-customisation lab (rev 34) ---------------------------
    // The owner's architecture document's first implementation test: the
    // hybrid assembly (humanoid torso + pelvis, canine head, digitigrade
    // legs, humanoid arms, avian wings, tail) beside its VARIANT (the test
    // operations as data: avian head, wings removed, wider torso, longer
    // legs, a second tail). Same staging pattern as the chamber: the
    // capability + the site arrive from the plugin; a run without them
    // never spawns the lab.
    void setCharacterLibrary(engine::CharacterCustomisationCapability *capability,
        const std::filesystem::path &dataDirectory);
    void setCharacterLabSite(const GiChamberSite &site);
    bool characterLabSpawned() const noexcept
    {
        return character_lab_spawned_;
    }

    // ---- Phase-2 test hooks ------------------------------------------------
    // Live character/corpse objects (presence spheres + FAR markers, corpse
    // boxes; the ring shells are part of the character object count).
    std::size_t characterObjectCount() const;
    std::size_t corpseObjectCount() const;
    // Character/corpse creations not yet spawned (waiting for a chunk slot).
    std::size_t pendingCharacterCreationCount() const;
    // The object kind a character currently carries:
    //   0 = none (static FAR), 1 = presence sphere, 2 = FAR movement marker.
    int characterObjectKind(characterId id) const;

private:
    void buildPendingQueueIfNeeded();
    void processPendingCreations();       // chunked <= SCENE_CREATE_CHUNK_PER_FRAME
    // Phase 5: publish the terrain mesh once (the first frame a TerrainField
    // is present). Idempotent — terrain_published_ guards re-entry.
    void publishTerrainIfNeeded();
    // The graphics-uplift round: atmosphere, water, scatter.
    void updateAtmosphere();          // sampleAtmosphere -> sun/sky/fog/exposure/moon push
    void publishWaterIfNeeded();      // the A-151 sea-level plane, once
    void publishScatterWindow();      // the A-183 window around the anchor
    // rev 24: the registered prop-mesh vocabulary (tree trunks/canopies, the
    // dead tree), once per run before the first scatter spawn.
    void registerPropMeshVocabulary();
    void spawnPropComposition(const class ScatterPlacement &placement); // one prop's primitive composition
    // Phase 2: sync characters + corpses into the registry (create chunked,
    // move per frame, remove on tier/exit flips).
    void syncCharacters();
    void syncCorpses();
    void syncJailCages();
    void syncGiChamber(); // rev 30: the GI validation chamber, once, when staged
    void drainCharacterCreations(); // A-119 chunk over the pending char/corpse queues

    float territoryMarkerHeight(TerritoryType t) const;
    std::array<float, 3> territoryMarkerTint(TerritoryType t) const;
    std::array<float, 3> skyTintFor(world::Weather w) const;
    std::array<float, 3> speciesColor(speciesId s) const;

    WorldViewForBridge &world_;
    ClockViewForBridge &clock_;
    SceneRegistrySink &sink_;
    CharactersViewForBridge *characters_ = nullptr; // null = Phase-1 static run
    const TerrainField *terrain_         = nullptr; // null = Phase-1 flat ground
    const class ScatterField *scatter_   = nullptr; // null = no scatter (Phase-2 contract)
    const class BordersField *borders_   = nullptr; // null = no water/borders

    // ---- atmosphere push state --------------------------------------------
    // The last pushed environment state: re-pushed only when it changes
    // (the sink's sequence bumps only on change, but skipping identical
    // pushes here keeps the adapter work at zero on a still frame).
    SceneEnvironmentState last_environment_;
    bool environment_pushed_ = false;
    world::Weather last_environment_weather_ = world::Weather::COUNT; // the env state's weather key
    float last_window_emissive_ = -1.0f; // the window-emissive state (day 0 / night 1)

    // R7: deterministic iteration via std::map (id-sorted).
    // One box per territory, one disc per safe zone. Kept across frames so
    // a second onFrame() does not re-spawn.
    std::map<territoryId, SceneRegistrySink::Handle> territory_boxes_;
    std::map<territoryId, SceneRegistrySink::Handle> safe_zone_discs_;

    // Pending creation tasks (built once from world_.territories(), drained
    // in chunks). Ordering is deterministic: territories in id order, and
    // for each territory the box task precedes its disc task.
    struct CreationTask
    {
        territoryId id;
        bool is_disc; // false = box, true = disc
    };
    std::vector<CreationTask> pending_creations_;
    bool pending_queue_built_ = false;

    // Sun update hysteresis: -1.0 forces the first frame to push.
    double last_sun_phase_ = -1.0;
    // Weather-state hysteresis: COUNT forces the first frame to push.
    world::Weather last_sky_weather_ = world::Weather::COUNT;

    bool sun_updated_last_frame_      = false;
    bool sky_tint_updated_last_frame_ = false;
    bool terrain_published_           = false; // Phase 5: spawnTerrainMesh called once

    // ---- Phase-2 state ------------------------------------------------------
    // The desired-shape record for one present character (rebuilt each frame
    // from the view; diffed against the live maps).
    struct CharacterDesired
    {
        int kind = 0; // 0 none, 1 sphere, 2 far-marker
        Position position{};
        speciesId species = 0;
        factionId faction = 0;
        bool moving       = false; // the view's movement flag (A-154's bob gate)
    };

    // Live character objects: id -> {shape kind, body handle, ring handle}.
    // The ring shell exists only for factioned presence spheres (handle 0 =
    // none). FAR markers carry no ring. bodyRadius carries the per-character
    // girth jitter (character_look.hpp) the move loop re-applies every frame
    // (the spawn's radius is presentation state, not a sim read).
    //
    // rev 26 - the blocky near body (kind 3): within
    // CHARACTER_DETAIL_RADIUS_M of the LOD anchor a NEAR/MID character
    // presents as the converted Kenney CC0 blocky model (npc_body_* /
    // npc_legs_* / npc_head_* mesh instances, one hair box above the head)
    // with the per-part randomized look (tunic / pants / skin / hair dyes).
    // Beyond the detail radius the kind-1 sphere+head LOD returns (the
    // object budget stays bounded as the crowd grows). kind transitions
    // recreate the object through the existing diff path.
    struct CharacterObject
    {
        int kind                       = 1;
        SceneRegistrySink::Handle body = SceneRegistrySink::NULL_HANDLE;
        SceneRegistrySink::Handle ring = SceneRegistrySink::NULL_HANDLE;
        // rev 26: the blocky body's extra parts (kind 3 only).
        SceneRegistrySink::Handle legs = SceneRegistrySink::NULL_HANDLE;
        SceneRegistrySink::Handle head = SceneRegistrySink::NULL_HANDLE;
        SceneRegistrySink::Handle hair = SceneRegistrySink::NULL_HANDLE;
        std::uint32_t bodyVariant      = 0;   // the npc mesh variant pick
        float smallScale               = 1.0f; // 1.0 lg / 0.88 sm mesh set
        float bodyRadius               = CHARACTER_SPHERE_RADIUS;
        float heightJitter             = 1.0f;
        // rev 27: the live gait pose (0 rest, 1 walk A, 2 walk B). The move
        // loop swaps the body+legs mesh instances when the bucket changes
        // (spawn-then-remove, so a swap never drops the villager for a
        // frame); presentation-only state derived from traveled distance.
        std::uint8_t gaitPose          = 0;
    };
    std::map<characterId, CharacterObject> character_objects_;

    // Corpses: victim id -> handle (one flattened gray box each).
    std::map<characterId, SceneRegistrySink::Handle> corpse_objects_;

    // Jail cages: one per territory (phase 3 puts prisoners there; the cage
    // marks the point from day one). Mirrors the territory box map.
    std::map<territoryId, SceneRegistrySink::Handle> jail_cages_;

    // Pending character/corpse creations (A-119 chunk). Deterministic order:
    // corpses first (id-ascending), then characters (id-ascending) - the
    // static world's survivors are the older, quieter objects.
    struct CharacterCreationTask
    {
        bool is_corpse = false;
        characterId id = 0;
    };
    std::vector<CharacterCreationTask> pending_character_creations_;
    std::map<characterId, CharacterDesired> desired_characters_;                 // rebuilt per frame
    std::map<characterId, CharactersViewForBridge::CorpseDesc> desired_corpses_; // rebuilt per frame
    std::vector<territoryId> pending_jail_cages_;                                // territories still needing a cage
    bool jail_cages_queued_ = false;

    // The GI validation chamber (rev 30): the staged site + the once-guard.
    GiChamberSite gi_chamber_site_{};
    bool gi_chamber_spawned_ = false;

    // The character-customisation lab (rev 34): the same pattern one rev
    // later. The lab is defined in character_lab.hpp (the loader + the two
    // proof assemblies); the bridge owns its lifetime and per-frame update.
    std::unique_ptr<CharacterLab> character_lab_;
    bool character_lab_spawned_ = false;

    // ---- the graphics-uplift state ------------------------------------------
    // Water: one plane handle (spawned once; the sea level never moves).
    SceneRegistrySink::Handle water_handle_ = SceneRegistrySink::NULL_HANDLE;
    bool water_published_                   = false;
    // rev 24: the prop-mesh vocabulary registration guard (registerPropMesh
    // is idempotent per name, the flag keeps the per-frame cost at one bool).
    bool prop_meshes_registered_            = false;
    // rev 24: the lamp point-light push (the scatter window's lamp posts as
    // real night lights, LightingCapability via the adapter's pool).
    void syncLampLights();

    // Scatter: the live window's placement index -> the handles its
    // composition spawned (bodies first). The window republishes as the
    // anchor moves: placements outside the despawn radius lose their
    // handles, placements inside the publish radius gain them (chunked at
    // A-119's budget shared with the rest of the bridge's creations).
    struct ScatterObjects
    {
        std::vector<SceneRegistrySink::Handle> parts;
    };
    std::map<std::size_t, ScatterObjects> scatter_objects_;
    std::vector<std::size_t> scatter_pending_;               // in-window indices awaiting creation
    std::uint64_t scatter_anchor_cell_ = ~std::uint64_t{0};  // the window's last anchor cell
    std::size_t placement_index_       = 0;                  // the placement spawnPropComposition is filling

    // Character bodies carry a head sphere (A-188) - the same map as above
    // gains the head handle; tracked separately so the despawn path removes
    // both.
    std::map<characterId, SceneRegistrySink::Handle> character_heads_;

    // Traveled-distance accumulator per character (A-154's bob phase; the
    // bridge tracks the distance each character moved since last seen -
    // presentation-only state, never sim state).
    std::map<characterId, double> character_traveled_;
    std::map<characterId, Position> character_last_position_;
};

} // namespace bridge
} // namespace harrowmere
