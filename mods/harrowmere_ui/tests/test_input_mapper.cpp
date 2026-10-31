// test_input_mapper.cpp - the pinned tests for task 7.4P1.
//
// Three tests are pinned by the card:
//   * TestInput_P1BindingsComplete  - the three bindings + F1: the P1 binding
//                                     set is exactly {W, A, S, D walk; Shift
//                                     sprint; mouse-delta look; F1 toggle},
//                                     and nothing else (the card's Goal).
//   * TestInput_NoDirectSimMutation - the mapper's only sim interaction is
//                                     the 4.2 write path (submit); it never
//                                     reads the views and never mutates sim
//                                     state directly.
//   * TestInput_IntentRoundTrip     - headless: inject key state -> a Move
//                                     intent lands in the queue -> the
//                                     fixed-update application (3.10's
//                                     applyIntents, step 9's home) moves the
//                                     player, camera-relative, at A-57's
//                                     speed.
//
// Everything runs headless with fakes over the mapper's two seams
// (UiInputGate, CameraRig - input_mapper.hpp): no window, no renderer, no
// plugin scanner. The fake input implements the engine's InputCapability
// interface directly (the published capability header is the vocabulary -
// the card's "polled InputCapability state" is this exact type), the fake
// sim records submit() traffic, and the roundtrip drains the recorded
// commands through harrowmere_core's player::IntentQueue + applyIntents -
// the same objects the runtime wiring will bind.

#include <doctest.h>

#include "input_mapper.hpp" // the deliverable under test

// The roundtrip's drain side is task 3.10's fixed-update application. This is
// a TEST, not mod code: the UI mod itself compiles against only
// harrowmere_capability.hpp (the review grep reads src/, and the pinned
// Done-when belongs to the mod's sources) - the roundtrip the card pins needs
// the real drain side, which lives here.
#include "core/constants.hpp" // MOVE_SPEED_MPS / MOVE_SPRINT_FACTOR (A-57)
#include "player/controller.hpp"

#include <cmath>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Fakes over the mapper's seams
// ---------------------------------------------------------------------------

namespace
{

// A scripted InputCapability: the test holds key state, queues one frame's
// cursor delta and a capture flag, and records every capture-mode write.
class FakeInput final : public engine::InputCapability
{
public:
    std::map<engine::KeyCode, bool> keys;
    double pendingDeltaX = 0.0;
    double pendingDeltaY = 0.0;
    bool captured        = false;
    std::vector<bool> captureWrites; // every setCursorCaptured argument, in order

    bool isKeyDown(
        engine::KeyCode keyCode
    ) override
    {
        const auto found = keys.find(keyCode);
        return found != keys.end() && found->second;
    }

    bool isMouseButtonDown(
        engine::MouseButton /*button*/
    ) override
    {
        return false;
    }

    void consumeMouseDelta(
        double &deltaX, double &deltaY
    ) override
    {
        deltaX       = pendingDeltaX;
        deltaY       = pendingDeltaY;
        pendingDeltaX = 0.0; // the accumulator drains: one frame reads each delta once
        pendingDeltaY = 0.0;
    }

    void cursorPosition(
        double &positionX, double &positionY
    ) override
    {
        positionX = 0.0;
        positionY = 0.0;
    }

    void consumeScrollDelta(
        double &deltaX, double &deltaY
    ) override
    {
        deltaX = 0.0;
        deltaY = 0.0;
    }

    void setCursorCaptured(
        bool newCaptured
    ) override
    {
        captured = newCaptured;
        captureWrites.push_back(newCaptured);
    }

    bool isCursorCaptured() const override
    {
        return captured;
    }

    bool isGamepadConnected() override
    {
        return false;
    }

    bool isGamepadButtonDown(
        engine::GamepadButton /*button*/
    ) override
    {
        return false;
    }

    float gamepadAxis(
        engine::GamepadAxis /*axis*/
    ) override
    {
        return 0.0f;
    }

    float actionStrength(
        const std::string & /*actionName*/
    ) override
    {
        return 0.0f;
    }
};

class FakeGate final : public harrowmere::ui::UiInputGate
{
public:
    bool mouseClaim    = false;
    bool keyboardClaim = false;

