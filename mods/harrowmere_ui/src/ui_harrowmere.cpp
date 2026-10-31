// ui_harrowmere.cpp -- harrowmere_ui mod component (Phase-1 slice of task 7.3).
//
// Owns the overlay draw pass. The renderer.vulkan.overlay host calls
// recordOverlay once per RENDERED frame (NOT EngineFixedUpdateEvent -- the
// overlay is frame-coupled by design, common.md §4.4 step 13). On the first
// *visible* frame, initializes the ImGui backend against the renderer's
// published handle bundle (D-UI-1). F1 toggles visibility (config
// [harrowmere_ui] key, default F1).
//
// Read the world through SimCapability views -- never include harrowmere_core
// internals. The 7.3P1 Done-when: "No file in mods/harrowmere_ui/ includes
// harrowmere_core internals (grep) -- only harrowmere_capability.hpp."

#include "ui_harrowmere.hpp"

// ImGui (third_party/imgui at the pinned tag) -- we only need the public entry
// points here; the backend bindings live inside ImGuiBackend.
#include <imgui.h>

// Logging shim (see imgui_backend.cpp for the rationale). Same fallback to
// stderr if the project's HARROWMERE_LOG_* macros are not on the include path.
#ifndef HARROWMERE_LOG_INFO
#  include <cstdio>
#  define HARROWMERE_LOG_INFO(cat, fmt, ...)  std::fprintf(stderr, "[ui][INFO] " fmt "\n", ##__VA_ARGS__)
#  define HARROWMERE_LOG_WARN(cat, fmt, ...)  std::fprintf(stderr, "[ui][WARN] " fmt "\n", ##__VA_ARGS__)
#  define HARROWMERE_LOG_TRACE(cat, fmt, ...) /* trace compiled out by default */
#endif

#define HARROWMERE_LOG_CATEGORY ui

