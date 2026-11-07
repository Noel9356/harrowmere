// Task 7.2 - Simulation <-> scene bridge implementation (Phase-2 full card;
// extends the 7.2P1 static slice in place).
//
// See scene_bridge.hpp for the contract and the pinned constants. This file
// implements the per-frame bridge logic: chunked creation of territory
// boxes, safe-zone discs, jail cages, character spheres and corpse boxes;
// per-frame position sync; sun-light updates on a >= 1% phase delta; sky-tint
// updates on a weather-state change. No gameplay logic, no sim-state
// mutation.

#include "bridge/scene_bridge.hpp"

#include "bridge/atmosphere.hpp"        // sampleAtmosphere (the 5.1.4 sky/fog/exposure path)
#include "bridge/borders.hpp"           // BordersField (the water plane state)
#include "bridge/character_look.hpp"    // characterLookFor (the per-character randomized look)
#include "bridge/character_lab.hpp"      // rev 34: the character-customisation lab (the owner's doc, first test)
#include "bridge/character_visuals.hpp" // computeCharacterVisual / speciesSilhouette (A-153/A-154/A-188)
#include "bridge/prop_mesh_data_cc0.h"  // rev 26: the converted Kenney CC0 mesh vocabulary
#include "bridge/prop_mesh_infra.h"     // rev 31: the roads/crops/docks/boats infrastructure meshes
#include "bridge/character_part_meshes.h" // rev 34: the character-assembly part vocabulary (downloaded CC0 + authored)
#include "bridge/prop_meshes.hpp"       // the rev-24 prop-mesh vocabulary (proper tree models)
#include "bridge/scatter.hpp"           // ScatterField/ScatterPlacement (the 5.2.1 placements)
#include "core/constants_phase5.hpp"    // A-182..A-188 (the uplift rows)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>

