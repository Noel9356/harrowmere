#include "animation.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <sol/sol.hpp>
#include <string>

namespace engine
{

namespace
{

// The easing evaluator. Kept as one pure function: adding a curve is one case
// here plus one constant in animation_easing, and nothing else in the module
// knows the curve shapes exist.
float evaluateEasing(
    std::uint32_t easing, float t
)
{
    switch (easing)
    {
    case animation_easing::ease_in:
        return t * t;
    case animation_easing::ease_out:
        return 1.0f - (1.0f - t) * (1.0f - t);
    case animation_easing::ease_in_out:
        // Smoothstep: the classic C1-continuous in/out blend.
        return t * t * (3.0f - 2.0f * t);
    case animation_easing::linear:
    default:
        return t;
    }
}

// Maps the elapsed time into the curve parameter [0, 1] per the loop mode.
float curveParameter(
    const double elapsedSeconds, const double durationSeconds, const std::uint32_t mode
)
{
    // NaN fails every comparison, so the historical `<= 0.0` guard let a NaN
    // duration through (NaN <= 0.0 is false) and poisoned every transform
    // downstream. The negated form catches zero, negative AND NaN in one test.
    if (!(durationSeconds > 0.0))
    {
        return 1.0f;
    }
    const double phase = elapsedSeconds / durationSeconds;
    switch (mode)
    {
    case animation_mode::loop: {
        const double wrapped = phase - std::floor(phase);
        return static_cast<float>(wrapped);
    }
    case animation_mode::ping_pong: {
        // Triangle wave: 2 cycles per full period, mirrored back down.
        const double wrapped  = phase - std::floor(phase);
        const double triangle = wrapped <= 0.5 ? wrapped * 2.0 : 2.0 - wrapped * 2.0;
        return static_cast<float>(triangle);
    }
    case animation_mode::once:
    default:
        return static_cast<float>(std::clamp(phase, 0.0, 1.0));
    }
}

} // namespace

namespace
{

// Table field helpers for the bridge's tween descriptions. Tables from Lua:
// { channel = 0, from_position = {0, 1, 2}, duration = 1.0 }.
std::array<float, 3> vec3Field(
    const sol::table &table, const char *key, const std::array<float, 3> &fallback
)
{
    const sol::object value = table[key];
    if (value.get_type() == sol::type::table)
    {
        const sol::table components = value;
        return {
            components[1].get_or(0.0f),
            components[2].get_or(0.0f),
            components[3].get_or(0.0f),
        };
    }
    return fallback;
}

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

// The dedicated Lua bridge extension object for the animation module. Owns
// the sol2 wiring that installs engine.animate_object, engine.animate_light
// and engine.cancel_tween into every script state. Holds a raw pointer to
// the AnimationCapability the module provides.
class AnimationLuaBridge final : public LuaBridgeExtension
{
public:
    explicit AnimationLuaBridge(
        AnimationCapability *capability
    )
        : capability(capability)
    {}

    const char *extensionName() const override
    {
        return "animation";
    }

