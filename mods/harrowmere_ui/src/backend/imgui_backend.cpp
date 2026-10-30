// imgui_backend.cpp -- ImGui GLFW+Vulkan backend wrapper.
//
// Phase-1 slice of task 7.3 (card 7.3P1).
//
// This file mirrors ImGui's own example backend (backends/imgui_impl_glfw.{h,cpp}
// and backends/imgui_impl_vulkan.{h,cpp}) shipped under third_party/imgui at
// the pinned tag. We do NOT reimplement those; we wrap their init/shutdown/
// new-frame/render calls so harrowmere_ui owns the widget tree (D-UI-1) without
// re-rolling platform code.
//
// Includes (third_party/imgui, pinned tag, added to the include path by
// harrowmere_ui's CMakeLists -- task 0.1's stub is replaced when this lands):
//   - imgui.h                       (core)
//   - backends/imgui_impl_glfw.h    (GLFW platform backend)
//   - backends/imgui_impl_vulkan.h  (Vulkan renderer backend)
// The GLFW/Vulkan headers come transitively through the engine capability
// headers (window_glfw, renderer_vulkan) -- this TU does not include them
// directly, on purpose, so the wrapper header stays platform-free.

#include "imgui_backend.hpp"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

// Logging shim.
//
// The HARROWMERE_LOG_* macros are defined in mods/harrowmere_core/src/core/
// log.hpp (task 0.5) as a wrapper over the engine's `logging` capability.
// That file is internal to harrowmere_core -- including it from harrowmere_ui
// would violate the 7.3P1 Done-when ("No file in mods/harrowmere_ui/ includes
// harrowmere_core internals -- only harrowmere_capability.hpp"). Two options:
//
//   (a) harrowmere_ui ships its own mods/harrowmere_ui/src/core/log.hpp that
//       wraps the engine's `logging` capability (the cleanest separation;
//       out of scope for this card's Files section).
//   (b) Include the macros here with a #ifndef fallback that uses stderr
//       when the project's macros are not yet on the include path.
//
// We take (b): if the integrator exposes harrowmere's log.hpp through a
// public include path (or copies it into harrowmere_ui/src/core/), the
// project macros are used and the `ui` category is honored. Otherwise, the
// fallback keeps the build green and the messages land on stderr. The
// integrator should wire (a) before the Phase-1 exit checklist.
#ifndef HARROWMERE_LOG_INFO
#  include <cstdio>
#  define HARROWMERE_LOG_INFO(cat, fmt, ...)  std::fprintf(stderr, "[ui][INFO] " fmt "\n", ##__VA_ARGS__)
#  define HARROWMERE_LOG_WARN(cat, fmt, ...)  std::fprintf(stderr, "[ui][WARN] " fmt "\n", ##__VA_ARGS__)
#  define HARROWMERE_LOG_TRACE(cat, fmt, ...) /* trace compiled out by default (HARROWMERE_TRACE gates it) */
#endif

#define HARROWMERE_LOG_CATEGORY ui

#include <utility>

namespace harrowmere::ui {

struct ImGuiBackend::Impl {
    bool initialized = false;
    ImGuiBackendHandles handles{};

    // Cached ImGui state for wantsMouse/wantsKeyboard between frames. ImGui
    // updates these inside ImGui::NewFrame (called from beginFrame), and the
    // values are valid until the next beginFrame.
    bool wantsMouseLastFrame = false;
    bool wantsKeyboardLastFrame = false;
};

ImGuiBackend::ImGuiBackend() : impl_(std::make_unique<Impl>()) {}
ImGuiBackend::~ImGuiBackend() { shutdown(); }

ImGuiBackend::ImGuiBackend(ImGuiBackend&&) noexcept = default;
ImGuiBackend& ImGuiBackend::operator=(ImGuiBackend&&) noexcept = default;

bool ImGuiBackend::init(const ImGuiBackendHandles& handles, std::string& errOut) {
    errOut.clear();

    // Idempotent: a second successful init is a logged no-op (test
    // TestUi_SeamRegisteredOnce pins this). The renderer.vulkan.overlay seam
    // is registered once on the first visible frame; HarrowmereUi's frame
    // handler may call init() defensively on every frame until ready().
    if (impl_->initialized) {
        HARROWMERE_LOG_INFO(HARROWMERE_LOG_CATEGORY,
                            "ImGui backend already initialized; seam registration is a no-op");
        return true;
    }

    // Validate the handle bundle. ImGui's own example backend checks these
    // too; we surface the failure up front with a precise message rather than
    // letting ImGui abort inside ImGui_ImplVulkan_Init.
    if (handles.glfwWindow == nullptr) {
        errOut = "renderer.vulkan.overlay handle bundle missing glfwWindow (headless run?)";
        return false;
    }
    if (handles.vkInstance == nullptr || handles.vkPhysicalDevice == nullptr ||
        handles.vkDevice == nullptr || handles.vkQueue == nullptr ||
        handles.vkRenderPass == nullptr) {
        errOut = "renderer.vulkan.overlay handle bundle missing a Vulkan handle";
        return false;
    }
    if (handles.queueFamily == ~0u) {
        errOut = "renderer.vulkan.overlay handle bundle missing queueFamily";
        return false;
    }
    if (handles.imageCount == 0) {
        errOut = "renderer.vulkan.overlay handle bundle missing imageCount";
        return false;
    }
    if (handles.acquireOverlayCommandBuffer == nullptr) {
        errOut = "renderer.vulkan.overlay handle bundle missing overlay command-buffer getter";
        return false;
    }

    // 1. ImGui context. We own it; HarrowmereUi::stop -> shutdown destroys it.
    IMGUI_CHECKVERSION();
    if (ImGui::GetCurrentContext() == nullptr) {
        ImGui::CreateContext();
    }
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // F1 toggles overlay; arrows navigate
    io.IniFilename = nullptr;  // we persist options through the engine's settings.toml (7.6), not ImGui's ini

    // 2. GLFW platform backend. ImGui_ImplGlfw_InitForVulkan is the canonical
    //    entry from the example backend; it installs callbacks on the GLFW
    //    window (which we are responsible for chaining through -- the engine's
    //    input_glfw module owns the originals, and ImGui's install can
    //    replace them; HarrowmereUi is responsible for not stomping the
    //    engine's input handling during its non-overlay phases, which is why
    //    install_callback=false is used and HarrowmereUi polls the engine's
    //    InputCapability separately for movement in 7.4P1).
    if (!ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(handles.glfwWindow), false)) {
        errOut = "ImGui_ImplGlfw_InitForVulkan failed";
        ImGui::DestroyContext();
        return false;
    }

