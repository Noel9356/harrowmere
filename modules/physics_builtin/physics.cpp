#include "physics.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <sol/sol.hpp>
#include <tuple>

namespace engine
{

namespace
{

// The world-space AABB of one registry object (spheres use their radius).
// Takes the transform slice: the bounds are a pure function of type,
// position and size - the fields every snapshot in this module carries
// (BodyStepState::object) and the raycast reads - so one helper serves the
// step loop, the broadphase and the ray without touching a descriptor.
AxisAlignedBoundingBox objectBounds(
    const SceneObjectTransform &object
)
{
    AxisAlignedBoundingBox bounds;
    const float radius = object.size[0];
    if (object.type == scene_object_type::sphere)
    {
        bounds.minPoint = {object.position[0] - radius, object.position[1] - radius, object.position[2] - radius};
        bounds.maxPoint = {object.position[0] + radius, object.position[1] + radius, object.position[2] + radius};
    } else
    {
        // Axis-aligned approximation: the box's rotation is ignored for
        // contact purposes (documented limitation - rotated box colliders
        // need the full OBB path a heavier provider brings).
        bounds.minPoint
            = {object.position[0] - object.size[0],
                object.position[1] - object.size[1],
                object.position[2] - object.size[2]};
        bounds.maxPoint
            = {object.position[0] + object.size[0],
                object.position[1] + object.size[1],
                object.position[2] + object.size[2]};
    }
    return bounds;
}

// std::array<float, 3> <-> glm::vec3 pack/unpack: the capability contracts
// speak plain arrays (published headers stay math-library-free), the solver
// computes in GLM. make_vec3 reinterprets the storage; the pack direction is
// spelled out componentwise.
glm::vec3 asVec3(
    const std::array<float, 3> &components
)
{
    return glm::make_vec3(components.data());
}

std::array<float, 3> asArray(
    const glm::vec3 &vector
)
{
    return {vector.x, vector.y, vector.z};
}

} // namespace

namespace
{

// Table field helper for the bridge's body descriptions.
double numberField(
    const sol::table &table, const char *key, double fallback
)
{
    const sol::object value = table[key];
    if (value.get_type() == sol::type::number)
    {
        return value.as<double>();
    }
    return fallback;
}

// The dedicated Lua bridge extension object for the physics module. Owns the
// sol2 wiring that installs engine.add_physics_body, engine.remove_physics_body,
// engine.set_body_velocity, engine.body_velocity, engine.set_gravity,
// engine.body_count, engine.set_body_sleeping, engine.is_body_awake and
// engine.raycast into every script state. Holds a raw pointer to the
// PhysicsCapability the module provides.
class PhysicsLuaBridge final : public LuaBridgeExtension
{
public:
    explicit PhysicsLuaBridge(
        PhysicsCapability *capability
    )
        : capability(capability)
    {}

    const char *extensionName() const override
    {
        return "physics";
    }

