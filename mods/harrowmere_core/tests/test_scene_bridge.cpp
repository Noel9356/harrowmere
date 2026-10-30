// Task 7.2P1 - Scene bridge tests.
//
// The three pinned tests from the card:
//   * TestBridge_OneObjectPerTerritory
//   * TestBridge_SunFollowsDayPhase
//   * TestBridge_PositionSyncBatched
//
// Plus one extra sanity test for the A-117 sky-tint table, since the card's
// Done-when ("windowed run shows markers/discs/sun/sky tracking the sim's
// world state") demands the sky path actually verify. All four use the
// bridge's abstract interfaces (SceneRegistrySink / WorldViewForBridge /
// ClockViewForBridge) so they run headless without the engine or a full
// WorldStore/WorldClock graph.

#include <doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "bridge/scene_bridge.hpp"

using namespace harrowmere::bridge;

// The bridge's interfaces reference game types that live in the enclosing
// `harrowmere` namespace (Position, territoryId, TerritoryType, Weather).
// Bring them in for the fakes.
using harrowmere::Position;
using harrowmere::territoryId;
using harrowmere::TerritoryType;
using harrowmere::world::Weather;

// ---- Fakes -----------------------------------------------------------------

class FakeSink : public SceneRegistrySink {
public:
    Handle spawnBox(const Position& center, float halfWidth, float height,
                    const std::array<float, 3>& tint) override {
        const Handle h = next_handle_++;
        boxes_.push_back({ center, halfWidth, height, tint });
        box_handles_.push_back(h);
        return h;
    }

    Handle spawnDisc(const Position& center, double radius, float height,
                     const std::array<float, 3>& tint) override {
        const Handle h = next_handle_++;
        discs_.push_back({ center, radius, height, tint });
        disc_handles_.push_back(h);
        return h;
    }

    void setObjectPosition(Handle /*h*/, const Position& /*p*/) override {
        // P1 never moves territory markers; this is a no-op in tests.
    }

    void setSunLight(const std::array<float, 3>& direction,
                     float intensity) override {
        sun_calls_.push_back({ direction, intensity });
    }

    void setSkyTint(const std::array<float, 3>& tint) override {
        sky_calls_.push_back(tint);
    }

    struct BoxCall {
        Position              center;
        float                 halfWidth;
        float                 height;
        std::array<float, 3>  tint;
    };
    struct DiscCall {
        Position              center;
        double                radius;
        float                 height;
        std::array<float, 3>  tint;
    };
    struct SunCall {
        std::array<float, 3>  direction;
        float                 intensity;
    };

    std::vector<BoxCall>                boxes_;
    std::vector<Handle>                 box_handles_;
    std::vector<DiscCall>               discs_;
    std::vector<Handle>                 disc_handles_;
    std::vector<SunCall>                sun_calls_;
    std::vector<std::array<float, 3>>   sky_calls_;

private:
    Handle next_handle_ = 1;  // 0 is NULL_HANDLE
};

class FakeWorld : public WorldViewForBridge {
public:
    void addTerritory(territoryId id, TerritoryType type,
                      Position center, bool safe) {
        territories_.push_back({ id, type, center, safe });
        // Keep sorted by id ascending so the bridge's pending queue is
        // deterministic (R7).
        std::sort(territories_.begin(), territories_.end(),
                  [](const TerritoryDesc& a, const TerritoryDesc& b) {
                      return a.id < b.id;
                  });
    }

    void setPrimaryWeather(Weather w) { weather_ = w; }

    std::vector<TerritoryDesc> territories() const override {
        return territories_;
    }
    Weather primaryWeather() const override { return weather_; }

private:
    std::vector<TerritoryDesc> territories_;
    Weather               weather_ = Weather::CLEAR;
};

class FakeClock : public ClockViewForBridge {
public:
    void setDayPhase01(double p) { phase_ = p; }
    double dayPhase01() const override { return phase_; }

private:
    double phase_ = 0.0;
};

// ---- Pinned tests ----------------------------------------------------------

