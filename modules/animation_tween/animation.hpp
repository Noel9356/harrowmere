#ifndef ENGINE_MODULES_ANIMATION_HPP
#define ENGINE_MODULES_ANIMATION_HPP

#include "animation_capability.hpp"
#include "logging_capability.hpp"
#include "lua_bridge_extension.hpp"
#include "scene_capability.hpp"
#include "engine/kernel/component_interface.hpp" // EngineFrameEvent (the tick
                                                 // source: declared in the
                                                 // kernel's component-facing
                                                 // header, so the module needs
                                                 // no renderer capability
                                                 // include and tweens in
                                                 // renderer-less runs too)
#include "engine/kernel/event_bus.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace engine
{

// Module "animation_tween": tween drivers over the scene registry and the light
// state.
//
// The module subscribes to the frame event, advances every active tween by
// the frame delta and writes the new values through the consumed
// capabilities (scene for objects, lighting for point lights). It owns no
// state of its own beyond the tween list: the scene registry stays the one
// source of truth for objects, and every write flows through the same
// permission-checked, event-publishing surface a script would use - which
// also means the renderer's incremental rebuild picks animated objects up
// with no animation-specific renderer path.
//
// The module additionally provides the "scripting.lua_bridge.animation" Lua
// bridge extension: it installs engine.animate_object, engine.animate_light
// and engine.cancel_tween into every script state through the
// LuaBridgeExtension contract published by lua_scripting. The host discovers
// this extension through the multi-provider acquisition of the
// "scripting.lua_bridge" family, so adding this capability to the module
// never touches the lua_scripting source.
class AnimationModule final : public ModuleInterface, public AnimationCapability
{
public:
    AnimationModule();
    ~AnimationModule() override;

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;

    // AnimationCapability.
    TweenHandle animateObject(SceneCapability::ObjectHandle objectHandle, const TweenDescription &tween) override;
    TweenHandle
    animatePointLight(LightingCapability::PointLightHandle lightHandle, const TweenDescription &tween) override;
    bool cancelTween(TweenHandle handle) override;
    std::uint32_t activeTweenCount() const override;

private:
    struct ActiveTween
    {
        TweenHandle handle    = invalidTween;
        std::uint32_t channel = animation_channel::position;
        // The target: a scene object handle or a point light handle (the
        // channel decides which capability the write goes to).
        std::uint32_t targetHandle        = 0;
        std::array<float, 3> fromPosition = {0.0f, 0.0f, 0.0f};
        std::array<float, 3> toPosition   = {0.0f, 0.0f, 0.0f};
        float fromValue                   = 0.0f;
        float toValue                     = 1.0f;
        double durationSeconds            = 1.0;
        double startDelaySeconds          = 0.0;
        std::uint32_t easing              = animation_easing::linear;
        std::uint32_t mode                = animation_mode::once;
        // Elapsed curve time, accumulated from the frame heartbeat's delta
        // (the composition root measures the iteration once and every
        // simulation consumer advances by the same number - docs/architecture.md,
        // "The frame clock"). The wall-clock anchor this used to carry
        // (steady_clock::now() at tween creation, subtracted per frame per
        // tween) needed one clock read per frame WITH tweens on top of the
        // heartbeat's own, and the seeded-clock bookkeeping around it, for a
        // delta that was already measured; the accumulated value keeps the
        // same elapsed curve - deltas sum to the same wall time - and the
        // same "first frame carries delta 0" contract every other consumer
        // of the heartbeat follows.
        double elapsedSeconds = 0.0;
    };

    void forwardFrameTick(const EngineFrameEvent &event);
    void advanceTweens(double frameDeltaSeconds);
    // Writes one tween's current value to its target. Returns false when the
    // target vanished (object removed, light removed): the tween then drops.
    bool applyTween(const ActiveTween &tween, const std::array<float, 3> &position, float value);

    ComponentManifest componentManifest;
    ComponentContext *boundContext   = nullptr;
    LoggerCapability *logger         = nullptr;
    SceneCapability *sceneCapability = nullptr;
    // Tri-state optional lighting resolution: light tweens degrade to a
    // one-time log when the pbr module is absent from the build. The probe
    // for the absent side is one-shot (lightingProbeDecided): the resolver
    // orders providers before consumers, so the answer is a property of the
    // assembly - a lighting provider can never appear mid-run without the
    // construct phase that would have ordered it first - and a script
    // calling animatePointLight per frame in a lighting-less build used to
    // re-walk the availability probe (permission scratch assign, resolver
    // walk) on every refused call.
    LightingCapability *lightingCapability = nullptr;
    bool lightingProbeDecided              = false;

    EventSubscription frameTickSubscription;
    EventChannel<EngineFrameEvent> *frameTickChannel       = nullptr;
    EventChannel<TweenFinishedEvent> *tweenFinishedChannel = nullptr;

    std::vector<ActiveTween> tweens;
    // advanceTweens' finished-handle scratch (member: capacity retained, the
    // frame path stops touching the heap after the high-water mark). The
    // TweenFinishedEvents publish AFTER the compaction loop - see
    // advanceTweens for why publishing inside it was a reentrancy hazard.
    std::vector<TweenHandle> finishedScratch;
    TweenHandle nextHandle = invalidTween + 1;
    // The Lua bridge extension object owned by this module. Constructed in
    // construct() and returned from capabilityPointerFor for the
    // "scripting.lua_bridge.animation" member. Held as the base
    // LuaBridgeExtension pointer: the concrete AnimationLuaBridge type
    // (defined in animation.cpp) stays private to this module's translation
    // unit.
    std::unique_ptr<LuaBridgeExtension> luaBridge;
};

} // namespace engine

#endif // ENGINE_MODULES_ANIMATION_HPP