    bool wantsMouse() const override
    {
        return mouseClaim;
    }

    bool wantsKeyboard() const override
    {
        return keyboardClaim;
    }
};

class FakeRig final : public harrowmere::ui::CameraRig
{
public:
    std::array<double, 3> positionValue = {0.0, 2.0, 0.0};
    std::array<double, 3> lookAtValue   = {0.0, 2.0, -10.0}; // the convention's default heading (-Z)
    std::uint32_t lookWrites            = 0;
    std::uint32_t positionWrites        = 0;

    std::array<double, 3> position() const override
    {
        return positionValue;
    }

    std::array<double, 3> lookAt() const override
    {
        return lookAtValue;
    }

    void setLookAt(
        const std::array<double, 3> &lookAt
    ) override
    {
        lookAtValue = lookAt;
        ++lookWrites;
    }

    void setPosition(
        const std::array<double, 3> &position
    ) override
    {
        // The renderer contract the seam mirrors: a set position keeps the
        // current look direction - so the fake writes the eye only.
        positionValue = position;
        ++positionWrites;
    }
};

// A strict SimCapability: counts every read (the mapper must make none) and
// records every submit (the only interaction the write path allows). The
// player view is seedable so the follow-cam test can stage a transform - the
// mapper under test still never touches it.
class RecordingSim final : public harrowmere::SimCapability
{
public:
    mutable std::uint32_t readCount = 0; // the view readers are const (4.2)
    std::vector<harrowmere::SimCommand> submitted;
    harrowmere::views::PlayerView playerView{}; // the staged player transform (tests only)

    const harrowmere::views::WorldView &world() const override
    {
        ++readCount;
        return SimCapability::world();
    }

    const harrowmere::views::CharactersView &characters() const override
    {
        ++readCount;
        return SimCapability::characters();
    }

    const harrowmere::views::PlayerView &player() const override
    {
        ++readCount;
        return playerView;
    }

    const harrowmere::views::JusticeView &justice() const override
    {
        ++readCount;
        return SimCapability::justice();
    }

    const harrowmere::views::EventsView &events() const override
    {
        ++readCount;
        return SimCapability::events();
    }

    const harrowmere::views::UiSignals &signals() const override
    {
        ++readCount;
        return SimCapability::signals();
    }

    void submit(
        harrowmere::SimCommand command
    ) override
    {
        submitted.push_back(command);
    }
};

// A mapper over the fakes (the rig is nullable - headless default here).
struct Harness
{
    FakeInput input;
    FakeGate gate;
    FakeRig rig;
    RecordingSim sim;
    harrowmere::ui::InputMapper mapper;

    Harness()
        : mapper(input, sim, gate, &rig)
    {
    }
};

// Drains the recorded commands through the fixed-update application (3.10)
// and returns the player's position after exactly one tick of dt seconds.
harrowmere::Position applyOneFixedUpdate(
    RecordingSim &sim, double dtSeconds
)
{
    harrowmere::player::PlayerController player{};
    harrowmere::player::IntentQueue queue;
    harrowmere::core::WorldClock clock{}; // realTime 0; the sim-side clock (R8)
    for (const harrowmere::SimCommand &command : sim.submitted)
    {
        queue.push(command);
    }
    harrowmere::player::applyIntents(player, queue, clock, dtSeconds);
    return player.position;
}

double squareLength2D(
    double x, double z
)
{
    return x * x + z * z;
}

} // namespace