TEST_CASE("TestBridge_OneObjectPerTerritory") {
    FakeWorld world;
    world.addTerritory(territoryId{101}, TerritoryType::VILLAGE,
                       Position{100.0, 0.0, 100.0}, /*safe=*/true);
    world.addTerritory(territoryId{102}, TerritoryType::CITY,
                       Position{200.0, 0.0, 200.0}, /*safe=*/false);
    world.addTerritory(territoryId{103}, TerritoryType::KINGDOM,
                       Position{300.0, 0.0, 300.0}, /*safe=*/false);
    world.addTerritory(territoryId{104}, TerritoryType::EMPIRE,
                       Position{400.0, 0.0, 400.0}, /*safe=*/true);

    FakeClock clock;
    FakeSink  sink;
    SceneBridge bridge(world, clock, sink);

    // Drain the pending queue across as many frames as needed. With 6 tasks
    // (4 boxes + 2 discs) and a 32-per-frame budget, one frame suffices.
    while (bridge.pendingCreationCount() > 0) {
        bridge.onFrame();
    }
    // One more frame to settle - must be a no-op for spawns.
    bridge.onFrame();

    // One box per territory (4), one disc per safe zone (2).
    CHECK(sink.boxes_.size() == 4);
    CHECK(sink.discs_.size() == 2);
    CHECK(bridge.spawnedObjectCount() == 6);

    // Heights match the type -> height table pinned in the 7.2P1 spec.
    // boxes_ ordering follows territory id order (101, 102, 103, 104).
    CHECK(sink.boxes_[0].height == doctest::Approx(TERRITORY_MARKER_HEIGHT_VILLAGE));
    CHECK(sink.boxes_[1].height == doctest::Approx(TERRITORY_MARKER_HEIGHT_CITY));
    CHECK(sink.boxes_[2].height == doctest::Approx(TERRITORY_MARKER_HEIGHT_KINGDOM));
    CHECK(sink.boxes_[3].height == doctest::Approx(TERRITORY_MARKER_HEIGHT_EMPIRE));

    // Half-width is the "thin" constant for every territory type.
    for (const auto& b : sink.boxes_) {
        CHECK(b.halfWidth == doctest::Approx(TERRITORY_MARKER_HALF_WIDTH));
    }

    // Safe-zone disc radius matches D-SHELTER-1.
    for (const auto& d : sink.discs_) {
        CHECK(d.radius == doctest::Approx(SAFE_ZONE_DISC_RADIUS));
    }
}

TEST_CASE("TestBridge_SunFollowsDayPhase") {
    FakeWorld world;
    FakeClock clock;
    FakeSink  sink;
    SceneBridge bridge(world, clock, sink);

    // First frame always pushes the sun (last_sun_phase_ starts at -1.0).
    // Noon -> intensity 1.0, direction straight down (0, -1, 0).
    clock.setDayPhase01(0.50);
    bridge.onFrame();
    REQUIRE(sink.sun_calls_.size() == 1);
    CHECK(sink.sun_calls_.back().intensity == doctest::Approx(1.0f).epsilon(0.001f));
    CHECK(sink.sun_calls_.back().direction[1] == doctest::Approx(-1.0f).epsilon(0.001f));
    CHECK(bridge.sunWasUpdatedOnLastFrame());

    // Phase change < 1% -> no sun update.
    clock.setDayPhase01(0.505);
    bridge.onFrame();
    CHECK(sink.sun_calls_.size() == 1);
    CHECK_FALSE(bridge.sunWasUpdatedOnLastFrame());

    // Phase change >= 1% -> sun update. Mid-afternoon: sun west of overhead,
    // light traveling downward, intensity between 0 and 1.
    clock.setDayPhase01(0.60);
    bridge.onFrame();
    CHECK(sink.sun_calls_.size() == 2);
    CHECK(bridge.sunWasUpdatedOnLastFrame());
    const auto& dir = sink.sun_calls_.back().direction;
    CHECK(dir[1] < 0.0f);  // light traveling downward
    CHECK(sink.sun_calls_.back().intensity > 0.0f);
    CHECK(sink.sun_calls_.back().intensity < 1.0f);

    // Midnight -> intensity 0.
    clock.setDayPhase01(0.0);
    bridge.onFrame();
    CHECK(sink.sun_calls_.size() == 3);
    CHECK(sink.sun_calls_.back().intensity == doctest::Approx(0.0f).epsilon(0.001f));

    // Wraparound: a tiny step across the midnight seam (0.999 -> 0.002) is
    // also small (wrapped distance 0.003) and must NOT trigger an update.
    clock.setDayPhase01(0.999);
    bridge.onFrame();  // large delta from 0.0 -> 0.999 -> pushes
    const std::size_t calls_before_wrap = sink.sun_calls_.size();
    clock.setDayPhase01(0.002);
    bridge.onFrame();
    CHECK(sink.sun_calls_.size() == calls_before_wrap);  // wrapped delta 0.003 < 1%
}