    void installLuaBridge(
        luaState *lua
    ) override
    {
        // The host (lua_scripting) guarantees the engine table exists when
        // this is called. The bridge functions call the PhysicsCapability
        // methods directly through the captured pointer - no capability
        // acquisition, no permission check.
        sol::state_view luaView(lua);
        sol::table engine = luaView["engine"];

        engine.set_function(
            "add_physics_body", [this](std::uint32_t objectHandle, const sol::table &body) -> std::uint32_t {
                PhysicsCapability::BodyDescription description;
                description.restitution   = static_cast<float>(numberField(body, "restitution", 0.4));
                description.linearDamping = static_cast<float>(numberField(body, "damping", 0.02));
                return capability->addDynamicBody(objectHandle, description);
            }
        );
        engine.set_function("remove_physics_body", [this](std::uint32_t bodyHandle) {
            return capability->removeBody(bodyHandle);
        });
        engine.set_function("set_body_velocity", [this](std::uint32_t bodyHandle, float x, float y, float z) {
            return capability->setBodyVelocity(bodyHandle, {x, y, z});
        });
        engine.set_function("body_velocity", [this](std::uint32_t bodyHandle, sol::this_state state) -> sol::object {
            std::array<float, 3> velocity = {0.0f, 0.0f, 0.0f};
            capability->bodyVelocity(bodyHandle, velocity);
            sol::state_view view(state);
            return sol::make_object(view, std::make_tuple(velocity[0], velocity[1], velocity[2]));
        });
        engine.set_function("set_gravity", [this](float x, float y, float z) { capability->setGravity({x, y, z}); });
        engine.set_function("body_count", [this]() { return capability->bodyCount(); });
        engine.set_function("set_body_sleeping", [this](std::uint32_t bodyHandle, bool sleeping) {
            return capability->setBodySleeping(bodyHandle, sleeping);
        });
        engine.set_function("is_body_awake", [this](std::uint32_t bodyHandle) {
            return capability->isBodyAwake(bodyHandle);
        });
        engine.set_function(
            "raycast",
            [this](
                float ox,
                float oy,
                float oz,
                float dx,
                float dy,
                float dz,
                sol::object maxDistance,
                sol::this_state state
            ) -> sol::object {
                RaycastResult result;
                if (!capability->raycast(
                        {ox, oy, oz},
                        {dx, dy, dz},
                        maxDistance.valid() ? static_cast<float>(maxDistance.as<double>()) : 100.0f,
                        result
                    ))
                {
                    return sol::make_object(state, sol::nil); // a miss is nil, not a table of defaults
                }
                sol::state_view view(state);
                sol::table hit   = view.create_table();
                hit["body"]      = result.bodyHandle;
                hit["object"]    = result.objectHandle;
                hit["distance"]  = result.distance;
                hit["point"]     = view.create_table();
                hit["point"][1]  = result.point[0];
                hit["point"][2]  = result.point[1];
                hit["point"][3]  = result.point[2];
                hit["normal"]    = view.create_table();
                hit["normal"][1] = result.normal[0];
                hit["normal"][2] = result.normal[1];
                hit["normal"][3] = result.normal[2];
                return sol::make_object(view, hit);
            }
        );
    }

private:
    PhysicsCapability *capability;
};

} // namespace

PhysicsModule::PhysicsModule()
{
    componentManifest = ComponentManifest{
        .name    = "physics",
        .kind    = ComponentKind::Module,
        .version = "0.1.0",
        // The implementation-bound member name of the "physics" family: a heavier
        // provider (Jolt, for example) declares its own member and wins through
        // the [providers] configuration; mods keep demanding the family.
        .providedCapabilities
        = {"physics.builtin",
            // The Lua bridge extension member: declares this module as a
            // provider of the "scripting.lua_bridge" family. The
            // lua_scripting host acquires every active member through
            // ComponentContext::acquireEachCapability and asks each
            // one to install its own bindings into the host's engine
            // table.
            "scripting.lua_bridge.physics"},
        .requiredCapabilities = {"logging", "scene", "bvh"},
        .optionalCapabilities = {},
        .requiredComponents   = {},
        .requiredPermissions  = {"capability.logging", "capability.scene", "capability.bvh"},
        .knownConfigKeys      = {
            "physics.gravity",
            "physics.ground_enabled",
            "physics.ground_height",
            "physics.max_step_seconds",
            "physics.sleep_speed",
            "physics.sleep_delay",
            "physics.substeps",
            "physics.restitution_mode",
            "physics.fixed_hz",
        },
    };
}

PhysicsModule::~PhysicsModule()
{
    stop();
}

float PhysicsModule::combineRestitution(
    float first, float second
) const
{
    // One branch per contact on an enum parsed once at construct: the same
    // cost class as the historical std::min it replaces for the default mode.
    switch (restitutionMode)
    {
    case RestitutionMode::Max:
        return std::max(first, second);
    case RestitutionMode::Average:
        return (first + second) * 0.5f;
    case RestitutionMode::Multiply:
        return first * second;
    case RestitutionMode::Min:
        break;
    }
    return std::min(first, second);
}

void PhysicsModule::construct(
    ComponentContext &context
)
{
    boundContext    = &context;
    logger          = &context.acquireCapability<LoggerCapability>();
    sceneCapability = &context.acquireCapability<SceneCapability>();
    bvhCapability   = &context.acquireCapability<BvhCapability>();

    const std::vector<double> configuredGravity = context.configurationFloatingArrayInSection("physics", "gravity");
    if (configuredGravity.size() == 3)
    {
        gravity_
            = {static_cast<float>(configuredGravity[0]),
                static_cast<float>(configuredGravity[1]),
                static_cast<float>(configuredGravity[2])};
    }
    groundEnabled = context.configurationBooleanInSection("physics", "ground_enabled").value_or(true);
    groundHeight = static_cast<float>(context.configurationFloatingInSection("physics", "ground_height").value_or(0.0));
    maxStepSeconds = static_cast<float>(
        context.configurationFloatingInSection("physics", "max_step_seconds").value_or(1.0 / 30.0)
    );
    // A zero, negative or non-finite cap clamps every frame delta to nothing
    // (min(delta, <=0) never steps): the simulation silently freezes. Fail at
    // construct like restitution_mode below - a precise startup error beats a
    // dead simulator.
    if (!(maxStepSeconds > 0.0f) || !std::isfinite(maxStepSeconds))
    {
        throw std::runtime_error(
            "physics module: [physics] max_step_seconds must be a positive finite number of seconds"
        );
    }
    sleepSpeed = static_cast<float>(context.configurationFloatingInSection("physics", "sleep_speed").value_or(0.15));
    sleepDelay = context.configurationFloatingInSection("physics", "sleep_delay").value_or(2.0);
    // Substepping: 1 (the default) is the historical single step of the
    // clamped delta. Higher counts split that same clamped delta into equal
    // slices and run the solver once per slice - faster-moving bodies get
    // more contact samples per frame at linearly more solver cost. The cap
    // keeps a typo from turning one frame into a physics warm loop.
    substeps = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(context.configurationIntegerInSection("physics", "substeps").value_or(1), 1, 8)
    );
    // Fixed timestep ([physics].fixed_hz, default 0 = off, the historical
    // variable stepping): when set, real time accumulates and the simulation
    // consumes it in exact 1/fixed_hz increments - every step gets the same
    // dt, so the simulation is frame-rate independent and reproducible, the
    // classic "fix your timestep" contract. The same max_step_seconds clamp
    // bounds the accumulated time (a stalled frame drops the excess instead
    // of spiraling), and substeps still divides each FIXED step, so raising
    // substeps only refines integration there too.
    const double configuredFixedHz = context.configurationFloatingInSection("physics", "fixed_hz").value_or(0.0);
    if (configuredFixedHz > 0.0)
    {
        fixedStepSeconds = 1.0 / std::clamp(configuredFixedHz, 10.0, 1000.0);
    }
    // Restitution combine rule for a contact between two bodies. The
    // historical rule is min (the conservative choice: a rubber ball hitting
    // concrete bounces like concrete). "max" (the Game Physics convention
    // some engines ship as default), "average" and "multiply" are the other
    // combine modes material systems commonly want; an unknown spelling is a
    // precise startup error because the mode silently changes every bounce.
    const std::string restitutionModeName
        = context.configurationStringInSection("physics", "restitution_mode").value_or("min");
    if (restitutionModeName == "min")
    {
        restitutionMode = RestitutionMode::Min;
    } else if (restitutionModeName == "max")
    {
        restitutionMode = RestitutionMode::Max;
    } else if (restitutionModeName == "average")
    {
        restitutionMode = RestitutionMode::Average;
    } else if (restitutionModeName == "multiply")
    {
        restitutionMode = RestitutionMode::Multiply;
    } else
    {
        throw std::runtime_error(
            "physics module: [physics] restitution_mode '" + restitutionModeName
            + "' is not a known mode (min, max, average, multiply)"
        );
    }