    // 3. Vulkan renderer backend. The init info mirrors the example backend's
    //    ImGui_ImplVulkan_InitInfo struct; we cast our void* handles back to
    //    the typed Vulkan handles here (imgui_impl_vulkan.h pulled in the
    //    Vulkan headers, so the casts are well-typed).
    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = static_cast<VkInstance>(handles.vkInstance);
    info.PhysicalDevice = static_cast<VkPhysicalDevice>(handles.vkPhysicalDevice);
    info.Device = static_cast<VkDevice>(handles.vkDevice);
    info.QueueFamily = handles.queueFamily;
    info.Queue = static_cast<VkQueue>(handles.vkQueue);
    info.PipelineCache = nullptr;       // optional; not exposed by the overlay seam in v1
    info.DescriptorPool = static_cast<VkDescriptorPool>(handles.overlayDescriptorPool);
    info.Subpass = 0;                   // overlay is its own subpass on the renderer's overlay pass
    info.MinImageCount = handles.minImageCount;
    info.ImageCount = handles.imageCount;
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;  // overlay is sampled at 1x; the renderer resolves
    info.Allocator = nullptr;
    info.CheckVkResultFn = [](VkResult r) {
        if (r != VK_SUCCESS) {
            HARROWMERE_LOG_WARN(HARROWMERE_LOG_CATEGORY, "ImGui Vulkan backend vkResult=%d", static_cast<int>(r));
        }
    };

    if (!ImGui_ImplVulkan_Init(&info, static_cast<VkRenderPass>(handles.vkRenderPass))) {
        errOut = "ImGui_ImplVulkan_Init failed";
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    impl_->handles = handles;
    impl_->initialized = true;
    HARROWMERE_LOG_INFO(HARROWMERE_LOG_CATEGORY, "ImGui backend initialized (GLFW+Vulkan seam registered)");
    return true;
}

bool ImGuiBackend::ready() const noexcept { return impl_ && impl_->initialized; }

void ImGuiBackend::beginFrame() {
    if (!impl_->initialized) return;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiBackend::endFrame() {
    if (!impl_->initialized) return;
    ImGui::Render();
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(impl_->handles.acquireOverlayCommandBuffer());
    if (cmd != nullptr) {
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    } else {
        // No overlay command buffer for this frame -- the renderer skipped the
        // overlay slot (e.g. minimized window). The draw data is leaked this
        // frame, which ImGui tolerates; next frame's NewFrame clears it.
        HARROWMERE_LOG_TRACE(HARROWMERE_LOG_CATEGORY, "overlay command buffer null this frame; draw skipped");
    }
    // Cache wants-input state -- valid until the next NewFrame.
    const ImGuiIO& io = ImGui::GetIO();
    impl_->wantsMouseLastFrame = io.WantCaptureMouse;
    impl_->wantsKeyboardLastFrame = io.WantCaptureKeyboard;
}

void ImGuiBackend::shutdown() {
    if (!impl_) return;
    if (impl_->initialized) {
        // Mirror the init order in reverse. ImGui_ImplVulkan_Shutdown
        // destroys internal pipelines; ImGui_ImplGlfw_Shutdown unhooks window
        // callbacks; DestroyContext frees the context.
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        if (ImGui::GetCurrentContext() != nullptr) {
            ImGui::DestroyContext();
        }
        impl_->initialized = false;
        impl_->handles = {};
        impl_->wantsMouseLastFrame = false;
        impl_->wantsKeyboardLastFrame = false;
        HARROWMERE_LOG_INFO(HARROWMERE_LOG_CATEGORY, "ImGui backend shut down");
    }
}

bool ImGuiBackend::wantsMouse() const noexcept {
    // Return the cached value from the last endFrame; before the first frame
    // returns false (no panel has focus yet).
    return impl_ && impl_->initialized && impl_->wantsMouseLastFrame;
}

bool ImGuiBackend::wantsKeyboard() const noexcept {
    return impl_ && impl_->initialized && impl_->wantsKeyboardLastFrame;
}

}  // namespace harrowmere::ui
