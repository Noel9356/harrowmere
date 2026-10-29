#include "ui.hpp"

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <algorithm>
#include <chrono>
#include <cfloat>
#include <stdexcept>

namespace engine
{

UiModule::UiModule()
{
    componentManifest = ComponentManifest{
        .name                 = "ui",
        .kind                 = ComponentKind::Module,
        .version              = "0.1.0",
        .providedCapabilities = {"ui.imgui"},
        // The overlay needs a renderer that hosts it (the overlay seam, demanded
        // by its implementation-bound member name - the seam's contract IS the
        // Vulkan handle bundle), a window to measure, input to feed the UI
        // library and the scene registry for the census panel. Logging is the
        // shared baseline.
        .requiredCapabilities = {"logging", "renderer.vulkan.overlay", "window", "input", "scene"},
        // capability.renderer is deliberately NOT in requiredCapabilities: the
        // overlay needs the overlay SEAM, not the frame API, and a renderer
        // build without frame capture still hosts the overlay. Declared as a
        // permission (the screenshot-capability pattern) and resolved lazily for
        // the screenshot button.
        // capability.persistence is the optional-capability declaration below:
        // the save/load buttons are the overlay's nicest feature, not its
        // purpose, and a run without the persistence module must still start.
        // Declaring it (not just granting the permission) means the resolver
        // orders the provider before this module when one is active, so the
        // first-press resolve below is a property of the assembly.
        .optionalCapabilities = {"persistence"},
        .requiredComponents   = {},
        .requiredPermissions
        = {"capability.logging",
            "capability.renderer",
            "capability.window",
            "capability.input",
            "capability.scene",
            "capability.renderer.vulkan.overlay"},
        .knownConfigKeys = {"ui.key", "ui.font_scale_percent", "ui.save_path"},
    };
}

UiModule::~UiModule()
{
    stop();
}

void UiModule::construct(
    ComponentContext &context
)
{
    boundContext     = &context;
    logger           = &context.acquireCapability<LoggerCapability>();
    windowCapability = &context.acquireCapability<WindowCapability>();
    inputCapability  = &context.acquireCapability<InputCapability>();
    sceneCapability  = &context.acquireCapability<SceneCapability>();

    const std::string toggleKeyName = context.configurationString("key").value_or("F1");
    const KeyCode parsedKey         = keyCodeFromName(toggleKeyName);
    if (parsedKey == KeyCode::Unknown)
    {
        throw std::runtime_error("ui module: configuration key '" + toggleKeyName + "' is not a known key name");
    }
    toggleKeyCode = static_cast<std::uint32_t>(parsedKey);
    // Integer percent keeps the configuration surface in TOML integers (the
    // configuration store's documented value vocabulary); 100 = 1.0.
    const std::int64_t fontScalePercent = context.configurationInteger("font_scale_percent").value_or(100);
    fontScale                           = std::clamp(static_cast<float>(fontScalePercent) / 100.0f, 0.5f, 4.0f);

    // Where the save button writes and the load button reads. The default is
    // the historical hard-coded location (inside the demo content package's
    // saves folder); a real assembly points this at its own package so the
    // generic UI module stops naming one specific mod's layout.
    savePath = context.configurationString("save_path").value_or("mods/test_scene/saves/overlay_save.toml");

    EventBus &bus          = context.eventBus();
    frameBeginChannel      = &bus.channel<FrameBeginEvent>();
    keyPressedChannel      = &bus.channel<KeyPressedEvent>();
    textInputChannel       = &bus.channel<TextInputEvent>();
    frameBeginSubscription = EventSubscription(
        &bus, typeid(FrameBeginEvent), frameBeginChannel->subscribe<UiModule, &UiModule::handleFrameBegin>(this)
    );
    keyPressedSubscription = EventSubscription(
        &bus, typeid(KeyPressedEvent), keyPressedChannel->subscribe<UiModule, &UiModule::handleToggleKey>(this)
    );
    textInputSubscription = EventSubscription(
        &bus, typeid(TextInputEvent), textInputChannel->subscribe<UiModule, &UiModule::handleTextInput>(this)
    );
}

void UiModule::start()
{
    // The renderer has constructed and started by now (providers run before
    // consumers in the start order), so its overlay host exists. The Vulkan
    // backend initialization waits for the first visible frame: the handle
    // bundle is complete then, and a device-less run (no swapchain) logs one
    // precise warning instead of failing a start that might never render.
    overlayHost = &boundContext->acquireCapability<RendererOverlayHostCapability>();
    if (logger != nullptr)
    {
        logger->logMessage(
            LogLevel::Information,
            "ui module: debug overlay ready (toggle with " + std::to_string(toggleKeyCode)
                + " or [ui].key in the configuration)"
        );
    }
}

void UiModule::stop()
{
    if (backendInitialized)
    {
        // The kernel tears components down in the reverse of the start
        // order, so the renderer's device still exists here.
        overlayHost->setOverlayRecorder(nullptr);
        ImGui_ImplVulkan_Shutdown();
        ImGui::DestroyContext();
        backendInitialized = false;
        // The backend is down: drop the result guard's write targets so the
        // hook cannot reference this (possibly soon-destroyed) module. The
        // next init re-hands them over after a successful ImGui_ImplVulkan_Init.
        setVulkanResultGuardTargets(nullptr, nullptr);
    }
    // EventSubscription is an RAII guard: release() unsubscribes and
    // disarms, so the destructor does not double-unsubscribe.
    frameBeginSubscription.release();
    keyPressedSubscription.release();
    textInputSubscription.release();
    mainWindowIdentifier = invalidWindowIdentifier; // the next run re-resolves
    rendererCapability   = nullptr;
    overlayHost          = nullptr;
    boundContext         = nullptr;
}

void *UiModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    if (capabilityName == "ui.imgui")
    {
        return static_cast<UiCapability *>(this);
    }
    return nullptr;
}