    // The simulation step rides the kernel's EngineFrameEvent heartbeat (the
    // composition root's loop clock), not the renderer's FrameBeginEvent: no
    // renderer capability include is needed, and a headless run - a dedicated
    // server, a batch simulation - steps exactly like a rendered one.
    EventBus &bus         = context.eventBus();
    frameTickChannel      = &bus.channel<EngineFrameEvent>();
    frameTickSubscription = EventSubscription(
        &bus,
        typeid(EngineFrameEvent),
        frameTickChannel->subscribe<PhysicsModule, &PhysicsModule::forwardFrameTick>(this)
    );

    // Construct the Lua bridge extension object. The module IS the
    // PhysicsCapability, so `this` is the capability pointer the bridge
    // calls back into.
    luaBridge = std::make_unique<PhysicsLuaBridge>(this);
}

void PhysicsModule::start()
{
    logger->logMessage(
        LogLevel::Information,
        "physics module started (builtin provider, gravity [" + std::to_string(gravity_[0]) + " "
            + std::to_string(gravity_[1]) + " " + std::to_string(gravity_[2]) + "], ground "
            + (groundEnabled ? "enabled" : "disabled") + ")"
    );
}

void PhysicsModule::stop()
{
    frameTickSubscription.release();
    bodies.clear();
    denseBodies.clear();
    // Reset the Lua bridge before nulling the capability pointers it captures.
    luaBridge.reset();
    boundContext    = nullptr;
    logger          = nullptr;
    sceneCapability = nullptr;
    bvhCapability   = nullptr;
}

void *PhysicsModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    if (capabilityName == "physics.builtin")
    {
        return static_cast<PhysicsCapability *>(this);
    }
    // The "scripting.lua_bridge.physics" member: returns the dedicated
    // PhysicsLuaBridge object this module owns (constructed in construct()).
    if (capabilityName == "scripting.lua_bridge.physics")
    {
        return luaBridge.get();
    }
    return nullptr;
}

PhysicsCapability::BodyHandle PhysicsModule::addDynamicBody(
    SceneCapability::ObjectHandle objectHandle, const BodyDescription &description
)
{
    // The transform slice is all this capability consumes of the object: the
    // position/size screen below and the collider shape derive from it, and
    // the full descriptor readback would copy the material payload (the
    // texture-set string) for nothing.
    SceneObjectTransform object;
    if (objectHandle == SceneCapability::invalidObject || !sceneCapability->objectTransform(objectHandle, object))
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning,
                "physics module: addDynamicBody refused (object " + std::to_string(objectHandle) + " is not live)"
            );
        }
        return invalidBody;
    }
    Body body;
    body.handle       = nextHandle++;
    body.objectHandle = objectHandle;
    body.description  = description;
    body.asleep       = description.startsAsleep;
    // Capability-boundary NaN screening: a script can hand over inf/nan
    // numbers (0/0 in Lua, huge table values); one poisoned field spreads
    // through integration and contact resolution, so refuse here instead.
    // The object's radius/half extents and position ride in from the scene
    // transform and are screened with the description's own numbers.
    const bool numbersFinite = std::isfinite(description.restitution) && std::isfinite(description.linearDamping)
                            && std::isfinite(object.position[0]) && std::isfinite(object.position[1])
                            && std::isfinite(object.position[2]) && std::isfinite(object.size[0])
                            && std::isfinite(object.size[1]) && std::isfinite(object.size[2]);
    if (!numbersFinite)
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning,
                "physics module: addDynamicBody refused (object " + std::to_string(objectHandle)
                    + " carries non-finite numbers - position, size and description must all be finite)"
            );
        }
        return invalidBody;
    }
    Body &stored = bodies.emplace(body.handle, body).first->second;
    denseBodies.push_back(&stored);
    if (logger != nullptr)
    {
        logger->logMessage(
            LogLevel::Debug,
            "physics module: body " + std::to_string(body.handle) + " bound to object " + std::to_string(objectHandle)
                + (body.asleep ? " (starts asleep)" : "")
        );
    }
    return body.handle;
}

bool PhysicsModule::removeBody(
    BodyHandle handle
)
{
    const auto found = bodies.find(handle);
    if (found == bodies.end())
    {
        return false;
    }
    // The dense mirror drops its pointer FIRST: it must never outlive the
    // map node it points at. Order-preserving erase - body removal is a
    // rare, off-frame-path mutation, and keeping the order keeps the
    // iteration order (and the pair pass's) stable for the survivors.
    for (std::size_t index = 0; index < denseBodies.size(); ++index)
    {
        if (denseBodies[index] == &found->second)
        {
            denseBodies.erase(denseBodies.begin() + static_cast<std::ptrdiff_t>(index));
            break;
        }
    }
    bodies.erase(found);
    return true;
}

bool PhysicsModule::setBodyVelocity(
    BodyHandle handle, const std::array<float, 3> &velocity
)
{
    const auto found = bodies.find(handle);
    if (found == bodies.end())
    {
        return false;
    }
    // Non-finite velocities poison every later integration step; ignore and
    // report the refusal rather than storing them.
    if (!std::isfinite(velocity[0]) || !std::isfinite(velocity[1]) || !std::isfinite(velocity[2]))
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning,
                "physics module: setBodyVelocity refused (body " + std::to_string(handle) + " velocity must be finite)"
            );
        }
        return false;
    }
    found->second.velocity = velocity;
    // A velocity assignment is an explicit intent to move: sleeping ends.
    if (found->second.asleep && (velocity[0] != 0.0f || velocity[1] != 0.0f || velocity[2] != 0.0f))
    {
        found->second.asleep           = false;
        found->second.stillnessSeconds = 0.0;
    }
    return true;
}

