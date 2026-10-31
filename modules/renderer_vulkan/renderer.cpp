#include "renderer.hpp"

#include "renderer_state.hpp"

#include "engine/kernel/event_bus.hpp"
#include "engine/kernel/filesystem_utilities.hpp"

#include <algorithm>
#include <memory>
#include <sol/sol.hpp>
#include <stdexcept>
#include <string>
#include <tuple>

namespace engine
{

// RendererModule lifecycle ------------------------------------------------------------

namespace
{

// The dedicated Lua bridge extension object for the renderer module. Owns the
// sol2 wiring that installs engine.camera_position, engine.camera_look_at,
// engine.set_camera_position and engine.set_camera_look_at into every script
// state. Holds a raw pointer to the RendererCapability the module provides.
class RendererLuaBridge final : public LuaBridgeExtension
{
public:
    explicit RendererLuaBridge(
        RendererCapability *capability
    )
        : capability(capability)
    {}

    const char *extensionName() const override
    {
        return "renderer";
    }

    void installLuaBridge(
        luaState *lua
    ) override
    {
        // The host (lua_scripting) guarantees the engine table exists when
        // this is called. The bridge functions call the RendererCapability
        // methods directly through the captured pointer - no capability
        // acquisition, no permission check.
        sol::state_view luaView(lua);
        sol::table engine = luaView["engine"];

        engine.set_function("camera_position", [this]() {
            const std::array<float, 3> position = capability->cameraPosition();
            return std::make_tuple(position[0], position[1], position[2]);
        });
        engine.set_function("camera_look_at", [this]() {
            const std::array<float, 3> lookAt = capability->cameraLookAt();
            return std::make_tuple(lookAt[0], lookAt[1], lookAt[2]);
        });
        engine.set_function("set_camera_position", [this](float x, float y, float z) {
            capability->setCameraPosition({x, y, z});
        });
        engine.set_function("set_camera_look_at", [this](float x, float y, float z) {
            capability->setCameraLookAt({x, y, z});
        });
    }

private:
    RendererCapability *capability;
};

} // namespace

RendererModule::RendererModule()
{
    componentManifest = ComponentManifest{
        .name    = "renderer",
        .kind    = ComponentKind::Module,
        .version = "0.1.0",
        // The implementation-bound member name: this module is the Vulkan
        // implementation of the "renderer" family. Consumers require the family;
        // a mod that needs this backend names the member exactly.
        .providedCapabilities
        = {"renderer.vulkan",
            "renderer.vulkan.overlay",
            // The Lua bridge extension member: declares this module as a
            // provider of the "scripting.lua_bridge" family. The
            // lua_scripting host acquires every active member through
            // ComponentContext::acquireEachCapability and asks each
            // one to install its own bindings into the host's engine
            // table.
            "scripting.lua_bridge.renderer"},
        // "input" and "scene" are OPTIONAL (declared, not hand-rolled): the
        // renderer degrades to a static camera over the static [scene] world
        // when the input or scene module is absent (minimal builds). Declaring
        // them through optionalCapabilities gives what the old
        // permission-without-edge + try/catch pattern could not: the resolver
        // orders the providers before this module when they are active, so the
        // probes below are a property of the assembly instead of construct-order
        // luck, and the derived capability.<name> grants replace the hand-rolled
        // permission entries.
        .requiredCapabilities = {"logging", "window"},
        .optionalCapabilities = {"input", "scene"},
        .requiredComponents   = {},
        // "ui" is deliberately NOT an optional declaration: the ui module
        // requires this module's overlay seam (renderer.vulkan.overlay), so an
        // optional edge back to "ui" would close a dependency cycle. The camera
        // integration re-resolves the ui capability tri-state at the first frame
        // that needs it (updateCameraIntegration), which is why the permission
        // stays hand-rolled here.
        .requiredPermissions = {"capability.logging", "capability.window", "capability.ui"},
        // Every configuration key the renderer reads (its own section and the
        // [scene] world content it rasterizes) - the known-key registry behind
        // the unknown-key warnings is built from these declarations.
        .knownConfigKeys = {
            "renderer.enable_validation",
            "renderer.fail_on_validation_error",
            "renderer.vsync",
            "renderer.prefer_discrete_device",
            "renderer.max_frames_in_flight",
            "renderer.exposure",
            "renderer.tonemap",
            "renderer.bloom",
            "renderer.bloom_threshold",
            "renderer.bloom_strength",
            "renderer.shader_directory",
            "renderer.texture_directory",
            "renderer.texture_maps.color",
            "renderer.texture_maps.roughness",
            "renderer.texture_maps.metalness",
            "renderer.texture_maps.normal",
            "scene.sky_radiance",
            "scene.sky.zenith",
            "scene.sky.horizon",
            "scene.sky.ground",
            "scene.sky.sun_disk",
            "scene.sky.sun_disk_size_degrees",
            "scene.camera.position",
            "scene.camera.look_at",
            "scene.camera.fov",
            "scene.camera.near",
            "scene.camera.far",
            "scene.camera.free_fly",
            "scene.camera.fly_speed",
            "scene.camera.fly_boost",
            "scene.camera.look_sensitivity",
            "scene.sun.direction",
            "scene.sun.color",
            "scene.sun.intensity",
            "scene.objects",
        },
    };
}

RendererModule::~RendererModule()
{
    stop();
}

void RendererModule::construct(
    ComponentContext &context
)
{
    boundContext     = &context;
    logger           = &context.acquireCapability<LoggerCapability>();
    windowCapability = &context.acquireCapability<WindowCapability>();

    state               = std::make_unique<RendererState>();
    state->boundContext = &context;

    // Mouse/keyboard camera control is optional (declared in the manifest):
    // the resolver ordered the input provider before this module when one is
    // active, so this probe is a stable answer - the fly controller keeps
    // the configured pose when the input module is not part of this build.
    if (context.isCapabilityAvailable(InputCapability::capabilityName))
    {
        state->inputCapability = &context.acquireCapability<InputCapability>();
    }

    // Fly-camera action bindings: when the assembly (or a mod's shipped
    // defaults) defines the semantic actions in [input.actions], the
    // controller polls THEM - bind by meaning, gamepads included - and the
    // absent keys keep the historical fixed keys. The demo content's shipped
    // "boost" and "release_cursor" actions become live here: they were
    // always meant for this consumer.
    state->cameraBindings.forwardAction
        = context.configurationStringInSection("input", "actions.camera_forward").value_or("");
    state->cameraBindings.backwardAction
        = context.configurationStringInSection("input", "actions.camera_back").value_or("");
    state->cameraBindings.leftAction
        = context.configurationStringInSection("input", "actions.camera_left").value_or("");
    state->cameraBindings.rightAction
        = context.configurationStringInSection("input", "actions.camera_right").value_or("");
    state->cameraBindings.upAction = context.configurationStringInSection("input", "actions.camera_up").value_or("");
    state->cameraBindings.downAction
        = context.configurationStringInSection("input", "actions.camera_down").value_or("");
    state->cameraBindings.boostAction = context.configurationStringInSection("input", "actions.boost").value_or("");
    state->cameraBindings.releaseCursorAction
        = context.configurationStringInSection("input", "actions.release_cursor").value_or("");

    // The ui module's overlay is optional the same way: the free-fly camera
    // consults wantsMouse() each frame and pauses its pointer handling while
    // a debug window is up. This acquire is best effort - the renderer
    // constructs BEFORE the ui module (the overlay consumes renderer.overlay,
    // so the resolver orders the renderer first), so the acquisition usually
    // fails here. The camera integration re-resolves tri-state at the first
    // frame that runs it (updateCameraIntegration), when every component has
    // constructed; a run without the ui module never re-throws.
    try
    {
        state->uiOverlayCapability = &context.acquireCapability<UiCapability>();
        state->uiResolutionState   = 1;
    } catch (const std::exception &)
    {
        state->uiOverlayCapability = nullptr;
        // uiResolutionState stays 0 (unresolved): the ui module's construct
        // may still run after this one; the first frame decides finally.
    }

    // Channel references are cached once: channels are heap anchored inside
    // the bus and stay valid for the engine lifetime, so the frame loop can
    // publish without map lookups and without touching the context again.
    frameBeginChannel           = &context.eventBus().channel<FrameBeginEvent>();
    frameRenderedChannel        = &context.eventBus().channel<FrameRenderedEvent>();
    state->frameCapturedChannel = &context.eventBus().channel<FrameCapturedEvent>();

    // The shipped defaults live in this module's config.toml; the code
    // fallbacks below agree with them, so a runtime whose defaults file went
    // missing behaves exactly like the documented defaults (the last-resort
    // fallback is a copy of the shipped value, never a different policy).
    state->enableValidation      = context.configurationBoolean("enable_validation").value_or(false);
    state->failOnValidationError = context.configurationBoolean("fail_on_validation_error").value_or(false);
    state->vsync                 = context.configurationBoolean("vsync").value_or(true);
    state->preferDiscreteDevice  = context.configurationBoolean("prefer_discrete_device").value_or(true);

    const long long frames = context.configurationInteger("max_frames_in_flight").value_or(defaultFramesInFlight);

    state->maxFramesInFlight = static_cast<std::uint32_t>(std::clamp<std::int64_t>(frames, 1, maxFramesInFlightLimit));
    state->exposure          = static_cast<float>(context.configurationFloating("exposure").value_or(1.15));
    state->tonemapMode       = context.configurationString("tonemap").value_or("aces");

    // Bloom (post-processing): opt-in, thresholded HDR bright pass + separable
    // blur, added back pre-tonemap (scene_composite_final.frag).
    state->bloomEnabled   = context.configurationBoolean("bloom").value_or(true);
    state->bloomThreshold = static_cast<float>(context.configurationFloating("bloom_threshold").value_or(1.0));
    state->bloomStrength  = static_cast<float>(context.configurationFloating("bloom_strength").value_or(0.15));

    if (state->tonemapMode != "aces" && state->tonemapMode != "reinhard" && state->tonemapMode != "none")
    {
        throw std::runtime_error(
            logPrefix + "unknown tonemap mode '" + state->tonemapMode + "' (known: aces, reinhard, none)"
        );
    }

    // Shared world content ([scene]): validated at construct so configuration
    // errors fail startup with a precise diagnostic instead of at first frame.
    std::string sceneError;
    if (!SceneDescription::parseFromConfiguration(context, state->scene, sceneError))
    {
        throw std::runtime_error(logPrefix + "" + sceneError);
    }
    // Lighting composition: the active lighting techniques attach themselves
    // during their own construct phases (demand-driven activation decides
    // which modules exist); the renderer only composes what attached.
    const std::string shaderDirectory = context.configurationString("shader_directory").value_or("shaders");
    state->shaderDirectory            = shaderDirectory;
    state->textureDirectory           = context.configurationString("texture_directory").value_or("");

    // Texture set file names ([renderer.texture_maps]): the ambientCG layout
    // (Color.png / Roughness.png / Metalness.png / NormalGL.png) stays the
    // default; a content pipeline that names its maps differently overrides
    // per slot without renaming files. Read once at construct - the frame
    // path never touches the strings.
    state->colorMapFileName
        = context.configurationStringInSection("renderer", "texture_maps.color").value_or(colorMapName);
    state->roughnessMapFileName
        = context.configurationStringInSection("renderer", "texture_maps.roughness").value_or(roughnessMapName);
    state->metalnessMapFileName
        = context.configurationStringInSection("renderer", "texture_maps.metalness").value_or(metalnessMapName);
    state->normalMapFileName
        = context.configurationStringInSection("renderer", "texture_maps.normal").value_or(normalMapName);

    EventChannel<WindowResizedEvent> &resizedChannel = context.eventBus().channel<WindowResizedEvent>();
    resizeSubscription                               = EventSubscription(
        &context.eventBus(),
        typeid(WindowResizedEvent),
        resizedChannel.subscribe<RendererModule, &RendererModule::handleWindowResized>(this)
    );

    // Hot reload (optional module): shader change events re-create the
    // pipelines from the re-compiled SPIR-V. The subscription is free when
    // the module is absent (nobody publishes the event).
    EventChannel<HotReloadEvent> &hotReloadChannel = context.eventBus().channel<HotReloadEvent>();
    hotReloadSubscription                          = EventSubscription(
        &context.eventBus(),
        typeid(HotReloadEvent),
        hotReloadChannel.subscribe<RendererModule, &RendererModule::handleHotReload>(this)
    );

    // Live configuration reload (kernel reloadConfiguration, driven by
    // [engine].config_reload_poll_seconds): the composite constants below are
    // push-constant values the very next frame consumes - no GPU resource is
    // created or destroyed, so a reload can never invalidate a pipeline or a
    // descriptor set. The subscription is free when reload is disabled (the
    // event is simply never published).
    EventChannel<ConfigurationReloadedEvent> &reloadedChannel
        = context.eventBus().channel<ConfigurationReloadedEvent>();
    configurationReloadedSubscription = EventSubscription(
        &context.eventBus(),
        typeid(ConfigurationReloadedEvent),
        reloadedChannel.subscribe<RendererModule, &RendererModule::handleConfigurationReloaded>(this)
    );

    // Construct the Lua bridge extension object. The module IS the
    // RendererCapability, so `this` is the capability pointer the bridge
    // calls back into.
    luaBridge = std::make_unique<RendererLuaBridge>(this);
}

void RendererModule::start()
{
    // Headless degrade (the game assembly's pinned CI shape, common.md
    // section 3.6 "headless degrades window/renderer, never input"): the
    // window module started windowless - no usable display - so there is no
    // surface to create an instance, a device or a swapchain against.
    // Failing the whole startup here would make the pinned headless smoke
    // run (--set engine.quit_after_seconds=1.0, exit 0) impossible, and the
    // gameplay layer is renderer-free by design (the boot card: the tick
    // skeleton, the world model and the sim capability run unchanged without
    // a frame). The module therefore starts render-free: headlessDegrade
    // short-circuits the per-frame path and the state below stays allocated
    // but uninitialized, which every remaining capability method already
    // guards on. A display-capable environment is unaffected: the window
    // list is non-empty and initialization proceeds exactly as before.
    if (windowCapability->windowIdentifiers().empty())
    {
        headlessDegrade = true;
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning, "renderer module: no usable display - running render-free (headless degrade)"
            );
        }
        return;
    }
    state->resolvedShaderDirectory  = state->shaderDirectory.is_relative()
                                        ? (executableDirectory() / state->shaderDirectory)
                                        : state->shaderDirectory;
    state->resolvedTextureDirectory = state->textureDirectory.is_relative()
                                        ? (executableDirectory() / state->textureDirectory)
                                        : state->textureDirectory;
    state->initialize(*windowCapability, *logger);

    // Scene registry connection: after initialize (all GPU resources exist,
    // including the capacity-sized primitive buffers) but before any frame.
    // The static world is pushed into the registry here, so scripts see the
    // configured room through the same handles they spawn with.
    connectSceneRegistry();

    if (state->lightingTechniques.empty())
    {
        logger->logMessage(
            LogLevel::Information,
            logPrefix
                + "no lighting techniques attached; the scene renders unlit "
                  "(albedo + emissive + sky)"
        );
    } else
    {
        std::string attached;
        for (const auto &technique : state->lightingTechniques)
        {
            if (!attached.empty())
            {
                attached += ", ";
            }
            attached += technique->techniqueName();
        }
        logger->logMessage(
            LogLevel::Information,
            logPrefix + "lighting techniques = [" + attached + "] (" + std::to_string(state->lightingTechniques.size())
                + " attached)"
        );
    }
}