// ---------------------------------------------------------------------------
// TestInput_P1BindingsComplete - the three bindings + F1, and nothing else.
// ---------------------------------------------------------------------------
TEST_CASE("TestInput_P1BindingsComplete")
{
    // The binding table IS the P1 set: walk W/A/S/D, sprint LeftShift, look
    // mouse-delta, toggle F1 - one row each, no seventh binding beyond the
    // toggle (the card's Goal: "nothing else"). Size pinned first so a row
    // can never be added (or dropped) silently.
    static_assert(harrowmere::ui::p1BindingTable.size() == 7, "the P1 binding set is pinned by card 7.4P1");
    REQUIRE(harrowmere::ui::p1BindingCount == 7);

    const harrowmere::ui::P1BindingRow *rows = harrowmere::ui::p1BindingTable.data();
    CHECK(std::string(rows[0].action) == "walk_forward");
    CHECK(rows[0].key == engine::KeyCode::W);
    CHECK(rows[0].kind == harrowmere::ui::BindingKind::PolledKey);
    CHECK(std::string(rows[1].action) == "walk_back");
    CHECK(rows[1].key == engine::KeyCode::S);
    CHECK(std::string(rows[2].action) == "walk_left");
    CHECK(rows[2].key == engine::KeyCode::A);
    CHECK(std::string(rows[3].action) == "walk_right");
    CHECK(rows[3].key == engine::KeyCode::D);
    CHECK(std::string(rows[4].action) == "sprint");
    CHECK(rows[4].key == engine::KeyCode::LeftShift);
    CHECK(rows[4].kind == harrowmere::ui::BindingKind::PolledKey);
    // Look: the consumed-delta query, not a key.
    CHECK(std::string(rows[5].action) == "look");
    CHECK(rows[5].key == engine::KeyCode::Unknown);
    CHECK(rows[5].kind == harrowmere::ui::BindingKind::MouseDelta);
    // F1: the overlay toggle, EVENT-owned (7.3P1's KeyPressedEvent
    // subscription) - the mapper never polls it (the card's double-fire
    // pitfall is answered by single ownership, and the row names the owner).
    CHECK(std::string(rows[6].action) == "toggle_overlay");
    CHECK(rows[6].key == engine::KeyCode::F1);
    CHECK(rows[6].kind == harrowmere::ui::BindingKind::EventToggle);

    // The polled constants the hot path reads agree with the table rows - the
    // two spellings cannot drift.
    CHECK(harrowmere::ui::kWalkForwardKey == engine::KeyCode::W);
    CHECK(harrowmere::ui::kWalkBackKey == engine::KeyCode::S);
    CHECK(harrowmere::ui::kWalkLeftKey == engine::KeyCode::A);
    CHECK(harrowmere::ui::kWalkRightKey == engine::KeyCode::D);
    CHECK(harrowmere::ui::kSprintKey == engine::KeyCode::LeftShift);
    CHECK(harrowmere::ui::kOverlayToggleKey == engine::KeyCode::F1);

    // Behavioral half: F1 held ALONE is invisible to the mapper - no Move
    // intent, no cursor write, no look change (the toggle's one flip per
    // press comes from the overlay's event subscription, not from here).
    Harness harness;
    harness.input.keys[engine::KeyCode::F1] = true;
    harness.mapper.update();
    CHECK(harness.sim.submitted.empty());
    CHECK(harness.input.captureWrites.empty());
    CHECK(harness.rig.lookWrites == 0);

    // F1 held WITH a walk key changes nothing about the walk: the toggle is
    // not a modifier, and the polled path reads only its own four keys.
    harness.input.keys[engine::KeyCode::W] = true;
    harness.mapper.update();
    REQUIRE(harness.sim.submitted.size() == 1);
    CHECK(harness.sim.submitted.back().kind == harrowmere::SimCommand::Kind::Move);
    CHECK(harness.sim.submitted.back().move.dirX == doctest::Approx(0.0));
    CHECK(harness.sim.submitted.back().move.dirZ == doctest::Approx(-1.0));
    CHECK_FALSE(harness.sim.submitted.back().move.sprint);
}