void UiModule::setOverlayVisible(
    bool visible
)
{
    if (visible == overlayVisible_)
    {
        return;
    }
    if (visible)
    {
        // Remember the run's cursor capture state; the overlay needs the
        // real cursor, so capture releases for the duration.
        cursorWasCaptured = inputCapability != nullptr && inputCapability->isCursorCaptured();
        if (inputCapability != nullptr)
        {
            inputCapability->setCursorCaptured(false);
        }
        logger->logMessage(LogLevel::Information, "ui module: debug overlay shown");
    } else
    {
        if (cursorWasCaptured && inputCapability != nullptr)
        {
            inputCapability->setCursorCaptured(true);
        }
        cursorWasCaptured = false;
        logger->logMessage(LogLevel::Information, "ui module: debug overlay hidden");
    }
    overlayVisible_ = visible;
}

void UiModule::toggleOverlay()
{
    setOverlayVisible(!overlayVisible_);
}

void UiModule::handleToggleKey(
    const KeyPressedEvent &event
)
{
    if (static_cast<std::uint32_t>(event.keyCode) == toggleKeyCode)
    {
        toggleOverlay();
    }
}

void UiModule::handleTextInput(
    const TextInputEvent &event
)
{
    pendingTextInput.push_back(event.codepoint);
}

