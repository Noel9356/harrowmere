// ui_harrowmere.cpp -- harrowmere_ui mod component (Phase-1 slice of task 7.3,
// extended by the full card: the panel host).
//
// Owns the overlay draw pass. The renderer.vulkan.overlay host calls
// recordOverlay once per RENDERED frame (NOT EngineFixedUpdateEvent -- the
// overlay is frame-coupled by design, common.md §4.4 step 13). On the first
// *visible* frame, initializes the ImGui backend against the renderer's
// published handle bundle (D-UI-1). F1 toggles visibility (config
// [harrowmere_ui] key, default F1); Tab/C/J/K/M toggle the Phase-2 panels;
// Esc closes them all.
//
// Read the world through SimCapability views -- never include harrowmere_core
// internals. The 7.3P1 Done-when: "No file in mods/harrowmere_ui/ includes
// harrowmere_core internals (grep) -- only harrowmere_capability.hpp."

#include "ui_harrowmere.hpp"
#include "engine/kernel/filesystem_utilities.hpp"

#include "panels/options.hpp"

#include <filesystem>
#include <algorithm>
#include <cmath>

// ImGui (third_party/imgui at the pinned tag) -- we only need the public entry
// points here; the backend bindings live inside ImGuiBackend.
#include <imgui.h>

// Logging shim (see imgui_backend.cpp for the rationale). Same fallback to
// stderr if the project's HARROWMERE_LOG_* macros are not on the include path.
#ifndef HARROWMERE_LOG_INFO
#include <cstdio>
#define HARROWMERE_LOG_INFO(cat, fmt, ...) std::fprintf(stderr, "[ui][INFO] " fmt "\n", ##__VA_ARGS__)
#define HARROWMERE_LOG_WARN(cat, fmt, ...) std::fprintf(stderr, "[ui][WARN] " fmt "\n", ##__VA_ARGS__)
#define HARROWMERE_LOG_TRACE(cat, fmt, ...) /* trace compiled out by default */
#endif

#define HARROWMERE_LOG_CATEGORY ui