bool PhysicsModule::bodyVelocity(
    BodyHandle handle, std::array<float, 3> &outVelocity
) const
{
    const auto found = bodies.find(handle);
    if (found == bodies.end())
    {
        return false;
    }
    outVelocity = found->second.velocity;
    return true;
}

void PhysicsModule::setGravity(
    const std::array<float, 3> &gravity
)
{
    // A non-finite gravity accelerates every body into NaN territory within
    // one step; keep the previous gravity and log the refusal.
    if (!std::isfinite(gravity[0]) || !std::isfinite(gravity[1]) || !std::isfinite(gravity[2]))
    {
        if (logger != nullptr)
        {
            logger->logMessage(LogLevel::Warning, "physics module: setGravity refused (gravity must be finite)");
        }
        return;
    }
    gravity_ = gravity;
}

std::array<float, 3> PhysicsModule::gravity() const
{
    return gravity_;
}

std::uint32_t PhysicsModule::bodyCount() const
{
    return static_cast<std::uint32_t>(bodies.size());
}

bool PhysicsModule::setBodySleeping(
    BodyHandle handle, bool sleeping
)
{
    const auto found = bodies.find(handle);
    if (found == bodies.end())
    {
        return false;
    }
    if (sleeping)
    {
        // Freezing the body where it stands: velocity goes to zero so a later
        // wake does not launch the object with stale momentum.
        found->second.velocity = {0.0f, 0.0f, 0.0f};
        found->second.asleep   = true;
    } else
    {
        found->second.asleep           = false;
        found->second.stillnessSeconds = 0.0;
    }
    return true;
}

bool PhysicsModule::isBodyAwake(
    BodyHandle handle
) const
{
    const auto found = bodies.find(handle);
    return found != bodies.end() && !found->second.asleep;
}

bool PhysicsModule::raycast(
    const std::array<float, 3> &origin,
    const std::array<float, 3> &direction,
    float maxDistance,
    RaycastResult &outResult
)
{
    outResult                    = RaycastResult{};
    const glm::vec3 rayOrigin    = asVec3(origin);
    const glm::vec3 rayDirection = asVec3(direction);
    const float lengthSquared    = glm::dot(rayDirection, rayDirection);
    if (lengthSquared < 1e-12f)
    {
        return false; // a zero-length ray hits nothing by definition
    }
    const float length   = std::sqrt(lengthSquared);
    const glm::vec3 unit = rayDirection / length;

    bool best = false;
    // maxDistance is in world units along the ray; every t below is a world
    // distance because all the intersection math runs on the normalized
    // direction. (Scaling the cutoff by |direction| - the old behavior -
    // let an unnormalized direction travel |d|x too far and report |d|x
    // too small a distance; the contract allows unnormalized directions.)
    float bestT   = maxDistance;
    auto consider = [&](float t,
                        const std::array<float, 3> &normal,
                        BodyHandle bodyHandle,
                        SceneCapability::ObjectHandle objectHandle) {
        if (t >= 0.0f && t < bestT)
        {
            const glm::vec3 hitPoint = rayOrigin + unit * t;
            bestT                    = t;
            best                     = true;
            outResult.hit            = true;
            outResult.bodyHandle     = bodyHandle;
            outResult.objectHandle   = objectHandle;
            outResult.distance       = t;
            outResult.point          = asArray(hitPoint);
            outResult.normal         = normal;
        }
    };

    for (const Body *bodyPointer : denseBodies)
    {
        const Body &body = *bodyPointer;
        SceneObjectTransform object;
        if (!sceneCapability->objectTransform(body.objectHandle, object))
        {
            continue;
        }
        if (object.type == scene_object_type::sphere)
        {
            // Ray-sphere: |o + t*d - c| = r, the standard quadratic.
            const glm::vec3 center   = asVec3(object.position);
            const glm::vec3 offset   = rayOrigin - center;
            const float b            = glm::dot(offset, unit);
            const float c            = glm::dot(offset, offset) - object.size[0] * object.size[0];
            const float discriminant = b * b - c;
            if (discriminant < 0.0f)
            {
                continue;
            }
            // Both roots: a ray starting inside the sphere has a negative
            // near root and hits through the far root (the box path accepts
            // that case too), so hand both to consider - it filters to the
            // smallest non-negative t.
            const float sqrtD          = std::sqrt(discriminant);
            const float nearRoot       = -b - sqrtD;
            const float farRoot        = -b + sqrtD;
            const glm::vec3 nearNormal = glm::normalize(rayOrigin + unit * nearRoot - center);
            consider(nearRoot, asArray(nearNormal), body.handle, body.objectHandle);
            const glm::vec3 farNormal = glm::normalize(rayOrigin + unit * farRoot - center);
            consider(farRoot, asArray(farNormal), body.handle, body.objectHandle);
        } else
        {
            // Ray-AABB slab test (boxes and mesh proxies; rotation ignored -
            // the provider's documented AABB approximation).
            const AxisAlignedBoundingBox bounds = objectBounds(object);
            float tNear                         = -1e30f;
            float tFar                          = 1e30f;
            std::size_t axisOfNear              = 0;
            float nearSign                      = -1.0f;
            std::size_t axisOfFar               = 0;
            float farSign                       = 1.0f;
            bool rejected                       = false;
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                if (std::abs(unit[axis]) < 1e-9f)
                {
                    if (origin[axis] < bounds.minPoint[axis] || origin[axis] > bounds.maxPoint[axis])
                    {
                        rejected = true;
                        break;
                    }
                    continue;
                }
                const float inverse = 1.0f / unit[axis];
                const float t0      = (bounds.minPoint[axis] - origin[axis]) * inverse;
                const float t1      = (bounds.maxPoint[axis] - origin[axis]) * inverse;
                const float entryT  = std::min(t0, t1);
                const float exitT   = std::max(t0, t1);
                if (entryT > tNear)
                {
                    tNear      = entryT;
                    axisOfNear = axis;
                    nearSign   = t0 > t1 ? 1.0f : -1.0f; // entering through the min face or the max face
                }
                if (exitT < tFar)
                {
                    tFar      = exitT;
                    axisOfFar = axis;
                    farSign   = t0 > t1 ? -1.0f : 1.0f; // exiting through the min face or the max face
                }
                if (tNear > tFar)
                {
                    rejected = true;
                    break;
                }
            }
            if (rejected || tFar < 0.0f)
            {
                continue;
            }
            // Inside the box: the ray exits through the far face, so both the
            // distance and the normal come from the far plane (reporting the
            // entry-face normal with the exit distance pointed the hit the
            // wrong way).
            const bool inside = tNear < 0.0f;
            const float t     = inside ? tFar : tNear;
            std::array<float, 3> normal{0.0f, 0.0f, 0.0f};
            normal[inside ? axisOfFar : axisOfNear] = inside ? farSign : nearSign;
            consider(t, normal, body.handle, body.objectHandle);
        }
    }

    // The ground plane (an infinite horizontal half-space boundary). Rays
    // from above pointing down hit it like every other collider.
    if (groundEnabled && unit[1] < -1e-6f && origin[1] > groundHeight)
    {
        const float t = (groundHeight - origin[1]) / unit[1];
        consider(t, {0.0f, 1.0f, 0.0f}, invalidBody, SceneCapability::invalidObject);
    }
    return best;
}

