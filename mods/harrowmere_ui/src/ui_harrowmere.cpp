// ui_harrowmere.cpp -- harrowmere_ui mod component (Phase-1 slice of task 7.3).
//
// Owns the overlay draw pass. Subscribes EngineFrameEvent (NOT
// EngineFixedUpdateEvent -- the overlay is frame-coupled by design, common.md
// §4.4 step 13). On the first *visible* frame, initializes the ImGui backend
// through the renderer.vulkan.overlay seam (D-UI-1). F1 toggles visibility
// (config [harrowmere_ui].key, default F1).
//
// Read the world through SimCapability views -- never include harrowmere_core
// internals. The 7.3P1 Done-when: "No file in mods/harrowmere_ui/ includes
// harrowmere_core internals (grep) -- only harrowmere_capability.hpp."

#include "ui_harrowmere.hpp"

// ImGui (third_party/imgui at the pinned tag) -- we only need the public entry
// points here; the backend bindings live inside ImGuiBackend.
#include <imgui.h>

// GLFW key constants -- the engine's window/input capabilities publish through
// <GLFW/glfw3.h>; the F1 default lives in this TU's construct().
#include <GLFW/glfw3.h>

// Logging shim (see imgui_backend.cpp for the rationale). Same fallback to
// stderr if the project's HARROWMERE_LOG_* macros are not on the include path.
#ifndef HARROWMERE_LOG_INFO
#  include <cstdio>
#  define HARROWMERE_LOG_INFO(cat, fmt, ...)  std::fprintf(stderr, "[ui][INFO] " fmt "\n", ##__VA_ARGS__)
#  define HARROWMERE_LOG_WARN(cat, fmt, ...)  std::fprintf(stderr, "[ui][WARN] " fmt "\n", ##__VA_ARGS__)
#  define HARROWMERE_LOG_TRACE(cat, fmt, ...) /* trace compiled out by default */
#endif

#define HARROWMERE_LOG_CATEGORY ui

#include <chrono>