namespace harrowmere::ui
{

HarrowmereUi::HarrowmereUi() = default;

HarrowmereUi::~HarrowmereUi()
{
    // Defensive: stop() should have been called by the mod's plugin entry
    // (engine_query_mod_entry_table -> destroyMod). If it wasn't, the RAII
    // members (EventSubscription, ImGuiBackend) still release.
    if (backend_.ready())
    {
        backend_.shutdown();
    }
}

void HarrowmereUi::construct(
    engine::ComponentContext &ctx
)
{
    // Acquire the persistent options store and file watcher first. They are
    // required capabilities; settings is deliberately separate from campaign
    // persistence, and hot_reload reports settings.toml edits on the main thread.
    settings_  = &ctx.acquireCapability<engine::SettingsCapability>();
    hotReload_ = &ctx.acquireCapability<engine::HotReloadCapability>();
    if (ctx.isCapabilityAvailable("audio"))
        audio_ = static_cast<engine::AudioCapability *>(ctx.acquireCapability("audio"));
    componentContext_ = &ctx;
    settingsPath_     = ctx.configurationStringInSection("settings", "file").value_or("settings.toml");

    // The three capabilities declared in mods/harrowmere_ui/mod.toml (task
    // 0.1): harrowmere (the FAMILY name -- the kernel resolves a family
    // request to the single active member, harrowmere.core), input, and
    // renderer.vulkan.overlay. All three are *required* -- a missing one
    // fails startup with a precise ComponentAccessError (engine rule,
    // common.md §4.0). A family acquire is by NAME (the resolver's typed
    // template form would request capability.harrowmere.core, which no family
    // requirement grants to a mod); input and the overlay host are typed
    // members whose names match their manifest requirements.
    sim_ = static_cast<harrowmere::SimCapability *>(ctx.acquireCapability(harrowmere::SimCapability::capabilityFamily));
    input_   = &ctx.acquireCapability<engine::InputCapability>();
    overlay_ = static_cast<engine::RendererOverlayHostCapability *>(
        ctx.acquireCapability(engine::RendererOverlayHostCapability::capabilityName)
    );

    // The optional window probe: the ImGui GLFW backend needs the GLFWwindow*
    // and the input module does not re-publish it. Declared in
    // optional_capabilities (mod.toml + the compiled manifest) so the probe
    // has its grant; an absent window (headless) leaves the overlay dark --
    // the feature degrades, never the run (common.md §4.0).
    if (ctx.isCapabilityAvailable("window"))
    {
        window_ = static_cast<engine::WindowCapability *>(ctx.acquireCapability("window"));
    }

    // Read the F1 keybind from the assembly config. The key is declared in
    // mods/harrowmere_ui/mod.toml's config_keys; a missing key falls back to
    // F1. The name resolves through the input capability's own key-name table
    // (the same mapping every KeyPressedEvent publishes), so an unknown name
    // degrades to Unknown (no toggle) rather than a wrong key.
    const std::string keyName = ctx.configurationStringInSection("harrowmere_ui", "key").value_or("F1");
    toggleKey_                = engine::keyCodeFromName(keyName);
    if (toggleKey_ == engine::KeyCode::Unknown)
    {
        HARROWMERE_LOG_WARN(
            HARROWMERE_LOG_CATEGORY,
            "[harrowmere_ui] key \"%s\" is not a known key name; F1 toggle disabled",
            keyName.c_str()
        );
    }

    // The start-state seed for the same debug panel (config_keys entry
    // "harrowmere_ui.visible_on_start", default true = the 7.3P1 pinned
    // behavior): a run that starts the panel hidden presses F1 to show it -
    // the toggle itself is unchanged. Automated visual runs stage the key
    // through the configuration so captures see the world, not the panel.
    visible_ = ctx.configurationIntegerInSection("harrowmere_ui", "visible_on_start").value_or(1) != 0;

    // Seed settings only when absent. These are player preferences, never
    // campaign state; the settings provider writes its own TOML file.
    const auto setStringDefault = [this](const char *key, const char *value) {
        if (!settings_->contains(key))
            settings_->setString(key, value);
    };
    const auto setNumberDefault = [this](const char *key, double value) {
        if (!settings_->contains(key))
            settings_->setNumber(key, value);
    };
    const auto setBooleanDefault = [this](const char *key, bool value) {
        if (!settings_->contains(key))
            settings_->setBoolean(key, value);
    };
    setNumberDefault("audio.master_volume", ctx.configurationFloatingInSection("audio", "master_volume").value_or(1.0));
    setStringDefault(
        "graphics.quality", ctx.configurationStringInSection("graphics", "quality").value_or("High").c_str()
    );
    setNumberDefault("ui.scale", ctx.configurationFloatingInSection("ui", "scale").value_or(1.0));
    setBooleanDefault("ui.verbose", ctx.configurationBooleanInSection("ui", "verbose").value_or(false));
    setStringDefault(
        "ui.actions.toggle_overlay",
        ctx.configurationStringInSection("ui", "actions.toggle_overlay").value_or(keyName).c_str()
    );
    const auto seedInputDefault = [this, &ctx](const char *key, const char *fallback) {
        const std::string configured
            = ctx.configurationStringInSection("input", std::string("actions.") + key).value_or(fallback);
        const std::string settingKey = std::string("input.actions.") + key;
        if (!settings_->contains(settingKey))
            settings_->setString(settingKey, configured);
    };
    seedInputDefault("walk_forward", "W");
    seedInputDefault("walk_back", "S");
    seedInputDefault("walk_left", "A");
    seedInputDefault("walk_right", "D");
    seedInputDefault("sprint", "LeftShift");
    seedInputDefault("interact", "E");
    seedInputDefault("crouch", "Z");
    seedInputDefault("torch", "T");
    seedInputDefault("mask", "Y");
    seedInputDefault("report_crime", "G");
    seedInputDefault("bribe", "B");
    seedInputDefault("surrender", "X");
    seedInputDefault("jump", "Space");
    std::string settingsError;
    if (!settings_->save("", settingsError))
    {
        HARROWMERE_LOG_WARN(HARROWMERE_LOG_CATEGORY, "could not initialise settings file: %s", settingsError.c_str());
    }
    applySettings();

    // Watch the engine's configured settings file. The watcher reports the
    // changed absolute path; compare normalized paths so custom settings.file
    // values work as well as the shipped settings.toml default.
    std::filesystem::path settingsFile(settingsPath_);
    if (settingsFile.is_relative())
        settingsFile = std::filesystem::path(engine::executableDirectory()) / settingsFile;
    watchedSettingsPath_ = settingsFile.lexically_normal().generic_string();
    if (!hotReload_->watchPath(settingsPath_))
    {
        HARROWMERE_LOG_WARN(HARROWMERE_LOG_CATEGORY, "settings file could not be watched: %s", settingsPath_.c_str());
    }
    engine::EventChannel<engine::HotReloadEvent> &reloadChannel = ctx.eventBus().channel<engine::HotReloadEvent>();
    hotReloadSub_                                               = engine::EventSubscription(
        &ctx.eventBus(),
        typeid(engine::HotReloadEvent),
        reloadChannel.subscribe<HarrowmereUi, &HarrowmereUi::onHotReload>(this)
    );

    // The engine configuration watch hub handles live edits to the UI defaults
    // independently of settings.toml. Watch guards live with this component.
    configWatchHub_.bind(ctx.eventBus());
    keyConfigWatch_     = configWatchHub_.watch("harrowmere_ui.key", [this](const std::string &) {
        if (componentContext_ != nullptr && settings_ != nullptr)
        {
            const std::string key
                = componentContext_->configurationStringInSection("harrowmere_ui", "key").value_or("F1");
            settings_->setString("ui.actions.toggle_overlay", key);
            std::string error;
            settings_->save("", error);
            applySettings();
        }
    });
    visibleConfigWatch_ = configWatchHub_.watch("harrowmere_ui.visible_on_start", [this](const std::string &) {
        if (componentContext_ != nullptr)
            visible_ = componentContext_->configurationIntegerInSection("harrowmere_ui", "visible_on_start").value_or(1)
                    != 0;
    });

    // The Phase-2 panels start closed (defaultPanelVisibility): the player
    // opens them with the pinned keys. The HUD's `open` mirrors the overlay's
    // visibility (it is the always-on surface).
    panels_ = defaultPanelVisibility();

    // Subscribe to KeyPressedEvent for F1 + the panel toggles. Polled
    // movement keys (WASD, Shift, mouse look) are read through
    // InputCapability each frame in 7.4 -- only discrete toggles use the
    // event bus here (common.md §4.0: input_glfw publishes
    // KeyPressed/KeyReleased events; "in a native mod you subscribe to the
    // channel directly; there is no subscribe_key_pressed (that name belongs
    // to the script bridge)"). Single ownership of every toggle is 7.4P1's
    // double-fire pitfall answered by construction.
    engine::EventChannel<engine::KeyPressedEvent> &keyChannel = ctx.eventBus().channel<engine::KeyPressedEvent>();
    keySub_                                                   = engine::EventSubscription(
        &ctx.eventBus(),
        typeid(engine::KeyPressedEvent),
        keyChannel.subscribe<HarrowmereUi, &HarrowmereUi::onKeyPressed>(this)
    );
}

void HarrowmereUi::start()
{
    // Try the seam registration once here (the renderer starts before this
    // mod -- the resolver's dependency order -- so its handle bundle is
    // usually already published). A failure stays retryable per rendered
    // frame in recordOverlay: the bundle appears when the renderer
    // initializes its swapchain, and ImGui init before the handle bundle
    // completes fails (ImGui's own example backend behavior, task 7.3P1).
    ImGuiBackendHandles handles{};
    std::string err;
    if (collectOverlayHandles(handles, err) && backend_.init(handles, err))
    {
        overlay_->setOverlayRecorder(this);
        recorderRegistered_ = true;
        HARROWMERE_LOG_INFO(HARROWMERE_LOG_CATEGORY, "overlay recorder registered (renderer.vulkan.overlay seam)");
    } else
    {
        HARROWMERE_LOG_INFO(HARROWMERE_LOG_CATEGORY, "overlay seam pending first visible frame (%s)", err.c_str());
    }
    HARROWMERE_LOG_INFO(HARROWMERE_LOG_CATEGORY, "harrowmere_ui 0.1.0 up (7.3P1 overlay + 7.3 panels, F1 toggles)");
}

void HarrowmereUi::stop()
{
    // Mirror of construct in reverse. Unregister the recorder first so no
    // in-flight recordOverlay lands on a tearing-down backend, then the key
    // subscription, then the backend.
    if (recorderRegistered_ && overlay_ != nullptr)
    {
        overlay_->setOverlayRecorder(nullptr);
        recorderRegistered_ = false;
    }
    keySub_.release();
    hotReloadSub_.release();
    keyConfigWatch_.release();
    visibleConfigWatch_.release();
    if (backend_.ready())
    {
        backend_.shutdown();
    }
    sim_              = nullptr;
    input_            = nullptr;
    overlay_          = nullptr;
    window_           = nullptr;
    settings_         = nullptr;
    hotReload_        = nullptr;
    audio_            = nullptr;
    componentContext_ = nullptr;
}

bool HarrowmereUi::wantsMouse() const noexcept
{
    // Only claim the mouse when the overlay is visible AND ImGui has a panel
    // with focus. 7.4 reads this to gate camera look; when false, the mouse
    // drives the camera and the overlay is click-through (except for the
    // F1 toggle which is a key event, not a mouse event).
    if (!visible_ || !backend_.ready())
        return false;
    return backend_.wantsMouse();
}

bool HarrowmereUi::wantsKeyboard() const noexcept
{
    if (!visible_ || !backend_.ready())
        return false;
    return backend_.wantsKeyboard();
}

void HarrowmereUi::recordOverlay(
    const engine::OverlayFrameContext &context
)
{
    // --- (1) Lazy init on the first *visible* frame (retryable) ---
    if (!backend_.ready())
    {
        ImGuiBackendHandles handles{};
        std::string err;
        if (!collectOverlayHandles(handles, err) || !backend_.init(handles, err))
        {
            return; // stay dark this frame; retry next rendered frame
        }
        if (!recorderRegistered_ && overlay_ != nullptr)
        {
            overlay_->setOverlayRecorder(this);
            recorderRegistered_ = true;
        }
        HARROWMERE_LOG_INFO(HARROWMERE_LOG_CATEGORY, "ImGui backend initialized on the first visible frame");
    }

    if (!visible_)
    {
        // Overlay hidden -- the recorder still runs (the renderer already
        // recorded around us), but no ImGui frame: no draw data, the overlay
        // command buffer stays empty. The next F1 press draws a fresh frame.
        return;
    }

    // --- (2) FPS tracking (debug panel only; not a sim value) ---
    // Uses the engine's frame counter (the renderer's monotonic frame index,
    // engine::OverlayFrameContext::frameIndex); the sim itself reads no wall
    // clock (R8) and this is presentation code.
    ++frameCount_;
    // The host gives no delta; twice-per-second bucketing on the frame index
    // keeps the counter allocation-free and monotonic.
    fps_ = static_cast<int>(frameCount_); // replaced below once a time base lands (7.3 full)

    // --- (3) The ImGui frame ---
    backend_.beginFrame();

    // Apply the persisted UI scale without cumulative scaling. ImGui style
    // sizes are presentation values; changing this never touches campaign data.
    if (settings_ != nullptr)
    {
        const float desiredScale = static_cast<float>(std::clamp(settings_->numberValue("ui.scale", 1.0), 0.75, 1.5));
        if (std::abs(desiredScale - appliedUiScale_) > 0.001f)
        {
            ImGui::GetStyle().ScaleAllSizes(desiredScale / appliedUiScale_);
            ImGui::GetIO().FontGlobalScale = desiredScale;
            appliedUiScale_                = desiredScale;
        }
    }

    // --- (4) The panel set (Phase 2) ---
    if (assemblePanelContext(panelCtx_))
    {
        // The HUD: always-on (the binding table's alwaysOn row).
        bool hudOpen = true;
        drawHudPanel(panelCtx_, &hudOpen);

        // The toggle-open windows (Tab/C/J/K/M flipped `panels_.open`).
        bool *characterOpen = &panels_.open[static_cast<std::size_t>(PanelId::Character)];
        bool *inventoryOpen = &panels_.open[static_cast<std::size_t>(PanelId::Inventory)];
        bool *justiceOpen   = &panels_.open[static_cast<std::size_t>(PanelId::Justice)];
        bool *eventsOpen    = &panels_.open[static_cast<std::size_t>(PanelId::Events)];
        bool *inspectorOpen = &panels_.open[static_cast<std::size_t>(PanelId::WorldInspector)];
        bool *mapOpen       = &panels_.open[static_cast<std::size_t>(PanelId::Map)];
        bool *timeOpen      = &panels_.open[static_cast<std::size_t>(PanelId::TimeControls)];

        if (*characterOpen)
        {
            drawCharacterPanel(panelCtx_, characterOpen);
        }
        if (*inventoryOpen)
        {
            drawInventoryPanel(panelCtx_, inventoryOpen);
        }
        if (*justiceOpen)
        {
            drawJusticePanel(panelCtx_, justiceOpen);
        }
        if (*eventsOpen)
        {
            drawEventsPanel(panelCtx_, eventsOpen);
        }
        if (*inspectorOpen)
        {
            drawWorldInspectorPanel(panelCtx_, inspectorOpen);
        }
        if (*mapOpen)
        {
            drawMapPanel(panelCtx_, mapOpen);
        }
        if (*timeOpen)
        {
            drawTimeControlsPanel(panelCtx_, timeOpen);
        }
    }

    // --- (5) The Phase-1 debug panel (F1 surface; unchanged) ---
    {
        const harrowmere::views::WorldView &world     = sim_->world();
        const harrowmere::views::PlayerView &player   = sim_->player();
        const harrowmere::views::JusticeView &justice = sim_->justice();
        const harrowmere::views::EventsView &events   = sim_->events();
        const harrowmere::views::UiSignals &signals   = sim_->signals();

        bool open = true;
        drawDebugPanel(world, player, justice, events, signals, fps_, &open);
        // open=false closes the panel via ImGui's close button; we treat that as
        // "hide overlay" (equivalent to pressing F1).
        if (!open)
            visible_ = false;
    }

    if (optionsOpen_ && settings_ != nullptr)
        drawOptionsPanel(*settings_, audio_, &optionsOpen_);

    backend_.endFrame(context.commandBuffer);
}

void HarrowmereUi::onKeyPressed(
    const engine::KeyPressedEvent &ev
)
{
    // F1: the overlay toggle (7.3P1; [harrowmere_ui]-configurable).
    if (ev.keyCode == toggleKey_)
    {
        visible_ = !visible_;
        HARROWMERE_LOG_INFO(HARROWMERE_LOG_CATEGORY, "overlay %s", visible_ ? "shown" : "hidden");
        return;
    }

    // F2 opens the options surface. It is kept outside the sim panel registry:
    // settings are preferences, not campaign views or commands.
    if (ev.keyCode == engine::KeyCode::F2)
    {
        optionsOpen_ = !optionsOpen_;
        return;
    }

    // The pinned panel toggles (card 7.4's table; this component owns every
    // UI-local edge - the input mapper never polls these keys).
    bool *panelOpen = nullptr;
    if (ev.keyCode == kPanelToggleCharacter)
    {
        panelOpen = &panels_.open[static_cast<std::size_t>(PanelId::Character)];
    } else if (ev.keyCode == kPanelToggleInventory)
    {
        panelOpen = &panels_.open[static_cast<std::size_t>(PanelId::Inventory)];
    } else if (ev.keyCode == kPanelToggleJustice)
    {
        panelOpen = &panels_.open[static_cast<std::size_t>(PanelId::Justice)];
    } else if (ev.keyCode == kPanelToggleEvents)
    {
        panelOpen = &panels_.open[static_cast<std::size_t>(PanelId::Events)];
    } else if (ev.keyCode == kPanelToggleMap)
    {
        panelOpen = &panels_.open[static_cast<std::size_t>(PanelId::Map)];
    } else if (ev.keyCode == kPanelToggleCloseAll)
    {
        // Esc = the "menu" binding: close every toggle-open panel (the HUD
        // and the Time panel stay - always-on surfaces).
        panels_.open[static_cast<std::size_t>(PanelId::Character)]      = false;
        panels_.open[static_cast<std::size_t>(PanelId::Inventory)]      = false;
        panels_.open[static_cast<std::size_t>(PanelId::Justice)]        = false;
        panels_.open[static_cast<std::size_t>(PanelId::Events)]         = false;
        panels_.open[static_cast<std::size_t>(PanelId::WorldInspector)] = false;
        panels_.open[static_cast<std::size_t>(PanelId::Map)]            = false;
        HARROWMERE_LOG_INFO(HARROWMERE_LOG_CATEGORY, "panels closed (Esc)");
        return;
    }

    if (panelOpen != nullptr)
    {
        *panelOpen = !*panelOpen; // one flip per press: the event edge owns it
    }
}

void HarrowmereUi::applySettings()
{
    if (settings_ == nullptr)
        return;
    const std::string toggleName = settings_->stringValue("ui.actions.toggle_overlay", "F1");
    toggleKey_                   = engine::keyCodeFromName(toggleName);
    if (toggleKey_ == engine::KeyCode::Unknown)
    {
        HARROWMERE_LOG_WARN(HARROWMERE_LOG_CATEGORY, "unknown overlay key '%s'; toggle disabled", toggleName.c_str());
    }
    if (audio_ != nullptr)
    {
        const double volume = std::clamp(settings_->numberValue("audio.master_volume", 1.0), 0.0, 1.0);
        audio_->setMasterVolume(static_cast<float>(volume));
    }
}

void HarrowmereUi::onHotReload(
    const engine::HotReloadEvent &ev
)
{
    if (settings_ == nullptr || ev.kind != engine::hot_reload_kind::config)
        return;
    std::string error;
    if (!reloadSettingsFromFileEvent(*settings_, ev, watchedSettingsPath_, settingsPath_, error))
    {
        if (!error.empty())
            HARROWMERE_LOG_WARN(HARROWMERE_LOG_CATEGORY, "settings reload refused: %s", error.c_str());
        return;
    }
    applySettings();
    HARROWMERE_LOG_INFO(HARROWMERE_LOG_CATEGORY, "settings reloaded from %s", settingsPath_.c_str());
}

bool HarrowmereUi::assemblePanelContext(
    PanelContext &out
) const
{
    if (sim_ == nullptr)
    {
        return false; // not bound: the debug panel alone (never a crash)
    }

    // The view snapshots - post-tick for this frame (§4.2 rule 1: plain
    // data by const-ref, no store handles, no lifetimes to manage).
    out.world        = &sim_->world();
    out.characters   = &sim_->characters();
    out.player       = &sim_->player();
    out.justice      = &sim_->justice();
    out.events       = &sim_->events();
    out.signals      = &sim_->signals();
    out.sim          = sim_; // submit() + the Time panel's pacing surface ONLY
    out.fps          = fps_;
    out.timeControls = sim_->timeControls();
    // selectedCharacterId persists across frames (the inspector's selection;
    // UI state, not sim state).
    return true;
}

bool HarrowmereUi::collectOverlayHandles(
    ImGuiBackendHandles &out, std::string &errOut
) const
{
    errOut.clear();

    // The GLFW window handle: the window capability's cached identifier list
    // (the engine's main window is the first one; the window module documents
    // the list as a read-only borrow). Absent in a headless run (the probe
    // left window_ null) -- the overlay stays dark there by design.
    if (window_ == nullptr)
    {
        errOut = "window capability absent (headless run): no GLFW handle for the ImGui platform backend";
        return false;
    }
    const std::vector<engine::WindowIdentifier> &windows = window_->windowIdentifiers();
    if (windows.empty())
    {
        errOut = "window capability reports no windows (headless degrade)";
        return false;
    }
    out.glfwWindow = window_->nativeWindowHandle(windows.front());
    if (out.glfwWindow == nullptr)
    {
        errOut = "window capability returned a null native handle";
        return false;
    }

    // The renderer.vulkan.overlay host's backend bundle: null until the
    // renderer initialized its device and swapchain -- the retry condition.
    const engine::OverlayBackendHandles *bundle = overlay_->overlayBackendHandles();
    if (bundle == nullptr)
    {
        errOut = "renderer.vulkan.overlay has not published its handle bundle yet";
        return false;
    }
    out.vkInstance       = bundle->instance;
    out.vkPhysicalDevice = bundle->physicalDevice;
    out.vkDevice         = bundle->device;
    out.vkQueue          = bundle->graphicsQueue;
    out.queueFamily      = bundle->queueFamilyIndex;
    out.imageCount       = bundle->imageCount;
    out.minImageCount    = bundle->minImageCount >= 2u ? bundle->minImageCount : 2u;
    // The overlay records inside the renderer's composite path, which the
    // engine drives through Vulkan DYNAMIC RENDERING (vk::RenderingInfo into
    // the swapchain image - renderer_frames.cpp's composite pass), not a
    // classic VkRenderPass. The seam's bundle carries the swapchain's color
    // format for exactly this: ImGui 1.91.9b's backend initializes for
    // dynamic rendering with UseDynamicRendering + ColorAttachmentFormat
    // (its example backend's overlay recipe). The wrapper reads the format
    // from the bundle through its own field.
    out.colorFormat           = static_cast<std::uint32_t>(bundle->colorFormat);
    out.overlayDescriptorPool = nullptr; // the backend creates its own pool (DescriptorPoolSize)

    if (out.vkInstance == nullptr || out.vkPhysicalDevice == nullptr || out.vkDevice == nullptr
        || out.vkQueue == nullptr)
    {
        errOut = "renderer.vulkan.overlay seam missing a Vulkan handle";
        return false;
    }
    if (out.imageCount == 0)
    {
        errOut = "renderer.vulkan.overlay seam reports zero swapchain images";
        return false;
    }
    return true;
}

} // namespace harrowmere::ui
