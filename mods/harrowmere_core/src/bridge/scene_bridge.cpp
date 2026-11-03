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

#include <algorithm>
#include <cmath>
#include <numbers>

namespace harrowmere
{
namespace bridge
{

namespace
{

// Sun light from dayPhase01. Phase 0 = midnight, 0.25 = sunrise (east),
// 0.5 = noon (overhead), 0.75 = sunset (west). The sun moves in the X-Y
// plane (Y up); intensity is 0 when the sun is below the horizon.
//
// This is presentation math (R5 allows f64 here), not a game rule.
struct SunLight
{
    std::array<float, 3> direction; // unit vector, light-travel direction
    float intensity;                // [0, 1]
};

SunLight computeSun(
    double dayPhase01
)
{
    // Map phase to an angle where phase 0.25 (sunrise) -> 0, phase 0.5
    // (noon) -> pi/2, phase 0.75 (sunset) -> pi, phase 0 (midnight) -> -pi/2.
    using std::numbers::pi_v;
    const double angle = (dayPhase01 - 0.25) * 2.0 * pi_v<double>;

    const float cosA = static_cast<float>(std::cos(angle));
    const float sinA = static_cast<float>(std::sin(angle));

    // Sun position relative to scene origin: (cosA, sinA, 0).
    // Light travels from sun toward scene: direction = -position.
    SunLight s;
    s.direction = {-cosA, -sinA, 0.0f};
    // Intensity tracks the sun's elevation: 0 at/below the horizon, 1 at
    // noon. max(0, sinA) clamps night-time negatives to zero.
    s.intensity = std::max(0.0f, sinA);
    return s;
}

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
    buildPendingQueueIfNeeded();
    processPendingCreations();
    syncJailCages();
    syncCharacters();
    syncCorpses();
    // One chunk budget per frame across ALL dynamic creations (A-119): the
    // static queue drains first (it is the oldest work), then characters,
    // then corpses - a teleport's burst can never spike the frame.
    drainCharacterCreations();
    updateSunIfPhaseChanged();
    updateSkyTintIfWeatherChanged();
}

void SceneBridge::publishTerrainIfNeeded()
{
    if (terrain_published_ || terrain_ == nullptr)
        return;
    const auto mesh = terrain_->meshGrid();
    if (mesh.empty())
        return;
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

void SceneBridge::syncCharacters()
{
    if (characters_ == nullptr)
        return;

    // ---- 1. Rebuild the desired set from the view (id-ascending, R7) ------
    desired_characters_.clear();
    const Position anchor = characters_->anchor();
    for (const auto &c : characters_->presentCharacters())
    {
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

        // The shape policy (see the header's reading note):
        //   presence sphere  - every NEAR/MID character (the exit
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
            d.kind = 1; // presence sphere
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
    // into one GPU refresh (the module README's cost model).
    for (const auto &[id, d] : desired_characters_)
    {
        auto it = character_objects_.find(id);
        if (it == character_objects_.end())
            continue;
        Position bodyPos = d.position;
        if (it->second.kind == 1)
        {
            bodyPos.y += CHARACTER_SPHERE_RADIUS; // rest on the ground plane
        } else
        {
            bodyPos.y += FAR_MARKER_BOX_HALF_EXTENT;
        }
        sink_.setObjectPosition(it->second.body, bodyPos);
        if (it->second.ring != SceneRegistrySink::NULL_HANDLE)
        {
            Position ringPos = d.position;
            ringPos.y += FACTION_RING_RADIUS;
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
        desired_corpses_.emplace(c.victim, c);
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

            CharacterObject object;
            object.kind  = d.kind;
            Position pos = d.position;
            if (d.kind == 1)
            {
                pos.y += CHARACTER_SPHERE_RADIUS;
                object.body = sink_.spawnSphere(pos, CHARACTER_SPHERE_RADIUS, speciesColor(d.species));
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
                pos.y += FAR_MARKER_BOX_HALF_EXTENT;
                object.body = sink_.spawnBox(
                    pos, FAR_MARKER_BOX_HALF_EXTENT, FAR_MARKER_BOX_HALF_EXTENT * 2.0f, TINT_FAR_MARKER
                );
            }
            character_objects_.emplace(task.id, object);
        }
    }
}

void SceneBridge::updateSunIfPhaseChanged()
{
    const double phase = clock_.dayPhase01();

    // First frame always pushes (last_sun_phase_ == -1.0).
    if (last_sun_phase_ >= 0.0)
    {
        const double delta = std::fabs(phase - last_sun_phase_);
        // dayPhase01 is in [0, 1); a tiny step across the midnight seam
        // (e.g. 0.999 -> 0.001) is also small. Take the wrapped distance so
        // the threshold behaves correctly at the wraparound.
        const double wrapped = std::min(delta, 1.0 - delta);
        if (wrapped < SUN_PHASE_UPDATE_THRESHOLD)
            return;
    }

    const SunLight sun = computeSun(phase);
    sink_.setSunLight(sun.direction, sun.intensity);
    last_sun_phase_         = phase;
    sun_updated_last_frame_ = true;
}

void SceneBridge::updateSkyTintIfWeatherChanged()
{
    const world::Weather w = world_.primaryWeather();
    if (w == last_sky_weather_)
        return;

    const std::array<float, 3> tint = skyTintFor(w);
    sink_.setSkyTint(tint);
    last_sky_weather_            = w;
    sky_tint_updated_last_frame_ = true;
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