void PhysicsModule::wakeOnContact(
    Body &body, float impulseMagnitude
)
{
    if (body.asleep && impulseMagnitude > sleepSpeed)
    {
        body.asleep           = false;
        body.stillnessSeconds = 0.0;
    }
}

void PhysicsModule::forwardFrameTick(
    const EngineFrameEvent &event
)
{
    // The frame clock (docs/architecture.md): the heartbeat's deltaSeconds
    // is the iteration's delta, measured once by the composition root and
    // shared by every frame-tick module. The module reads no clock of its
    // own; the first frame of a run carries delta 0, which integrates
    // nothing - the same no-time-yet contract the seeded self-measured clock
    // provided, without the per-frame clock read or the seed state.
    const double frameDelta = event.deltaSeconds;
    if (fixedStepSeconds > 0.0)
    {
        // Fixed timestep path: accumulate real time (clamped exactly like
        // the variable path), consume it in exact fixed increments. The
        // remainder carries to the next frame - no time is invented or
        // lost, only bounded by the same stall clamp.
        fixedStepAccumulator += std::min(frameDelta, static_cast<double>(maxStepSeconds));
        while (fixedStepAccumulator >= fixedStepSeconds)
        {
            const double slice = fixedStepSeconds / static_cast<double>(substeps);
            for (std::uint32_t substep = 0; substep < substeps; ++substep)
            {
                step(slice);
            }
            fixedStepAccumulator -= fixedStepSeconds;
        }
        return;
    }
    // Clamp, then step in slices: a stalled frame must not teleport bodies
    // through geometry, and a 1/30 s cap keeps the analytic contacts stable
    // without a substep tuning story. substeps (default 1) divides the same
    // clamped delta into equal slices - the total simulated time per frame is
    // unchanged, so raising it only refines the integration, never speeds it
    // up.
    const double clamped = std::min(frameDelta, static_cast<double>(maxStepSeconds));
    if (clamped > 0.0)
    {
        const double slice = clamped / static_cast<double>(substeps);
        for (std::uint32_t substep = 0; substep < substeps; ++substep)
        {
            step(slice);
        }
    }
}

