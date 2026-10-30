// input_mapper.cpp - the game's input layer, Phase-1 slice (task 7.4P1).
//
// See input_mapper.hpp for the contract (ownership map, seams, look
// conventions, sim write path). This TU is the card's pinned Files entry.
//
// The one-per-frame update() is deliberately the ONLY entry point: input is
// frame-coupled (the sim advances on the fixed-update lane; this layer reads
// devices and submits intents), so there is no per-tick hook to
// double-register when 3.1's Scheduler lands - and no F1 handling here at
// all, because the overlay component's own KeyPressedEvent subscription owns
// that edge (7.3P1; the double-fire pitfall is why the split is hard).

#include "input_mapper.hpp"

#include <cmath>

namespace harrowmere::ui
{

namespace
{

// The engine camera's pitch clamp, re-derived once (input_mapper.hpp carries
// the constant and its provenance).
constexpr double kMaxPitch = MAX_LOOK_PITCH_RADIANS;

// Numerical floor for the direction normalizations (the R2 explicit-guard
// pattern at the only division-shaped sites in this file: a degenerate input
// falls back to a pinned direction, never a NaN).
constexpr double kDirectionEpsilon = 1e-9;

// The look target the rig receives, one world unit along the forward - the
// exact shape the engine camera's own write-back produces
// (camera_controller.hpp applyToCamera: lookAt = position + forward).
constexpr double kLookTargetDistance = 1.0;

} // namespace

InputMapper::InputMapper(
    engine::InputCapability &input,
    harrowmere::SimCapability &sim,
    const UiInputGate &uiGate,
    CameraRig *cameraRig
)
    : input_(input)
    , sim_(sim)
    , uiGate_(uiGate)
    , cameraRig_(cameraRig)
{
}

std::array<double, 3> InputMapper::lookDirection() const noexcept
{
    // The engine camera's parameterization (camera_controller.hpp, verbatim):
    // yaw wraps the world Y axis starting from -Z, pitch positive upwards.
    const double cosPitch = std::cos(pitch_);
    return {cosPitch * std::sin(yaw_), std::sin(pitch_), -cosPitch * std::cos(yaw_)};
}

std::array<double, 2> InputMapper::groundFrameForward() const noexcept
{
    // The walk heading is the look direction projected on the ground plane
    // (x/z) and normalized. The pitch clamp keeps the horizontal component
    // non-degenerate (|cos(pitch)| >= cos(89 degrees) > 0), but the guard
    // stays for the seed path (a configured look_at straight overhead).
    const std::array<double, 3> forward = lookDirection();
    const double horizontalLength = std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
    if (horizontalLength <= kDirectionEpsilon)
    {
        // The pinned fallback (R2's explicit-guard pattern): the convention's
        // default heading. Unreachable while the pitch clamp holds.
        return {0.0, -1.0};
    }
    return {forward[0] / horizontalLength, forward[2] / horizontalLength};
}

std::array<double, 2> InputMapper::groundFrameRight() const noexcept
{
    // The level perpendicular of the walk heading - the engine camera's own
    // strafe frame ("Right = forward x worldUp, level, independent of pitch",
    // camera_controller.hpp): right = (-forwardZ, 0, forwardX). Verified
    // against the convention: looking down -Z, right is +X.
    const std::array<double, 2> forward = groundFrameForward();
    return {-forward[1], forward[0]};
}

void InputMapper::update()
{
    // ---- 1. Seed the look state once from the configured framing ----------
    //
    // The same contract the engine camera integration keeps (syncFromCamera):
    // the configured [scene.camera] look_at keeps its framing until the first
    // look arrives. Inverting the convention's parameterization (see
    // lookDirection) is the exact inversion camera_controller.hpp runs.
    if (!seeded_)
    {
        seeded_ = true;
        if (cameraRig_ != nullptr)
        {
            const std::array<double, 3> position = cameraRig_->position();
            const std::array<double, 3> lookAt   = cameraRig_->lookAt();
            const double fx = lookAt[0] - position[0];
            const double fy = lookAt[1] - position[1];
            const double fz = lookAt[2] - position[2];
            const double length = std::sqrt(fx * fx + fy * fy + fz * fz);
            if (length > kDirectionEpsilon)
            {
                pitch_ = std::asin(fy / length);
                yaw_   = std::atan2(fx, -fz);
            }
        }
    }

    // ---- 2. Hand the cursor to the overlay when it claims it --------------
    //
    // One-directional (input_mapper.hpp): the mapper releases, never steals
    // back - the engine controller's own F / Esc / left-click flow remains
    // the re-capture path in a dev run, and the overlay (7.3P1) wins the
    // cursor for as long as it claims it. Released once per claimed frame;
    // setCursorCaptured is a mode change, so the steady state costs one
    // false-returning isCursorCaptured() poll.
    if (uiGate_.wantsMouse() && input_.isCursorCaptured())
    {
        input_.setCursorCaptured(false);
    }

    // ---- 3. Look: drain the accumulator, integrate while unclaimed --------
    //
    // The drain is UNCONDITIONAL - the mapper is the one delta consumer per
    // frame (the frame event dispatches before the renderer's camera
    // integration, whose reads therefore see a settled accumulator), and a
    // gated frame must still settle the device or the next unclaimed frame
    // would inherit stale motion and snap the camera. The ui_capability modal
    // rule spells the same guarantee from the provider side. The integration
    // itself is guarded on a non-zero delta: an idle cursor rewrites nothing
    // (the rig's write-back stays quiet), which is also why the seed frame
    // costs no rig write - the configured framing IS the state until real
    // motion arrives.
    double deltaX = 0.0;
    double deltaY = 0.0;
    input_.consumeMouseDelta(deltaX, deltaY);

    const bool lookActive = input_.isCursorCaptured() && !uiGate_.wantsMouse();
    if (lookActive && (deltaX != 0.0 || deltaY != 0.0))
    {
        // Screen x grows right -> turn right; screen y grows down -> look up
        // when the cursor moves up. Same sign conventions as the engine
        // camera integration (camera_controller.hpp), same sensitivity scale
        // (MOUSE_LOOK_RADIANS_PER_PIXEL matches the shipped default).
        yaw_ += deltaX * MOUSE_LOOK_RADIANS_PER_PIXEL;
        pitch_ -= deltaY * MOUSE_LOOK_RADIANS_PER_PIXEL;
        if (pitch_ > kMaxPitch)
        {
            pitch_ = kMaxPitch;
        }
        if (pitch_ < -kMaxPitch)
        {
            pitch_ = -kMaxPitch;
        }

        // Apply through the rig (the free-fly controller's state, which the
        // renderer capability doc names as "the one camera truth"): the
        // controller re-derives its yaw/pitch from position -> target on the
        // setter, its own accumulator read gets zeros, and its per-frame
        // write-back lands the mapper's look in the rendered camera. With a
        // null rig the heading stays tracked (the walk mapping below follows
        // it) and nothing is applied - the degrade-if-absent contract.
        if (cameraRig_ != nullptr)
        {
            const std::array<double, 3> position  = cameraRig_->position();
            const std::array<double, 3> direction = lookDirection();
            cameraRig_->setLookAt({position[0] + direction[0] * kLookTargetDistance,
                                   position[1] + direction[1] * kLookTargetDistance,
                                   position[2] + direction[2] * kLookTargetDistance});
        }
    }

    // ---- 4. Movement: poll, map camera-relative, submit -------------------
    //
    // Gated on the overlay's keyboard claim (ui_harrowmere.hpp's contract:
    // the mapper gates "the camera look and movement intents" - WASD must not
    // walk the player while the user types into a panel). The polling is
    // isKeyDown per frame, card verbatim; the mapping is camera-relative
    // (card verbatim) through the tracked heading's ground frame.
    double moveX = 0.0;
    double moveZ = 0.0;
    bool   moving = false;

    if (!uiGate_.wantsKeyboard())
    {
        const std::array<double, 2> forward = groundFrameForward();
        const std::array<double, 2> right   = groundFrameRight();

        if (input_.isKeyDown(kWalkForwardKey))
        {
            moveX += forward[0];
            moveZ += forward[1];
            moving = true;
        }
        if (input_.isKeyDown(kWalkBackKey))
        {
            moveX -= forward[0];
            moveZ -= forward[1];
            moving = true;
        }
        if (input_.isKeyDown(kWalkRightKey))
        {
            moveX += right[0];
            moveZ += right[1];
            moving = true;
        }
        if (input_.isKeyDown(kWalkLeftKey))
        {
            moveX -= right[0];
            moveZ -= right[1];
            moving = true;
        }
    }

    if (!moving)
    {
        // Idle frames stay silent: no intent, no AFK refresh (D-AFK-1 reads
        // intents as presence), and the fixed update's slice simply carries
        // no Move - the drain's hasMotion flag stays false and nothing
        // integrates (controller.hpp's motion-state rule).
        return;
    }

    // Normalize the diagonal (W+D must not outrun W) - the pinned explicit
    // guard at the file's second division-shaped site.
    const double lengthSquared = moveX * moveX + moveZ * moveZ;
    const double length        = std::sqrt(lengthSquared);
    if (length <= kDirectionEpsilon)
    {
        return; // degenerate frame (opposite keys cancel exactly): no motion
    }

    const harrowmere::MoveIntent intent{moveX / length, moveZ / length, input_.isKeyDown(kSprintKey)};
    // The 4.2 write path, and the ONLY line in this file that touches the
    // sim (TestInput_NoDirectSimMutation pins the shape): a queued command
    // the fixed-update application drains at step 9 - no input path mutates
    // sim state directly.
    harrowmere::SimCommand command{};
    command.kind = harrowmere::SimCommand::Kind::Move;
    command.move = intent;
    sim_.submit(command);
}

} // namespace harrowmere::ui