TEST_CASE("TestBridge_PositionSyncBatched") {
    FakeWorld world;
    // 100 territories, 20 of them safe -> 120 objects total (100 boxes + 20
    // discs). With A-119's 32-per-frame budget, draining takes 4 frames.
    for (std::uint64_t i = 1; i <= 100; ++i) {
        const TerritoryType t =
            (i % 4 == 0) ? TerritoryType::CITY :
            (i % 4 == 1) ? TerritoryType::VILLAGE :
            (i % 4 == 2) ? TerritoryType::KINGDOM :
                           TerritoryType::EMPIRE;
        const bool safe = (i % 5 == 0);  // i in {5,10,...,100} -> 20 safe zones
        world.addTerritory(territoryId{i}, t,
                           Position{static_cast<double>(i) * 10.0, 0.0,
                                    static_cast<double>(i) * 10.0},
                           safe);
    }

    FakeClock clock;
    FakeSink  sink;
    SceneBridge bridge(world, clock, sink);

    // Frame 1: chunk budget is 32 -> at most 32 spawn calls total.
    bridge.onFrame();
    const std::size_t after_frame1 =
        sink.boxes_.size() + sink.discs_.size();
    CHECK(after_frame1 <= SCENE_CREATE_CHUNK_PER_FRAME);

    // Drain the rest frame by frame. Each frame must also respect the
    // chunk budget (no rebuild-per-frame spike).
    std::size_t prev_count = after_frame1;
    while (bridge.pendingCreationCount() > 0) {
        bridge.onFrame();
        const std::size_t now = sink.boxes_.size() + sink.discs_.size();
        CHECK(now - prev_count <= SCENE_CREATE_CHUNK_PER_FRAME);
        prev_count = now;
    }

    // Total spawns: 100 boxes + 20 discs = 120.
    CHECK(sink.boxes_.size() == 100);
    CHECK(sink.discs_.size() == 20);
    CHECK(bridge.pendingCreationCount() == 0);

    // A follow-up frame with an empty queue must not spawn anything new.
    bridge.onFrame();
    CHECK(sink.boxes_.size() == 100);
    CHECK(sink.discs_.size() == 20);
}

// ---- Extra sanity: A-117 sky-tint table -----------------------------------
//
// The card's Done-when ("windowed run shows ... sky ... tracking the sim's
// world state") needs at least one test that exercises the weather -> tint
// path. The card does not pin a name for this test; it is an addition.

TEST_CASE("TestBridge_SkyTintPerWeatherState") {
    FakeWorld world;
    FakeClock clock;
    FakeSink  sink;
    SceneBridge bridge(world, clock, sink);

    // First frame: weather = CLEAR -> tint = SKY_TINT_CLEAR.
    world.setPrimaryWeather(Weather::CLEAR);
    bridge.onFrame();
    REQUIRE(sink.sky_calls_.size() == 1);
    CHECK(sink.sky_calls_.back()[0] == doctest::Approx(SKY_TINT_CLEAR[0]));
    CHECK(sink.sky_calls_.back()[1] == doctest::Approx(SKY_TINT_CLEAR[1]));
    CHECK(sink.sky_calls_.back()[2] == doctest::Approx(SKY_TINT_CLEAR[2]));
    CHECK(bridge.skyTintWasUpdatedOnLastFrame());

    // Same weather next frame -> no new tint call.
    bridge.onFrame();
    CHECK(sink.sky_calls_.size() == 1);
    CHECK_FALSE(bridge.skyTintWasUpdatedOnLastFrame());

    // Storm -> CLEAR tint x 0.45 (A-117).
    world.setPrimaryWeather(Weather::STORM);
    bridge.onFrame();
    REQUIRE(sink.sky_calls_.size() == 2);
    CHECK(sink.sky_calls_.back()[0] ==
          doctest::Approx(SKY_TINT_CLEAR[0] * SKY_TINT_STORM_FACTOR));
    CHECK(sink.sky_calls_.back()[1] ==
          doctest::Approx(SKY_TINT_CLEAR[1] * SKY_TINT_STORM_FACTOR));
    CHECK(sink.sky_calls_.back()[2] ==
          doctest::Approx(SKY_TINT_CLEAR[2] * SKY_TINT_STORM_FACTOR));

    // Snow -> CLEAR tint x 1.2 (A-117).
    world.setPrimaryWeather(Weather::SNOW);
    bridge.onFrame();
    REQUIRE(sink.sky_calls_.size() == 3);
    CHECK(sink.sky_calls_.back()[0] ==
          doctest::Approx(SKY_TINT_CLEAR[0] * SKY_TINT_SNOW_FACTOR));
    CHECK(sink.sky_calls_.back()[2] ==
          doctest::Approx(SKY_TINT_CLEAR[2] * SKY_TINT_SNOW_FACTOR));
}