namespace harrowmere
{
namespace bridge
{

namespace
{

// Planar (x/z) distance between two positions - R5's rule: distances compare
// as f64 on the plane (y is cosmetic, card 1.6).
double planarDistance(
    const Position &a, const Position &b
)
{
    const double dx = a.x - b.x;
    const double dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

} // anonymous namespace

// ---- the rev-27 gait bucket -------------------------------------------------

std::uint8_t gaitPoseBucket(double traveledDistanceM, bool inMotion) noexcept
{
    // Pose 0 (the rest pose) when the character is not in motion - a
    // villager who arrived (or is blocked) settles back onto both feet
    // instead of freezing mid-stride. While in motion the bucket
    // alternates walk A / walk B on the SAME phase the A-154 walk bob uses
    // (traveled / A-57 speed * WALK_BOB_HZ): one stride pose per bob
    // half-cycle, so the legs swap in step with the vertical bob. The
    // motion source is the sim's own moving intent ORed with this frame's
    // measured displacement - the render loop runs faster than the fixed
    // simulation tick, so a displacement-only test would flicker the pose
    // rest/stride between ticks. Pure from the inputs (D-IMG-2: no RNG
    // stream, no wall clock) - the same traveled distance wears the same
    // pose.
    if (!inMotion)
    {
        return 0;
    }
    const double phase = (traveledDistanceM / NPC_GAIT_REFERENCE_SPEED_MPS) * WALK_BOB_HZ;
    const double twice = phase * 2.0;
    const auto step = static_cast<std::uint64_t>(twice < 0.0 ? 0.0 : twice);
    return static_cast<std::uint8_t>(1u + (step & 1u));
}

// ---- SceneBridge -----------------------------------------------------------

SceneBridge::SceneBridge(
    WorldViewForBridge &world, ClockViewForBridge &clock, SceneRegistrySink &sink
)
    : SceneBridge(world, clock, sink, nullptr)
{}

SceneBridge::SceneBridge(
    WorldViewForBridge &world, ClockViewForBridge &clock, SceneRegistrySink &sink, CharactersViewForBridge *characters
)
    : SceneBridge(world, clock, sink, characters, nullptr)
{}

SceneBridge::SceneBridge(
    WorldViewForBridge &world,
    ClockViewForBridge &clock,
    SceneRegistrySink &sink,
    CharactersViewForBridge *characters,
    const TerrainField *terrain
)
    : world_(world), clock_(clock), sink_(sink), characters_(characters), terrain_(terrain)
{}

SceneBridge::~SceneBridge() = default;

void SceneBridge::onFrame()
{
    // Reset per-frame flags before any work; each updater sets its own flag
    // if it actually pushed to the sink this frame.
    sun_updated_last_frame_      = false;
    sky_tint_updated_last_frame_ = false;

    // Phase 5: publish the terrain mesh once, before any other object work.
    // The mesh is the scene's ground claim (replacing scene.toml's flat
    // plane); it must exist before characters/corpses so they sit on the
    // land, not inside it.
    publishTerrainIfNeeded();
    // The graphics-uplift round: the atmosphere state (sky gradient, fog,
    // exposure, moon) replaces the two standalone sun/sky updaters (which
    // keep their thresholds and flags - the pinned sun contract now rides
    // the atmosphere sample's own sun math).
    updateAtmosphere();
    publishWaterIfNeeded();
    buildPendingQueueIfNeeded();
    processPendingCreations();
    syncJailCages();
    syncGiChamber();
    syncCharacters();
    syncCorpses();
    // One chunk budget per frame across ALL dynamic creations (A-119): the
    // static queue drains first (it is the oldest work), then characters,
    // then corpses - a teleport's burst can never spike the frame.
    drainCharacterCreations();
    // The scatter window runs last: the world's props fill whatever chunk
    // budget the frame's characters/corpses left over, and the first frames
    // after a teleport spend their budget on the characters the player
    // actually stands next to.
    publishScatterWindow();
    // rev 34: the character-customisation lab (the two proof assemblies);
    // the animated parts (tails, wings) re-pose every frame after.
    if (character_lab_ != nullptr)
    {
        character_lab_->ensureSpawned(sink_);
        if (character_lab_->liveParts() > 0)
        {
            // The animation clock rides the day phase (seconds within the
            // world day): the capture rig pins time_of_day, so every
            // capture frame is a deterministic animation phase - the same
            // determinism contract the GI gather's frozen jitter follows.
            character_lab_->updateFrame(sink_, clock_.dayPhase01() * 86400.0);
            character_lab_spawned_ = true;
        }
    }
    // rev 24: the window's lamp posts become the frame's point lights (the
    // night ground stops reading as a black field around the lit windows).
    syncLampLights();
}

void SceneBridge::publishTerrainIfNeeded()
{
    if (terrain_published_ || terrain_ == nullptr)
        return;
    const auto mesh = terrain_->meshGrid();
    if (mesh.empty())
        return;
    if (std::getenv("HARROWMERE_DUMP_TERRAIN") != nullptr)
    {
        // rev 23 diagnosis: the analytic height field around the west-coast
        // camera (the floating-trees capture) - compare against the
        // [mesh-vertex] lines the sink adapter prints for the same region.
        for (double sz = 7600.0; sz <= 8700.0; sz += 100.0)
        {
            for (double sx = 0.0; sx <= 1100.0; sx += 100.0)
            {
                std::fprintf(stderr, "[terrain-at] (%.0f,%.0f) h=%.2f\n", sx, sz, terrain_->heightAt(sx, sz));
            }
        }
        // rev 24: the coast camera needs to LOOK AT WATER - the water plane
        // shows only where the heightfield dips below the datum. Scan the
        // whole patch on a 512 m grid and report every sub-datum cell (and
        // the world minimum) so the suite can aim the camera at a real
        // shoreline.
        double worldMin = 1e9;
        double minX = 0, minZ = 0;
        for (double sz = 256.0; sz < TERRAIN_WORLD_EXTENT_M; sz += 512.0)
        {
            for (double sx = 256.0; sx < TERRAIN_WORLD_EXTENT_M; sx += 512.0)
            {
                const double h = terrain_->heightAt(sx, sz);
                if (h < worldMin)
                {
                    worldMin = h;
                    minX = sx;
                    minZ = sz;
                }
                if (h < 1.0)
                {
                    std::fprintf(stderr, "[terrain-water] (%.0f,%.0f) h=%.2f\n", sx, sz, h);
                }
            }
        }
        std::fprintf(stderr, "[terrain-min] (%.0f,%.0f) h=%.2f\n", minX, minZ, worldMin);
        // rev 24: the 64 m fine grid around the world minimum - the camera
        // staging needs the exact shoreline geometry (where the basin's
        // ground crosses the 0.3 m water line).
        for (double sz = minZ - 320.0; sz <= minZ + 320.0; sz += 64.0)
        {
            for (double sx = minX - 320.0; sx <= minX + 320.0; sx += 64.0)
            {
                std::fprintf(stderr, "[terrain-basin] (%.0f,%.0f) h=%.2f\n", sx, sz, terrain_->heightAt(sx, sz));
            }
        }
        // The four staged capture-suite cameras: a buried camera renders the
        // terrain's culled backfaces as sky and the y=0 water plane as a
        // "bay" - the coast capture's floating trees were this. The suite
        // seed is pinned, so these heights are constants.
        constexpr double kCamProbe[8] = {430.0, 8140.0, 13890.0, 14914.0, 5712.0, 6718.0, 5530.0, 6830.0};
        for (int i = 0; i < 4; ++i)
        {
            std::fprintf(stderr, "[terrain-cam] cam%d (%.0f,%.0f) h=%.2f\n", i, kCamProbe[2 * i], kCamProbe[2 * i + 1],
                terrain_->heightAt(kCamProbe[2 * i], kCamProbe[2 * i + 1]));
        }
        // rev 26: the spawn village's square heights - the street-level
        // camera staging needs the actual pad elevation. The suite seed's
        // player spawn (the beginner village center) is the pinned
        // (6656, 7680) - the default scene.toml look_at does NOT name it.
        for (double sz = 7560.0; sz <= 7800.0; sz += 30.0)
        {
            for (double sx = 6540.0; sx <= 6780.0; sx += 30.0)
            {
                std::fprintf(stderr, "[spawn-at] (%.0f,%.0f) h=%.2f\n", sx, sz, terrain_->heightAt(sx, sz));
            }
        }
    }
    // The mesh is a flat (x, y, z) triple array over a
    // TERRAIN_GRID_STEPS x TERRAIN_GRID_STEPS grid (A-148). The sink's
    // spawnTerrainMesh takes the vertices + the grid resolution; the
    // production adapter forwards to the scene capability (or logs until
    // the engine grows the seam). The handle is stored for a future
    // remove-on-rebuild path (the terrain is static per run, so the handle
    // is currently fire-and-forget).
    (void)sink_.spawnTerrainMesh(mesh, TERRAIN_GRID_STEPS);
    terrain_published_ = true;
}

void SceneBridge::buildPendingQueueIfNeeded()
{
    if (pending_queue_built_)
        return;

    const auto descs = world_.territories(); // already sorted by id
    pending_creations_.clear();
    pending_creations_.reserve(descs.size() * 2);
    for (const auto &d : descs)
    {
        pending_creations_.push_back({d.id, /*is_disc=*/false});
        if (d.safe)
        {
            pending_creations_.push_back({d.id, /*is_disc=*/true});
        }
    }
    pending_queue_built_ = true;
}

void SceneBridge::processPendingCreations()
{
    if (pending_creations_.empty())
        return;

    // Look up the territory descriptor once per id for this frame's chunk.
    // The world view returns a fresh vector each call; build a local map for
    // O(log n) lookup. std::map for R7 determinism (territoryId-sorted).
    std::map<territoryId, WorldViewForBridge::TerritoryDesc> by_id;
    {
        const auto descs = world_.territories();
        for (const auto &d : descs)
        {
            by_id.emplace(d.id, d);
        }
    }

    std::uint32_t processed = 0;
    while (!pending_creations_.empty() && processed < SCENE_CREATE_CHUNK_PER_FRAME)
    {
        const CreationTask task = pending_creations_.front();
        pending_creations_.erase(pending_creations_.begin());

        auto it = by_id.find(task.id);
        if (it == by_id.end())
            continue; // territory vanished between frames

        const auto &d = it->second;
        if (task.is_disc)
        {
            // Idempotent: skip if a disc already exists for this territory
            // (e.g. after a queue rebuild).
            if (safe_zone_discs_.count(d.id) != 0)
                continue;
            const SceneRegistrySink::Handle h
                = sink_.spawnDisc(d.center, SAFE_ZONE_DISC_RADIUS, SAFE_ZONE_DISC_HEIGHT, TINT_SAFE);
            safe_zone_discs_.emplace(d.id, h);
        } else
        {
            if (territory_boxes_.count(d.id) != 0)
                continue;
            const SceneRegistrySink::Handle h = sink_.spawnBox(
                d.center, TERRITORY_MARKER_HALF_WIDTH, territoryMarkerHeight(d.type), territoryMarkerTint(d.type)
            );
            territory_boxes_.emplace(d.id, h);
        }
        ++processed;
    }
}

void SceneBridge::syncJailCages()
{
    // Phase-2 feature, gated on the Phase-2 constructor: the 3-arg (Phase-1)
    // constructor keeps the static world's exact object budget (the P1
    // pinned tests count boxes/discs and must stay green unmodified).
    if (characters_ == nullptr)
        return;

    // One cage per SAFE-ZONE territory (see the header's scope note: the
    // Phase-2 jail is the safe zone's - 4.3 extends the set). Queued once,
    // drained through the shared A-119 chunk; the cage sits at the pinned
    // offset from the territory center, on the ground plane.
    if (!jail_cages_queued_)
    {
        jail_cages_queued_ = true;
        for (const auto &d : world_.territories())
        {
            if (d.safe && jail_cages_.count(d.id) == 0)
            {
                pending_jail_cages_.push_back(d.id);
            }
        }
        // R7: id-ascending order (territories() already returns that order).
    }

    if (pending_jail_cages_.empty())
        return;

    std::map<territoryId, WorldViewForBridge::TerritoryDesc> by_id;
    {
        const auto descs = world_.territories();
        for (const auto &d : descs)
        {
            by_id.emplace(d.id, d);
        }
    }

    std::uint32_t processed = 0;
    while (!pending_jail_cages_.empty() && processed < SCENE_CREATE_CHUNK_PER_FRAME)
    {
        const territoryId id = pending_jail_cages_.front();
        pending_jail_cages_.erase(pending_jail_cages_.begin());

        auto it = by_id.find(id);
        if (it == by_id.end() || jail_cages_.count(id) != 0)
            continue;

        Position cage = it->second.center;
        cage.x += JAIL_CAGE_OFFSET_M;
        cage.z += JAIL_CAGE_OFFSET_M;
        const SceneRegistrySink::Handle h
            = sink_.spawnBox(cage, JAIL_CAGE_HALF_EXTENT, JAIL_CAGE_HALF_EXTENT * 2.0f, TINT_JAIL_CAGE);
        jail_cages_.emplace(id, h);
        ++processed;
    }
}

void SceneBridge::setGiChamberSite(
    const GiChamberSite &site
)
{
    gi_chamber_site_ = site;
}

void SceneBridge::setCharacterLibrary(
    engine::CharacterCustomisationCapability *capability, const std::filesystem::path &dataDirectory
)
{
    if (character_lab_ == nullptr)
    {
        character_lab_ = std::make_unique<bridge::CharacterLab>();
    }
    character_lab_->setCapability(capability);
    character_lab_->setDataDirectory(dataDirectory);
}

void SceneBridge::setCharacterLabSite(
    const GiChamberSite &site
)
{
    if (character_lab_ == nullptr)
    {
        character_lab_ = std::make_unique<bridge::CharacterLab>();
    }
    character_lab_->setSite({site.x, site.z, site.groundY});
}

void SceneBridge::syncGiChamber()
{
    // rev 30 - the GI validation chamber (the owner's "create a small room
    // to test UE5's Lumen-like GI"). The Cornell-box move: one sealed room
    // whose every surface is a known albedo, so one frame reads out the
    // whole GI pipeline:
    //   * the white pedestal tints RED near the red wall, GREEN near the
    //     green one (the bleed the composite's constant ambient never drew);
    //   * corners and contact points darken smoothly (the occlusion the
    //     gather computes, not an AO hack);
    //   * the emissive ceiling panel lights the room THROUGH the GI (the
    //     direct pass has no area lights - only the transport does);
    //   * the staged point light anchors the direct shading (shadow rays)
    //     while the panel's own glow arrives indirectly.
    // Geometry stays inside the square-box vocabulary the tracer handles
    // exactly (spawnBoxTextured honors the center's y and takes the half
    // height - the cottage compositions share both conventions).
    if (gi_chamber_spawned_ || (gi_chamber_site_.x == 0.0 && gi_chamber_site_.z == 0.0))
    {
        return;
    }
    gi_chamber_spawned_ = true;

    const double x      = gi_chamber_site_.x;
    const double z      = gi_chamber_site_.z;
    const double ground = gi_chamber_site_.groundY;
    const char *kNoSet  = ""; // an unregistered set -> flat albedo (layer -1)

    // Palette: the Cornell read needs saturation the pastel world palette
    // never uses - the walls are the light sources' only reflectors here.
    const std::array<float, 3> kWhite = {0.82f, 0.82f, 0.84f};
    const std::array<float, 3> kRed   = {0.72f, 0.10f, 0.08f};
    const std::array<float, 3> kGreen = {0.12f, 0.55f, 0.14f};
    const std::array<float, 3> kBlue  = {0.15f, 0.28f, 0.70f};

    // Floor: sunk 0.35 into the terrain (the slope can never poke through);
    // its top is exactly the site's ground height.
    sink_.spawnBoxExtents({x, ground - 0.35, z}, 4.6f, 0.35f, 4.6f, kWhite, kNoSet, 1.0f, 0.0f);
    // Walls: interior faces at +/-4.0 m, 4.2 m tall, 0.6 m thick. The RED
    // wall faces the camera from the south (+z), the GREEN one from the
    // north (-z) - the classic Cornell arrangement. (rev 30 fix: the first
    // cut spawned these through the square halfWidth helper, which stamped
    // 9.2 x 9.2 slabs through the room's plan - the camera stood inside
    // solid concrete.)
    sink_.spawnBoxExtents({x, ground + 2.2, z + 4.3}, 4.6f, 2.2f, 0.3f, kRed, kNoSet, 1.0f, 0.0f);
    sink_.spawnBoxExtents({x, ground + 2.2, z - 4.3}, 4.6f, 2.2f, 0.3f, kGreen, kNoSet, 1.0f, 0.0f);
    sink_.spawnBoxExtents({x - 4.3, ground + 2.2, z}, 0.3f, 2.2f, 4.6f, kWhite, kNoSet, 1.0f, 0.0f);
    sink_.spawnBoxExtents({x + 4.3, ground + 2.2, z}, 0.3f, 2.2f, 4.6f, kWhite, kNoSet, 1.0f, 0.0f);
    // Ceiling: a sealed lid (its interior face at ground + 4.2).
    sink_.spawnBoxExtents({x, ground + 4.5, z}, 4.6f, 0.3f, 4.6f, kWhite, kNoSet, 1.0f, 0.0f);
    // The emissive ceiling panel (the "softbox"): flush under the lid, east
    // of center. Its glow is pure GI transport: the direct pass knows no
    // area lights, so any illumination arriving from this slab PROVES the
    // gather carries emissive surfaces.
    sink_.spawnBoxExtents({x + 1.2, ground + 4.14, z}, 1.2f, 0.06f, 1.2f, {1.0f, 0.95f, 0.88f}, kNoSet, 1.0f, 2.5f);
    // The white pedestal (the bleed read): the vertical faces tint toward
    // red/green with distance from the colored walls.
    sink_.spawnBoxExtents({x - 0.8, ground + 0.55, z - 0.6}, 0.7f, 0.55f, 0.7f, {0.87f, 0.87f, 0.87f}, kNoSet, 1.0f, 0.0f);
    // The blue block: a saturated secondary reflector on the sun-warmed side.
    sink_.spawnBoxExtents({x + 1.4, ground + 0.45, z + 1.2}, 0.5f, 0.45f, 0.5f, kBlue, kNoSet, 1.0f, 0.0f);
    // The sphere: curved-surface occlusion + the SDF sphere branch's exercise.
    sink_.spawnSphereTextured({x - 0.6, ground + 0.5, z + 1.6}, 0.5f, {0.85f, 0.85f, 0.88f}, kNoSet, 1.0f);
}

void SceneBridge::syncCharacters()
{
    if (characters_ == nullptr)
        return;

    // ---- 1. Rebuild the desired set from the view (id-ascending, R7) ------
    desired_characters_.clear();
    const Position anchor = characters_->anchor();
    for (const auto &c : characters_->presentCharacters())
    {
        // The LOCAL player never publishes a visual (species 0, the hardcoded
        // player row): the follow camera rides at the player's eye height
        // (PLAYER_EYE_HEIGHT_M, 7.4P1), which is inside the A-188 silhouette
        // (the head sphere spans y+1.66..y+2.34 around the eye at y+1.7), so
        // the avatar's own unlit backface filled the follow view with a giant
        // black blob. First person: the world is the player's visual; NPCs
        // keep theirs (A-153/A-154/A-188 unchanged for every other species).
        if (c.species == 0)
        {
            continue;
        }
        // A-118 re-check (defense in depth): the view's contract is
        // present-only, but a stale adapter can never leak a far NPC into
        // the renderer - the bridge measures the anchor distance itself.
        if (planarDistance(c.position, anchor) > PRESENTATION_RADIUS_M)
        {
            continue;
        }
        CharacterDesired d;
        d.position = c.position;
        d.species  = c.species;
        d.faction  = c.faction;
        d.moving   = c.moving;

        // rev 26: the sim's planar positions carry y = 0 (card 1.6's "y is
        // cosmetic"), while the presentation world stands on the heightfield
        // (the spawn village's pad rides ~11 m on the suite seed). Every
        // visual therefore LIFTS to the terrain here - the buried-crowd
        // defect the first street-level capture exposed (the blocky parts
        // sat 10.9 m under the pad). The lift is presentation-only: the sim
        // never reads a bridge output (R5).
        if (terrain_ != nullptr)
        {
            d.position.y = terrain_->heightAt(d.position.x, d.position.z);
        }
        // rev 26: the personal-space spread (characterStandOffset) - every
        // intent pulls toward the settlement center, so a gathered crowd
        // stacks into one totem; the per-id offset fans the visuals out
        // around the sim point, identically at creation and on every move.
        const StandOffset stand = characterStandOffset(c.id);
        d.position.x += stand.x;
        d.position.z += stand.z;

        // The shape policy (see the header's reading note):
        //   blocky body      - every NEAR/MID character within
        //                      CHARACTER_DETAIL_RADIUS_M of the anchor (rev
        //                      26: the converted Kenney CC0 model with the
        //                      per-part randomized look - the crowd a
        //                      street-level camera can tell apart);
        //   presence sphere  - every other NEAR/MID character (the exit
        //                      checklist's "one visual per present character");
        //   FAR movement box - a FAR character that is currently moving
        //                      (migration walk/teleport): the transient
        //                      "FAR-movement marker";
        //   nothing          - a static FAR character ("far NPCs never
        //                      render" - statistical residents stay
        //                      invisible; their settlement markers already
        //                      stand in the scene).
        if (c.tier != kTierFar)
        {
            d.kind = planarDistance(c.position, anchor) <= CHARACTER_DETAIL_RADIUS_M ? 3 : 1;
        } else if (c.moving)
        {
            d.kind = 2; // FAR movement marker
        } else
        {
            d.kind = 0; // static FAR: no object
        }
        desired_characters_.emplace(c.id, d);
    }

    // ---- 2. Despawn what the desired set no longer carries ----------------
    // (tier flipped to static FAR, walked out of the presentation radius,
    // died -> the corpse path owns the visual now). Handles are removed via
    // the sink - never reused by the registry, dropped here unconditionally.
    for (auto it = character_objects_.begin(); it != character_objects_.end();)
    {
        const auto want = desired_characters_.find(it->first);
        const bool keep = want != desired_characters_.end() && want->second.kind == it->second.kind;
        if (!keep)
        {
            if (it->second.body != SceneRegistrySink::NULL_HANDLE)
            {
                sink_.removeObject(it->second.body);
            }
            if (it->second.ring != SceneRegistrySink::NULL_HANDLE)
            {
                sink_.removeObject(it->second.ring);
            }
            // The A-188 head goes with the body (the map entry is dropped;
            // the head map is the same lifecycle).
            if (const auto head = character_heads_.find(it->first); head != character_heads_.end())
            {
                if (head->second != SceneRegistrySink::NULL_HANDLE)
                {
                    sink_.removeObject(head->second);
                }
                character_heads_.erase(head);
            }
            // rev 26: the blocky body's extra parts ride the same lifecycle.
            if (it->second.legs != SceneRegistrySink::NULL_HANDLE)
            {
                sink_.removeObject(it->second.legs);
            }
            if (it->second.hair != SceneRegistrySink::NULL_HANDLE)
            {
                sink_.removeObject(it->second.hair);
            }
            character_traveled_.erase(it->first);
            character_last_position_.erase(it->first);
            it = character_objects_.erase(it);
        } else
        {
            ++it;
        }
    }

    // ---- 3. Queue creations for the missing (id-ascending order) ----------
    for (const auto &[id, d] : desired_characters_)
    {
        if (d.kind == 0)
            continue;
        if (character_objects_.count(id) != 0)
            continue;
        pending_character_creations_.push_back({/*is_corpse=*/false, id});
    }

    // ---- 4. Move everything that IS live (batched per frame) --------------
    // set_object_position per moved object; the registry coalesces the edits
    // into one GPU refresh (the module README's cost model). The A-188 body
    // composition rides here: the bob (A-154, phase from traveled distance)
    // lifts both parts, the head rides the body at the A-153 silhouette
    // height, offset forward along the yaw the movement direction implies.
    for (const auto &[id, d] : desired_characters_)
    {
        auto it = character_objects_.find(id);
        if (it == character_objects_.end())
        {
            continue;
        }
        // The traveled-distance accumulator (presentation-only state) and
        // the per-frame movement delta (the yaw source: A-154's phase and
        // A-153's facing come from how the character MOVED, never sim
        // state - the visual is composed AFTER reading the sim position).
        Position velocityDelta{0.0, 0.0, 0.0};
        double traveled = 0.0;
        double movedThisFrame = 0.0;
        if (const auto last = character_last_position_.find(id); last != character_last_position_.end())
        {
            velocityDelta = {d.position.x - last->second.x, d.position.y - last->second.y,
                d.position.z - last->second.z};
            movedThisFrame = planarDistance(d.position, last->second);
            traveled      = character_traveled_[id] + movedThisFrame;
        }
        character_last_position_[id] = d.position;
        character_traveled_[id]      = traveled;

        const CharacterVisual visual = computeCharacterVisual({d.species, d.position, velocityDelta, traveled,
            d.moving, false});
        const float bob              = static_cast<float>(visual.bobOffsetM);
        if (it->second.kind == 3)
        {
            // rev 26 - the blocky near body: the three CC0 mesh parts share
            // the character's ground point (their vertices are pre-grounded
            // and pre-offset), so one pose update per part with the facing
            // yaw and the bob lift moves the whole villager. The hair cap
            // (an analytic box) rides at its variant's head-top height. The
            // pose path carries the yaw - the position-only updates would
            // reset every villager to face one way after the first step.
            const float yaw        = static_cast<float>(visual.yawRad);
            const float lift       = 0.02f; // clear of the plaza ground boxes
            Position basePos       = d.position;
            basePos.y += lift + bob;

            // rev 27 - the gait swap: a moving villager alternates the two
            // baked stride poses (whole-mesh swaps; the renderer's mesh
            // instances translate + rotateY only, so there is no skeletal
            // path to drive). A villager whose frame did not move settles
            // back into the rest pose instead of freezing mid-stride. The
            // swap is spawn-THEN-remove: a failed spawn (the A-130 object
            // budget) keeps the old pose, and a successful one never leaves
            // the villager invisible for a frame. The dyes recompute from
            // the id - characterLookFor is pure (D-IMG-2), so the swap
            // cannot drift from the creation look.
            const std::uint8_t wantGait =
                gaitPoseBucket(traveled, d.moving || movedThisFrame > 1e-5);
            if (wantGait != it->second.gaitPose && it->second.body != SceneRegistrySink::NULL_HANDLE)
            {
                const CharacterLook lookSwap = characterLookFor(id, speciesColor(d.species));
                const int scaleIdxSwap = it->second.smallScale > 0.9f ? 0 : 1;
                const std::uint32_t variantSwap = it->second.bodyVariant % 3u;
                const char *bodyName = wantGait == 0
                    ? kNpcBodyVariants[variantSwap][scaleIdxSwap]
                    : kNpcBodyWalkVariants[wantGait - 1u][variantSwap][scaleIdxSwap];
                const char *legsName = wantGait == 0
                    ? kNpcLegsVariants[variantSwap][scaleIdxSwap]
                    : kNpcLegsWalkVariants[wantGait - 1u][variantSwap][scaleIdxSwap];
                const SceneRegistrySink::Handle newBody = sink_.spawnPropMeshInstance(
                    bodyName, basePos, yaw, lookSwap.clothing, "", NPC_MESH_UV_SCALE);
                const SceneRegistrySink::Handle newLegs = sink_.spawnPropMeshInstance(
                    legsName, basePos, yaw, lookSwap.pants, "", NPC_MESH_UV_SCALE);
                if (newBody != SceneRegistrySink::NULL_HANDLE && newLegs != SceneRegistrySink::NULL_HANDLE)
                {
                    if (it->second.body != SceneRegistrySink::NULL_HANDLE)
                    {
                        sink_.removeObject(it->second.body);
                    }
                    if (it->second.legs != SceneRegistrySink::NULL_HANDLE)
                    {
                        sink_.removeObject(it->second.legs);
                    }
                    it->second.body     = newBody;
                    it->second.legs     = newLegs;
                    it->second.gaitPose = wantGait;
                }
            }
            sink_.setObjectPose(it->second.body, basePos, yaw);
            if (it->second.legs != SceneRegistrySink::NULL_HANDLE)
            {
                sink_.setObjectPose(it->second.legs, basePos, yaw);
            }
            if (it->second.head != SceneRegistrySink::NULL_HANDLE)
            {
                sink_.setObjectPose(it->second.head, basePos, yaw);
            }
            if (it->second.hair != SceneRegistrySink::NULL_HANDLE)
            {
                // rev 28: the hair is a pre-offset mesh part now - it rides
                // the same base pose as the body/legs/head.
                sink_.setObjectPose(it->second.hair, basePos, yaw);
            }
            if (it->second.ring != SceneRegistrySink::NULL_HANDLE)
            {
                Position ringPos  = d.position;
                ringPos.y += FACTION_RING_RADIUS + bob;
                sink_.setObjectPosition(it->second.ring, ringPos);
            }
        } else if (it->second.kind == 1)
        {
            // The per-character girth jitter (character_look.hpp) rides here:
            // the spawn stored it in the CharacterObject, the move loop keeps
            // applying it so the crowd's silhouettes vary.
            const float r    = it->second.bodyRadius;
            Position bodyPos = d.position;
            bodyPos.y += r + bob; // rest on the ground plane, lifted by the bob
            sink_.setObjectPosition(it->second.body, bodyPos);
            // The head: forward along the facing, on top of the silhouette
            // (the height jitter stretches the A-153 ratio per character).
            const double yaw        = visual.yawRad;
            const float forwardX    = static_cast<float>(std::sin(yaw)) * r * CHARACTER_HEAD_OFFSET_LR;
            const float forwardZ    = static_cast<float>(-std::cos(yaw)) * r * CHARACTER_HEAD_OFFSET_LR;
            const auto silhouette   = speciesSilhouette(d.species);
            Position headPos        = d.position;
            headPos.x += forwardX;
            headPos.z += forwardZ;
            headPos.y += r * (1.0 + silhouette.heightRatio * 0.75 * it->second.heightJitter) + bob;
            if (const auto head = character_heads_.find(id); head != character_heads_.end() && head->second != SceneRegistrySink::NULL_HANDLE)
            {
                sink_.setObjectPosition(head->second, headPos);
            }
        } else
        {
            // The far marker's jittered half extent rides in bodyRadius (the
            // field holds "the body size of this character's object", kind
            // decides the shape).
            Position bodyPos = d.position;
            bodyPos.y += it->second.bodyRadius;
            sink_.setObjectPosition(it->second.body, bodyPos);
        }
        if (it->second.ring != SceneRegistrySink::NULL_HANDLE)
        {
            Position ringPos = d.position;
            ringPos.y += FACTION_RING_RADIUS + bob;
            sink_.setObjectPosition(it->second.ring, ringPos);
        }
    }
}

void SceneBridge::syncCorpses()
{
    if (characters_ == nullptr)
        return;

    desired_corpses_.clear();
    const Position corpseAnchor = characters_->anchor();
    for (const auto &c : characters_->presentCorpses())
    {
        if (planarDistance(c.position, corpseAnchor) > PRESENTATION_RADIUS_M)
        {
            continue; // decayed out of the presentation window or stale view row
        }
        CharactersViewForBridge::CorpseDesc lifted = c;
        if (terrain_ != nullptr)
        {
            lifted.position.y = terrain_->heightAt(lifted.position.x, lifted.position.z);
        }
        desired_corpses_.emplace(c.victim, lifted);
    }

    // Despawn decayed/gone corpses (3.3's CORPSE_DECAY_DAYS sweep removed
    // them from the store; the view stopped returning them).
    for (auto it = corpse_objects_.begin(); it != corpse_objects_.end();)
    {
        if (desired_corpses_.count(it->first) == 0)
        {
            if (it->second != SceneRegistrySink::NULL_HANDLE)
            {
                sink_.removeObject(it->second);
            }
            it = corpse_objects_.erase(it);
        } else
        {
            ++it;
        }
    }

    // Queue the missing.
    for (const auto &[victim, desc] : desired_corpses_)
    {
        if (corpse_objects_.count(victim) != 0)
            continue;
        pending_character_creations_.push_back({/*is_corpse=*/true, victim});
    }
}

void SceneBridge::drainCharacterCreations()
{
    if (pending_character_creations_.empty())
        return;

    std::uint32_t processed = 0;
    while (!pending_character_creations_.empty() && processed < SCENE_CREATE_CHUNK_PER_FRAME)
    {
        const CharacterCreationTask task = pending_character_creations_.front();
        pending_character_creations_.erase(pending_character_creations_.begin());
        ++processed;

        if (task.is_corpse)
        {
            const auto want = desired_corpses_.find(task.id);
            if (want == desired_corpses_.end() || corpse_objects_.count(task.id) != 0)
                continue;
            Position pos = want->second.position;
            pos.y += CORPSE_BOX_HALF_Y; // flattened onto the ground
            const SceneRegistrySink::Handle h
                = sink_.spawnBox(pos, CORPSE_BOX_HALF_X, CORPSE_BOX_HALF_Y * 2.0f, TINT_CORPSE);
            corpse_objects_.emplace(task.id, h);
        } else
        {
            const auto want = desired_characters_.find(task.id);
            if (want == desired_characters_.end() || character_objects_.count(task.id) != 0)
                continue;
            const CharacterDesired &d = want->second;

            // The per-character randomized look (character_look.hpp): the
            // tunic dye, the jittered head tone around the species base, the
            // height/girth jitters. Pure from (id, species) — same id, same
            // clothes, every session.
            const CharacterLook look = characterLookFor(task.id, speciesColor(d.species));

            CharacterObject object;
            object.kind         = d.kind;
            object.bodyRadius   = CHARACTER_SPHERE_RADIUS * look.girthJitter;
            object.heightJitter = look.heightJitter;
            Position pos = d.position;
            if (d.kind == 3)
            {
                // rev 28 - the organic near body (the procedural rounded
                // humanoids of prop_mesh_data_cc0.h: capsule limbs, ellipsoid
                // head, smooth welded normals - the owner's "made out of
                // blocks" defect). The parts are pre-grounded at y=0 and
                // pre-offset (legs at the feet, torso over them, head and
                // hair on top), so every part instances from the SAME base
                // point and one pose update per part moves the villager. The
                // variant pick + the baked lg/sm scale are hash draws (the
                // crowd's silhouettes vary); the per-part dyes are the look's
                // (tunic / pants / skin / hair).
                const std::uint64_t scaleHash = propMeshSeed(
                    static_cast<double>(task.id) * 1.6180339887, static_cast<double>(task.id) * 0.7071067812);
                const bool small = (scaleHash % 100u) < 34u; // ~1/3 wear the small build
                object.smallScale = small ? 0.88f : 1.0f;
                const std::uint32_t variant = look.bodyVariant % 3u;
                object.bodyVariant = variant;
                const int scaleIdx = small ? 1 : 0;
                const float lift   = 0.02f; // clear of the plaza ground boxes
                Position base      = d.position;
                base.y += lift;
                // rev 26: the parts spawn with NO texture set - the raster
                // path's flat fallback renders the raw albedo at FULL
                // strength (the A-189 55% tinted-photo mix only applies to
                // textured objects, and it washed every medieval dye to
                // pastel salmon on the torsos). The blocky style is
                // flat-colored anyway; the weave reads at prop scale, not
                // on 1.8 m figures.
                object.body = sink_.spawnPropMeshInstance(
                    kNpcBodyVariants[variant][scaleIdx], base, 0.0f, look.clothing, "", NPC_MESH_UV_SCALE);
                object.legs = sink_.spawnPropMeshInstance(
                    kNpcLegsVariants[variant][scaleIdx], base, 0.0f, look.pants, "", NPC_MESH_UV_SCALE);
                object.head = sink_.spawnPropMeshInstance(
                    kNpcHeadVariants[variant][scaleIdx], base, 0.0f, look.headTone, "", NPC_MESH_UV_SCALE);
                // rev 28: the hair cap is a MESH now (a rounded shell over
                // the ellipsoid head, three styles) - the analytic box read
                // as a brick on the rounded heads. It instances from the
                // same base point like every other part.
                object.hair = sink_.spawnPropMeshInstance(
                    kNpcHairVariants[variant][scaleIdx], base, 0.0f, look.hairTone, "", NPC_MESH_UV_SCALE);
                // The faction ring (optional at the seam; the sink may drop
                // it under the A-130 object budget).
                if (d.faction != 0 && object.body != SceneRegistrySink::NULL_HANDLE)
                {
                    const auto &palette                 = FACTION_RING_COLORS;
                    const std::array<float, 3> ringTint = palette[(d.faction - 1) % palette.size()];
                    Position ringPos                    = d.position;
                    ringPos.y += FACTION_RING_RADIUS;
                    object.ring = sink_.spawnRing(ringPos, FACTION_RING_RADIUS, ringTint);
                }
            } else if (d.kind == 1)
            {
                pos.y += object.bodyRadius;
                // The body wears the tunic dye (was: the one species tint —
                // the uniform-crowd defect). The species read stays on the
                // head's jittered tone.
                object.body = sink_.spawnSphere(pos, object.bodyRadius, look.clothing);
                // The A-188 head: a skin-tone sphere on top of the silhouette
                // (spawned with the body; the move loop drives both).
                const auto silhouette = speciesSilhouette(d.species);
                Position headPos      = d.position;
                headPos.y += object.bodyRadius * (1.0 + silhouette.heightRatio * 0.75 * look.heightJitter);
                const auto head = sink_.spawnSphereTextured(
                    headPos, object.bodyRadius * CHARACTER_HEAD_RADIUS, look.headTone, "skin", 1.0f);
                character_heads_.emplace(task.id, head);
                // The faction ring (optional at the seam; the sink may drop
                // it under the A-130 object budget).
                if (d.faction != 0 && object.body != SceneRegistrySink::NULL_HANDLE)
                {
                    const auto &palette                 = FACTION_RING_COLORS;
                    const std::array<float, 3> ringTint = palette[(d.faction - 1) % palette.size()];
                    Position ringPos                    = d.position;
                    ringPos.y += FACTION_RING_RADIUS;
                    object.ring = sink_.spawnRing(ringPos, FACTION_RING_RADIUS, ringTint);
                }
            } else
            {
                // The far marker keeps the cheap box LOD but drops the one
                // shared grey tint: it wears the character's tunic dye at a
                // jittered size, so a distant crowd reads as villagers, not
                // a stamp of identical silhouettes (the owner's defect).
                const float half = FAR_MARKER_BOX_HALF_EXTENT * (0.8f + 0.5f * look.girthJitter);
                object.bodyRadius = half; // the move loop's lift reads it
                pos.y += half;
                object.body = sink_.spawnBox(pos, half, half * 2.0f, look.clothing);
            }
            character_objects_.emplace(task.id, object);
        }
    }
}

void SceneBridge::updateAtmosphere()
{
    // The atmosphere module owns the sun/sky/fog/exposure math (5.1.4); the
    // bridge samples it once per frame and pushes through the sink. The
    // pinned Phase-2 contracts ride unchanged: the sun pushes on the same
    // >= 1% phase delta with the same [0, 1] intensity (noon 1.0, midnight
    // 0.0 - the pinned tests), the sky tint pushes on a weather change, and
    // both set the same per-frame flags.
    const double phase = clock_.dayPhase01();

    const auto mapWeather = [](world::Weather w) {
        switch (w)
        {
        case world::Weather::CLEAR:
            return AtmosphereWeather::CLEAR;
        case world::Weather::RAIN:
            return AtmosphereWeather::RAIN;
        case world::Weather::STORM:
            return AtmosphereWeather::STORM;
        case world::Weather::SNOW:
            return AtmosphereWeather::SNOW;
        case world::Weather::FOG:
            return AtmosphereWeather::FOG;
        default:
            return AtmosphereWeather::CLEAR;
        }
    };

    AtmosphereInput input;
    input.dayPhase01 = phase;
    input.weather    = mapWeather(world_.primaryWeather());
    input.season     = static_cast<AtmosphereSeason>(std::clamp(world_.seasonIndex(), 0u, 3u));
    const AtmosphereSample sample = sampleAtmosphere(input);

    // ---- sun (the pinned >= 1% delta threshold) ---------------------------
    bool pushSun = false;
    if (last_sun_phase_ < 0.0)
    {
        pushSun = true; // first frame always pushes
    } else
    {
        const double delta   = std::fabs(phase - last_sun_phase_);
        const double wrapped = std::min(delta, 1.0 - delta);
        pushSun              = wrapped >= SUN_PHASE_UPDATE_THRESHOLD;
    }
    if (pushSun)
    {
        if (sample.sunIntensity > SUN_ELEVATION_EPSILON)
        {
            sink_.setSunLight(sample.sunDirection, sample.sunIntensity);
        } else
        {
            // Night: the moon rides the sun channel as the directional light.
            // The uplift round drew the moon as a sky disk only - the relight
            // pass's sun channel went dark at dusk and every surface read pure
            // black (the 2026-11-05 spawn_night / corpse_aftermath captures).
            // Riding the channel gives the night a real, dim, cool key light
            // through the exact path the shadow rays already trace.
            sink_.setSunLight(MOON_DIRECTION, MOON_IRRADIANCE);
        }
        last_sun_phase_         = phase;
        sun_updated_last_frame_ = true;
    }

    // ---- sky tint (the A-117 weather table, on a weather change) ----------
    // The pinned A-117 contract keeps its exact semantics: the tint is the
    // weather table's value, pushed only when the weather state flips. The
    // A-155 time-of-day gradient rides the environment state below (the
    // renderer merges it over the static base; the A-117 tint stays the
    // bridge's own sky-radiance surface).
    const world::Weather currentWeather = world_.primaryWeather();
    if (currentWeather != last_sky_weather_)
    {
        sink_.setSkyTint(skyTintFor(currentWeather));
        last_sky_weather_            = currentWeather;
        sky_tint_updated_last_frame_ = true;
    }

    // ---- the environment state (gradient + fog + exposure + moon) ----------
    // The A-155 tint is the gradient's base: the horizon leans warm (the
    // pastel look's soft haze), the ground half-stops darker. The moon takes
    // over as the visible disk (and the adapter takes over as the
    // directional light) under the A-186 elevation epsilon.
    SceneEnvironmentState env;
    env.skyZenith = sample.skyTint;
    // rev 24: the horizon stops washing to near-white. The old 65/35
    // tint-to-warm blend lifted noon's horizon to a pale grey that read as
    // overcast even under the clear anchor; 82/18 keeps the warm haze as a
    // BAND at the horizon and leaves the blue in the mid sky. Rev 26: the
    // warm share drops 18% -> 12% - with the afternoon anchor exponent
    // deepened the band was still painting every 15-16h sky mauve.
    const std::array<float, 3> warmHorizon{1.0f, 0.88f, 0.68f};
    for (std::size_t i = 0; i < 3; ++i)
    {
        env.skyHorizon[i] = sample.skyTint[i] * 0.88f + warmHorizon[i] * 0.12f;
        env.skyGround[i]  = sample.skyTint[i] * 0.42f;
    }
    env.fogDensity = sample.fogDensity;
    env.fogTint    = sample.fogTint;
    env.exposure   = static_cast<float>(sample.exposureTarget);
    if (sample.sunIntensity <= SUN_ELEVATION_EPSILON)
    {
        // Night: the moon is the visible disk; the sun disk's intensity
        // drops to zero (the sky shader's disk enablement keys on > 0).
        // The env moon vector is the LOOK direction (the sky shader dots the
        // view ray against it, like the sun disk's negated direction) - the
        // uplift round stored the travel direction, which hung the disk below
        // the horizon where no camera could see it.
        env.moonDirection = {-MOON_DIRECTION[0], -MOON_DIRECTION[1], -MOON_DIRECTION[2]};
        env.moonIntensity = MOON_IRRADIANCE;
        env.sunIntensity  = 0.0f;
        env.sunColor      = {0.70f, 0.78f, 0.95f};
    } else
    {
        env.sunIntensity = sample.sunIntensity * SUN_IRRADIANCE_SCALE;
        // rev 25 (the drama round): the golden hour deepened. The old curve
        // sat at near-white (0.97/0.90) from mid-morning on, so every
        // capture keyed the scene with the same neutral sheet. The key now
        // leans amber across the whole day: (1.0, 0.92, 0.80) at high sun,
        // (1.0, 0.58, 0.28) at the horizon - warm light against the cool
        // sky is the contrast the "lighting is boring" verdict named.
        const float elevation = -sample.sunDirection[1]; // the travel direction points down; y = -height
        const float t         = std::clamp((elevation - 0.10f) / 0.40f, 0.0f, 1.0f);
        const float curve     = t * t * (3.0f - 2.0f * t);
        env.sunColor = {1.0f, 0.58f + 0.34f * curve, 0.28f + 0.52f * curve};
        // rev 26: the visible disk follows the light (the zero default kept
        // the static [scene].sun direction forever - the coast staging
        // framed an empty sky with the glitter pointing at nothing).
        env.sunDirection = sample.sunDirection;
        env.moonIntensity = 0.0f;
    }

    const bool pushEnvironment = !environment_pushed_ || pushSun
        || env.fogDensity != last_environment_.fogDensity || currentWeather != last_environment_weather_;
    if (pushEnvironment)
    {
        sink_.setEnvironmentState(env);
        last_environment_         = env;
        environment_pushed_       = true;
        last_environment_weather_ = currentWeather;
    }
}

void SceneBridge::publishWaterIfNeeded()
{
    if (water_published_ || borders_ == nullptr)
    {
        return;
    }
    water_published_ = true;
    // One plane at the A-151 sea level, the size of the world patch (the
    // mesh reads as ocean wherever the heightfield dips below the datum).
    water_handle_ = sink_.spawnWaterPlane(TERRAIN_WORLD_EXTENT_M);
}

void SceneBridge::registerPropMeshVocabulary()
{
    // rev 26 - the vocabulary is the CONVERTED KENNEY CC0 MODELS
    // (prop_mesh_data_cc0.h; Nature Kit + Blocky Characters + Fantasy Town
    // Kit, all CC0): the owner's "can't you use proper tree models?" asked
    // for real downloaded models, and rev 24's procedural branching meshes
    // still read as stylized blobs beside them. The tree slots keep the
    // EXACT kMesh* name strings the scatter field's species pick already
    // uses (oak/pine/birch trunk+canopy, small variants, the dead tree), so
    // the placement code is untouched - only the geometry behind the names
    // changes. Three blocky-character part sets (body/legs/head x three
    // variants x two baked scales) give the crowd real models with
    // per-part randomized dyes; the market props dress the village square.
    // Registration is idempotent per name in the adapter; a NULL handle
    // (fakes, a full mesh cap) leaves that name unavailable - the scatter
    // and character paths fall back exactly as before.
    for (const Cc0MeshRow &row : CC0_MESHES)
    {
        sink_.registerPropMesh(
            row.name,
            std::vector<float>(row.vertices, row.vertices + row.vertexFloats),
            std::vector<std::uint32_t>(row.indices, row.indices + row.indexCount)
        );
    }
    // The dead tree has no Kenney counterpart - the rev-24 procedural bare
    // trunk stays (it reads correctly against the CC0 canopy shapes).
    {
        const PropMeshData data = buildDeadTree(0xD1u, 1.00f);
        sink_.registerPropMesh(kMeshDeadTree, data.vertices, data.indices);
    }
    // rev 31 (the inhabited-land round): the infrastructure vocabulary - the
    // road slab, the crop strip, the pier plank, the boat hull (generated by
    // scripts/generate_infra_meshes.py, flat-shaded boxes in the same
    // registered format). The scatter field's road/field/dock placements
    // instance these by name.
    for (const InfraMeshRow &row : INFRA_MESHES)
    {
        sink_.registerPropMesh(
            row.name,
            std::vector<float>(row.vertices, row.vertices + row.vertexFloats),
            std::vector<std::uint32_t>(row.indices, row.indices + row.indexCount)
        );
    }
    // rev 34 (the character-customisation module): the part vocabulary - the
    // downloaded CC0 donor nodes (the parrot's wings/tail/head) + the
    // authored organic-style parts (canine head, pelvis, digitigrade legs,
    // torso, arms). The assembly lab instances these by name.
    for (const ChimeraMeshRow &row : CHIMERA_MESHES)
    {
        sink_.registerPropMesh(
            row.name,
            std::vector<float>(row.vertices, row.vertices + row.vertexFloats),
            std::vector<std::uint32_t>(row.indices, row.indices + row.indexCount)
        );
    }
}

void SceneBridge::spawnPropComposition(
    const ScatterPlacement &placement
)
{
    // One prop's primitive composition through the sink. The pastel palette
    // (A-187) names every tint and texture set; the box/sphere vocabulary is
    // what the tracer and the relight path already handle exactly. The
    // placement's y is the terrain height (the field guarantees it); every
    // part sits relative to that ground point.
    const ScatterTables &tables = scatter_->tables();
    const ScatterProp &prop     = tables.props[placement.propIndex];
    const float s               = static_cast<float>(placement.scaleM);
    const float ground          = static_cast<float>(placement.position.y);
    const Position base{placement.position.x, 0.0, placement.position.z}; // y composed per part
    ScatterObjects objects;

    auto track = [&objects](SceneRegistrySink::Handle h) {
        if (h != SceneRegistrySink::NULL_HANDLE)
        {
            objects.parts.push_back(h);
        }
    };

    if (prop.key == "house")
    {
        // rev 24: three cottage styles keyed on the placement hash (the one
        // plaster-box-with-flat-roof design read as a depot of identical
        // sheds). Every style keeps the box vocabulary the tracer handles:
        //   - plaster cottage (the classic cream walls),
        //   - stone cottage (quarry stone, always a slate roof),
        //   - timber-framed (cream walls + four corner posts + a lintel).
        // All gain a chimney and a front door so the silhouette stops
        // reading as a bare slab.
        const std::uint64_t seed = propMeshSeed(placement.position.x, placement.position.z);
        const double pick        = static_cast<double>(seed % 1000u) / 1000.0;
        const bool stoneHouse    = pick < 0.25;
        const bool timberHouse   = pick >= 0.25 && pick < 0.55;
        const bool slateRoof     = stoneHouse || static_cast<double>((seed >> 7) % 1000u) / 1000.0 < 0.45;

        const float wallH = s * 0.76f;
        const std::array<float, 3> wallTint =
            stoneHouse ? paletteColor("stone") : paletteColor("plaster");
        const char *wallSet = stoneHouse ? "stone" : "plaster";
        track(sink_.spawnBoxTextured(
            {base.x, ground + wallH * 0.5f, base.z}, s * 0.46f, wallH * 0.5f, wallTint, wallSet,
            s * 0.55f, 0.0f));
        // The timber frame: four corner posts + a front lintel (the
        // medieval look the theme pins).
        if (timberHouse)
        {
            const float half  = s * 0.46f;
            const float postH = wallH * 0.5f;
            const float postOff = half - s * 0.04f;
            track(sink_.spawnBoxTextured(
                {base.x - postOff, ground + postH, base.z - postOff}, s * 0.035f, postH, paletteColor("timber"),
                "timber", 1.5f, 0.0f));
            track(sink_.spawnBoxTextured(
                {base.x + postOff, ground + postH, base.z - postOff}, s * 0.035f, postH, paletteColor("timber"),
                "timber", 1.5f, 0.0f));
            track(sink_.spawnBoxTextured(
                {base.x - postOff, ground + postH, base.z + postOff}, s * 0.035f, postH, paletteColor("timber"),
                "timber", 1.5f, 0.0f));
            track(sink_.spawnBoxTextured(
                {base.x + postOff, ground + postH, base.z + postOff}, s * 0.035f, postH, paletteColor("timber"),
                "timber", 1.5f, 0.0f));
            track(sink_.spawnBoxTextured(
                {base.x, ground + wallH * 0.96f, base.z + postOff}, half, s * 0.03f, paletteColor("timber"),
                "timber", 1.5f, 0.0f));
        }
        // The roof: a two-tier hip (deep eaves slab + a narrower ridge cap)
        // replaces the flat slab - the flat roofs read as factories.
        const char *roofSet  = slateRoof ? "roof_slate" : "roof_terra";
        const auto roofTint  = slateRoof ? paletteColor("roof_slate") : paletteColor("roof_terra");
        const float roofDrop = s * 0.20f;
        track(sink_.spawnBoxTextured(
            {base.x, ground + wallH + roofDrop * 0.35f, base.z}, s * 0.55f, roofDrop * 0.40f, roofTint,
            roofSet, s * 0.5f, 0.0f));
        track(sink_.spawnBoxTextured(
            {base.x, ground + wallH + roofDrop * 0.95f, base.z}, s * 0.30f, roofDrop * 0.42f, roofTint,
            roofSet, s * 0.5f, 0.0f));
        // The chimney (stone, on the ridge) + the front door.
        track(sink_.spawnBoxTextured(
            {base.x + s * 0.28f, ground + wallH + roofDrop + s * 0.10f, base.z - s * 0.08f}, s * 0.06f,
            s * 0.18f, paletteColor("stone"), "stone", s * 0.8f, 0.0f));
        track(sink_.spawnBoxTextured(
            {base.x, ground + s * 0.22f, base.z + s * 0.47f}, s * 0.09f, s * 0.22f, paletteColor("timber"), "timber",
            1.5f, 0.0f));
        const float winY  = ground + wallH * 0.62f;
        const float winZ  = s * 0.47f;
        const float winDX = s * 0.26f;
        track(sink_.spawnBoxTextured(
            {base.x - winDX, winY, base.z + winZ}, s * 0.09f, s * 0.11f, paletteColor("window_lit"), "window_lit",
            1.0f, 0.55f));
        track(sink_.spawnBoxTextured(
            {base.x + winDX, winY, base.z + winZ}, s * 0.09f, s * 0.11f, paletteColor("window_lit"), "window_lit",
            1.0f, 0.55f));
    } else if (prop.key == "pine")
    {
        // rev 24 - the proper tree models: a registered branching trunk + a
        // layered/blob canopy per species, instanced from the prop-mesh
        // vocabulary (prop_meshes.hpp). The species pick is a pure hash of
        // the placement (oak 45% / pine 35% / birch 20%); the size variant
        // reads the authored scaleM (below 6 m -> the small variant). A
        // refused registration (NULL handle) falls back to the old analytic
        // lollipop so the world never loses its trees.
        const std::uint64_t seed = propMeshSeed(placement.position.x, placement.position.z);
        const double pick = static_cast<double>(seed % 1000u) / 1000.0;
        const bool small  = placement.scaleM < 6.0;
        const char *trunkName;
        const char *canopyName;
        std::array<float, 3> canopyBase;
        if (pick < 0.45)
        {
            trunkName  = small ? kMeshOakTrunkSm : kMeshOakTrunkLg;
            canopyName = small ? kMeshOakCanopySm : kMeshOakCanopyLg;
            canopyBase = {0.560f, 0.610f, 0.270f}; // rev 25: lifted — the CC0 leaf map is dark
                                                   // (linear mean ~0.05/0.13/0.02); the old
                                                   // 0.30/0.42/0.15 tint left canopies near-black blobs
        } else if (pick < 0.80)
        {
            trunkName  = small ? kMeshPineTrunkSm : kMeshPineTrunkLg;
            canopyName = small ? kMeshPineCanopySm : kMeshPineCanopyLg;
            canopyBase = {0.340f, 0.480f, 0.220f}; // rev 25: lifted, see the oak note
        } else
        {
            trunkName  = small ? kMeshBirchTrunkSm : kMeshBirchTrunkLg;
            canopyName = small ? kMeshBirchCanopySm : kMeshBirchCanopyLg;
            canopyBase = {0.620f, 0.640f, 0.300f}; // rev 25: lifted, see the oak note
        }
        const float yaw = static_cast<float>(placement.rotationRad);
        // Per-tree tint jitter (±18% luminance, warm/cool swing) — a stand
        // of trees stops reading as one stamp.
        const float jitter = 0.82f + 0.36f * static_cast<float>((seed >> 17) % 1000u) / 1000.0f;
        const float warm   = 0.85f + 0.30f * static_cast<float>((seed >> 29) % 1000u) / 1000.0f;
        const std::array<float, 3> canopyTint{
            std::clamp(canopyBase[0] * jitter * (2.0f - warm), 0.0f, 1.0f),
            std::clamp(canopyBase[1] * jitter, 0.0f, 1.0f),
            std::clamp(canopyBase[2] * jitter * warm, 0.0f, 1.0f)};
        const std::array<float, 3> trunkTint{0.74f * jitter, 0.58f * jitter, 0.40f * jitter};
        const Position treeBase{placement.position.x, ground, placement.position.z};
        const bool trunkOk = sink_.spawnPropMeshInstance(trunkName, treeBase, yaw, trunkTint, "bark", 1.4f)
                             != SceneRegistrySink::NULL_HANDLE;
        if (trunkOk)
        {
            sink_.spawnPropMeshInstance(canopyName, treeBase, yaw, canopyTint, "leaf", 2.2f);
        } else
        {
            // The analytic fallback (the vocabulary was refused - fakes, or
            // a full mesh cap): the old lollipop keeps the canopy line.
            track(sink_.spawnBoxTextured(
                {base.x, ground + s * 0.24f, base.z}, s * 0.06f, s * 0.24f, paletteColor("wood"), "wood", 1.5f, 0.0f));
            track(sink_.spawnSphereTextured(
                {base.x, ground + s * 0.66f, base.z}, s * 0.30f, paletteColor("grass"), "grass", s * 0.6f));
        }
    } else if (prop.key == "boulder")
    {
        // A cluster of 2-3 partially sunk spheres (the single-sphere boulder
        // read as a dropped ball). Analytic spheres - the tracer sees them,
        // so boulders cast the shadows trees cannot.
        const std::uint64_t seed = propMeshSeed(placement.position.x, placement.position.z);
        const int count = 2 + static_cast<int>(seed % 2u);
        for (int b = 0; b < count; ++b)
        {
            const std::uint64_t bh = seed >> (b * 9u);
            const double angle  = static_cast<double>((bh % 360u)) * 0.017453292519943295;
            const double spread = 0.35 * s * static_cast<double>(b);
            const double r = s * (0.34 + 0.14 * static_cast<double>((bh >> 8) % 100u) / 100.0);
            track(sink_.spawnSphereTextured(
                {base.x + static_cast<float>(std::cos(angle) * spread),
                 ground + static_cast<float>(r) * 0.52f,
                 base.z + static_cast<float>(std::sin(angle) * spread)},
                static_cast<float>(r), paletteColor("stone"), "stone", s * 0.8f));
        }
    } else if (prop.key == "shrub")
    {
        track(sink_.spawnSphereTextured(
            {base.x, ground + s * 0.40f, base.z}, s * 0.5f, paletteColor("grass"), "grass", s * 0.5f));
    } else if (prop.key == "dead_tree")
    {
        // rev 24: the bare branchy trunk mesh (no canopy); the analytic
        // box stays the fallback.
        const float yaw = static_cast<float>(placement.rotationRad);
        const std::array<float, 3> trunkTint{0.46f, 0.35f, 0.24f};
        const Position deadBase{placement.position.x, ground, placement.position.z};
        if (sink_.spawnPropMeshInstance(kMeshDeadTree, deadBase, yaw, trunkTint, "bark", 1.4f)
            == SceneRegistrySink::NULL_HANDLE)
        {
            track(sink_.spawnBoxTextured(
                {base.x, ground + s * 0.38f, base.z}, s * 0.07f, s * 0.38f, paletteColor("timber"), "timber", 1.5f, 0.0f));
        }
    } else if (prop.key == "well")
    {
        // A village well: stone curb, timber windlass beam.
        track(sink_.spawnSphereTextured(
            {base.x, ground + s * 0.12f, base.z}, s * 0.34f, paletteColor("stone"), "stone", s * 0.8f));
        track(sink_.spawnBoxTextured(
            {base.x, ground + s * 0.42f, base.z}, s * 0.07f, s * 0.10f, paletteColor("timber"), "timber", 1.5f, 0.0f));
    } else if (prop.key == "crate")
    {
        track(sink_.spawnBoxTextured(
            {base.x, ground + s * 0.28f, base.z}, s * 0.28f, s * 0.28f, paletteColor("wood"), "wood", 1.5f, 0.0f));
    } else if (prop.key == "fence")
    {
        // rev 24: a fence BAY, not a lone post - two posts plus the two
        // horizontal rails between them (the bare posts read as stray
        // sticks). The bay runs along the placement's yaw.
        const float yaw   = static_cast<float>(placement.rotationRad);
        const float dirX  = std::cos(yaw);
        const float dirZ  = std::sin(yaw);
        const float span  = s * 1.6f;
        const float postH = s * 0.42f;
        track(sink_.spawnBoxTextured(
            {base.x - dirX * span, ground + postH, base.z - dirZ * span}, s * 0.05f, postH, paletteColor("timber"),
            "timber", 1.5f, 0.0f));
        track(sink_.spawnBoxTextured(
            {base.x + dirX * span, ground + postH, base.z + dirZ * span}, s * 0.05f, postH, paletteColor("timber"),
            "timber", 1.5f, 0.0f));
        const float railY = ground + postH * 0.62f;
        track(sink_.spawnBoxTextured(
            {base.x, railY + s * 0.16f, base.z}, span + s * 0.10f, s * 0.035f, paletteColor("timber"),
            "timber", 1.5f, 0.0f));
        track(sink_.spawnBoxTextured(
            {base.x, railY - s * 0.10f, base.z}, span + s * 0.10f, s * 0.035f, paletteColor("timber"),
            "timber", 1.5f, 0.0f));
    } else if (prop.key == "cart")
    {
        // A handcart: timber bed on two dark wheels.
        track(sink_.spawnBoxTextured(
            {base.x, ground + s * 0.30f, base.z}, s * 0.34f, s * 0.14f, paletteColor("wood"), "wood", 1.5f, 0.0f));
        track(sink_.spawnSphereTextured(
            {base.x - s * 0.22f, ground + s * 0.16f, base.z + s * 0.28f}, s * 0.16f, paletteColor("timber"), "timber", 1.5f));
        track(sink_.spawnSphereTextured(
            {base.x + s * 0.22f, ground + s * 0.16f, base.z + s * 0.28f}, s * 0.16f, paletteColor("timber"), "timber", 1.5f));
    } else if (prop.key == "lamp")
    {
        // A village lamp post: timber mast, a warm lantern head - the night
        // views get their light points.
        track(sink_.spawnBoxTextured(
            {base.x, ground + s * 0.55f, base.z}, s * 0.04f, s * 0.55f, paletteColor("timber"), "timber", 1.5f, 0.0f));
        track(sink_.spawnBoxTextured(
            {base.x, ground + s * 1.14f, base.z}, s * 0.09f, s * 0.09f, paletteColor("window_lit"), "window_lit", 1.0f, 0.85f));
    } else if (prop.key == "barrel")
    {
        // rev 24: a barrel - wood body with dark hoops, twice the visual
        // weight of a crate.
        track(sink_.spawnBoxTextured(
            {base.x, ground + s * 0.30f, base.z}, s * 0.20f, s * 0.30f, paletteColor("wood"), "wood", 1.5f, 0.0f));
        track(sink_.spawnBoxTextured(
            {base.x, ground + s * 0.12f, base.z}, s * 0.21f, s * 0.03f, paletteColor("timber"), "timber", 1.5f, 0.0f));
        track(sink_.spawnBoxTextured(
            {base.x, ground + s * 0.48f, base.z}, s * 0.21f, s * 0.03f, paletteColor("timber"), "timber", 1.5f, 0.0f));
    } else if (prop.key == "hay")
    {
        // rev 24: a hay pile - the ochre-stained grass texture reads as
        // stored fodder beside the cottages.
        track(sink_.spawnBoxTextured(
            {base.x, ground + s * 0.22f, base.z}, s * 0.34f, s * 0.22f, paletteColor("hay"), "grass", s * 0.5f, 0.0f));
        track(sink_.spawnSphereTextured(
            {base.x, ground + s * 0.44f, base.z}, s * 0.24f, paletteColor("hay"), "grass", s * 0.5f));
    } else if (prop.key == "campfire")
    {
        // rev 24: a campfire - a stone ring, two crossed logs, and a hot
        // ember core; syncLampLights gives it a warm night point light.
        for (int st = 0; st < 4; ++st)
        {
            const double angle = static_cast<double>(st) * 1.5707963267948966;
            track(sink_.spawnSphereTextured(
                {base.x + static_cast<float>(std::cos(angle)) * s * 0.30f, ground + s * 0.06f,
                 base.z + static_cast<float>(std::sin(angle)) * s * 0.30f},
                s * 0.09f, paletteColor("stone"), "stone", s * 0.8f));
        }
        track(sink_.spawnBoxTextured(
            {base.x, ground + s * 0.07f, base.z}, s * 0.26f, s * 0.05f, paletteColor("timber"), "timber", 1.5f, 0.0f));
        track(sink_.spawnBoxTextured(
            {base.x, ground + s * 0.16f, base.z}, s * 0.05f, s * 0.10f, paletteColor("ember"), "window_lit", 1.0f, 1.3f));
    } else if (prop.key == "stall")
    {
        // rev 26: the converted Kenney Fantasy Town market stall mesh (CC0)
        // replaces the post-slab composition; the canopy color (red/green)
        // is a placement-hash pick. The analytic posts-and-canopy stays as
        // the fallback when the mesh registry refuses (fakes, a full cap).
        const std::uint64_t seed = propMeshSeed(placement.position.x, placement.position.z);
        const char *stallMesh = ((seed >> 3) % 2u) == 0u ? "prop_stall_red" : "prop_stall_green";
        const float yaw = static_cast<float>(placement.rotationRad);
        const std::array<float, 3> stallTint{1.0f, 1.0f, 1.0f};
        if (sink_.spawnPropMeshInstance(stallMesh, base, yaw, stallTint, "timber", 1.4f)
            == SceneRegistrySink::NULL_HANDLE)
        {
            track(sink_.spawnBoxTextured(
                {base.x - s * 0.32f, ground + s * 0.42f, base.z}, s * 0.04f, s * 0.42f, paletteColor("timber"),
                "timber", 1.5f, 0.0f));
            track(sink_.spawnBoxTextured(
                {base.x + s * 0.32f, ground + s * 0.42f, base.z}, s * 0.04f, s * 0.42f, paletteColor("timber"),
                "timber", 1.5f, 0.0f));
            track(sink_.spawnBoxTextured(
                {base.x, ground + s * 0.88f, base.z}, s * 0.42f, s * 0.05f, paletteColor("roof_terra"), "roof_terra",
                s * 0.5f, 0.0f));
            track(sink_.spawnBoxTextured(
                {base.x - s * 0.15f, ground + s * 0.20f, base.z}, s * 0.14f, s * 0.20f, paletteColor("wood"),
                "wood", 1.5f, 0.0f));
        }
    } else if (prop.key == "plaza")
    {
        // rev 26: the market square's packed-dirt ground (the A-187 dirt
        // row). Flat boxes 2 cm thick compose an irregular plaza (~20 m
        // across) with two street strips heading north and east - the
        // village reads as a PLACE with streets, not furniture on grass.
        // The boxes are analytic: the tracer sees them (they receive the
        // sun with their own albedo), and 2 cm keeps every step's shadow
        // ray clear of self-shadowing. Rev 27: the boxes wear the NEW CC0
        // "dirt" set (A-205, PolyHaven park_dirt) - the old "grass" tint
        // capped the plaza at pale sand once A-204 let the sun actually
        // reach it; packed earth needs its own photo.
        const std::array<float, 3> dirt = paletteColor("dirt");
        track(sink_.spawnBoxTextured(
            {base.x, ground + 0.01f, base.z}, 9.0f, 0.01f, dirt, "dirt", 2.0f, 0.0f));
        track(sink_.spawnBoxTextured(
            {base.x + 6.5f, ground + 0.012f, base.z + 2.5f}, 6.5f, 0.012f, dirt, "dirt", 2.0f, 0.0f));
        track(sink_.spawnBoxTextured(
            {base.x - 6.0f, ground + 0.012f, base.z - 3.0f}, 6.0f, 0.012f, dirt, "dirt", 2.0f, 0.0f));
        // The two street stubs (8 m square patches north and east - the
        // spawnBox vocabulary is square-footprint; the overlap reads as one
        // paved shape, not tiles).
        track(sink_.spawnBoxTextured(
            {base.x, ground + 0.010f, base.z + 14.0f}, 4.0f, 0.010f, dirt, "dirt", 2.0f, 0.0f));
        track(sink_.spawnBoxTextured(
            {base.x + 14.0f, ground + 0.010f, base.z}, 4.0f, 0.010f, dirt, "dirt", 2.0f, 0.0f));
    } else if (prop.key == "tower")
    {
        // Rev 25 (the drama round): the settlement watchtower - the vertical
        // anchor every village view was missing. Stone shaft, an overhanging
        // batters, a timber-roofed look-out with corner parapets, a door and
        // a slit window. All analytic boxes: the tracer sees the tower
        // exactly (it shadows, it occludes GI rays), which is the point.
        const float shaftH  = 7.5f;
        track(sink_.spawnBoxTextured(
            {base.x, ground + shaftH * 0.5f, base.z}, 2.2f, shaftH * 0.5f, paletteColor("stone"), "stone", 1.6f, 0.0f));
        // The overhanging look-out chamber.
        track(sink_.spawnBoxTextured(
            {base.x, ground + shaftH + 1.4f, base.z}, 2.8f, 1.4f, paletteColor("stone"), "stone", 1.6f, 0.0f));
        // Corner parapets (four posts crowning the chamber).
        for (int corner = 0; corner < 4; ++corner)
        {
            const float ox = (corner % 2 == 0) ? -2.4f : 2.4f;
            const float oz = (corner < 2) ? -2.4f : 2.4f;
            track(sink_.spawnBoxTextured(
                {base.x + ox, ground + shaftH + 3.1f, base.z + oz}, 0.35f, 0.55f, paletteColor("stone"),
                "stone", 1.6f, 0.0f));
        }
        // The timber roof cap.
        track(sink_.spawnBoxTextured(
            {base.x, ground + shaftH + 4.0f, base.z}, 1.9f, 0.5f, paletteColor("roof_slate"), "roof_slate", 1.5f, 0.0f));
        track(sink_.spawnBoxTextured(
            {base.x, ground + shaftH + 4.8f, base.z}, 1.0f, 0.45f, paletteColor("roof_slate"), "roof_slate", 1.5f, 0.0f));
        // Door + arrow slit.
        track(sink_.spawnBoxTextured(
            {base.x, ground + 1.0f, base.z + 2.2f}, 0.55f, 0.95f, paletteColor("timber"), "timber", 1.5f, 0.0f));
        track(sink_.spawnBoxTextured(
            {base.x, ground + shaftH * 0.62f, base.z + 2.21f}, 0.16f, 0.30f, paletteColor("window_lit"),
            "window_lit", 1.0f, 0.6f));
    } else if (prop.key == "road")
    {
        // rev 31: the dirt road slab (the prop_road mesh instance). One
        // placement = one 12 m slab; the rotation carries the walk direction
        // (+ the field's hand-laid wiggle). Lifted 2 cm clear of the terrain
        // like the plaza slabs so the grazing shadow rays stay clean.
        const float yaw = static_cast<float>(placement.rotationRad);
        const std::array<float, 3> roadTint = paletteColor("dirt");
        if (sink_.spawnPropMeshInstance("prop_road", {base.x, ground + 0.02f, base.z}, yaw, roadTint, "dirt", 2.0f)
            == SceneRegistrySink::NULL_HANDLE)
        {
            track(sink_.spawnBoxExtents(
                {base.x, ground + 0.01f, base.z}, 2.75f, 0.01f, 6.0f, roadTint, "dirt", 2.0f, 0.0f));
        }
    } else if (prop.key == "crops")
    {
        // rev 31: one crop strip of a farm field - the ridged bed mesh with
        // a green-to-wheat tint split by the placement hash (adjacent rows
        // never read as one plastic sheet).
        const float yaw = static_cast<float>(placement.rotationRad);
        const std::uint64_t seed = propMeshSeed(placement.position.x, placement.position.z);
        const double wheatPick = static_cast<double>(seed % 1000u) / 1000.0;
        const std::array<float, 3> tint = wheatPick < 0.45
                                              ? std::array<float, 3>{0.62f, 0.52f, 0.18f}  // ripe wheat
                                              : std::array<float, 3>{0.22f, 0.42f, 0.13f}; // green crop
        if (sink_.spawnPropMeshInstance("prop_crops", {base.x, ground + 0.01f, base.z}, yaw, tint, "grass", 1.2f)
            == SceneRegistrySink::NULL_HANDLE)
        {
            track(sink_.spawnBoxTextured(
                {base.x, ground + 0.12f, base.z}, 0.7f, 0.12f, tint, "grass", 1.2f, 0.0f));
        }
    } else if (prop.key == "dock")
    {
        // rev 31: one pier plank of the coastal dock (the deck Y arrives in
        // the placement - the field stepped it down towards the water).
        const float yaw = static_cast<float>(placement.rotationRad);
        const std::array<float, 3> dockTint = paletteColor("timber");
        if (sink_.spawnPropMeshInstance("prop_dock_plank", {base.x, ground, base.z}, yaw, dockTint, "timber", 1.4f)
            == SceneRegistrySink::NULL_HANDLE)
        {
            track(sink_.spawnBoxExtents(
                {base.x, ground - 0.07f, base.z}, 1.2f, 0.07f, 1.8f, dockTint, "timber", 1.4f, 0.0f));
        }
    } else if (prop.key == "boat")
    {
        // rev 31: the fishing boat at the pier end. The placement's Y is the
        // waterline keel height - the hull rides the water, and the
        // transparency layer shows the submerged part through the surface.
        const float yaw = static_cast<float>(placement.rotationRad);
        const std::array<float, 3> boatTint = paletteColor("timber");
        if (sink_.spawnPropMeshInstance("prop_boat", {base.x, ground, base.z}, yaw, boatTint, "timber", 1.4f)
            == SceneRegistrySink::NULL_HANDLE)
        {
            track(sink_.spawnBoxExtents(
                {base.x, ground + 0.18f, base.z}, 0.8f, 0.18f, 2.2f, boatTint, "timber", 1.4f, 0.0f));
        }
    } else
    {
        // Unknown prop kind: one neutral box at the authored primitive, so a
        // data mod's new kind still stands in the world instead of nothing.
        track(sink_.spawnBoxTextured(
            {base.x, ground + s * 0.5f, base.z}, s * 0.4f, s, paletteColor("wood"), "wood", 1.5f, 0.0f));
    }

    if (!objects.parts.empty())
    {
        scatter_objects_[placement_index_] = std::move(objects);
    }
}

void SceneBridge::syncLampLights()
{
    // The scatter window's lamp posts as REAL point lights (rev 24): the
    // emissive lantern heads stay, and the pbr/relight passes now light the
    // ground around them. The night factor keys on the pushed sun intensity
    // (full on under the moon, off once the sun clears ~0.25 - the lamps
    // fade out through dawn instead of popping).
    if (scatter_ == nullptr)
    {
        return;
    }
    const float sunUp = last_environment_.sunIntensity > 0.0f ? last_environment_.sunIntensity : 0.0f;
    const float nightFactor = std::clamp(1.0f - sunUp / 0.25f, 0.0f, 1.0f);

    std::vector<SceneRegistrySink::LampLight> lamps;
    const double window2 = SCATTER_DESPAWN_RADIUS_M * SCATTER_DESPAWN_RADIUS_M;
    const Position anchor = characters_ != nullptr ? characters_->anchor() : Position{};
    const std::vector<ScatterPlacement> &placements = scatter_->placements();
    for (const auto &[index, objects] : scatter_objects_)
    {
        if (index >= placements.size() || objects.parts.empty())
            continue;
        const ScatterPlacement &p = placements[index];
        const double dx = p.position.x - anchor.x;
        const double dz = p.position.z - anchor.z;
        if (dx * dx + dz * dz > window2)
            continue;
        const auto propIdx = static_cast<std::size_t>(p.propIndex);
        if (propIdx >= scatter_->tables().props.size())
            continue;
        const std::string &key = scatter_->tables().props[propIdx].key;
        SceneRegistrySink::LampLight lamp;
        if (key == "lamp")
        {
            lamp.position  = {p.position.x, p.position.y + static_cast<float>(p.scaleM) * 1.14f, p.position.z};
            lamp.intensity = 2.6f * nightFactor;
            lamp.radius    = 17.0f;
        } else if (key == "campfire")
        {
            // The campfire burns all night: a smaller, hotter pool of light.
            lamp.color     = {1.0f, 0.52f, 0.22f};
            lamp.position  = {p.position.x, p.position.y + static_cast<float>(p.scaleM) * 0.3f, p.position.z};
            lamp.intensity = 2.1f * nightFactor;
            lamp.radius    = 9.0f;
        } else
        {
            continue;
        }
        lamps.push_back(lamp);
        if (lamps.size() >= 16)
            break; // the engine's point-light cap
    }
    sink_.pushPointLights(lamps);
}

void SceneBridge::publishScatterWindow()
{
    if (scatter_ == nullptr)
    {
        return;
    }
    // rev 24: the tree vocabulary registers once, before the first spawn —
    // a NULL-handle registration (fakes, or a refused mesh) degrades the
    // trees back to the analytic fallback below, never a crash.
    if (!prop_meshes_registered_)
    {
        prop_meshes_registered_ = true;
        registerPropMeshVocabulary();
    }
    // The anchor: the same position the character window keys on (the
    // player's transform, or the beginner spawn). The window republishes
    // when the anchor crosses a cell boundary (128 m cells - the republish
    // scan is a full placements() pass, so it must not run every frame).
    const Position anchor = characters_ != nullptr ? characters_->anchor() : Position{};
    const std::uint64_t cell = static_cast<std::uint64_t>(
        (static_cast<std::int64_t>(anchor.x / 128.0) & 0xFFFFull) | ((static_cast<std::int64_t>(anchor.z / 128.0) & 0xFFFFull) << 16)
    );
    if (cell != scatter_anchor_cell_)
    {
        scatter_anchor_cell_ = cell;
        // Despawn out of the hysteresis band; queue the missing in-window
        // placements (id-ascending by index, R7).
        const double despawn2 = SCATTER_DESPAWN_RADIUS_M * SCATTER_DESPAWN_RADIUS_M;
        for (auto it = scatter_objects_.begin(); it != scatter_objects_.end();)
        {
            const ScatterPlacement &p = scatter_->placements()[it->first];
            const double dx           = p.position.x - anchor.x;
            const double dz           = p.position.z - anchor.z;
            if (dx * dx + dz * dz > despawn2)
            {
                for (const SceneRegistrySink::Handle h : it->second.parts)
                {
                    sink_.removeObject(h);
                }
                it = scatter_objects_.erase(it);
            } else
            {
                ++it;
            }
        }
        scatter_pending_.clear();
        const double window2 = SCATTER_WINDOW_RADIUS_M * SCATTER_WINDOW_RADIUS_M;
        const std::vector<ScatterPlacement> &placements = scatter_->placements();
        std::vector<std::pair<double, std::size_t>> inWindow;
        for (std::size_t i = 0; i < placements.size(); ++i)
        {
            if (scatter_objects_.count(i) != 0)
            {
                continue;
            }
            const double dx = placements[i].position.x - anchor.x;
            const double dz = placements[i].position.z - anchor.z;
            const double d2 = dx * dx + dz * dz;
            if (d2 <= window2)
            {
                inWindow.emplace_back(d2, i);
            }
        }
        // The A-183 window cap: the NEAREST instances fill first (deterministic:
        // distance ascending, ties by index). The cap keeps the composition
        // parts inside the registry's object budget.
        std::sort(inWindow.begin(), inWindow.end());
        if (inWindow.size() > SCATTER_WINDOW_INSTANCES)
        {
            inWindow.resize(SCATTER_WINDOW_INSTANCES);
        }
        for (const auto &[d2, i] : inWindow)
        {
            scatter_pending_.push_back(i);
        }
    }

    // Drain the pending queue at a dedicated slice of the A-119 chunk (the
    // characters own the rest): 16 parts-worth of props per frame keeps the
    // creation spike bounded while a village fills in over ~2 frames.
    std::uint32_t processed = 0;
    while (!scatter_pending_.empty() && processed < 16)
    {
        const std::size_t index = scatter_pending_.front();
        scatter_pending_.erase(scatter_pending_.begin());
        if (scatter_objects_.count(index) != 0)
        {
            continue;
        }
        placement_index_ = index;
        spawnPropComposition(scatter_->placements()[index]);
        ++processed;
    }
}

float SceneBridge::territoryMarkerHeight(
    TerritoryType t
) const
{
    switch (t)
    {
    case TerritoryType::VILLAGE:
        return TERRITORY_MARKER_HEIGHT_VILLAGE;
    case TerritoryType::CITY:
        return TERRITORY_MARKER_HEIGHT_CITY;
    case TerritoryType::KINGDOM:
        return TERRITORY_MARKER_HEIGHT_KINGDOM;
    case TerritoryType::EMPIRE:
        return TERRITORY_MARKER_HEIGHT_EMPIRE;
    default:
        return TERRITORY_MARKER_HEIGHT_VILLAGE;
    }
}

std::array<float, 3> SceneBridge::territoryMarkerTint(
    TerritoryType t
) const
{
    switch (t)
    {
    case TerritoryType::VILLAGE:
        return TINT_VILLAGE;
    case TerritoryType::CITY:
        return TINT_CITY;
    case TerritoryType::KINGDOM:
        return TINT_KINGDOM;
    case TerritoryType::EMPIRE:
        return TINT_EMPIRE;
    default:
        return TINT_VILLAGE;
    }
}

std::array<float, 3> SceneBridge::skyTintFor(
    world::Weather w
) const
{
    // A-117: CLEAR / RAIN / STORM / SNOW. Fog is not in A-117; it falls
    // back to CLEAR (documented behavior - add an A-row if fog needs its
    // own tint).
    switch (w)
    {
    case world::Weather::CLEAR:
        return SKY_TINT_CLEAR;
    case world::Weather::RAIN:
        return {
            SKY_TINT_CLEAR[0] * SKY_TINT_RAIN_FACTOR,
            SKY_TINT_CLEAR[1] * SKY_TINT_RAIN_FACTOR,
            SKY_TINT_CLEAR[2] * SKY_TINT_RAIN_FACTOR
        };
    case world::Weather::STORM:
        return {
            SKY_TINT_CLEAR[0] * SKY_TINT_STORM_FACTOR,
            SKY_TINT_CLEAR[1] * SKY_TINT_STORM_FACTOR,
            SKY_TINT_CLEAR[2] * SKY_TINT_STORM_FACTOR
        };
    case world::Weather::SNOW:
        return {
            SKY_TINT_CLEAR[0] * SKY_TINT_SNOW_FACTOR,
            SKY_TINT_CLEAR[1] * SKY_TINT_SNOW_FACTOR,
            SKY_TINT_CLEAR[2] * SKY_TINT_SNOW_FACTOR
        };
    default:
        // FOG and any future state (COUNT sentinel included) -> CLEAR.
        return SKY_TINT_CLEAR;
    }
}

std::array<float, 3> SceneBridge::speciesColor(
    speciesId s
) const
{
    for (const auto &row : SPECIES_COLORS)
    {
        if (row.species == s)
        {
            return row.color;
        }
    }
    return SPECIES_COLOR_FALLBACK;
}

std::size_t SceneBridge::spawnedObjectCount() const
{
    return territory_boxes_.size() + safe_zone_discs_.size();
}

std::size_t SceneBridge::pendingCreationCount() const
{
    return pending_creations_.size();
}

std::size_t SceneBridge::characterObjectCount() const
{
    return character_objects_.size();
}

std::size_t SceneBridge::corpseObjectCount() const
{
    return corpse_objects_.size();
}

std::size_t SceneBridge::pendingCharacterCreationCount() const
{
    return pending_character_creations_.size();
}

int SceneBridge::characterObjectKind(
    characterId id
) const
{
    const auto it = character_objects_.find(id);
    return it == character_objects_.end() ? 0 : it->second.kind;
}

} // namespace bridge
} // namespace harrowmere

namespace harrowmere
{

// The A-187 palette lookup (declared in constants_phase5.hpp; the plugin's
// adapter and the bridge's compositions share this one door).
std::array<float, 3> paletteColor(const char *name) noexcept
{
    for (const auto &row : HARROWMERE_PALETTE)
    {
        if (std::strcmp(row.name, name) == 0)
        {
            return row.color;
        }
    }
    return HARROWMERE_PALETTE.front().color;
}

} // namespace harrowmere