// ---------------------------------------------------------------------------
// TestInput_NoDirectSimMutation - the 4.2 write path is the only sim touch.
// ---------------------------------------------------------------------------
TEST_CASE("TestInput_NoDirectSimMutation")
{
    Harness harness;
    harness.input.captured = true; // look active (unclaimed + captured)

    // A maximal frame: every walk key, sprint, and cursor motion.
    harness.input.keys[engine::KeyCode::W]         = true;
    harness.input.keys[engine::KeyCode::A]         = true;
    harness.input.keys[engine::KeyCode::D]         = true;
    harness.input.keys[engine::KeyCode::S]         = true; // W+S cancel; the guard drops it
    harness.input.keys[engine::KeyCode::LeftShift] = true;
    harness.input.pendingDeltaX                    = 40.0;
    harness.input.pendingDeltaY                    = -25.0;
    harness.mapper.update();

    // Opposite keys cancel exactly -> the degenerate guard submits nothing
    // (a zero-length direction is no motion, never a NaN - controller.hpp's
    // R2 pattern mirrored on the producer side).
    CHECK(harness.sim.submitted.empty());

    // A clean frame: W only. Exactly ONE submit, kind Move, unit direction,
    // sprint from the polled key - and ZERO view reads: the input layer has
    // no business touching the read side of the capability in P1.
    harness.input.keys[engine::KeyCode::A] = false;
    harness.input.keys[engine::KeyCode::D] = false;
    harness.input.keys[engine::KeyCode::S] = false;
    harness.mapper.update();
    REQUIRE(harness.sim.submitted.size() == 1);
    CHECK(harness.sim.submitted.back().kind == harrowmere::SimCommand::Kind::Move);
    CHECK(harness.sim.readCount == 0); // no view reads from the input path
    const double length = std::sqrt(squareLength2D(harness.sim.submitted.back().move.dirX,
                                                   harness.sim.submitted.back().move.dirZ));
    CHECK(length == doctest::Approx(1.0)); // normalized: W+D must not outrun W
    CHECK(harness.sim.submitted.back().move.sprint); // Shift polled into the flag

    // An idle frame submits nothing: silent idle is the D-AFK-1 contract
    // (intents are presence; an idle player must age into AFK).
    for (auto &entry : harness.input.keys)
    {
        entry.second = false;
    }
    harness.mapper.update();
    CHECK(harness.sim.submitted.size() == 1); // unchanged

    // The overlay's keyboard claim gates movement entirely (ui_harrowmere.hpp:
    // the mapper gates "the camera look and movement intents"): keys held
    // behind the claim produce no intent.
    harness.input.keys[engine::KeyCode::W] = true;
    harness.gate.keyboardClaim             = true;
    harness.mapper.update();
    CHECK(harness.sim.submitted.size() == 1); // still just the one clean frame

    // The mouse claim gates LOOK, not the accumulator: the delta drains (no
    // stale motion can leak into the next unclaimed frame) and no rotation
    // is applied (the rig write count stays where the earlier clean frame
    // left it - relative, because the busy frame above legitimately moved the
    // camera).
    harness.gate.keyboardClaim = false;
    harness.input.keys[engine::KeyCode::W] = false;
    const double yawBeforeGatedLook = harness.mapper.lookYawRadians();
    const std::uint32_t lookWritesBeforeGatedLook = harness.rig.lookWrites;
    harness.gate.mouseClaim         = true;
    harness.input.pendingDeltaX     = 100.0;
    harness.input.pendingDeltaY     = 100.0;
    harness.mapper.update();
    CHECK(harness.mapper.lookYawRadians() == doctest::Approx(yawBeforeGatedLook));
    CHECK(harness.input.pendingDeltaX == doctest::Approx(0.0)); // drained while gated
    CHECK(harness.rig.lookWrites == lookWritesBeforeGatedLook); // nothing applied to the camera
    // ...and the overlay received the cursor (the one-directional release).
    REQUIRE_FALSE(harness.input.captureWrites.empty());
    CHECK_FALSE(harness.input.captureWrites.back());
}