void PhysicsModule::step(
    double seconds
)
{
    if (denseBodies.empty())
    {
        return;
    }
    // Snapshot the live state of every bound object, integrate, resolve,
    // write back. A body whose object vanished mid-step (removed by a script
    // between frames) is dropped. Asleep bodies are skipped entirely: they
    // rest exactly where they are and re-enter the simulation only when
    // woken (setBodyVelocity, setBodySleeping, a contact impulse).
    // deadHandlesScratch and stepStatesScratch are members: clearing them
    // preserves capacity, so the per-step drop-list and the per-body
    // snapshot stop touching the heap after the high-water mark. The
    // snapshot feeds resolveBodyPairs directly: the registry cannot change
    // inside the step (single thread, no reentrancy), so re-reading every
    // body there - the previous behavior, one to three extra registry reads
    // per body per step - bought nothing. Each snapshot also carries its
    // Body*, so the pair pass below stops re-hashing the body map per
    // contacting pair.
    //
    // The bodies iterate through the dense pointer mirror (denseBodies):
    // the hash map's node layout costs a pointer chase per body per substep
    // just to visit the set, where the mirror is one contiguous array. The
    // readbacks are transform-only (objectTransform): the full descriptor
    // this loop used to copy per body per step carried the material payload
    // - a texture-set string copy per body per step, a heap round trip
    // whenever the name outgrows its small-string buffer - for fields the
    // simulation never reads.
    deadHandlesScratch.clear();
    stepStatesScratch.clear();
    stepStatesScratch.reserve(denseBodies.size());
    // Gravity is loop-invariant: one unpack per step instead of one per body
    // per substep (the per-body work is the damping exp, which depends on
    // each body's own linearDamping and stays where it is).
    const float dtF           = static_cast<float>(seconds);
    const glm::vec3 gravity   = asVec3(gravity_);
    const glm::vec3 gravityDt = gravity * dtF;
    for (Body *bodyPointer : denseBodies)
    {
        Body &body = *bodyPointer;
        SceneObjectTransform object;
        if (!sceneCapability->objectTransform(body.objectHandle, object))
        {
            deadHandlesScratch.push_back(body.handle);
            continue;
        }

        if (body.asleep)
        {
            // Sleeping bodies still participate in the pair pass; their
            // transform enters the snapshot exactly as read.
            const AxisAlignedBoundingBox asleepBounds = objectBounds(object);
            stepStatesScratch.push_back(BodyStepState{body.handle, &body, object, asleepBounds});
            continue;
        }

        // The as-read position, kept for the no-op write-back check below.
        const std::array<float, 3> asReadPosition = object.position;
        // Semi-implicit Euler: velocity first, then position - the stable
        // order for gravity-driven motion under restitution.
        glm::vec3 velocity = asVec3(body.velocity);
        velocity += gravityDt;
        // Exponential damping instead of the old (1 - damping*dt) factor:
        // the linear form crosses zero into AMPLIFICATION once damping*dt > 1
        // (a long frame or a large damping setting), while exp() is
        // unconditionally stable and frame-rate independent.
        velocity *= std::exp(-body.description.linearDamping * dtF);
        const glm::vec3 position = asVec3(object.position) + velocity * dtF;
        body.velocity            = asArray(velocity);
        object.position          = asArray(position);

        const AxisAlignedBoundingBox preContactBounds = objectBounds(object);
        // resolveGroundContact reports whether it moved the snapshot: bounds
        // are a pure function of the transform, so an unanswered ground pass
        // leaves preContactBounds EXACTLY the post-contact value and the
        // second derivation this loop used to run per awake body per step
        // re-computed numbers already in hand. Only a ground push (a body
        // resting on or sinking into the plane) re-derives.
        const bool groundMovedTheObject = resolveGroundContact(body, object, preContactBounds, seconds);

        // Auto-sleep AFTER ground resolution: the ground pass zeroes the
        // vertical velocity of a resting body (and damps the tangential
        // one), so evaluating before it meant a body sitting on the plane
        // always carried contact residue above the threshold and never fell
        // asleep. Stillness accumulates on the post-contact velocity below
        // exactly as it did before.
        const glm::vec3 resolvedVelocity = asVec3(body.velocity);
        const float speedSquared         = glm::dot(resolvedVelocity, resolvedVelocity);
        if (speedSquared < sleepSpeed * sleepSpeed)
        {
            body.stillnessSeconds += seconds;
            if (body.stillnessSeconds >= sleepDelay)
            {
                body.asleep           = true;
                body.stillnessSeconds = 0.0;
                body.velocity         = {0.0f, 0.0f, 0.0f};
                if (logger != nullptr && logger->wouldLog(LogLevel::Debug))
                {
                    logger->logMessage(
                        LogLevel::Debug, "physics module: body " + std::to_string(body.handle) + " fell asleep"
                    );
                }
            }
        } else
        {
            body.stillnessSeconds = 0.0;
        }

        // One write-back per body per step: the integrated AND ground-resolved
        // state. The previous code wrote the ground push and then immediately
        // overwrote it with the uncorrected integration result - the push-out
        // never reached the registry, contradicting the documented
        // integrate-resolve-write-back order. One event per changed transform
        // per step also halves the SceneObjectChangedEvent traffic the
        // renderer's dirty flag used to absorb.
        //
        // The no-op skip: a body whose resolved transform equals the one the
        // registry already holds (the ground cycle's fixed point - sag and
        // push-out cancel) would write bit-identical floats and fire a change
        // event that re-dirties the renderer's frame for content that moved
        // not at all. Exact float equality, no epsilon: writing identical
        // values is unobservable, so skipping the write AND the event is too
        // - and once a body reaches its fixed point it reproduces the same
        // numbers every step, so the skip holds for every later step.
        if (object.position[0] != asReadPosition[0] || object.position[1] != asReadPosition[1]
            || object.position[2] != asReadPosition[2])
        {
            sceneCapability->setObjectTransform(body.objectHandle, object.position, object.rotationYRadians);
        }

        const AxisAlignedBoundingBox resolvedBounds = groundMovedTheObject ? objectBounds(object) : preContactBounds;
        stepStatesScratch.push_back(BodyStepState{body.handle, &body, object, resolvedBounds});
    }
    for (BodyHandle handle : deadHandlesScratch)
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Debug,
                "physics module: body " + std::to_string(handle) + " dropped (its scene object was removed)"
            );
        }
        removeBody(handle);
    }

    resolveBodyPairs();
}

bool PhysicsModule::resolveGroundContact(
    Body &body, SceneObjectTransform &object, const AxisAlignedBoundingBox &worldBounds, double dt
)
{
    (void)dt;
    if (!groundEnabled)
    {
        return false;
    }
    // Penetration of the object's lowest point into the ground plane.
    const float penetration = groundHeight - worldBounds.minPoint[1];
    if (penetration <= 0.0f)
    {
        return false;
    }
    // Push out along Y (in place on the step's snapshot - the caller's single
    // write-back publishes it), reflect the vertical velocity per restitution,
    // damp the tangential velocity a touch (the built-in provider's stand-in
    // for friction - documented as such). The boolean return tells the caller
    // whether the push happened: only then do the snapshot's bounds need
    // re-deriving (see the header comment).
    object.position[1] += penetration;
    if (body.velocity[1] < 0.0f)
    {
        body.velocity[1] = -body.velocity[1] * body.description.restitution;
        if (std::abs(body.velocity[1]) < 0.15f)
        {
            body.velocity[1] = 0.0f; // rest: no micro-bouncing forever
        }
        body.velocity[0] *= 0.98f;
        body.velocity[2] *= 0.98f;
    }
    return true;
}

