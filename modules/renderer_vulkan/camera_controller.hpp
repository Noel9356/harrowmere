#ifndef ENGINE_MODULES_RENDERER_CAMERA_CONTROLLER_HPP
#define ENGINE_MODULES_RENDERER_CAMERA_CONTROLLER_HPP

// Free-fly camera controller for the renderer module. Header only and
// Vulkan-free on purpose: it integrates keyboard and mouse state (through the
// mod-facing input contract) into the [scene.camera] state the renderer
// uploads every frame.
//
// Binding (two shapes):
//   1. Semantic action names ([input.actions] entries read by the renderer
//      at construct into CameraBindings): camera_forward / camera_back /
//      camera_left / camera_right / camera_up / camera_down name the
//      movement axes, "boost" the speed multiplier, "release_cursor" the
//      pointer release. When an action name is configured the controller
//      polls actionStrength(action) - bind by meaning, gamepads included.
//   2. Absent names keep the historical fixed keys (all rebindable in the
//      sense that the keys live in ONE function):
//   W / S           fly forward / back along the view direction
//   A / D           strafe left / right
//   Space / E       up        (world +Y)
//   LeftCtrl / Q    down      (world -Y)
//   LeftShift       speed boost (fly_boost multiplier)
//   mouse           look (yaw/pitch) while the cursor is captured
//   wheel           fly speed * 1.15 per notch (clamped)
//   F               toggle cursor capture
//   Left button     recapture the cursor
//   Escape          release the cursor
//
// The controller starts from the configured position/look_at and derives a
// yaw/pitch pair from that direction, so a configuration authored as a look
// target keeps its framing until the first input arrives.

