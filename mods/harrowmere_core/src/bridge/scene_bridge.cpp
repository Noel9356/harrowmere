// Task 7.2P1 - Static scene bridge implementation (Phase-1 slice of card 7.2).
//
// See scene_bridge.hpp for the contract and the pinned constants. This file
// implements the per-frame bridge logic: chunked creation of territory
// boxes and safe-zone discs, sun-light updates on a >= 1% phase delta, and
// sky-tint updates on a weather-state change. No gameplay logic, no sim-
// state mutation.

#include "bridge/scene_bridge.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace harrowmere {
namespace bridge {

namespace {

// Sun light from dayPhase01. Phase 0 = midnight, 0.25 = sunrise (east),
// 0.5 = noon (overhead), 0.75 = sunset (west). The sun moves in the X-Y
// plane (Y up); intensity is 0 when the sun is below the horizon.
//
// This is presentation math (R5 allows f64 here), not a game rule.
struct SunLight {
    std::array<float, 3> direction;  // unit vector, light-travel direction
    float                intensity;  // [0, 1]
};

SunLight computeSun(double dayPhase01) {
    // Map phase to an angle where phase 0.25 (sunrise) -> 0, phase 0.5
    // (noon) -> pi/2, phase 0.75 (sunset) -> pi, phase 0 (midnight) -> -pi/2.
    using std::numbers::pi_v;
    const double angle = (dayPhase01 - 0.25) * 2.0 * pi_v<double>;

    const float cosA = static_cast<float>(std::cos(angle));
    const float sinA = static_cast<float>(std::sin(angle));

    // Sun position relative to scene origin: (cosA, sinA, 0).
    // Light travels from sun toward scene: direction = -position.
    SunLight s;
    s.direction = { -cosA, -sinA, 0.0f };
    // Intensity tracks the sun's elevation: 0 at/below the horizon, 1 at
    // noon. max(0, sinA) clamps night-time negatives to zero.
    s.intensity = std::max(0.0f, sinA);
    return s;
}

} // anonymous namespace

// ---- SceneBridge -----------------------------------------------------------

SceneBridge::SceneBridge(WorldViewForBridge& world,
                         ClockViewForBridge& clock,
                         SceneRegistrySink&  sink)
    : world_(world), clock_(clock), sink_(sink) {}

SceneBridge::~SceneBridge() = default;

void SceneBridge::onFrame() {
    // Reset per-frame flags before any work; each updater sets its own flag
    // if it actually pushed to the sink this frame.
    sun_updated_last_frame_      = false;
    sky_tint_updated_last_frame_ = false;

    buildPendingQueueIfNeeded();
    processPendingCreations();
    updateSunIfPhaseChanged();
    updateSkyTintIfWeatherChanged();
}

void SceneBridge::buildPendingQueueIfNeeded() {
    if (pending_queue_built_) return;

    const auto descs = world_.territories();  // already sorted by id
    pending_creations_.clear();
    pending_creations_.reserve(descs.size() * 2);
    for (const auto& d : descs) {
        pending_creations_.push_back({ d.id, /*is_disc=*/false });
        if (d.safe) {
            pending_creations_.push_back({ d.id, /*is_disc=*/true });
        }
    }
    pending_queue_built_ = true;
}

void SceneBridge::processPendingCreations() {
    if (pending_creations_.empty()) return;

    // Look up the territory descriptor once per id for this frame's chunk.
    // The world view returns a fresh vector each call; build a local map for
    // O(log n) lookup. std::map for R7 determinism (territoryId-sorted).
    std::map<territoryId, WorldViewForBridge::TerritoryDesc> by_id;
    {
        const auto descs = world_.territories();
        for (const auto& d : descs) {
            by_id.emplace(d.id, d);
        }
    }

    std::uint32_t processed = 0;
    while (!pending_creations_.empty() &&
           processed < SCENE_CREATE_CHUNK_PER_FRAME) {
        const CreationTask task = pending_creations_.front();
        pending_creations_.erase(pending_creations_.begin());

        auto it = by_id.find(task.id);
        if (it == by_id.end()) continue;  // territory vanished between frames

        const auto& d = it->second;
        if (task.is_disc) {
            // Idempotent: skip if a disc already exists for this territory
            // (e.g. after a queue rebuild).
            if (safe_zone_discs_.count(d.id) != 0) continue;
            const SceneRegistrySink::Handle h =
                sink_.spawnDisc(d.center,
                                SAFE_ZONE_DISC_RADIUS,
                                SAFE_ZONE_DISC_HEIGHT,
                                TINT_SAFE);
            safe_zone_discs_.emplace(d.id, h);
        } else {
            if (territory_boxes_.count(d.id) != 0) continue;
            const SceneRegistrySink::Handle h =
                sink_.spawnBox(d.center,
                               TERRITORY_MARKER_HALF_WIDTH,
                               territoryMarkerHeight(d.type),
                               territoryMarkerTint(d.type));
            territory_boxes_.emplace(d.id, h);
        }
        ++processed;
    }
}

void SceneBridge::updateSunIfPhaseChanged() {
    const double phase = clock_.dayPhase01();

    // First frame always pushes (last_sun_phase_ == -1.0).
    if (last_sun_phase_ >= 0.0) {
        const double delta = std::fabs(phase - last_sun_phase_);
        // dayPhase01 is in [0, 1); a tiny step across the midnight seam
        // (e.g. 0.999 -> 0.001) is also small. Take the wrapped distance so
        // the threshold behaves correctly at the wraparound.
        const double wrapped = std::min(delta, 1.0 - delta);
        if (wrapped < SUN_PHASE_UPDATE_THRESHOLD) return;
    }

    const SunLight sun = computeSun(phase);
    sink_.setSunLight(sun.direction, sun.intensity);
    last_sun_phase_          = phase;
    sun_updated_last_frame_  = true;
}

void SceneBridge::updateSkyTintIfWeatherChanged() {
    const WeatherState w = world_.primaryWeather();
    if (w == last_sky_weather_) return;

    const std::array<float, 3> tint = skyTintFor(w);
    sink_.setSkyTint(tint);
    last_sky_weather_           = w;
    sky_tint_updated_last_frame_ = true;
}

float SceneBridge::territoryMarkerHeight(TerritoryType t) const {
    switch (t) {
        case TerritoryType::VILLAGE: return TERRITORY_MARKER_HEIGHT_VILLAGE;
        case TerritoryType::CITY:    return TERRITORY_MARKER_HEIGHT_CITY;
        case TerritoryType::KINGDOM: return TERRITORY_MARKER_HEIGHT_KINGDOM;
        case TerritoryType::EMPIRE:  return TERRITORY_MARKER_HEIGHT_EMPIRE;
        default:                     return TERRITORY_MARKER_HEIGHT_VILLAGE;
    }
}

std::array<float, 3> SceneBridge::territoryMarkerTint(TerritoryType t) const {
    switch (t) {
        case TerritoryType::VILLAGE: return TINT_VILLAGE;
        case TerritoryType::CITY:    return TINT_CITY;
        case TerritoryType::KINGDOM: return TINT_KINGDOM;
        case TerritoryType::EMPIRE:  return TINT_EMPIRE;
        default:                     return TINT_VILLAGE;
    }
}

std::array<float, 3> SceneBridge::skyTintFor(WeatherState w) const {
    // A-117: CLEAR / RAIN / STORM / SNOW. Fog is not in A-117; it falls
    // back to CLEAR (documented behavior - add an A-row if fog needs its
    // own tint).
    switch (w) {
        case WeatherState::CLEAR:
            return SKY_TINT_CLEAR;
        case WeatherState::RAIN:
            return { SKY_TINT_CLEAR[0] * SKY_TINT_RAIN_FACTOR,
                     SKY_TINT_CLEAR[1] * SKY_TINT_RAIN_FACTOR,
                     SKY_TINT_CLEAR[2] * SKY_TINT_RAIN_FACTOR };
        case WeatherState::STORM:
            return { SKY_TINT_CLEAR[0] * SKY_TINT_STORM_FACTOR,
                     SKY_TINT_CLEAR[1] * SKY_TINT_STORM_FACTOR,
                     SKY_TINT_CLEAR[2] * SKY_TINT_STORM_FACTOR };
        case WeatherState::SNOW:
            return { SKY_TINT_CLEAR[0] * SKY_TINT_SNOW_FACTOR,
                     SKY_TINT_CLEAR[1] * SKY_TINT_SNOW_FACTOR,
                     SKY_TINT_CLEAR[2] * SKY_TINT_SNOW_FACTOR };
        default:
            // FOG and any future state (COUNT sentinel included) -> CLEAR.
            return SKY_TINT_CLEAR;
    }
}

std::size_t SceneBridge::spawnedObjectCount() const {
    return territory_boxes_.size() + safe_zone_discs_.size();
}

std::size_t SceneBridge::pendingCreationCount() const {
    return pending_creations_.size();
}

} // namespace bridge
} // namespace harrowmere
