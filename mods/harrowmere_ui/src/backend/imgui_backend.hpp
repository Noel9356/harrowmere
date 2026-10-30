// imgui_backend.hpp -- ImGui GLFW+Vulkan backend wrapper owned by harrowmere_ui.
//
// Phase-1 slice of task 7.3 (card 7.3P1).
//
// Why this file exists (D-UI-1, common.md §4.1/§4.0):
//   harrowmere_ui *provides* ui.harrowmere and consumes renderer.vulkan.overlay
//   directly. The engine's ui_imgui module is restored at build per §3.6's
//   build-edge rule but is *runtime-inert* -- it is in the resolved graph only
//   because its published ui_capability.hpp header is included by other kept
//   modules (input_glfw, renderer_vulkan); no active component demands the
//   `ui` capability, so the boot test's `ui_imgui`-absent assertion still holds
//   and the engine never draws an overlay through it. The widget tree --
//   including this debug panel -- belongs to harrowmere_ui. We therefore mirror
//   (do NOT link) ImGui's own GLFW+Vulkan example backend shipped in
//   third_party/imgui, wrapping it in a thin class HarrowmereUi owns.
//
// Lifecycle (task 7.3P1 spec):
//   1. HarrowmereUi::construct acquires renderer.vulkan.overlay, input, and the
//      harrowmere (harrowmere.core) capability.
//   2. On the first *visible* frame (renderer present AND window not iconified),
//      HarrowmereUi calls ImGuiBackend::init. ImGui init before the backend
//      handle bundle completes fails -- registering on the first visible frame
//      mirrors ImGui's own example backend.
//   3. Each frame HarrowmereUi::onFrameBegin calls beginFrame() before any
//      ImGui:: call, draws its panels, then calls endFrame() to record the
//      draw data into the overlay command buffer the renderer executes after
//      the world pass.
//   4. HarrowmereUi::stop calls shutdown().
//
// All Vulkan/GLFW handles come from the engine's published capabilities; this
// wrapper owns only the ImGui context and the backend bindings. No direct
// glfwCreateWindow or vkCreate* calls happen here.
//
// Conventions: snake_case file, PascalCase type, camelCase function (common.md
// §3.3). The harrowmere_ prefix is reserved for game modules; this namespace is
// `harrowmere::ui` so a future third-party native mod consuming ui.harrowmere
// can never collide.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace harrowmere::ui {

// Bundle of engine handles pulled through the renderer.vulkan.overlay and
// window/input capabilities. HarrowmereUi is responsible for acquiring them and
// passing them in; this wrapper never queries the engine itself.
//
// Every Vulkan/GLFW pointer is opaque (void*) on purpose: the engine's
// capability headers expose typed handles, but we keep this struct
// platform-free (no <vulkan/vulkan.h> include) so the wrapper header can be
// included from any TU -- including the test TU that compiles against
// harrowmere_capability.hpp alone. The cpp translates the void* back to the
// typed Vulkan handles inside imgui_impl_vulkan.h's init struct.
struct ImGuiBackendHandles {
    // From the window capability (window.glfw). Null in a headless run --
    // HarrowmereUi must not call init() when this is null (the overlay is
    // renderer-coupled by design; common.md §4.4 step 13 runs per FRAME on
    // EngineFrameEvent, which does not fire without a renderer).
    void* glfwWindow = nullptr;          // GLFWwindow*

    // From the renderer.vulkan.overlay capability.
    void* vkInstance       = nullptr;     // VkInstance
    void* vkPhysicalDevice = nullptr;     // VkPhysicalDevice
    void* vkDevice         = nullptr;     // VkDevice
    std::uint32_t queueFamily = ~0u;       // graphics queue family index
    void* vkQueue          = nullptr;      // VkQueue
    std::uint32_t imageCount   = 0;       // swapchain image count
    void* vkRenderPass     = nullptr;      // VkRenderPass -- the overlay pass
    std::uint32_t minImageCount = 2;       // swapchain min image count

    // The overlay command buffer + descriptor pool come from the renderer's
    // per-frame overlay slot. The renderer hands HarrowmereUi a function
    // pointer that returns the command buffer for the *current* frame (so we
    // never cache a stale one across frame boundaries); HarrowmereUi forwards
    // it here so beginFrame can hand it to ImGui's Vulkan backend via
    // ImGui_ImplVulkan_NewFrame's per-frame descriptor set plumbing.
    void* (*acquireOverlayCommandBuffer)() = nullptr;
    void* overlayDescriptorPool = nullptr;  // VkDescriptorPool
};

// Thin wrapper around ImGui's GLFW + Vulkan backends. One instance per
// HarrowmereUi component.
//
// Thread-safety: single-threaded by contract -- the engine's event bus is
// synchronous, single-threaded (common.md §4.0), and the overlay draws on the
// main thread. No locking here.
class ImGuiBackend {
public:
    ImGuiBackend();
    ~ImGuiBackend();

    ImGuiBackend(const ImGuiBackend&) = delete;
    ImGuiBackend& operator=(const ImGuiBackend&) = delete;
    ImGuiBackend(ImGuiBackend&&) noexcept;
    ImGuiBackend& operator=(ImGuiBackend&&) noexcept;

    // Initializes the ImGui context, the GLFW platform backend, and the Vulkan
    // rendering backend using handles in `handles`.
    //
    // Idempotent: a second successful call is a logged no-op (test
    // TestUi_SeamRegisteredOnce pins this -- registering the seam twice must
    // not double-init ImGui, which would leak the prior context). A call after
    // a failed init retries (the failed init left no live state).
    //
    // Returns false if a required handle is missing or initialization fails;
    // the caller must treat the overlay as unavailable for this frame and
    // try again next visible frame. `errOut` receives a one-line diagnostic
    // (it is cleared on success).
    bool init(const ImGuiBackendHandles& handles, std::string& errOut);

    // True once init() has succeeded and shutdown() has not been called.
    // HarrowmereUi gates beginFrame()/endFrame() on this.
    bool ready() const noexcept;

    // Begins a new ImGui frame. Must be paired with endFrame().
    //
    // Calls ImGui_ImplVulkan_NewFrame + ImGui_ImplGlfw_NewFrame +
    // ImGui::NewFrame. The renderer's overlay command buffer for this frame
    // is fetched through handles.acquireOverlayCommandBuffer (cached at
    // init(), resolved per-frame through the function pointer).
    void beginFrame();

    // Ends the ImGui frame, recording draw data into the overlay command
    // buffer the renderer executes after the world pass.
    //
    // Calls ImGui::Render + ImGui_ImplVulkan_RenderDrawData with the per-frame
    // command buffer. The renderer is responsible for submitting it; we only
    // record.
    void endFrame();

    // Tears down the Vulkan backend, the GLFW backend, and the ImGui context
    // in that order (mirror of init's reverse). Safe to call on an un-
    // initialized backend (no-op). Safe to call twice (second is a no-op).
    void shutdown();

    // True if ImGui wants mouse input this frame (the overlay is interactive
    // AND a window inside it has focus). The input mapper (7.4P1) gates
    // camera look on this so the overlay never steals the cursor mid-look.
    // Returns false before the first beginFrame/endFrame pair of the run.
    bool wantsMouse() const noexcept;

    // True if ImGui wants keyboard input this frame. Gated identically to
    // wantsMouse. Used by 7.4P1 to suppress movement intents while the player
    // is typing in an ImGui input field (Phase-2 panels will need this).
    bool wantsKeyboard() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace harrowmere::ui