namespace harrowmere::ui {

HarrowmereUi::HarrowmereUi() = default;

HarrowmereUi::~HarrowmereUi() {
    // Defensive: stop() should have been called by the mod's plugin entry
    // (engine_query_mod_entry_table -> destroyMod). If it wasn't, the RAII
    // members (EventSubscription, ImGuiBackend) still release.
    if (backend_.ready()) {
        backend_.shutdown();
    }
}

void HarrowmereUi::construct(engine::ComponentContext& ctx) {
    // The three capabilities declared in mods/harrowmere_ui/mod.toml (task
    // 0.1): harrowmere (the FAMILY name -- the kernel resolves a family
    // request to the single active member, harrowmere.core), input, and
    // renderer.vulkan.overlay. All three are *required* -- a missing one
    // fails startup with a precise ComponentAccessError (engine rule,
    // common.md §4.0). A family acquire is by NAME (the resolver's typed
    // template form would request capability.harrowmere.core, which no family
    // requirement grants to a mod); input and the overlay host are typed
    // members whose names match their manifest requirements.
    sim_ = static_cast<harrowmere::SimCapability *>(
        ctx.acquireCapability(harrowmere::SimCapability::capabilityFamily));
    input_ = &ctx.acquireCapability<engine::InputCapability>();
    overlay_ = static_cast<engine::RendererOverlayHostCapability *>(
        ctx.acquireCapability(engine::RendererOverlayHostCapability::capabilityName));

    // The optional window probe: the ImGui GLFW backend needs the GLFWwindow*
    // and the input module does not re-publish it. Declared in
    // optional_capabilities (mod.toml + the compiled manifest) so the probe
    // has its grant; an absent window (headless) leaves the overlay dark --
    // the feature degrades, never the run (common.md §4.0).
    if (ctx.isCapabilityAvailable("window")) {
        window_ = static_cast<engine::WindowCapability *>(ctx.acquireCapability("window"));
    }

    // Read the F1 keybind from the assembly config. The key is declared in
    // mods/harrowmere_ui/mod.toml's config_keys; a missing key falls back to
    // F1. The name resolves through the input capability's own key-name table
    // (the same mapping every KeyPressedEvent publishes), so an unknown name
    // degrades to Unknown (no toggle) rather than a wrong key.
    const std::string keyName = ctx.configurationStringInSection("harrowmere_ui", "key").value_or("F1");
    toggleKey_ = engine::keyCodeFromName(keyName);
    if (toggleKey_ == engine::KeyCode::Unknown) {
        HARROWMERE_LOG_WARN(HARROWMERE_LOG_CATEGORY, "[harrowmere_ui] key \"%s\" is not a known key name; F1 toggle disabled",
                            keyName.c_str());
    }

    // The start-state seed for the same debug panel (config_keys entry
    // "harrowmere_ui.visible_on_start", default true = the 7.3P1 pinned
    // behavior): a run that starts the panel hidden presses F1 to show it -
    // the toggle itself is unchanged. Automated visual runs stage the key
    // through the configuration so captures see the world, not the panel.
    visible_ = ctx.configurationIntegerInSection("harrowmere_ui", "visible_on_start").value_or(1) != 0;

    // Subscribe to KeyPressedEvent for F1 toggles. Polled movement keys
    // (WASD, Shift, mouse look) are read through InputCapability each frame
    // in 7.4P1 -- only discrete toggles use the event bus here (common.md
    // §4.0: input_glfw publishes KeyPressed/KeyReleased events; "in a native
    // mod you subscribe to the channel directly; there is no subscribe_key_
    // pressed (that name belongs to the script bridge)"). Single ownership of
    // the toggle is 7.4P1's double-fire pitfall answered by construction.
    engine::EventChannel<engine::KeyPressedEvent> &keyChannel = ctx.eventBus().channel<engine::KeyPressedEvent>();
    keySub_ = engine::EventSubscription(
        &ctx.eventBus(), typeid(engine::KeyPressedEvent),
        keyChannel.subscribe<HarrowmereUi, &HarrowmereUi::onKeyPressed>(this));
}

void HarrowmereUi::start() {
    // Try the seam registration once here (the renderer starts before this
    // mod -- the resolver's dependency order -- so its handle bundle is
    // usually already published). A failure stays retryable per rendered
    // frame in recordOverlay: the bundle appears when the renderer
    // initializes its swapchain, and ImGui init before the handle bundle
    // completes fails (ImGui's own example backend behavior, task 7.3P1).
    ImGuiBackendHandles handles{};
    std::string err;
    if (collectOverlayHandles(handles, err) && backend_.init(handles, err)) {
        overlay_->setOverlayRecorder(this);
        recorderRegistered_ = true;
        HARROWMERE_LOG_INFO(HARROWMERE_LOG_CATEGORY, "overlay recorder registered (renderer.vulkan.overlay seam)");
    } else {
        HARROWMERE_LOG_INFO(HARROWMERE_LOG_CATEGORY,
                            "overlay seam pending first visible frame (%s)", err.c_str());
    }
    HARROWMERE_LOG_INFO(HARROWMERE_LOG_CATEGORY, "harrowmere_ui 0.1.0 up (7.3P1 debug overlay, F1 toggles)");
}

void HarrowmereUi::stop() {
    // Mirror of construct in reverse. Unregister the recorder first so no
    // in-flight recordOverlay lands on a tearing-down backend, then the key
    // subscription, then the backend.
    if (recorderRegistered_ && overlay_ != nullptr) {
        overlay_->setOverlayRecorder(nullptr);
        recorderRegistered_ = false;
    }
    keySub_.release();
    if (backend_.ready()) {
        backend_.shutdown();
    }
    sim_ = nullptr;
    input_ = nullptr;
    overlay_ = nullptr;
    window_ = nullptr;
}

bool HarrowmereUi::wantsMouse() const noexcept {
    // Only claim the mouse when the overlay is visible AND ImGui has a panel
    // with focus. 7.4P1 reads this to gate camera look; when false, the mouse
    // drives the camera and the overlay is click-through (except for the
    // F1 toggle which is a key event, not a mouse event).
    if (!visible_ || !backend_.ready()) return false;
    return backend_.wantsMouse();
}

bool HarrowmereUi::wantsKeyboard() const noexcept {
    if (!visible_ || !backend_.ready()) return false;
    return backend_.wantsKeyboard();
}

void HarrowmereUi::recordOverlay(const engine::OverlayFrameContext& context) {
    // --- (1) Lazy init on the first *visible* frame (retryable) ---
    if (!backend_.ready()) {
        ImGuiBackendHandles handles{};
        std::string err;
        if (!collectOverlayHandles(handles, err) || !backend_.init(handles, err)) {
            return; // stay dark this frame; retry next rendered frame
        }
        if (!recorderRegistered_ && overlay_ != nullptr) {
            overlay_->setOverlayRecorder(this);
            recorderRegistered_ = true;
        }
        HARROWMERE_LOG_INFO(HARROWMERE_LOG_CATEGORY, "ImGui backend initialized on the first visible frame");
    }

    if (!visible_) {
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

    // Pull the view snapshots -- rebuilt each frame by the sim (common.md
    // §4.2/§4.4). The SimCapability views are *snapshots, not live
    // references* -- ids and copies only, no store handles (R10, §4.5).
    const harrowmere::views::WorldView&    world    = sim_->world();
    const harrowmere::views::PlayerView&   player   = sim_->player();
    const harrowmere::views::JusticeView&  justice  = sim_->justice();
    const harrowmere::views::EventsView&   events   = sim_->events();
    const harrowmere::views::UiSignals&    signals  = sim_->signals();

    // The debug panel. drawDebugPanel is forward-declared in ui_harrowmere.hpp
    // and defined in panels/debug.cpp. Taking the WorldView + other views by
    // const-ref here is the compile-time binding proof (test
    // TestUi_DebugPanelBindsWorldView).
    bool open = true;
    drawDebugPanel(world, player, justice, events, signals, fps_, &open);
    // open=false closes the panel via ImGui's close button; we treat that as
    // "hide overlay" (equivalent to pressing F1).
    if (!open) visible_ = false;

    backend_.endFrame(context.commandBuffer);
}

void HarrowmereUi::onKeyPressed(const engine::KeyPressedEvent& ev) {
    if (ev.keyCode == toggleKey_) {
        visible_ = !visible_;
        HARROWMERE_LOG_INFO(HARROWMERE_LOG_CATEGORY, "overlay %s", visible_ ? "shown" : "hidden");
    }
}

bool HarrowmereUi::collectOverlayHandles(ImGuiBackendHandles& out, std::string& errOut) const {
    errOut.clear();

    // The GLFW window handle: the window capability's cached identifier list
    // (the engine's main window is the first one; the window module documents
    // the list as a read-only borrow). Absent in a headless run (the probe
    // left window_ null) -- the overlay stays dark there by design.
    if (window_ == nullptr) {
        errOut = "window capability absent (headless run): no GLFW handle for the ImGui platform backend";
        return false;
    }
    const std::vector<engine::WindowIdentifier> &windows = window_->windowIdentifiers();
    if (windows.empty()) {
        errOut = "window capability reports no windows (headless degrade)";
        return false;
    }
    out.glfwWindow = window_->nativeWindowHandle(windows.front());
    if (out.glfwWindow == nullptr) {
        errOut = "window capability returned a null native handle";
        return false;
    }

    // The renderer.vulkan.overlay host's backend bundle: null until the
    // renderer initialized its device and swapchain -- the retry condition.
    const engine::OverlayBackendHandles *bundle = overlay_->overlayBackendHandles();
    if (bundle == nullptr) {
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
    out.colorFormat      = static_cast<std::uint32_t>(bundle->colorFormat);
    out.overlayDescriptorPool = nullptr; // the backend creates its own pool (DescriptorPoolSize)

    if (out.vkInstance == nullptr || out.vkPhysicalDevice == nullptr ||
        out.vkDevice == nullptr || out.vkQueue == nullptr) {
        errOut = "renderer.vulkan.overlay seam missing a Vulkan handle";
        return false;
    }
    if (out.imageCount == 0) {
        errOut = "renderer.vulkan.overlay seam reports zero swapchain images";
        return false;
    }
    return true;
}

}  // namespace harrowmere::ui
