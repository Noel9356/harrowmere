// input_mapper.cpp - the game's input layer, Phase-1 slice (task 7.4P1)
// extended by the full card (task 7.4): the complete interaction loop.
//
// See input_mapper.hpp for the contract (ownership map, seams, look
// conventions, sim write path, targeting). This TU is the card's pinned Files
// entry.
//
// The one-per-frame update() is deliberately the ONLY entry point: input is
// frame-coupled (the sim advances on the fixed-update lane; this layer reads
// devices and submits intents), so there is no per-tick hook to
// double-register when 3.1's Scheduler lands - and no F1/panel-toggle
// handling here at all, because the overlay component's own KeyPressedEvent
// subscription owns those edges (7.3; the double-fire pitfall is why the
// split is hard).

#include "input_mapper.hpp"

#include "settings_capability.hpp"

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

// Planar (x/z) distance - R5: distances compare on the plane (y is cosmetic).
double planarDistance(
    double ax, double az, double bx, double bz
)
{
    const double dx = ax - bx;
    const double dz = az - bz;
    return std::sqrt(dx * dx + dz * dz);
}

} // namespace

InputBindings loadInputBindings(
    const engine::SettingsCapability &settings
)
{
    InputBindings bindings;
    const auto read = [&settings](const char *name, engine::KeyCode fallback) {
        const std::string key = settings.stringValue(std::string("input.actions.") + name, "");
        if (key.empty())
            return fallback;
        const engine::KeyCode parsed = engine::keyCodeFromName(key);
        return parsed == engine::KeyCode::Unknown ? fallback : parsed;
    };
    bindings.walk_forward = read("walk_forward", bindings.walk_forward);
    bindings.walk_back    = read("walk_back", bindings.walk_back);
    bindings.walk_left    = read("walk_left", bindings.walk_left);
    bindings.walk_right   = read("walk_right", bindings.walk_right);
    bindings.sprint       = read("sprint", bindings.sprint);
    bindings.interact     = read("interact", bindings.interact);
    bindings.crouch       = read("crouch", bindings.crouch);
    bindings.torch        = read("torch", bindings.torch);
    bindings.mask         = read("mask", bindings.mask);
    bindings.report_crime = read("report_crime", bindings.report_crime);
    bindings.bribe        = read("bribe", bindings.bribe);
    bindings.surrender    = read("surrender", bindings.surrender);
    bindings.jump         = read("jump", bindings.jump);
    return bindings;
}

InputMapper::InputMapper(
    engine::InputCapability &input,
    harrowmere::SimCapability &sim,
    const UiInputGate &uiGate,
    CameraRig *cameraRig,
    InputBindings bindings
)
    : input_(input), sim_(sim), uiGate_(uiGate), cameraRig_(cameraRig), bindings_(bindings)
{}

