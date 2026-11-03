#ifndef ENGINE_MODULES_PHYSICS_HPP
#define ENGINE_MODULES_PHYSICS_HPP

#include "bvh_capability.hpp"
#include "logging_capability.hpp"
#include "lua_bridge_extension.hpp"
#include "physics_capability.hpp"
#include "scene_capability.hpp"
#include "engine/kernel/component_interface.hpp"
#include "engine/kernel/event_bus.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine
{

// How a contact's restitution is combined from the two bodies' coefficients.
// The historical rule (min) stays the default; the other three are the
// combine modes material systems commonly expect. Parsed once at construct
// ([physics].restitution_mode) into this enum, so the contact solver below
// switches on an integer like every other hot-path branch in the module.
enum class RestitutionMode
{
    Min,     // the more absorbing surface wins (the historical default)
    Max,     // the bouncier surface wins
    Average, // arithmetic mean of the two coefficients
    Multiply // product of the two coefficients (0 anywhere kills the bounce)
};

// Module "physics_builtin": the built-in rigid body provider (family member
// "physics.builtin").
//
// Simulation contract, in one paragraph: the module subscribes to the frame
// event, advances the simulation by the event's deltaSeconds - the frame
// delta the composition root measured once for the whole iteration
// (docs/architecture.md, "The frame clock"), so the simulation runs on the
// same canonical clock every other frame-tick module follows instead of one
// measured per module - integrates every dynamic body with semi-implicit
// Euler under the configured gravity, resolves contacts (bodies vs the
// ground plane, sphere vs sphere, sphere vs box) with restitution, and
// writes the resulting transforms back through the scene registry. The
// registry stays the source of truth - a body whose object was removed
// drops, a resized object resizes its collider, and the renderer's
// incremental rebuild picks every moved object up with no physics-specific
// renderer path.
//
// Collision pair culling runs through the bvh module's hierarchy
// over the bodies' swept bounds (rebuilt per step; at the registry cap that
// costs microseconds) - the same structure a future CPU SDF bake would use.
//
// The module additionally provides the "scripting.lua_bridge.physics" Lua
// bridge extension: it installs engine.add_physics_body,
// engine.remove_physics_body, engine.set_body_velocity, engine.body_velocity,
// engine.set_gravity, engine.body_count, engine.set_body_sleeping,
// engine.is_body_awake and engine.raycast into every script state through a
// dedicated PhysicsLuaBridge object owned by this module (see physics.cpp).
// The host discovers this extension through the multi-provider acquisition of
// the "scripting.lua_bridge" family, so adding this capability to the module
// never touches the lua_scripting source.
class PhysicsModule final : public ModuleInterface, public PhysicsCapability
{
public:
    PhysicsModule();
    ~PhysicsModule() override;

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;

    // PhysicsCapability.
    BodyHandle addDynamicBody(SceneCapability::ObjectHandle objectHandle, const BodyDescription &description) override;
    bool removeBody(BodyHandle handle) override;
    bool setBodyVelocity(BodyHandle handle, const std::array<float, 3> &velocity) override;
    bool bodyVelocity(BodyHandle handle, std::array<float, 3> &outVelocity) const override;
    void setGravity(const std::array<float, 3> &gravity) override;
    std::array<float, 3> gravity() const override;
    std::uint32_t bodyCount() const override;
    bool setBodySleeping(BodyHandle handle, bool sleeping) override;
    bool isBodyAwake(BodyHandle handle) const override;
    bool raycast(
        const std::array<float, 3> &origin,
        const std::array<float, 3> &direction,
        float maxDistance,
        RaycastResult &outResult
    ) override;

private:
    struct Body
    {
        BodyHandle handle                          = invalidBody;
        SceneCapability::ObjectHandle objectHandle = SceneCapability::invalidObject;
        BodyDescription description{};
        std::array<float, 3> velocity = {0.0f, 0.0f, 0.0f};
        bool asleep                   = false;
        // Seconds spent below the sleep threshold (auto-sleep accumulator).
        double stillnessSeconds = 0.0;
    };

    // One simulation step of the given length (clamped internally; a stalled
    // frame must not teleport bodies through geometry).
    void step(double seconds);
    void forwardFrameTick(const EngineFrameEvent &event);

    // Contact resolution helpers. All positions come from the registry right
    // before the step, all results go back through it. resolveGroundContact
    // mutates the snapshot in place (pushing it out of the ground) and
    // reports whether it did: the caller derives the snapshot's post-contact
    // bounds from the pre-contact value it already holds when the answer is
    // "no" (bounds are a pure function of the transform, and the transform
    // is untouched), instead of re-deriving identical numbers per body per
    // step. The caller owns the single write-back. The BOX-SIDED resolvers
    // receive the box's world bounds as an argument: the step snapshot
    // already carries exactly those bounds (BodyStepState.bounds, derived
    // from the same transform by the same objectBounds), and the pair pass
    // never mutates a snapshot transform - the resolvers write through the
    // registry - so recomputing them per contacting pair (a box against many
    // spheres paid one AABB build per pair) re-derived a value that was
    // already in hand.
    bool resolveGroundContact(
        Body &body, SceneObjectTransform &object, const AxisAlignedBoundingBox &worldBounds, double dt
    );
    void resolveSphereSphere(
        Body &first, const SceneObjectTransform &firstObject, Body &second, const SceneObjectTransform &secondObject
    );
    void resolveSphereBox(
        Body &sphereBody,
        const SceneObjectTransform &sphereObject,
        Body &boxBody,
        const AxisAlignedBoundingBox &boxBounds
    );
    // Axis-aligned box vs box: MTV along the shallowest penetration axis.
    // Rotations are ignored (the provider's documented AABB approximation).
    void resolveBoxBox(
        Body &first,
        const SceneObjectTransform &firstObject,
        const AxisAlignedBoundingBox &firstBounds,
        Body &second,
        const SceneObjectTransform &secondObject,
        const AxisAlignedBoundingBox &secondBounds
    );
    // Wakes one body when an impulse from a moving body exceeds the wake
    // threshold (contact with a sleeping body changes nothing).
    void wakeOnContact(Body &body, float impulseMagnitude);
    // The configured combine rule for one contact's restitution
    // (RestitutionMode; Min reproduces the historical std::min pair rule).
    float combineRestitution(float first, float second) const;
    // Broadphase (bvh capability) + narrowphase over all live body pairs.
    void resolveBodyPairs();

    // The per-step snapshot of one body: its post-step transform (integrated
    // and ground-resolved for awake bodies, as-read for sleeping ones) plus
    // the world bounds derived from it. Captured once in step() - which
    // already reads every body - and consumed by resolveBodyPairs, which
    // previously re-read every body from the registry one to three times
    // more per step (bounds pass + one re-read per pair member). The registry
    // cannot have changed in between: the step is single-threaded and nothing
    // else runs inside it, so the snapshot IS the registry state.
    //
    // The snapshot carries the registry object's TRANSFORM slice (type,
    // position, size, Y rotation - the fields the integration, the contact
    // pass and the write-backs consume) instead of the full descriptor: the
    // full readback copies the material payload too, whose texture-set name
    // is a heap round trip per body per step whenever the name outgrows its
    // small-string buffer. The material fields are dead weight on this path.
    // body points at the step's live Body record; the body map is never
    // mutated inside a step (dropped handles are erased only after the pair
    // pass), and unordered_map pointers are stable until their element is
    // erased - so the pointer is valid for exactly the snapshot's lifetime.
    // Carrying it here removes the per-contact-pair handle hash lookups.
    struct BodyStepState
    {
        BodyHandle handle = invalidBody;
        Body *body        = nullptr;
        SceneObjectTransform object{};
        AxisAlignedBoundingBox bounds{};
    };

    ComponentManifest componentManifest;
    ComponentContext *boundContext   = nullptr;
    LoggerCapability *logger         = nullptr;
    SceneCapability *sceneCapability = nullptr;
    BvhCapability *bvhCapability     = nullptr;

    EventSubscription frameTickSubscription;
    EventChannel<EngineFrameEvent> *frameTickChannel = nullptr;

    std::unordered_map<BodyHandle, Body> bodies;
    BodyHandle nextHandle = invalidBody + 1;
    // The same bodies as a dense vector of record pointers, iterated by the
    // per-step integration loop: the hash map's node layout walks a
    // heap-scattered pointer per body (a cache miss per body per substep
    // just to visit the set), where this walks one contiguous array. The map
    // stays the handle -> body source of truth for the capability entry
    // points (they hash a handle, they do not iterate); this mirror is
    // maintained on the three mutations the map has (add, remove, clear) and
    // iterated everywhere else. Erase keeps order (rare path, small n), so
    // the iteration order - and with it the pair pass's - stays stable for a
    // given body set.
    std::vector<Body *> denseBodies;

    // Simulation parameters (the [physics] configuration section; defaults
    // are earth gravity and a ground plane at y=0).
    std::array<float, 3> gravity_ = {0.0f, -9.81f, 0.0f};
    bool groundEnabled            = true;
    float groundHeight            = 0.0f;
    float maxStepSeconds          = 1.0f / 30.0f;
    // Auto-sleep: a body whose speed stays below sleepSpeed for sleepDelay
    // seconds stops being integrated (stillnessSeconds accumulates). Waking:
    // setBodyVelocity, setBodySleeping, or a contact impulse above the same
    // threshold.
    float sleepSpeed  = 0.15f;
    double sleepDelay = 2.0;
    // Substepping ([physics].substeps, default 1): how many equal slices the
    // clamped frame delta is split into per frame. Total simulated time is
    // unchanged; only the integration granularity rises.
    std::uint32_t substeps = 1;
    // Fixed timestep ([physics].fixed_hz, default 0 = off): when set, real
    // time accumulates and the simulation consumes it in exact
    // 1/fixed_hz increments - a frame-rate independent, reproducible step.
    // The remainder carries across frames (bounded by maxStepSeconds).
    double fixedStepSeconds     = 0.0;
    double fixedStepAccumulator = 0.0;
    // The contact restitution combine rule ([physics].restitution_mode,
    // default Min - the historical behavior).
    RestitutionMode restitutionMode = RestitutionMode::Min;

    // Per-step scratch vectors kept as members so the broadphase stops
    // touching the heap every frame: the BVH capability's overlappingPairs
    // now preserves inner-vector capacity across calls when the caller
    // reuses the outOverlaps, and these bounds/handles/pairNeighbors
    // buffers stay at their high-water mark across frames. deadHandles
    // is small but kept on the same pattern for consistency.
    std::vector<AxisAlignedBoundingBox> broadphaseBoundsScratch;
    std::vector<std::vector<std::uint32_t>> broadphasePairNeighborsScratch;
    std::vector<BodyStepState> stepStatesScratch;
    std::vector<BodyHandle> deadHandlesScratch;
    // The Lua bridge extension object owned by this module. Constructed in
    // construct() and returned from capabilityPointerFor for the
    // "scripting.lua_bridge.physics" member. Held as the base
    // LuaBridgeExtension pointer: the concrete PhysicsLuaBridge type
    // (defined in physics.cpp) stays private to this module's translation
    // unit.
    std::unique_ptr<LuaBridgeExtension> luaBridge;
};

} // namespace engine

#endif // ENGINE_MODULES_PHYSICS_HPP