void RendererModule::stop()
{
    configurationReloadedSubscription.release();
    resizeSubscription.release();
    sceneAddedSubscription.release();
    sceneRemovedSubscription.release();
    sceneChangedSubscription.release();
    // validationLogger borrows the logging module's capability for the state's
    // whole lifetime, but the kernel stops modules in dependency order and the
    // teardown below (device waitIdle, instance/device destruction) can still
    // emit validation callbacks. Clear the pointer BEFORE any of that runs, so
    // no callback logs through a module that already stopped.
    if (state != nullptr)
    {
        state->validationLogger = nullptr;
    }
    if (state != nullptr && state->device != nullptr)
    {
        state->device->waitIdle();
    }
    state.reset();
    boundContext         = nullptr;
    frameBeginChannel    = nullptr;
    frameRenderedChannel = nullptr;
    windowCapability     = nullptr;
    logger               = nullptr;
    // Reset the Lua bridge after the capability pointers it captures are
    // nulled is safe because the bridge's destructor does not dereference
    // them (it is = default).
    luaBridge.reset();
}

void *RendererModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    if (capabilityName == "renderer.vulkan")
    {
        return static_cast<RendererCapability *>(this);
    }
    if (capabilityName == "renderer.vulkan.overlay")
    {
        return static_cast<RendererOverlayHostCapability *>(this);
    }
    // The "scripting.lua_bridge.renderer" member: returns the dedicated
    // RendererLuaBridge object this module owns (constructed in construct()).
    if (capabilityName == "scripting.lua_bridge.renderer")
    {
        return luaBridge.get();
    }
    return nullptr;
}