    void installLuaBridge(
        luaState *lua
    ) override
    {
        // The host (lua_scripting) guarantees the engine table exists when
        // this is called. The bridge functions call the AnimationCapability
        // methods directly through the captured pointer - no capability
        // acquisition, no permission check.
        sol::state_view luaView(lua);
        sol::table engine = luaView["engine"];

        engine.set_function(
            "animate_object", [this](std::uint32_t objectHandle, const sol::table &tween) -> std::uint64_t {
                TweenDescription description;
                description.channel
                    = static_cast<std::uint32_t>(numberField(tween, "channel", animation_channel::position));
                description.fromPosition      = vec3Field(tween, "from_position", {0.0f, 0.0f, 0.0f});
                description.toPosition        = vec3Field(tween, "to_position", {0.0f, 0.0f, 0.0f});
                description.fromValue         = static_cast<float>(numberField(tween, "from_value", 0.0));
                description.toValue           = static_cast<float>(numberField(tween, "to_value", 1.0));
                description.durationSeconds   = numberField(tween, "duration", 1.0);
                description.startDelaySeconds = numberField(tween, "delay", 0.0);
                description.easing = static_cast<std::uint32_t>(numberField(tween, "easing", animation_easing::linear));
                description.mode   = static_cast<std::uint32_t>(numberField(tween, "mode", animation_mode::once));
                return capability->animateObject(objectHandle, description);
            }
        );
        engine.set_function(
            "animate_light", [this](std::uint32_t lightHandle, const sol::table &tween) -> std::uint64_t {
                TweenDescription description;
                description.channel
                    = static_cast<std::uint32_t>(numberField(tween, "channel", animation_channel::lightIntensity));
                description.fromValue         = static_cast<float>(numberField(tween, "from_value", 0.0));
                description.toValue           = static_cast<float>(numberField(tween, "to_value", 1.0));
                description.durationSeconds   = numberField(tween, "duration", 1.0);
                description.startDelaySeconds = numberField(tween, "delay", 0.0);
                description.easing = static_cast<std::uint32_t>(numberField(tween, "easing", animation_easing::linear));
                description.mode   = static_cast<std::uint32_t>(numberField(tween, "mode", animation_mode::once));
                return capability->animatePointLight(lightHandle, description);
            }
        );
        engine.set_function("cancel_tween", [this](std::uint64_t handle) { return capability->cancelTween(handle); });
    }

private:
    AnimationCapability *capability;
};

} // namespace

AnimationModule::AnimationModule()
{
    componentManifest = ComponentManifest{
        .name    = "animation",
        .kind    = ComponentKind::Module,
        .version = "0.1.0",
        .providedCapabilities
        = {"animation.tweens",
            // The Lua bridge extension member: declares this module as a
            // provider of the "scripting.lua_bridge" family. The
            // lua_scripting host acquires every active member through
            // ComponentContext::acquireEachCapability and asks each
            // one to install its own bindings into the host's engine
            // table.
            "scripting.lua_bridge.animation"},
        // The scene registry is a hard edge (object tweens write through it); the
        // lighting state is optional (light tweens degrade precisely when the pbr
        // module is absent). Declaring it through optionalCapabilities means the
        // resolver orders the light provider before this module when one is
        // active, and the derived permission replaces the old hand-rolled
        // capability.lighting grant.
        .requiredCapabilities = {"logging", "scene"},
        .optionalCapabilities = {"lighting"},
        .requiredComponents   = {},
        .requiredPermissions  = {"capability.logging", "capability.scene"},
        .knownConfigKeys      = {},
    };
}

AnimationModule::~AnimationModule()
{
    stop();
}

void AnimationModule::construct(
    ComponentContext &context
)
{
    boundContext    = &context;
    logger          = &context.acquireCapability<LoggerCapability>();
    sceneCapability = &context.acquireCapability<SceneCapability>();

    // Lighting is optional: the graph edge is deliberately absent, so a build
    // without the pbr module still animates objects. First use decides.
    // The tween tick rides the kernel's EngineFrameEvent heartbeat (the loop
    // clock), not the renderer's FrameBeginEvent: a headless run tweens
    // exactly like a rendered one.
    EventBus &bus         = context.eventBus();
    frameTickChannel      = &bus.channel<EngineFrameEvent>();
    tweenFinishedChannel  = &bus.channel<TweenFinishedEvent>();
    frameTickSubscription = EventSubscription(
        &bus,
        typeid(EngineFrameEvent),
        frameTickChannel->subscribe<AnimationModule, &AnimationModule::forwardFrameTick>(this)
    );

    // Construct the Lua bridge extension object. The module IS the
    // AnimationCapability, so `this` is the capability pointer the bridge
    // calls back into.
    luaBridge = std::make_unique<AnimationLuaBridge>(this);
}

void AnimationModule::start()
{
    logger->logMessage(LogLevel::Information, "animation module started");
}

void AnimationModule::stop()
{
    frameTickSubscription.release();
    tweens.clear();
    // Reset the Lua bridge before nulling the capability pointers it captures.
    luaBridge.reset();
    boundContext       = nullptr;
    logger             = nullptr;
    sceneCapability    = nullptr;
    lightingCapability = nullptr;
}

void *AnimationModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    if (capabilityName == "animation.tweens")
    {
        return static_cast<AnimationCapability *>(this);
    }
    // The "scripting.lua_bridge.animation" member: returns the dedicated
    // AnimationLuaBridge object this module owns (constructed in construct()).
    if (capabilityName == "scripting.lua_bridge.animation")
    {
        return luaBridge.get();
    }
    return nullptr;
}