void PhysicsModule::resolveBodyPairs()
{
    if (denseBodies.size() < 2)
    {
        return;
    }
    // Broadphase: one BVH over the bodies' current world bounds, then the
    // narrowphase on the surviving pairs. The bvh module rebuilds its
    // hierarchy per call (at the registry cap: microseconds) - the capability
    // keeps the pair loop near-linear without any tuning surface. The
    // bounds/pairNeighbors buffers are member scratch vectors: clearing them
    // preserves capacity (and the BVH's overlappingPairs preserves the
    // inner-vector capacity of pairNeighbors across calls), so the per-step
    // broadphase stops touching the heap after the high-water mark. The
    // state comes from stepStatesScratch, captured in step(): zero registry
    // reads here, where the previous code re-read every body once for bounds
    // and twice more per contacting pair. The broadphase previously ALSO
    // copied every descriptor (std::string included) into a parallel scratch
    // vector; the snapshot is the same data in the same order, so the pair
    // pass now reads it where it lives - one copy per body per step dropped.
    broadphaseBoundsScratch.clear();
    broadphaseBoundsScratch.reserve(stepStatesScratch.size());
    for (const BodyStepState &state : stepStatesScratch)
    {
        broadphaseBoundsScratch.push_back(state.bounds);
    }
    bvhCapability->overlappingPairs(broadphaseBoundsScratch, broadphasePairNeighborsScratch);

    for (std::size_t index = 0; index < stepStatesScratch.size(); ++index)
    {
        for (std::uint32_t neighborIndex : broadphasePairNeighborsScratch[index])
        {
            if (static_cast<std::size_t>(neighborIndex) <= index)
            {
                continue; // each unordered pair once
            }
            const BodyStepState &firstState          = stepStatesScratch[index];
            const BodyStepState &secondState         = stepStatesScratch[neighborIndex];
            Body *first                              = firstState.body;
            Body *second                             = secondState.body;
            const SceneObjectTransform &firstObject  = firstState.object;
            const SceneObjectTransform &secondObject = secondState.object;
            const bool firstIsSphere                 = firstObject.type == scene_object_type::sphere;
            const bool secondIsSphere                = secondObject.type == scene_object_type::sphere;
            if (firstIsSphere && secondIsSphere)
            {
                resolveSphereSphere(*first, firstObject, *second, secondObject);
            } else if (firstIsSphere != secondIsSphere)
            {
                // The box side's bounds ride in from the snapshot (see the
                // resolver comments in the header): the state was built by
                // the same objectBounds call this loop used to repeat per
                // pair.
                if (firstIsSphere)
                {
                    resolveSphereBox(*first, firstObject, *second, secondState.bounds);
                } else
                {
                    resolveSphereBox(*second, secondObject, *first, firstState.bounds);
                }
            } else
            {
                // Axis-aligned box vs box: resolved along the shallowest
                // penetration axis (the provider's documented AABB
                // approximation - rotated boxes need the OBB path a heavier
                // provider brings).
                resolveBoxBox(*first, firstObject, firstState.bounds, *second, secondObject, secondState.bounds);
            }
        }
    }
}

void PhysicsModule::resolveSphereSphere(
    Body &first, const SceneObjectTransform &firstObject, Body &second, const SceneObjectTransform &secondObject
)
{
    const float radiusSum = firstObject.size[0] + secondObject.size[0];
    const glm::vec3 delta = asVec3(secondObject.position) - asVec3(firstObject.position);
    const float distance  = glm::length(delta);
    if (distance >= radiusSum || distance < 1e-6f)
    {
        return;
    }
    const glm::vec3 normal  = delta / distance;
    const float penetration = radiusSum - distance;

    // Split the positional correction between the two bodies, then resolve
    // the relative velocity along the normal with restitution.
    const glm::vec3 firstPosition      = asVec3(firstObject.position) - normal * penetration * 0.5f;
    const glm::vec3 secondPosition     = asVec3(secondObject.position) + normal * penetration * 0.5f;
    const glm::vec3 relativeVelocity   = asVec3(second.velocity) - asVec3(first.velocity);
    const float relativeNormalVelocity = glm::dot(relativeVelocity, normal);
    if (relativeNormalVelocity < 0.0f)
    {
        const float restitution = combineRestitution(first.description.restitution, second.description.restitution);
        const float impulse     = -(1.0f + restitution) * relativeNormalVelocity * 0.5f;
        const glm::vec3 firstVelocity  = asVec3(first.velocity) - normal * impulse;
        const glm::vec3 secondVelocity = asVec3(second.velocity) + normal * impulse;
        first.velocity                 = asArray(firstVelocity);
        second.velocity                = asArray(secondVelocity);
        // A sleeping body stays asleep under a feather's touch but wakes when
        // something fast hits it.
        wakeOnContact(first, impulse);
        wakeOnContact(second, impulse);
    }
    sceneCapability->setObjectTransform(first.objectHandle, asArray(firstPosition), firstObject.rotationYRadians);
    sceneCapability->setObjectTransform(second.objectHandle, asArray(secondPosition), secondObject.rotationYRadians);
}