void RendererModule::setOverlayRecorder(
    OverlayRecorder *recorder
)
{
    if (state == nullptr)
    {
        throw std::logic_error(
            "renderer module: overlay registration before the renderer module constructed its state - "
            "register from a start() phase, never from a construct() that runs before the renderer's"
        );
    }
    // Registration is main-thread lifecycle work: the frame path reads the
    // pointer without synchronization, which is safe because every start()
    // (and stop()) completes before the first (or after the last) frame.
    state->overlayRecorder = recorder;
}

const OverlayBackendHandles *RendererModule::overlayBackendHandles() const
{
    if (state == nullptr || state->instance == nullptr || state->device == nullptr)
    {
        return nullptr;
    }
    // Refilled on every query: nine field copies, and the accessor is called
    // once per UI backend initialization - never per frame.
    state->overlayHandles.instance         = static_cast<VkInstance>(**state->instance);
    state->overlayHandles.physicalDevice   = static_cast<VkPhysicalDevice>(**state->physicalDevice);
    state->overlayHandles.device           = static_cast<VkDevice>(**state->device);
    state->overlayHandles.graphicsQueue    = static_cast<VkQueue>(state->graphicsQueue);
    state->overlayHandles.queueFamilyIndex = state->graphicsQueueFamily;
    state->overlayHandles.framesInFlight   = state->maxFramesInFlight;
    state->overlayHandles.minImageCount    = state->swapchainMinImageCount;
    state->overlayHandles.imageCount       = static_cast<std::uint32_t>(state->swapchainImages.size());
    state->overlayHandles.colorFormat      = static_cast<std::int32_t>(state->swapchainFormat);
    return &state->overlayHandles;
}