AnimationCapability::TweenHandle AnimationModule::animateObject(
    SceneCapability::ObjectHandle objectHandle, const TweenDescription &tween
)
{
    if (objectHandle == SceneCapability::invalidObject)
    {
        if (logger != nullptr)
        {
            logger->logMessage(LogLevel::Warning, "animation module: animateObject refused (invalid object handle)");
        }
        return invalidTween;
    }
    if (tween.channel != animation_channel::position && tween.channel != animation_channel::rotation
        && tween.channel != animation_channel::emissive)
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning,
                "animation module: animateObject refused (channel " + std::to_string(tween.channel)
                    + " does not drive a scene object)"
            );
        }
        return invalidTween;
    }
    // A NaN duration slips past any `<= 0` style test (NaN compares false) and
    // then writes NaN transforms every frame; sub-microsecond durations are
    // garbage, not curves. A NaN delay poisons elapsed the same way, so both
    // are screened here at the capability boundary.
    if (!(tween.durationSeconds > 1e-4) || !std::isfinite(tween.durationSeconds)
        || !std::isfinite(tween.startDelaySeconds))
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning,
                "animation module: animateObject refused (duration must be finite and >= 0.0001 seconds)"
            );
        }
        return invalidTween;
    }
    // The target must exist NOW: a tween on a typo'd handle is a programming
    // error the caller wants to hear about, not a tween that silently never
    // moves anything. The transform-only readback answers the existence
    // question - the descriptor readback this check used to make copied the
    // material payload only to discard it.
    SceneObjectTransform current;
    if (!sceneCapability->objectTransform(objectHandle, current))
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning,
                "animation module: animateObject refused (object " + std::to_string(objectHandle) + " is not live)"
            );
        }
        return invalidTween;
    }

    ActiveTween active;
    active.handle            = nextHandle++;
    active.channel           = tween.channel;
    active.targetHandle      = objectHandle;
    active.fromPosition      = tween.fromPosition;
    active.toPosition        = tween.toPosition;
    active.fromValue         = tween.fromValue;
    active.toValue           = tween.toValue;
    active.durationSeconds   = tween.durationSeconds;
    active.startDelaySeconds = tween.startDelaySeconds;
    active.easing            = tween.easing;
    active.mode              = tween.mode;
    // elapsedSeconds starts at zero: the curve advances from the next frame
    // heartbeat's delta, the same "first tick has no elapsed time" contract
    // the scheduler and the physics step keep. The wall-clock anchor this
    // used to take here (plus the per-frame subtraction and the seeded-clock
    // bookkeeping) served a delta the heartbeat already measured.
    tweens.push_back(active);
    return active.handle;
}

AnimationCapability::TweenHandle AnimationModule::animatePointLight(
    LightingCapability::PointLightHandle lightHandle, const TweenDescription &tween
)
{
    if (tween.channel != animation_channel::lightIntensity && tween.channel != animation_channel::lightRadius)
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning,
                "animation module: animatePointLight refused (channel " + std::to_string(tween.channel)
                    + " does not drive a light)"
            );
        }
        return invalidTween;
    }
    // Same duration/delay screen as animateObject: a NaN duration would write
    // NaN light intensities every frame from inside the loop.
    if (!(tween.durationSeconds > 1e-4) || !std::isfinite(tween.durationSeconds)
        || !std::isfinite(tween.startDelaySeconds))
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning,
                "animation module: animatePointLight refused (duration must be finite and >= 0.0001 seconds)"
            );
        }
        return invalidTween;
    }
    // Lighting is optional (declared in the manifest): the resolver ordered
    // the provider before this module when one is active, so the probe below
    // is a stable answer, never construct-order luck. First light tween
    // decides finally (lightingProbeDecided keeps the absent answer too, so
    // a script polling animatePointLight in a lighting-less build pays the
    // probe once, not once per call); caching it keeps a per-tween probe off
    // the frame path.
    if (!lightingProbeDecided && lightingCapability == nullptr && boundContext != nullptr)
    {
        lightingProbeDecided = true;
        if (boundContext->isCapabilityAvailable(LightingCapability::capabilityName))
        {
            lightingCapability = &boundContext->acquireCapability<LightingCapability>();
        }
    }
    if (lightingCapability == nullptr)
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning,
                "animation module: animatePointLight refused (the lighting capability is not part of this build)"
            );
        }
        return invalidTween;
    }
    if (lightHandle == LightingCapability::invalidPointLight || lightHandle > lightingCapability->pointLightCount())
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning,
                "animation module: animatePointLight refused (light " + std::to_string(lightHandle)
                    + " is not live; light handles are renumbered by removals)"
            );
        }
        return invalidTween;
    }

    ActiveTween active;
    active.handle            = nextHandle++;
    active.channel           = tween.channel;
    active.targetHandle      = lightHandle;
    active.fromValue         = tween.fromValue;
    active.toValue           = tween.toValue;
    active.durationSeconds   = tween.durationSeconds;
    active.startDelaySeconds = tween.startDelaySeconds;
    active.easing            = tween.easing;
    active.mode              = tween.mode;
    // Same zero-elapsed start as animateObject: the heartbeat's delta drives
    // the curve from the next frame on.
    tweens.push_back(active);
    return active.handle;
}