void PhysicsModule::resolveSphereBox(
    Body &sphereBody, const SceneObjectTransform &sphereObject, Body &boxBody, const AxisAlignedBoundingBox &boxBounds
)
{
    // The box bounds arrive from the step snapshot (BodyStepState.bounds):
    // derived from this exact descriptor by the same objectBounds the old
    // per-call computation ran, and the descriptor is never mutated inside
    // the pair pass (results go back through the registry), so the cached
    // value and the recomputed one are bit-identical.
    // Closest point on the box to the sphere center (axis-aligned).
    const glm::vec3 center  = asVec3(sphereObject.position);
    const glm::vec3 closest = glm::clamp(center, asVec3(boxBounds.minPoint), asVec3(boxBounds.maxPoint));
    const glm::vec3 delta   = center - closest;
    const float distance    = glm::length(delta);
    const float radius      = sphereObject.size[0];

    // Containment scan: a center inside the box clamps to itself, so delta
    // collapses to zero and the surface-distance test below would early-out
    // with the bodies still interpenetrating. Detect containment up front and
    // remember the shallowest exit face while at it (also the fallback push
    // axis for the degenerate near-surface case further down).
    bool contained         = true;
    std::size_t shallowest = 0;
    float shallowestSign   = -1.0f;
    float bestDepth        = 1e9f;
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        if (sphereObject.position[axis] < boxBounds.minPoint[axis]
            || sphereObject.position[axis] > boxBounds.maxPoint[axis])
        {
            contained = false;
            break;
        }
        const float toMin = sphereObject.position[axis] - boxBounds.minPoint[axis];
        const float toMax = boxBounds.maxPoint[axis] - sphereObject.position[axis];
        const float depth = std::min(toMin, toMax);
        if (depth < bestDepth)
        {
            bestDepth      = depth;
            shallowest     = axis;
            shallowestSign = toMin <= toMax ? -1.0f : 1.0f; // exit through the min face or the max face
        }
    }

    glm::vec3 normal{};
    float penetration = 0.0f;
    if (contained)
    {
        // Inside: exit through the nearest face - the push spans the
        // center-to-face depth plus the full radius, so the sphere ends up
        // resting on the box surface instead of on its rim.
        normal[static_cast<glm::length_t>(shallowest)] = shallowestSign;
        penetration                                    = radius + bestDepth;
    } else
    {
        if (distance >= radius || distance < 1e-6f)
        {
            return;
        }
        // Normal from the box surface towards the sphere center; the
        // degenerate near-zero delta falls back to the shallowest exit axis
        // from the scan above (delta/direction is meaningless there).
        if (distance > 1e-4f)
        {
            normal = delta / distance;
        } else
        {
            normal[static_cast<glm::length_t>(shallowest)] = shallowestSign;
        }
        penetration = radius - distance;
    }

    const glm::vec3 spherePosition     = center + normal * penetration;
    const glm::vec3 relativeVelocity   = asVec3(boxBody.velocity) - asVec3(sphereBody.velocity);
    const float relativeNormalVelocity = glm::dot(relativeVelocity, normal);
    if (relativeNormalVelocity < 0.0f)
    {
        const float restitution
            = combineRestitution(sphereBody.description.restitution, boxBody.description.restitution);
        const float impulse            = -(1.0f + restitution) * relativeNormalVelocity * 0.5f;
        const glm::vec3 sphereVelocity = asVec3(sphereBody.velocity) - normal * impulse;
        const glm::vec3 boxVelocity    = asVec3(boxBody.velocity) + normal * impulse;
        sphereBody.velocity            = asArray(sphereVelocity);
        boxBody.velocity               = asArray(boxVelocity);
        wakeOnContact(sphereBody, impulse);
        wakeOnContact(boxBody, impulse);
    }
    sceneCapability->setObjectTransform(
        sphereBody.objectHandle, asArray(spherePosition), sphereObject.rotationYRadians
    );
}

void PhysicsModule::resolveBoxBox(
    Body &first,
    const SceneObjectTransform &firstObject,
    const AxisAlignedBoundingBox &firstBounds,
    Body &second,
    const SceneObjectTransform &secondObject,
    const AxisAlignedBoundingBox &secondBounds
)
{
    // Axis-aligned overlap on every axis; no overlap on any axis means no
    // contact. The minimum TRANSLATION VECTOR axis carries the resolution:
    // push the two boxes apart half/half along the shallowest penetration,
    // then resolve the relative normal velocity like every other pair.
    // Both bounds arrive from the step snapshot - same values the two
    // objectBounds calls below used to rebuild per pair.
    std::size_t shallowest = 0;
    float penetration      = 0.0f;
    float shallowestDepth  = 1e9f;
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        const float overlap = std::min(firstBounds.maxPoint[axis], secondBounds.maxPoint[axis])
                            - std::max(firstBounds.minPoint[axis], secondBounds.minPoint[axis]);
        if (overlap <= 0.0f)
        {
            return; // separated on this axis
        }
        if (overlap < shallowestDepth)
        {
            shallowestDepth = overlap;
            shallowest      = axis;
            penetration     = overlap;
        }
    }
    const float direction = secondObject.position[shallowest] >= firstObject.position[shallowest] ? 1.0f : -1.0f;
    glm::vec3 normal{};
    normal[static_cast<glm::length_t>(shallowest)] = direction;

    const glm::vec3 firstCenter    = asVec3(firstObject.position);
    const glm::vec3 secondCenter   = asVec3(secondObject.position);
    const glm::vec3 push           = normal * penetration * 0.5f;
    const glm::vec3 firstPosition  = firstCenter - push;
    const glm::vec3 secondPosition = secondCenter + push;

    const glm::vec3 relativeVelocity   = asVec3(second.velocity) - asVec3(first.velocity);
    const float relativeNormalVelocity = glm::dot(relativeVelocity, normal);
    if (relativeNormalVelocity < 0.0f)
    {
        const float restitution = combineRestitution(first.description.restitution, second.description.restitution);
        const float impulse     = -(1.0f + restitution) * relativeNormalVelocity * 0.5f;
        const glm::vec3 firstVelocity  = asVec3(first.velocity) - normal * impulse;
        const glm::vec3 secondVelocity = asVec3(second.velocity) + normal * impulse;
        first.velocity                 = asArray(firstVelocity);
        second.velocity                = asArray(secondVelocity);
        wakeOnContact(first, impulse);
        wakeOnContact(second, impulse);
    }
    sceneCapability->setObjectTransform(first.objectHandle, asArray(firstPosition), firstObject.rotationYRadians);
    sceneCapability->setObjectTransform(second.objectHandle, asArray(secondPosition), secondObject.rotationYRadians);
}

} // namespace engine