void UiModule::handleFrameBegin(
    const FrameBeginEvent &event
)
{
    // Frame statistics first, on EVERY frame: the sample must not depend on
    // the overlay being visible (the early return below skips everything
    // else). The interval between consecutive FrameBeginEvents is the frame
    // loop's true wall time - present waits included - which is what an FPS
    // counter has to report. The sanity clamp only guards against clock
    // glitches and multi-second debugger stalls dominating the window; normal
    // stutter passes through untouched.
    const std::int64_t statNow
        = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
              .count();
    if (lastStatFrameTime != 0)
    {
        float frameDelta                  = static_cast<float>(static_cast<double>(statNow - lastStatFrameTime) / 1e6);
        frameDelta                        = std::clamp(frameDelta, 0.0001f, 60.0f);
        frameTimeSamples[frameTimeCursor] = frameDelta;
        frameTimeCursor                   = (frameTimeCursor + 1) % frameTimeWindow;
        frameTimeCount                    = std::min(frameTimeCount + 1u, static_cast<std::uint32_t>(frameTimeWindow));
    }
    lastStatFrameTime = statNow;

    if (!overlayVisible_)
    {
        // Hidden: keep the path inert. The toggle key arrives through
        // KeyPressedEvent, not through this handler, and the draw data of a
        // previous visible frame is stale - recordOverlay must not re-emit
        // it (it checks overlayVisible_ for exactly that reason).
        // Text input keeps arriving while hidden and only a visible frame
        // drains the queue, so drop it here: an unbounded buffer that would
        // replay every keystroke ever typed the moment the overlay reopens.
        pendingTextInput.clear();
        (void)event;
        return;
    }

    if (!ensureBackendInitialized())
    {
        return; // decided once; backendFailed carries the precise log
    }

    // Frame pacing: the UI library wants seconds since the previous frame.
    // One clock read serves the whole handler: the stats timestamp above is
    // taken at the same point of the same handler invocation the old second
    // steady_clock::now() sampled (a few branches apart), so reusing it
    // halves the visible-frame clock reads without moving either delta.
    const std::int64_t now = statNow;
    float deltaSeconds     = lastFrameBeginTime == 0
                               ? 1.0f / 60.0f
                               : static_cast<float>(static_cast<double>(now - lastFrameBeginTime) / 1e6);
    lastFrameBeginTime     = now;
    deltaSeconds           = std::clamp(deltaSeconds, 1.0f / 500.0f, 1.0f / 10.0f);

    // The modal rule (UiCapability): while visible, the overlay owns the
    // pointer. Draining the accumulators makes the claim physical - camera
    // scripts that ignore wantsMouse() still read a settled 0.
    double ignoredX = 0.0;
    double ignoredY = 0.0;
    inputCapability->consumeMouseDelta(ignoredX, ignoredY);

    ImGuiIO &io        = ImGui::GetIO();
    io.DeltaTime       = deltaSeconds;
    io.FontGlobalScale = fontScale;
    // The main window identifier resolves once (see the member comment): the
    // window list is constant for the run. The list now arrives as a
    // read-only reference to the provider's cache (see the capability
    // header), so even this once-per-run resolve makes no copy.
    if (mainWindowIdentifier == invalidWindowIdentifier)
    {
        const std::vector<WindowIdentifier> &identifiers = windowCapability->windowIdentifiers();
        if (identifiers.empty())
        {
            return; // no window: nothing to size the io against this frame
        }
        mainWindowIdentifier = identifiers.front();
    }
    io.DisplaySize = ImVec2(
        static_cast<float>(windowCapability->framebufferWidth(mainWindowIdentifier)),
        static_cast<float>(windowCapability->framebufferHeight(mainWindowIdentifier))
    );

    // Feed the UI library the polled input state: position, buttons,
    // keyboard, queued text characters. Wheel motion is drained BEFORE the
    // feed, then fed: the overlay is modal, so it is the only legitimate
    // wheel consumer while visible.
    double wheelX = 0.0;
    double wheelY = 0.0;
    inputCapability->consumeScrollDelta(wheelX, wheelY);
    io.AddMouseWheelEvent(static_cast<float>(wheelX), static_cast<float>(wheelY));

    double mouseX = 0.0;
    double mouseY = 0.0;
    inputCapability->cursorPosition(mouseX, mouseY);
    io.AddMousePosEvent(static_cast<float>(mouseX), static_cast<float>(mouseY));
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, inputCapability->isMouseButtonDown(MouseButton::Left));
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, inputCapability->isMouseButtonDown(MouseButton::Right));
    io.AddMouseButtonEvent(ImGuiMouseButton_Middle, inputCapability->isMouseButtonDown(MouseButton::Middle));
    io.AddKeyEvent(ImGuiKey_LeftShift, inputCapability->isKeyDown(KeyCode::LeftShift));
    io.AddKeyEvent(ImGuiKey_RightShift, inputCapability->isKeyDown(KeyCode::RightShift));
    io.AddKeyEvent(ImGuiKey_LeftCtrl, inputCapability->isKeyDown(KeyCode::LeftControl));
    io.AddKeyEvent(ImGuiKey_RightCtrl, inputCapability->isKeyDown(KeyCode::RightControl));
    io.AddKeyEvent(ImGuiKey_LeftAlt, inputCapability->isKeyDown(KeyCode::LeftAlt));
    io.AddKeyEvent(ImGuiKey_RightAlt, inputCapability->isKeyDown(KeyCode::RightAlt));
    io.AddKeyEvent(ImGuiKey_LeftSuper, inputCapability->isKeyDown(KeyCode::LeftSuper));
    for (const UiKeyMapping &mapped : uiKeyMap())
    {
        io.AddKeyEvent(
            static_cast<ImGuiKey>(mapped.imguiKey), inputCapability->isKeyDown(static_cast<KeyCode>(mapped.keyCode))
        );
    }
    flushQueuedTextInput();

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    buildDebugWindow();
    ImGui::Render();
}

void UiModule::flushQueuedTextInput()
{
    for (const std::uint32_t codepoint : pendingTextInput)
    {
        ImGui::GetIO().AddInputCharacter(codepoint);
    }
    pendingTextInput.clear();
}