void RendererModule::attachLightingTechnique(
    const std::shared_ptr<LightingTechniquePass> &pass
)
{
    if (state == nullptr || pass == nullptr)
    {
        return;
    }
    // One pass per algorithm name: a re-attach replaces the previous pass
    // (keeps hot-reload style experimentation sane).
    std::erase_if(state->lightingTechniques, [&pass](const std::shared_ptr<LightingTechniquePass> &existing) {
        return std::string_view(existing->techniqueName()) == pass->techniqueName();
    });
    state->lightingTechniques.push_back(pass);
    // The record order is a physical contract (the member comment): direct
    // emitters evaluate before the passes that transport their light. Enforce
    // it here instead of trusting the configuration's module order; the
    // stable sort keeps equal-priority passes in attach order.
    const auto targetRank = [](const std::shared_ptr<LightingTechniquePass> &technique) {
        if (hasLightingTarget(technique->targets(), LightingTarget::Direct))
        {
            return 0;
        }
        if (hasLightingTarget(technique->targets(), LightingTarget::IndirectDiffuse))
        {
            return 1;
        }
        return 2;
    };
    std::stable_sort(
        state->lightingTechniques.begin(),
        state->lightingTechniques.end(),
        [&targetRank](
            const std::shared_ptr<LightingTechniquePass> &left, const std::shared_ptr<LightingTechniquePass> &right
        ) { return targetRank(left) < targetRank(right); }
    );
    // Recompute which radiance targets exist: the composite reads a target
    // only when at least one attached technique declares it.
    state->hasDirectRadianceTarget   = false;
    state->hasIndirectDiffuseTarget  = false;
    state->hasIndirectSpecularTarget = false;
    for (const auto &technique : state->lightingTechniques)
    {
        state->hasDirectRadianceTarget |= hasLightingTarget(technique->targets(), LightingTarget::Direct);
        state->hasIndirectDiffuseTarget |= hasLightingTarget(technique->targets(), LightingTarget::IndirectDiffuse);
        state->hasIndirectSpecularTarget |= hasLightingTarget(technique->targets(), LightingTarget::IndirectSpecular);
    }
}