bool AnimationModule::cancelTween(
    TweenHandle handle
)
{
    const auto found = std::find_if(tweens.begin(), tweens.end(), [handle](const ActiveTween &tween) {
        return tween.handle == handle;
    });
    if (found == tweens.end())
    {
        return false;
    }
    tweens.erase(found);
    return true;
}

std::uint32_t AnimationModule::activeTweenCount() const
{
    return static_cast<std::uint32_t>(tweens.size());
}

bool AnimationModule::applyTween(
    const ActiveTween &tween, const std::array<float, 3> &position, float value
)
{
    switch (tween.channel)
    {
    case animation_channel::position: {
        // Rotation is preserved: a position tween must not reorient. The
        // transform-only readback answers with the one field the move
        // needs - the full descriptor copy this channel used to make copied
        // the material payload (the texture-set string) once per frame per
        // tweened object to read one float.
        SceneObjectTransform current;
        if (!sceneCapability->objectTransform(tween.targetHandle, current))
        {
            return false;
        }
        return sceneCapability->setObjectTransform(tween.targetHandle, position, current.rotationYRadians);
    }
    case animation_channel::rotation: {
        SceneObjectTransform current;
        if (!sceneCapability->objectTransform(tween.targetHandle, current))
        {
            return false;
        }
        return sceneCapability->setObjectTransform(tween.targetHandle, current.position, value);
    }
    case animation_channel::emissive: {
        // The single-field material write: the read-modify-write through
        // object() + setObjectMaterial() produced exactly this end state
        // while copying the whole descriptor (its texture-set string
        // included) once per frame per tweened object.
        return sceneCapability->setObjectEmissive(tween.targetHandle, value);
    }
    case animation_channel::lightIntensity:
    case animation_channel::lightRadius: {
        if (lightingCapability == nullptr)
        {
            return false;
        }
        // Light handles are index+1, but a removal renumbers the rest: a
        // stale tween whose index now points past the live count must drop
        // itself here instead of throwing std::out_of_range out of the
        // frame loop. Returning false drops the tween (the capability
        // contract: a removed light invalidates the tween).
        if (tween.targetHandle - 1 >= lightingCapability->pointLightCount())
        {
            return false;
        }
        // Read-modify-write keeps the other fields exactly as they are.
        PointLightDescription light = lightingCapability->pointLight(tween.targetHandle - 1);
        if (tween.channel == animation_channel::lightIntensity)
        {
            light.intensity = value;
        } else
        {
            light.radius = value;
        }
        lightingCapability->updatePointLight(tween.targetHandle, light);
        return true;
    }
    default:
        return false;
    }
}