// ---------------------------------------------------------------------------
// TestInput_IntentRoundTrip - headless: key state -> Move intent -> applied
// on the fixed-update subscription (3.10's applyIntents, step 9's home).
// ---------------------------------------------------------------------------
TEST_CASE("TestInput_IntentRoundTrip")
{
    Harness harness;
    harness.input.captured = true;

    // The rig's configured framing: position (0, 2, 0) looking down -Z, so
    // the seed derives the convention's default heading (yaw 0, pitch 0) and
    // "forward" is -Z - the same seed contract the engine camera keeps.
    harness.mapper.update(); // seed frame (no keys yet)

    // ---- walk: W for one fixed update at A-57's speed ---------------------
    harness.input.keys[engine::KeyCode::W] = true;
    harness.mapper.update();
    REQUIRE(harness.sim.submitted.size() == 1);
    const harrowmere::Position walked = applyOneFixedUpdate(harness.sim, 0.1);
    CHECK(walked.x == doctest::Approx(0.0));
    CHECK(walked.z == doctest::Approx(-harrowmere::MOVE_SPEED_MPS * 0.1)); // 4 m/s forward, ground plane
    CHECK(walked.y == doctest::Approx(0.0));                              // y is cosmetic (card 1.6)

    // ---- sprint: A-57's "sprint x2" ----------------------------------------
    harness.sim.submitted.clear();
    harness.input.keys[engine::KeyCode::LeftShift] = true;
    harness.mapper.update();
    REQUIRE(harness.sim.submitted.size() == 1);
    CHECK(harness.sim.submitted.back().move.sprint);
    const harrowmere::Position sprinted = applyOneFixedUpdate(harness.sim, 0.1);
    CHECK(sprinted.z == doctest::Approx(-harrowmere::MOVE_SPEED_MPS * harrowmere::MOVE_SPRINT_FACTOR * 0.1));

    // ---- strafe + diagonal: camera-relative right, normalized -------------
    harness.sim.submitted.clear();
    for (auto &entry : harness.input.keys)
    {
        entry.second = false;
    }
    harness.input.keys[engine::KeyCode::D] = true;
    harness.mapper.update();
    const harrowmere::Position strafed = applyOneFixedUpdate(harness.sim, 0.1);
    CHECK(strafed.x == doctest::Approx(harrowmere::MOVE_SPEED_MPS * 0.1)); // right of -Z is +X
    CHECK(strafed.z == doctest::Approx(0.0));

    harness.sim.submitted.clear();
    harness.input.keys[engine::KeyCode::W] = true; // W+D: the diagonal is unit length
    harness.mapper.update();
    CHECK(harness.sim.submitted.back().move.dirX == doctest::Approx(1.0 / std::sqrt(2.0)));
    CHECK(harness.sim.submitted.back().move.dirZ == doctest::Approx(-1.0 / std::sqrt(2.0)));

    // ---- look then walk: the walk mapping follows the camera heading ------
    harness.sim.submitted.clear();
    for (auto &entry : harness.input.keys)
    {
        entry.second = false;
    }
    harness.input.pendingDeltaX = 100.0; // turn right 100 px -> yaw +0.22 rad
    harness.mapper.update();
    const double expectedYaw = 100.0 * harrowmere::ui::MOUSE_LOOK_RADIANS_PER_PIXEL;
    CHECK(harness.mapper.lookYawRadians() == doctest::Approx(expectedYaw));
    CHECK(harness.rig.lookWrites == 1); // the look reached the camera rig
    // Forward is now (sin(yaw), -cos(yaw)) on the ground plane; W walks there.
    harness.input.keys[engine::KeyCode::W] = true;
    harness.mapper.update();
    const double fx = std::sin(expectedYaw);
    const double fz = -std::cos(expectedYaw);
    CHECK(harness.sim.submitted.back().move.dirX == doctest::Approx(fx));
    CHECK(harness.sim.submitted.back().move.dirZ == doctest::Approx(fz));
    harness.sim.submitted.clear();

    // ---- the polled cadence: bursts collapse to the LATEST intent ---------
    // Two update() frames (W, then W released and S held) land two intents;
    // ONE fixed update integrates the LAST one only - motion state, not a
    // displacement instruction (controller.hpp's drain rule, from the input
    // side: the sim's speed stays frame-rate independent). The heading from
    // the look leg persists (yaw 0.22 rad), so W/S now walk along the rotated
    // forward: forward = (sin(yaw), -cos(yaw)).
    for (auto &entry : harness.input.keys)
    {
        entry.second = false;
    }
    harness.input.keys[engine::KeyCode::W] = true;
    harness.mapper.update();
    for (auto &entry : harness.input.keys)
    {
        entry.second = false;
    }
    harness.input.keys[engine::KeyCode::S] = true;
    harness.mapper.update();
    REQUIRE(harness.sim.submitted.size() == 2);
    CHECK(harness.sim.submitted[0].move.dirZ == doctest::Approx(-std::cos(expectedYaw)));
    CHECK(harness.sim.submitted[1].move.dirZ == doctest::Approx(std::cos(expectedYaw)));
    const harrowmere::Position burst = applyOneFixedUpdate(harness.sim, 0.1);
    CHECK(burst.z == doctest::Approx(harrowmere::MOVE_SPEED_MPS * 0.1 * std::cos(expectedYaw))); // the LATEST wins

    // ---- stop: keys released, no intent, no motion ------------------------
    harness.sim.submitted.clear();
    for (auto &entry : harness.input.keys)
    {
        entry.second = false;
    }
    harness.mapper.update();
    CHECK(harness.sim.submitted.empty());
    const harrowmere::Position stopped = applyOneFixedUpdate(harness.sim, 0.1);
    CHECK(stopped.x == doctest::Approx(0.0));
    CHECK(stopped.z == doctest::Approx(0.0));
}