void RendererModule::handleWindowResized(
    const WindowResizedEvent & /*event*/
)
{
    if (state != nullptr)
    {
        state->framebufferResized = true;
    }
}

void RendererModule::connectSceneRegistry()
{
    if (boundContext == nullptr || state == nullptr)
    {
        return;
    }
    // Tri-state resolution at START time: every component has constructed by
    // now, so an available scene module registered its capability. Absent
    // module (or denied permission) means the world stays the static [scene]
    // array - a precise, logged decision, not a silent mode change.
    if (sceneResolutionState == 0)
    {
        try
        {
            state->sceneCapability = &boundContext->acquireCapability<SceneCapability>();
            sceneResolutionState   = 1;
        } catch (const std::exception &error)
        {
            state->sceneCapability = nullptr;
            sceneResolutionState   = -1;
            logger->logMessage(
                LogLevel::Information,
                logPrefix + std::string("scene capability not available; the world stays the static [scene] ")
                    + "configuration (" + error.what() + ")"
            );
        }
    }
    if (sceneResolutionState != 1 || state->sceneCapability == nullptr)
    {
        return;
    }

    // Subscribe BEFORE pushing the world: any mutation from here on - by
    // scripts whose top level runs during another module's start, or by
    // sibling modules - marks the data dirty for the first frame.
    EventBus &bus                                         = boundContext->eventBus();
    EventChannel<SceneObjectAddedEvent> &addedChannel     = bus.channel<SceneObjectAddedEvent>();
    EventChannel<SceneObjectRemovedEvent> &removedChannel = bus.channel<SceneObjectRemovedEvent>();
    EventChannel<SceneObjectChangedEvent> &changedChannel = bus.channel<SceneObjectChangedEvent>();
    sceneAddedSubscription                                = EventSubscription(
        &bus,
        typeid(SceneObjectAddedEvent),
        addedChannel.subscribe<RendererModule, &RendererModule::handleSceneAdded>(this)
    );
    sceneRemovedSubscription = EventSubscription(
        &bus,
        typeid(SceneObjectRemovedEvent),
        removedChannel.subscribe<RendererModule, &RendererModule::handleSceneRemoved>(this)
    );
    sceneChangedSubscription = EventSubscription(
        &bus,
        typeid(SceneObjectChangedEvent),
        changedChannel.subscribe<RendererModule, &RendererModule::handleSceneChanged>(this)
    );

    // Push the static world into the registry: one handle per configured
    // object, in scene order. From here the registry is the single source of
    // truth for OBJECTS; the configured camera, sun, sky and texture sets
    // stay the renderer's own.
    state->registryHandles.clear();
    state->registryHandles.reserve(state->scene.objects.size());
    for (const SceneObject &object : state->scene.objects)
    {
        const SceneCapability::ObjectHandle handle
            = state->sceneCapability->addObject(RendererState::sceneObjectToDescriptor(object));
        state->registryHandles.push_back(handle);
    }

    // Initial sync: adopt the registry (which now holds the static world plus
    // anything registered before the renderer started) and upload once. The
    // first frame then renders exactly what the registry holds.
    state->sceneRegistryDirty = false;
    state->refreshSceneRegistry();
    logger->logMessage(
        LogLevel::Information,
        logPrefix + "scene registry connected: " + std::to_string(state->activeObjects.size())
            + " object(s) live, capacity " + std::to_string(state->sceneCapability->objectCapacity())
    );
}