void AnimationModule::advanceTweens(
    double frameDeltaSeconds
)
{
    if (tweens.empty())
    {
        return;
    }
    // The heartbeat's delta is the whole gameplay layer's clock
    // (docs/architecture.md, "The frame clock"): the composition root
    // measured this iteration once, and every simulation consumer advances
    // by the same number. The wall-clock read this function used to take
    // (plus the per-tween now - started subtraction) reproduced a delta the
    // event already carried, one steady_clock::now() per frame with active
    // tweens; the accumulated elapsedSeconds below keeps the identical curve
    // - deltas sum to the same wall time - without the extra clock read.

    // Compact in place: walk tweens with a read index, move survivors down
    // to a write index, then resize to the survivor count. The previous
    // shape built a fresh `survivors` vector every frame and `tweens =
    // std::move(survivors)` swapped it in - two heap operations per frame
    // (allocate the survivor buffer, free the old tweens buffer) on every
    // frame with at least one active tween. ActiveTween is a POD-only
    // struct (no owning resources), so move is a memcpy and the in-place
    // pattern keeps the tweens buffer's capacity stable across frames -
    // no allocation once the buffer has warmed up to its high-water mark.
    std::size_t writeIndex = 0;
    for (std::size_t readIndex = 0; readIndex < tweens.size(); ++readIndex)
    {
        ActiveTween &tween = tweens[readIndex];
        // The delay phase accumulates too (a tween waiting out its delay
        // must consume frame time), then the curve time is what remains.
        tween.elapsedSeconds += frameDeltaSeconds;
        const double elapsed = tween.elapsedSeconds - tween.startDelaySeconds;
        if (elapsed < 0.0)
        {
            // Has not started yet: survives untouched.
            if (writeIndex != readIndex)
            {
                tweens[writeIndex] = std::move(tween);
            }
            ++writeIndex;
            continue;
        }
        const float raw   = curveParameter(elapsed, tween.durationSeconds, tween.mode);
        const float eased = evaluateEasing(tween.easing, raw);
        float value       = 0.0f;
        std::array<float, 3> position{};
        switch (tween.channel)
        {
        case animation_channel::position: {
            // glm::mix is the componentwise blend the per-axis loop used
            // to expand by hand (from + (to - from) * eased).
            const glm::vec3 blended
                = glm::mix(glm::make_vec3(tween.fromPosition.data()), glm::make_vec3(tween.toPosition.data()), eased);
            position = {blended.x, blended.y, blended.z};
            break;
        }
        case animation_channel::rotation:
        case animation_channel::emissive:
        case animation_channel::lightIntensity:
        case animation_channel::lightRadius:
            value = tween.fromValue + (tween.toValue - tween.fromValue) * eased;
            break;
        default:
            break;
        }
        const bool alive = applyTween(tween, position, value);
        if (!alive)
        {
            // Target vanished (object removed, light renumbered): drop
            // silently - the removal already published its own event.
            continue;
        }
        if (tween.mode == animation_mode::once && raw >= 1.0f)
        {
            // Defer the publish: firing the event INSIDE this compaction loop
            // handed a subscriber a live mutation mid-walk - the natural
            // reaction, cancelTween(handle), erased from `tweens` and shifted
            // every element after the cursor, invalidating `tween` and
            // letting the trailing resize() drop freshly added tweens. The
            // finished handles collect here and publish after resize(), when
            // the vector is stable again; within one frame the publish order
            // is the same finish order as before.
            finishedScratch.push_back(tween.handle);
            continue;
        }
        if (writeIndex != readIndex)
        {
            tweens[writeIndex] = std::move(tween);
        }
        ++writeIndex;
    }
    tweens.resize(writeIndex);

    // The tweens vector is compact and stable: subscribers may now cancel
    // (their tween is gone, so the cancel answers false) or start new tweens
    // (push_back lands past the resize) without re-entering the walk.
    if (!finishedScratch.empty())
    {
        if (tweenFinishedChannel != nullptr)
        {
            for (const TweenHandle finishedHandle : finishedScratch)
            {
                tweenFinishedChannel->publish(TweenFinishedEvent{.tweenHandle = finishedHandle});
            }
        }
        finishedScratch.clear();
    }
}

void AnimationModule::forwardFrameTick(
    const EngineFrameEvent &event
)
{
    // Tween-free frames (the common case - most scenes tween for a few
    // seconds and then settle) return at the empty check: no clock read, no
    // state to seed, nothing at all. The first frame of a run carries delta
    // 0 (the composition root's seeded-first-frame contract), which advances
    // no curve - the same first-tick behavior every heartbeat consumer
    // keeps, with the delta itself taken from the event instead of a second
    // clock the module used to read and seed for the idle-gap property the
    // heartbeat's delta already provides.
    if (tweens.empty())
    {
        return;
    }
    advanceTweens(event.deltaSeconds);
}

} // namespace engine