#include "input_capability.hpp"
#include "logging_capability.hpp"
#include "scene_description.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace engine
{

class FlyCameraController
{
public:
    // Semantic action bindings, resolved by the renderer at construct
    // ([input.actions] entries). An empty name keeps that axis on its
    // historical fixed key - the shipped demo defines only boost and
    // release_cursor, and an assembly that defines none of them flies
    // exactly like it always did.
    struct CameraBindings
    {
        std::string forwardAction;
        std::string backwardAction;
        std::string leftAction;
        std::string rightAction;
        std::string upAction;
        std::string downAction;
        std::string boostAction;
        std::string releaseCursorAction;
    };

    // Adopts the configured camera: position and yaw/pitch from
    // position -> look_at, speed knobs from the camera block.
    void syncFromCamera(
        const SceneCamera &camera
    )
    {
        position_        = camera.position;
        flySpeed_        = camera.flySpeed;
        flyBoost_        = camera.flyBoost;
        lookSensitivity_ = camera.lookSensitivity;

        float forward[3]
            = {camera.lookAt[0] - camera.position[0],
                camera.lookAt[1] - camera.position[1],
                camera.lookAt[2] - camera.position[2]};
        const float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
        if (length > 1e-6f)
        {
            forward[0] /= length;
            forward[1] /= length;
            forward[2] /= length;
        }
        // Convention: yaw wraps the world Y axis starting from -Z, pitch is
        // positive upwards. forward = (cos(pitch) sin(yaw), sin(pitch),
        // -cos(pitch) cos(yaw)); the inverse below degenerates only for a
        // straight-up/down look_at, which the pitch clamp makes impossible to
        // re-enter afterwards.
        pitch_ = std::asin(std::clamp(forward[1], -1.0f, 1.0f));
        yaw_   = std::atan2(forward[0], -forward[2]);
    }

    // Writes the current fly state back into the camera block the renderer
    // uploads: position moves, look_at tracks the yaw/pitch direction.
    void applyToCamera(
        SceneCamera &camera
    ) const
    {
        const float forwardX = std::cos(pitch_) * std::sin(yaw_);
        const float forwardY = std::sin(pitch_);
        const float forwardZ = -std::cos(pitch_) * std::cos(yaw_);
        camera.position      = position_;
        camera.lookAt        = {position_[0] + forwardX, position_[1] + forwardY, position_[2] + forwardZ};
    }

    // Adopts the semantic action bindings resolved at construct (renderer.cpp
    // reads the [input.actions] entries into CameraBindings). Call once,
    // before the first update.
    void setBindings(
        const CameraBindings &bindings
    )
    {
        bindings_ = bindings;
    }

    // One frame of input integration. input may be null (capability absent):
    // the controller then keeps the camera exactly where it is, so the rest
    // of the engine cannot tell. dt must be > 0.
    void update(
        InputCapability *input, double deltaTime, LoggerCapability &logger, bool pointerClaimedElsewhere = false
    )
    {
        if (input == nullptr || deltaTime <= 0.0)
        {
            return;
        }

        // The ui module's overlay owns the pointer while it is visible (the
        // modal rule documented on UiCapability): no look, no fly speed
        // wheel, no capture toggling - and the accumulators drain so nothing
        // stale leaks into the next frame the camera is back in control.
        if (pointerClaimedElsewhere)
        {
            double drainX = 0.0;
            double drainY = 0.0;
            input->consumeMouseDelta(drainX, drainY);
            input->consumeScrollDelta(drainX, drainY);
            return;
        }

        handleCaptureKeys(input, logger);

        if (input->isCursorCaptured())
        {
            double deltaX = 0.0;
            double deltaY = 0.0;
            input->consumeMouseDelta(deltaX, deltaY);
            // Screen x grows right -> turn right; screen y grows down -> look
            // up when the cursor moves up. Raw motion where available.
            yaw_ += static_cast<float>(deltaX) * lookSensitivity_;
            pitch_ -= static_cast<float>(deltaY) * lookSensitivity_;
            pitch_ = std::clamp(pitch_, -maxPitch, maxPitch);
        } else
        {
            // Not captured: drain and drop so a click-to-recapture does not
            // inherit stale motion.
            double deltaX = 0.0;
            double deltaY = 0.0;
            input->consumeMouseDelta(deltaX, deltaY);
        }

        double scrollY = 0.0;
        double scrollX = 0.0;
        input->consumeScrollDelta(scrollX, scrollY);
        if (scrollY != 0.0)
        {
            flySpeed_ = std::clamp(
                flySpeed_ * std::pow(flyWheelSpeedFactor, static_cast<float>(scrollY)), minSpeed, maxSpeed
            );
        }

        const float forwardX = std::cos(pitch_) * std::sin(yaw_);
        const float forwardY = std::sin(pitch_);
        const float forwardZ = -std::cos(pitch_) * std::cos(yaw_);
        // Right = forward x worldUp (level, independent of pitch: a strafe
        // should not tilt with the look direction).
        float rightX            = -forwardZ;
        float rightZ            = forwardX;
        const float rightLength = std::sqrt(rightX * rightX + rightZ * rightZ);
        if (rightLength > 1e-6f)
        {
            rightX /= rightLength;
            rightZ /= rightLength;
        }

        float moveX = 0.0f;
        float moveY = 0.0f;
        float moveZ = 0.0f;
        // Each axis reads its configured action when one exists (bind by
        // meaning - a gamepad stick or button works with zero code here),
        // else the historical fixed key. The two shapes cost the same per
        // frame: one poll call on the input capability either way.
        if (held(input, bindings_.forwardAction, KeyCode::W))
        {
            moveX += forwardX;
            moveY += forwardY;
            moveZ += forwardZ;
        }
        if (held(input, bindings_.backwardAction, KeyCode::S))
        {
            moveX -= forwardX;
            moveY -= forwardY;
            moveZ -= forwardZ;
        }
        if (held(input, bindings_.rightAction, KeyCode::D))
        {
            moveX += rightX;
            moveZ += rightZ;
        }
        if (held(input, bindings_.leftAction, KeyCode::A))
        {
            moveX -= rightX;
            moveZ -= rightZ;
        }
        if (held(input, bindings_.upAction, KeyCode::Space) || input->isKeyDown(KeyCode::E))
        {
            moveY += 1.0f;
        }
        if (held(input, bindings_.downAction, KeyCode::LeftControl) || input->isKeyDown(KeyCode::Q))
        {
            moveY -= 1.0f;
        }

        const float moveLength = std::sqrt(moveX * moveX + moveY * moveY + moveZ * moveZ);
        if (moveLength > 1e-6f)
        {
            const float speed = flySpeed_ * (held(input, bindings_.boostAction, KeyCode::LeftShift) ? flyBoost_ : 1.0f);
            const float scale = speed * static_cast<float>(deltaTime) / moveLength;
            position_[0] += moveX * scale;
            position_[1] += moveY * scale;
            position_[2] += moveZ * scale;
        }
    }

    // Camera pose access for script bridges and modules: the free-fly state
    // is the one camera truth, so external setters mutate IT (not a copy) and
    // the controller keeps integrating input around the new pose.
    std::array<float, 3> position() const
    {
        return position_;
    }

    // The current look target: one world unit along the forward direction
    // from the position (the same point applyToCamera writes into
    // SceneCamera::lookAt).
    std::array<float, 3> lookTarget() const
    {
        const float forwardX = std::cos(pitch_) * std::sin(yaw_);
        const float forwardY = std::sin(pitch_);
        const float forwardZ = -std::cos(pitch_) * std::cos(yaw_);
        return {position_[0] + forwardX, position_[1] + forwardY, position_[2] + forwardZ};
    }

    void setPosition(
        const std::array<float, 3> &worldPosition
    )
    {
        position_ = worldPosition;
    }

    // Keeps the position, re-derives yaw/pitch from position -> target (the
    // same inverse syncFromCamera uses; the pitch clamp keeps the controller
    // out of the straight-up/down degeneracy).
    void lookAtPoint(
        const std::array<float, 3> &target
    )
    {
        float forward[3]   = {target[0] - position_[0], target[1] - position_[1], target[2] - position_[2]};
        const float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
        if (length > 1e-6f)
        {
            forward[0] /= length;
            forward[1] /= length;
            forward[2] /= length;
        }
        pitch_ = std::asin(std::clamp(forward[1], -1.0f, 1.0f));
        yaw_   = std::atan2(forward[0], -forward[2]);
    }

private:
    static constexpr float maxPitch = 1.55334f; // just under 89 degrees
    static constexpr float minSpeed = 0.1f;
    static constexpr float maxSpeed = 200.0f;

    // One movement axis: the configured action's strength when bound
    // (non-zero means held), else the fixed key's polled state. A pure
    // dispatcher - the caller keeps the axis semantics.
    static bool held(
        InputCapability *input, const std::string &action, KeyCode fallbackKey
    )
    {
        if (!action.empty())
        {
            return input->actionStrength(action) > 0.0f;
        }
        return input->isKeyDown(fallbackKey);
    }

    void handleCaptureKeys(
        InputCapability *input, LoggerCapability &logger
    )
    {
        if (togglePending_ && !input->isKeyDown(KeyCode::F))
        {
            togglePending_ = false;
        }
        if (!togglePending_ && input->isKeyDown(KeyCode::F))
        {
            togglePending_      = true;
            const bool captured = !input->isCursorCaptured();
            input->setCursorCaptured(captured);
            logger.logMessage(
                LogLevel::Information,
                std::string("camera: cursor ") + (captured ? "captured (mouse look active, Esc releases)" : "released")
            );
        }
        // Cursor release: the configured "release_cursor" action when the
        // assembly defined one (the demo content ships it), else Escape.
        const bool releaseRequested = !bindings_.releaseCursorAction.empty()
                                        ? input->actionStrength(bindings_.releaseCursorAction) > 0.0f
                                        : input->isKeyDown(KeyCode::Escape);
        if (releaseRequested && input->isCursorCaptured())
        {
            input->setCursorCaptured(false);
        }
        if (!input->isCursorCaptured() && input->isMouseButtonDown(MouseButton::Left))
        {
            input->setCursorCaptured(true);
        }
    }

    CameraBindings bindings_;

    std::array<float, 3> position_{};
    float yaw_             = 0.0f;
    float pitch_           = 0.0f;
    float flySpeed_        = defaultFlySpeed;
    float flyBoost_        = defaultFlyBoost;
    float lookSensitivity_ = defaultLookSensitivity;
    bool togglePending_    = false;
};

} // namespace engine

#endif // ENGINE_MODULES_RENDERER_CAMERA_CONTROLLER_HPP