namespace harrowmere::ui {

HarrowmereUi::HarrowmereUi() = default;

HarrowmereUi::~HarrowmereUi() {
    // Defensive: stop() should have been called by the mod's plugin entry
    // (engine_query_mod_entry_table -> destroyMod). If it wasn't, the RAII
    // members (unique_ptr<EventSubscription>, ImGuiBackend) still release.
    if (backend_.ready()) {
        backend_.shutdown();
    }
}

void HarrowmereUi::construct(engine::ComponentContext& ctx) {
    // Acquire the three capabilities declared in mods/harrowmere_ui/mod.toml
    // (task 0.1): harrowmere (the family, resolved to harrowmere.core), input,
    // and renderer.vulkan.overlay. All three are *required* -- a missing one
    // fails startup with a precise ComponentAccessError (engine rule, common.md
    // §4.0). The harrowmere capability is the sim itself; the mod cannot run
    // without it.
    //
    // The template parameter identifies the capability: the type's static
    // `capabilityName` (e.g. SimCapability::capabilityName = "harrowmere.core")
    // is what the engine resolves against the active providers (common.md §4.0:
    // "acquireCapability<T>() checks the capability.<name> permission and pulls
    // the implementation pointer from the declaring provider").
    sim_     = ctx.acquireCapability<harrowmere::SimCapability>();
    input_   = ctx.acquireCapability<engine::InputCapability>();
    overlay_ = ctx.acquireCapability<engine::RendererVulkanOverlayCapability>();

    // Read the F1 keybind from the assembly config. The key is declared in
    // mods/harrowmere_ui/mod.toml's config_keys (task 0.1's shell extended
    // when this lands); a missing key falls back to GLFW_KEY_F1. The string
    // form ("F1") is parsed through the engine's known-keys table; an unknown
    // name logs a startup warning and uses F1.
    //
    // The wrapper from task 0.4 wraps the engine's typed reader here.
    const std::string keyName = ctx.configValueOrDefault<std::string>(
        "harrowmere_ui", "key", /*fallback=*/"F1");
    toggleKey_ = engine::parseGlfwKeyName(keyName, /*fallback=*/GLFW_KEY_F1);

    // Subscribe to EngineFrameEvent for the per-frame draw pass. The priority
    // is pinned *below* the scene bridge (7.2P1) so the world is rendered
    // before the overlay is composited on top -- the renderer's overlay pass
    // runs after the world pass by construction, and our subscription order
    // only affects *when we record* into the overlay command buffer, not the
    // GPU submission order. We use a low priority so any frame-setup system
    // (camera, sun) has updated state before we read SimCapability views.
    frameSub_ = ctx.eventBus().subscribe<engine::EngineFrameEvent>(
        [this](const engine::EngineFrameEvent& ev) { onFrameBegin(ev); },
        engine::SubscriptionPriority::Low);

    // Subscribe to KeyPressedEvent for F1 toggles. Polled movement keys
    // (WASD, Shift, mouse look) are read through InputCapability each frame
    // in 7.4P1 -- only discrete toggles use the event bus here (common.md
    // §4.0: input_glfw publishes KeyPressed/KeyReleased events; "in a native
    // mod you subscribe to the channel directly; there is no subscribe_key_
    // pressed (that name belongs to the script bridge)").
    keySub_ = ctx.eventBus().subscribe<engine::KeyPressedEvent>(
        [this](const engine::KeyPressedEvent& ev) { onKeyPressed(ev); },
        engine::SubscriptionPriority::Normal);
}

void HarrowmereUi::start() {
    // Nothing eager. The ImGui backend waits for the first *visible* frame:
    // ImGui init before the backend handle bundle completes fails (ImGui's
    // own example backend behavior, task 7.3P1 spec). HarrowmereUi::onFrameBegin
    // attempts init() once a frame, after the renderer's first non-iconified
    // present, and bails until then.
    HARROWMERE_LOG_INFO(HARROWMERE_LOG_CATEGORY, "harrowmere_ui 0.1.0 up (overlay pending first visible frame)");
}

void HarrowmereUi::stop() {
    // Mirror of construct in reverse. Subscriptions release first so no
    // in-flight dispatch lands on a tearing-down backend.
    keySub_.reset();
    frameSub_.reset();
    if (backend_.ready()) {
        backend_.shutdown();
    }
    backendInitAttempted_ = false;
    sim_.reset();
    input_.reset();
    overlay_.reset();
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

void HarrowmereUi::onFrameBegin(const engine::EngineFrameEvent& ev) {
    // --- (1) Lazy backend init on the first *visible* frame ---
    if (!backend_.ready()) {
        if (backendInitAttempted_) {
            // We already tried and failed this run; do not retry every frame
            // (it would spam the log and the failure is structural -- a
            // missing handle won't appear mid-run). The overlay stays dark.
            return;
        }
        // Skip when the renderer is not presenting (iconified, headless).
        // In a headless CI run, EngineFrameEvent does not fire at all
        // (no renderer), so this branch is dead code there -- but the guard
        // keeps a dev run with a minimized window from spamming init failures.
        if (!overlay_->isPresentingThisFrame()) {
            return;
        }
        backendInitAttempted_ = true;
        ImGuiBackendHandles handles{};
        std::string err;
        if (!collectOverlayHandles(handles, err)) {
            HARROWMERE_LOG_WARN(HARROWMERE_LOG_CATEGORY, "overlay handle bundle incomplete: %s", err.c_str());
            return;
        }
        if (!backend_.init(handles, err)) {
            HARROWMERE_LOG_WARN(HARROWMERE_LOG_CATEGORY, "ImGui backend init failed: %s", err.c_str());
            return;
        }
    }

    if (!visible_) {
        // Overlay hidden -- still tick the backend so wantsMouse/wantsKeyboard
        // return false cleanly and the next toggle draws a fresh frame. We
        // skip beginFrame/endFrame entirely (no ImGui::NewFrame -> no draw
        // data -> renderer's overlay command buffer records nothing).
        return;
    }

    // --- (2) FPS tracking (debug panel only; not a sim value) ---
    // Uses the engine's measured deltaSeconds (the heartbeat, common.md §4.0);
    // never reads wall clock inside sim rules (R8). The overlay is
    // presentation code, so wall-clock here is fine -- but we still use the
    // engine's measured delta to stay consistent with the rest of the frame.
    ++frameCount_;
    lastFpsTime_ += ev.deltaSeconds;
    if (lastFpsTime_ >= 0.5) {  // update twice per second
        fps_ = static_cast<int>(static_cast<double>(frameCount_) / lastFpsTime_);
        frameCount_ = 0;
        lastFpsTime_ = 0.0;
    }

    // --- (3) Draw pass ---
    backend_.beginFrame();

    // Pull the view snapshots -- rebuilt at step 13 each frame by the sim
    // (common.md §4.2/§4.4). A UI frame reads them between ticks. The
    // SimCapability views are *snapshots, not live references* -- ids and
    // copies only, no store handles (R10, common.md §4.5).
    const harrowmere::WorldView&    world    = sim_->world();
    const harrowmere::PlayerView&  player   = sim_->player();
    const harrowmere::JusticeView& justice  = sim_->justice();
    const harrowmere::EventsView&  events   = sim_->events();
    const harrowmere::UiSignals&    signals  = sim_->signals();

    // The debug panel. drawDebugPanel is forward-declared in ui_harrowmere.hpp
    // and defined in panels/debug.cpp. Taking the WorldView + other views by
    // const-ref here is the compile-time binding proof (test
    // TestUi_DebugPanelBindsWorldView).
    bool open = true;
    drawDebugPanel(world, player, justice, events, signals, fps_, &open);
    // open=false closes the panel via ImGui's close button; we treat that as
    // "hide overlay" (equivalent to pressing F1).
    if (!open) visible_ = false;

    backend_.endFrame();
}

void HarrowmereUi::onKeyPressed(const engine::KeyPressedEvent& ev) {
    if (ev.key == toggleKey_) {
        visible_ = !visible_;
        HARROWMERE_LOG_INFO(HARROWMERE_LOG_CATEGORY, "overlay %s", visible_ ? "shown" : "hidden");
    }
}

bool HarrowmereUi::collectOverlayHandles(ImGuiBackendHandles& out, std::string& errOut) const {
    errOut.clear();

    // The renderer.vulkan.overlay capability publishes typed getters; we copy
    // them into the void*-typed ImGuiBackendHandles struct so the wrapper TU
    // can stay platform-free (no <vulkan/vulkan.h> in imgui_backend.hpp).
    out.glfwWindow = input_->glfwWindowHandle();  // from window.glfw via the input capability
    if (out.glfwWindow == nullptr) {
        errOut = "glfwWindow handle null (window capability not presenting)";
        return false;
    }

    out.vkInstance       = overlay_->vkInstance();
    out.vkPhysicalDevice = overlay_->vkPhysicalDevice();
    out.vkDevice         = overlay_->vkDevice();
    out.queueFamily      = overlay_->graphicsQueueFamily();
    out.vkQueue          = overlay_->graphicsQueue();
    out.imageCount       = overlay_->swapchainImageCount();
    out.vkRenderPass     = overlay_->overlayRenderPass();
    out.minImageCount    = overlay_->minSwapchainImageCount();
    out.acquireOverlayCommandBuffer = []() -> void* {
        // The function pointer is wired through RendererVulkanOverlayCapability
        // (it caches the *current* frame's overlay command buffer; we call it
        // once per beginFrame through ImGuiBackend). We forward a trampoline
        // that the overlay capability's per-frame update populates.
        return engine::RendererVulkanOverlayCapability::currentOverlayCommandBuffer();
    };
    out.overlayDescriptorPool = overlay_->overlayDescriptorPool();

    if (out.vkInstance == nullptr || out.vkPhysicalDevice == nullptr ||
        out.vkDevice == nullptr || out.vkQueue == nullptr ||
        out.vkRenderPass == nullptr) {
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