void RendererModule::handleSceneAdded(
    const SceneObjectAddedEvent & /*event*/
)
{
    if (state != nullptr)
    {
        state->sceneRegistryDirty = true;
    }
}

void RendererModule::handleSceneRemoved(
    const SceneObjectRemovedEvent & /*event*/
)
{
    if (state != nullptr)
    {
        state->sceneRegistryDirty = true;
    }
}

void RendererModule::handleSceneChanged(
    const SceneObjectChangedEvent & /*event*/
)
{
    if (state != nullptr)
    {
        state->sceneRegistryDirty = true;
    }
}

void RendererModule::handleHotReload(
    const HotReloadEvent &event
)
{
    if (state == nullptr)
    {
        return;
    }
    if (event.kind == hot_reload_kind::shader)
    {
        // Any .spv change re-creates every renderer pipeline: the modules
        // are loaded from disk at creation, and a partial reload (one
        // pipeline, the changed one) would need per-pipeline change
        // tracking for no dev-loop gain.
        shadersNeedReload = true;
    }
}

void RendererModule::handleConfigurationReloaded(
    const ConfigurationReloadedEvent &event
)
{
    if (state == nullptr || boundContext == nullptr)
    {
        return;
    }
    // The event carries dotted effective paths ("renderer.exposure"); only
    // this module's own keys are consulted, and only the push-constant ones:
    // bloom (pipeline structure) and the device-level keys keep their
    // startup values until the next run. Keep-last-good everywhere: a
    // handler on the live bus must not throw into the frame loop, so an
    // invalid reload value logs and keeps the previous one.
    const auto touches = [&event](const char *key) {
        return std::find(event.changedKeys.begin(), event.changedKeys.end(), std::string("renderer.") + key)
            != event.changedKeys.end();
    };
    if (touches("exposure"))
    {
        state->exposure = static_cast<float>(boundContext->configurationFloating("exposure").value_or(1.15));
        logger->logMessage(LogLevel::Information, logPrefix + "reloaded exposure");
    }
    if (touches("tonemap"))
    {
        const std::string reloadedMode = boundContext->configurationString("tonemap").value_or("aces");
        if (reloadedMode == "aces" || reloadedMode == "reinhard" || reloadedMode == "none")
        {
            state->tonemapMode = reloadedMode;
            logger->logMessage(LogLevel::Information, logPrefix + "reloaded tonemap mode '" + reloadedMode + "'");
        } else
        {
            logger->logMessage(
                LogLevel::Error,
                logPrefix + "reload carried unknown tonemap mode '" + reloadedMode
                    + "' (known: aces, reinhard, none); keeping '" + state->tonemapMode + "'"
            );
        }
    }
    if (touches("bloom_threshold"))
    {
        state->bloomThreshold
            = static_cast<float>(boundContext->configurationFloating("bloom_threshold").value_or(1.0));
        logger->logMessage(LogLevel::Information, logPrefix + "reloaded bloom threshold");
    }
    if (touches("bloom_strength"))
    {
        state->bloomStrength = static_cast<float>(boundContext->configurationFloating("bloom_strength").value_or(0.15));
        logger->logMessage(LogLevel::Information, logPrefix + "reloaded bloom strength");
    }
}