void InputMapper::setBindings(
    InputBindings bindings
) noexcept
{
    bindings_        = bindings;
    prevInteract_    = false;
    prevJump_        = false;
    prevCrouch_      = false;
    prevTorch_       = false;
    prevMask_        = false;
    prevReportCrime_ = false;
    prevBribe_       = false;
    prevSurrender_   = false;
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
    const double horizontalLength       = std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
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

bool InputMapper::risingEdge(
    engine::KeyCode key
)
{
    const bool now = input_.isKeyDown(key);
    bool *previous = key == bindings_.interact     ? &prevInteract_
                   : key == bindings_.jump         ? &prevJump_
                   : key == bindings_.crouch       ? &prevCrouch_
                   : key == bindings_.torch        ? &prevTorch_
                   : key == bindings_.mask         ? &prevMask_
                   : key == bindings_.report_crime ? &prevReportCrime_
                   : key == bindings_.bribe        ? &prevBribe_
                   : key == bindings_.surrender    ? &prevSurrender_
                                                   : nullptr;
    if (previous == nullptr)
        return false; // not an edge key the mapper owns
    const bool edge = now && !*previous;
    *previous       = now;
    return edge;
}

bool InputMapper::risingEdgeMouseButton(
    engine::MouseButton button
)
{
    const bool now = input_.isMouseButtonDown(button);
    bool *previous = button == kAttackMouseButton      ? &prevAttack_
                   : button == kAltInteractMouseButton ? &prevAltInteract_
                                                       : nullptr;
    if (previous == nullptr)
    {
        return false;
    }
    const bool edge = now && !*previous;
    *previous       = now;
    return edge;
}

std::uint64_t InputMapper::resolveNearestTarget(
    double rangeM
)
{
    // Lazy view read: edge frames only (the P1 NoDirectSimMutation contract
    // holds - movement-only frames never reach this path).
    const harrowmere::views::CharactersView &characters = sim_.characters();
    const harrowmere::views::Position &me               = sim_.player().position;

    std::uint64_t best  = 0;
    double bestDistance = rangeM;
    for (const auto &c : characters.present)
    {
        if (c.isPlayer)
        {
            continue; // never target yourself
        }
        const double d = planarDistance(c.position.x, c.position.z, me.x, me.z);
        if (d <= bestDistance) // boundary inclusive (A-121's own rule)
        {
            best         = c.id;
            bestDistance = d;
        }
    }
    return best;
}

std::uint64_t InputMapper::resolveAimedTarget(
    double rangeM
)
{
    // The look-direction cone pick: among the present non-player characters
    // within rangeM, the one whose direction from the player best matches
    // the look heading (dot product >= AIM_CONE_MIN_DOT; nearest on ties).
    const harrowmere::views::CharactersView &characters = sim_.characters();
    const harrowmere::views::Position &me               = sim_.player().position;
    const std::array<double, 2> forward                 = groundFrameForward();

    std::uint64_t best  = 0;
    double bestDot      = AIM_CONE_MIN_DOT;
    double bestDistance = rangeM;
    for (const auto &c : characters.present)
    {
        if (c.isPlayer)
        {
            continue;
        }
        const double dx = c.position.x - me.x;
        const double dz = c.position.z - me.z;
        const double d  = std::sqrt(dx * dx + dz * dz);
        if (d > rangeM || d <= kDirectionEpsilon)
        {
            continue;
        }
        const double dot = (dx / d) * forward[0] + (dz / d) * forward[1];
        if (dot >= bestDot && d <= bestDistance)
        {
            best         = c.id;
            bestDot      = dot;
            bestDistance = d;
        }
    }
    return best;
}

void InputMapper::submitInteract(
    std::uint64_t target, harrowmere::InteractIntent::Action action
)
{
    harrowmere::SimCommand command{};
    command.kind            = harrowmere::SimCommand::Kind::Interact;
    command.interact.target = target;
    command.interact.action = action;
    lastTarget_             = target;
    sim_.submit(command); // the 4.2 write path - queued, applied at step 9
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
            const double fx                      = lookAt[0] - position[0];
            const double fy                      = lookAt[1] - position[1];
            const double fz                      = lookAt[2] - position[2];
            const double length                  = std::sqrt(fx * fx + fy * fy + fz * fz);
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
    // the re-capture path in a dev run, and the overlay (7.3) wins the
    // cursor for as long as it claims it.
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
            cameraRig_->setLookAt(
                {position[0] + direction[0] * kLookTargetDistance,
                    position[1] + direction[1] * kLookTargetDistance,
                    position[2] + direction[2] * kLookTargetDistance}
            );
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
    bool moving  = false;

    if (!uiGate_.wantsKeyboard())
    {
        const std::array<double, 2> forward = groundFrameForward();
        const std::array<double, 2> right   = groundFrameRight();

        if (input_.isKeyDown(bindings_.walk_forward))
        {
            moveX += forward[0];
            moveZ += forward[1];
            moving = true;
        }
        if (input_.isKeyDown(bindings_.walk_back))
        {
            moveX -= forward[0];
            moveZ -= forward[1];
            moving = true;
        }
        if (input_.isKeyDown(bindings_.walk_right))
        {
            moveX += right[0];
            moveZ += right[1];
            moving = true;
        }
        if (input_.isKeyDown(bindings_.walk_left))
        {
            moveX -= right[0];
            moveZ -= right[1];
            moving = true;
        }
    }

    if (moving)
    {
        // Normalize the diagonal (W+D must not outrun W) - the pinned explicit
        // guard at the file's second division-shaped site.
        const double lengthSquared = moveX * moveX + moveZ * moveZ;
        const double length        = std::sqrt(lengthSquared);
        if (length > kDirectionEpsilon)
        {
            const harrowmere::MoveIntent intent{moveX / length, moveZ / length, input_.isKeyDown(bindings_.sprint)};
            // The 4.2 write path, and the ONLY line in this file that touches
            // the sim on a movement frame (TestInput_NoDirectSimMutation pins
            // the shape): a queued command the fixed-update application drains
            // at step 9 - no input path mutates sim state directly.
            harrowmere::SimCommand command{};
            command.kind = harrowmere::SimCommand::Kind::Move;
            command.move = intent;
            sim_.submit(command);
        }
    }

    // ---- 5. Edge intents (7.4's interaction loop) --------------------------
    //
    // Gated on the overlay's keyboard claim too: an intent fired while the
    // user types into a panel is a misfire (the same gate the walk takes).
    // Targets resolve lazily here - a movement-only frame never reads a view.
    // Every branch submits EXACTLY ONE command (the binding table's rule);
    // Space is pinned cosmetic (no command) and just tracks its edge.
    if (!uiGate_.wantsKeyboard())
    {
        // Attack: LMB rising edge -> Attack{aimed target} (the v1 melee pick
        // window; A-28..A-30 resolve the swing sim-side).
        if (risingEdgeMouseButton(kAttackMouseButton))
        {
            const std::uint64_t target = resolveAimedTarget(ATTACK_RANGE_UI_M);
            if (target != 0)
            {
                harrowmere::SimCommand command{};
                command.kind          = harrowmere::SimCommand::Kind::Attack;
                command.attack.target = target;
                lastTarget_           = target;
                sim_.submit(command);
            }
        }

        // Alt-interact: RMB rising edge -> Interact{aimed, TALK}.
        if (risingEdgeMouseButton(kAltInteractMouseButton))
        {
            const std::uint64_t target = resolveAimedTarget(INTERACT_RANGE_UI_M);
            if (target != 0)
            {
                submitInteract(target, harrowmere::InteractIntent::Action::TALK);
            }
        }

        // Interact: E -> Interact{nearest, TALK}; the sim re-gates the range
        // (A-121) and dispatches to the owner handler (3.10's routing).
        if (risingEdge(bindings_.interact))
        {
            const std::uint64_t target = resolveNearestTarget(INTERACT_RANGE_UI_M);
            if (target != 0)
            {
                submitInteract(target, harrowmere::InteractIntent::Action::TALK);
            }
        }

        // The targeted intents (their owner handlers land in Phase 3; the
        // gate reports NoHandler meanwhile - 3.10's deterministic null).
        if (risingEdge(bindings_.report_crime))
        {
            submitInteract(resolveNearestTarget(INTERACT_RANGE_UI_M), harrowmere::InteractIntent::Action::REPORT_CRIME);
        }
        if (risingEdge(bindings_.bribe))
        {
            submitInteract(resolveNearestTarget(INTERACT_RANGE_UI_M), harrowmere::InteractIntent::Action::BRIBE);
        }
        if (risingEdge(bindings_.surrender))
        {
            submitInteract(resolveNearestTarget(INTERACT_RANGE_UI_M), harrowmere::InteractIntent::Action::SURRENDER);
        }

        // The self-state toggles: one Emote per rising edge (the sim flips
        // the act; the suspicion surface reads the action log).
        if (risingEdge(bindings_.crouch))
        {
            harrowmere::SimCommand command{};
            command.kind      = harrowmere::SimCommand::Kind::Emote;
            command.emote.act = harrowmere::EmoteIntent::Act::CROUCH;
            sim_.submit(command);
        }
        if (risingEdge(bindings_.torch))
        {
            harrowmere::SimCommand command{};
            command.kind      = harrowmere::SimCommand::Kind::Emote;
            command.emote.act = harrowmere::EmoteIntent::Act::LIGHT_TORCH;
            sim_.submit(command);
        }
        if (risingEdge(bindings_.mask))
        {
            harrowmere::SimCommand command{};
            command.kind      = harrowmere::SimCommand::Kind::Emote;
            command.emote.act = harrowmere::EmoteIntent::Act::MASK;
            sim_.submit(command);
        }

        // Jump: pinned cosmetic in v1 - the edge is tracked (so a future sim
        // command wires in without a stale-edge jump), and NO command exists
        // to submit (the binding table's documented N/A row).
        (void)risingEdge(bindings_.jump);
    } else
    {
        // Keyboard claimed: settle the edge state so a press DURING the claim
        // does not fire on release (one flip per press, even across a gate).
        (void)risingEdge(bindings_.interact);
        (void)risingEdge(bindings_.jump);
        (void)risingEdge(bindings_.crouch);
        (void)risingEdge(bindings_.torch);
        (void)risingEdge(bindings_.mask);
        (void)risingEdge(bindings_.report_crime);
        (void)risingEdge(bindings_.bribe);
        (void)risingEdge(bindings_.surrender);
        (void)risingEdgeMouseButton(kAttackMouseButton);
        (void)risingEdgeMouseButton(kAltInteractMouseButton);
    }
}

// ---- PlayerFollowCamera -------------------------------------------------------

PlayerFollowCamera::PlayerFollowCamera(
    const harrowmere::SimCapability &sim, CameraRig &rig
)
    : sim_(sim), rig_(rig)
{}

void PlayerFollowCamera::update()
{
    // The sim's player view IS the authoritative transform (rebuilt at tick
    // step 13; the frame reads a settled snapshot). The eye rides
    // PLAYER_EYE_HEIGHT_M above the ground-plane transform; the look
    // direction stays exactly where the look integration left it - the rig's
    // setPosition keeps it by contract (the renderer capability's setter
    // semantics, mirrored on the seam).
    const harrowmere::views::PlayerView &player = sim_.player();
    rig_.setPosition({player.position.x, player.position.y + PLAYER_EYE_HEIGHT_M, player.position.z});
}

} // namespace harrowmere::ui