// ---------------------------------------------------------------------------
// TestInput_FollowCameraMirrorsPlayerView - the presentation half of "the
// player walks" (DEVIATIONS 2026-10-31): the follow-cam reads the sim's
// player view and writes ONLY the rig's eye position (view + eye height);
// the look direction the mapper owns is never touched, and the mapper itself
// still makes zero view reads (TestInput_NoDirectSimMutation stays true).
// ---------------------------------------------------------------------------
TEST_CASE("TestInput_FollowCameraMirrorsPlayerView")
{
    Harness harness;
    REQUIRE(harness.sim.readCount == 0); // the mapper frame so far: no view reads

    // The sim's authoritative transform (as worldgen staged it: the beginner
    // village center, ground plane y=0). The eye rides PLAYER_EYE_HEIGHT_M
    // above it; x/z mirror exactly.
    harness.sim.playerView.position = harrowmere::views::Position{100.0, 0.0, -40.0};
    const std::array<double, 3> lookBefore = harness.rig.lookAtValue;

    harrowmere::ui::PlayerFollowCamera follow(harness.sim, harness.rig);
    follow.update();

    CHECK(harness.sim.readCount == 1); // exactly one view read per frame
    CHECK(harness.rig.positionWrites == 1);
    CHECK(harness.rig.positionValue[0] == doctest::Approx(100.0));
    CHECK(harness.rig.positionValue[1] == doctest::Approx(harrowmere::ui::PLAYER_EYE_HEIGHT_M));
    CHECK(harness.rig.positionValue[2] == doctest::Approx(-40.0));
    // The look contract: the teleport keeps the direction - the look target
    // is untouched and the mapper-owned state never moved.
    CHECK(harness.rig.lookWrites == 0);
    CHECK(harness.rig.lookAtValue == lookBefore);

    // A walk frame: the sim advances the player (4 m/s * 0.1 s, as the
    // roundtrip test integrates it), the next follow update mirrors it - the
    // eye FOLLOWS, which is what makes the walk observable from the view.
    harness.sim.playerView.position = harrowmere::views::Position{100.0, 0.0, -40.4};
    follow.update();
    CHECK(harness.sim.readCount == 2);
    CHECK(harness.rig.positionValue[2] == doctest::Approx(-40.4));
    CHECK(harness.rig.lookWrites == 0); // still: position only, every frame

    // A raised transform (the sim's y is cosmetic today, but the mirror adds
    // the eye height to whatever the view carries - Phase 2's avatar can
    // lift the eye by staging a taller transform).
    harness.sim.playerView.position = harrowmere::views::Position{0.0, 12.0, 0.0};
    follow.update();
    CHECK(harness.rig.positionValue[1] == doctest::Approx(12.0 + harrowmere::ui::PLAYER_EYE_HEIGHT_M));
}