void RendererModule::renderFrame()
{
    // Headless degrade: no surface, no frame. The frame-skeleton events stay
    // unpublished - their consumers (screenshot captures, the trim-deleted ui
    // module's widgets) are meaningless without a frame, and the composition
    // root's own EngineFrameEvent heartbeat is what the gameplay layer ticks
    // from, renderer-free (the boot card).
    if (headlessDegrade)
    {
        return;
    }
    if (state == nullptr || state->swapchain == nullptr)
    {
        throw std::runtime_error(logPrefix + "renderFrame called before start");
    }
    if (frameBeginChannel == nullptr || frameRenderedChannel == nullptr)
    {
        throw std::runtime_error(logPrefix + "renderFrame called before construct");
    }

    FrameBeginEvent begin;
    begin.frameIndex = state->frameCounter;
    frameBeginChannel->publish(begin);

    if (state->framebufferResized)
    {
        state->recreateSwapchain(*windowCapability, *logger);
    }

    if (shadersNeedReload && state->device != nullptr)
    {
        shadersNeedReload = false;
        state->device->waitIdle();
        state->createPipelines();
        for (const auto &technique : state->lightingTechniques)
        {
            technique->notifyFrameResourcesRecreated();
        }
        logger->logMessage(LogLevel::Information, logPrefix + "shaders reloaded: pipelines re-created");
    }

    state->renderFrame(*logger);

    // Optional loud failure for development runs: abort at the first observed
    // validation error instead of continuing through a storm of duplicate
    // reports. Off by default; headless runs surface the same
    // condition through their exit code without it.
    if (state->failOnValidationError && state->validationErrorSeen.load())
    {
        throw std::runtime_error(
            logPrefix
            + "graphics backend reported a validation error "
              "(fail_on_validation_error is enabled)"
        );
    }

    FrameRenderedEvent rendered;
    rendered.frameIndex = state->frameCounter;
    frameRenderedChannel->publish(rendered);
}

void RendererModule::waitUntilIdle()
{
    if (state != nullptr && state->device != nullptr)
    {
        state->device->waitIdle();
    }
}

void RendererModule::requestFrameCapture()
{
    if (state != nullptr)
    {
        state->captureRequested = true;
    }
}

bool RendererModule::validationErrorsObserved() const
{
    return state != nullptr && state->validationErrorSeen.load();
}

std::array<float, 3> RendererModule::cameraPosition() const
{
    return state != nullptr ? state->flyCamera.position() : std::array<float, 3>{0.0f, 0.0f, 0.0f};
}

std::array<float, 3> RendererModule::cameraLookAt() const
{
    return state != nullptr ? state->flyCamera.lookTarget() : std::array<float, 3>{0.0f, 0.0f, 0.0f};
}

void RendererModule::setCameraPosition(
    const std::array<float, 3> &position
)
{
    if (state != nullptr)
    {
        state->flyCamera.setPosition(position);
    }
}

void RendererModule::setCameraLookAt(
    const std::array<float, 3> &lookAt
)
{
    if (state != nullptr)
    {
        state->flyCamera.lookAtPoint(lookAt);
    }
}

} // namespace engine