void UiModule::buildDebugWindow()
{
    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("engine", nullptr, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    // ---- frame statistics ----------------------------------------------------
    ImGui::Text(
        "frame %.2f ms (%.1f fps)",
        static_cast<double>(ImGui::GetIO().DeltaTime * 1000.0f),
        static_cast<double>(ImGui::GetIO().Framerate)
    );
    // Min/max/avg over the statistics window (frameTimeSamples, updated on
    // every frame). fps min = 1/max interval, fps avg = samples/sum, fps max
    // = 1/min interval; the ms row states the same samples directly - the
    // scale tuning actually reads.
    if (frameTimeCount > 0)
    {
        // samples[0] is part of the window: the sum starts on it (the old
        // loop summed index 1..count-1 but divided by count, biasing the
        // average low).
        float sum      = frameTimeSamples[0];
        float shortest = frameTimeSamples[0];
        float longest  = frameTimeSamples[0];
        for (std::uint32_t index = 1; index < frameTimeCount; ++index)
        {
            sum += frameTimeSamples[index];
            shortest = std::min(shortest, frameTimeSamples[index]);
            longest  = std::max(longest, frameTimeSamples[index]);
        }
        const float average = sum / static_cast<float>(frameTimeCount);
        ImGui::Text(
            "fps  min %.1f / avg %.1f / max %.1f",
            static_cast<double>(1.0f / longest),
            static_cast<double>(static_cast<float>(frameTimeCount) / sum),
            static_cast<double>(1.0f / shortest)
        );
        ImGui::Text(
            "ms   min %.2f / avg %.2f / max %.2f  (%u frames)",
            static_cast<double>(shortest * 1000.0f),
            static_cast<double>(average * 1000.0f),
            static_cast<double>(longest * 1000.0f),
            static_cast<unsigned>(frameTimeCount)
        );
    }

    // ---- scene census --------------------------------------------------------
    if (sceneCapability != nullptr)
    {
        ImGui::Text(
            "scene: %u / %u objects",
            static_cast<unsigned>(sceneCapability->objectCount()),
            static_cast<unsigned>(sceneCapability->objectCapacity())
        );
    }

    // ---- camera pose ---------------------------------------------------------
    if (rendererCapability != nullptr)
    {
        const std::array<float, 3> position = rendererCapability->cameraPosition();
        const std::array<float, 3> lookAt   = rendererCapability->cameraLookAt();
        ImGui::Text(
            "camera pos  %.1f, %.1f, %.1f",
            static_cast<double>(position[0]),
            static_cast<double>(position[1]),
            static_cast<double>(position[2])
        );
        ImGui::Text(
            "camera look %.1f, %.1f, %.1f",
            static_cast<double>(lookAt[0]),
            static_cast<double>(lookAt[1]),
            static_cast<double>(lookAt[2])
        );
    }

    ImGui::Separator();

    // ---- run actions ---------------------------------------------------------
    if (rendererCapability != nullptr && ImGui::Button("screenshot"))
    {
        rendererCapability->requestFrameCapture();
    }
    if (ImGui::Button("save scene"))
    {
        PersistenceCapability *persistence = ensurePersistenceCapability();
        std::string errorReport;
        if (persistence != nullptr && persistence->saveScene(savePath, errorReport))
        {
            logger->logMessage(LogLevel::Information, "ui module: scene saved to " + savePath);
        } else if (logger != nullptr)
        {
            logger->logMessage(LogLevel::Warning, "ui module: scene save failed: " + errorReport);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("load scene"))
    {
        PersistenceCapability *persistence = ensurePersistenceCapability();
        std::string errorReport;
        if (persistence != nullptr)
        {
            const std::uint32_t spawned = persistence->loadScene(savePath, errorReport);
            logger->logMessage(
                LogLevel::Information,
                "ui module: scene load spawned " + std::to_string(spawned) + " objects"
                    + (errorReport.empty() ? std::string() : " (" + errorReport + ")")
            );
        } else if (logger != nullptr)
        {
            logger->logMessage(LogLevel::Warning, "ui module: scene load skipped: " + errorReport);
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("F1 toggles this overlay");
    ImGui::End();
}

PersistenceCapability *UiModule::ensurePersistenceCapability()
{
    if (persistenceResolutionState != 0) // 1 = available, -1 = decided unavailable
    {
        return persistenceResolutionState == 1 ? persistenceCapability : nullptr;
    }
    if (boundContext == nullptr)
    {
        return nullptr;
    }
    try
    {
        persistenceCapability      = &boundContext->acquireCapability<PersistenceCapability>();
        persistenceResolutionState = 1;
    } catch (const std::exception &)
    {
        // The persistence module is not part of this build (or its
        // permission is denied). Decided at first PRESS time - every
        // component has constructed by then - so "unavailable" is final;
        // caching it keeps a button press from spamming the log.
        persistenceCapability      = nullptr;
        persistenceResolutionState = -1;
        if (!persistenceWarnedUnavailable && logger != nullptr)
        {
            persistenceWarnedUnavailable = true;
            logger->logMessage(
                LogLevel::Warning,
                "ui module: the save/load buttons are unavailable for this run (the persistence module "
                "is not part of the build or its capability permission is denied)"
            );
        }
    }
    return persistenceResolutionState == 1 ? persistenceCapability : nullptr;
}

} // namespace engine
