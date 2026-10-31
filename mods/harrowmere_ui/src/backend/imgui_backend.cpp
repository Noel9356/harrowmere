// imgui_backend.cpp -- ImGui GLFW+Vulkan backend wrapper.
//
// Phase-1 slice of task 7.3 (card 7.3P1).
//
// This file mirrors ImGui's own example backend (backends/imgui_impl_glfw.{h,cpp}
// and backends/imgui_impl_vulkan.{h,cpp}) shipped under third_party/imgui at
// the pinned tag (v1.91.9b, the engine's own imgui pin). We do NOT reimplement
// those; we wrap their init/shutdown/new-frame/render calls so harrowmere_ui
// owns the widget tree (D-UI-1) without re-rolling platform code.
//
// The Vulkan handles arrive as opaque void* (see imgui_backend.hpp for why);
// this TU casts them back through imgui_impl_vulkan.h's own types -- the
// header pulls the Vulkan headers the engine's engine_vulkan_hpp_bindings
// target publishes (the pinned Khronos Vulkan-Headers fetch), so the casts
// are well-typed and the TU never includes <vulkan/vulkan.h> itself.

#include "imgui_backend.hpp"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

// Logging shim.
//
// The HARROWMERE_LOG_* macros live in harrowmere_core's src/core/log.hpp --
// an internal header the UI mod must NOT include (the 7.3P1 Done-when: "No
// file in mods/harrowmere_ui/ includes harrowmere_core internals -- only
// harrowmere_capability.hpp"). This file is not a sim component either: it
// logs to stderr (the engine's own logging module still carries the
// lifecycle lines through HarrowmereUi's start/stop diagnostics).
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
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED; // the dynamic-rendering color format (Init reads it)

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
    // is registered once on the first visible frame; HarrowmereUi's recorder
    // registration may call init() defensively until ready().
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
        handles.vkDevice == nullptr || handles.vkQueue == nullptr) {
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
    if (handles.colorFormat == 0) {
        errOut = "renderer.vulkan.overlay handle bundle missing the swapchain color format";
        return false;
    }

    // 1. ImGui context. We own it; HarrowmereUi::stop -> shutdown destroys it.
    IMGUI_CHECKVERSION();
    if (ImGui::GetCurrentContext() == nullptr) {
        ImGui::CreateContext();
    }
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // arrows navigate; F1 stays a bus toggle
    io.IniFilename = nullptr;  // we persist options through the engine's settings.toml (7.6), not ImGui's ini

    // 2. GLFW platform backend. ImGui_ImplGlfw_InitForVulkan is the canonical
    //    entry from the example backend; install_callbacks=false keeps the
    //    engine's input_glfw module as the sole owner of the window callbacks
    //    (7.4P1 polls the InputCapability; ImGui's io reads the polled state
    //    through the same GLFW window state, and the overlay claims the
    //    cursor through io.WantCaptureMouse, not through a second callback).
    if (!ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(handles.glfwWindow), false)) {
        errOut = "ImGui_ImplGlfw_InitForVulkan failed";
        ImGui::DestroyContext();
        return false;
    }

    // 3. Vulkan renderer backend (imgui 1.91.9b signature: one InitInfo*).
    //    The renderer's overlay path records through Vulkan DYNAMIC RENDERING
    //    into the swapchain image (renderer_frames.cpp's composite pass), so
    //    the backend initializes for dynamic rendering: UseDynamicRendering +
    //    the swapchain's color format (the seam's bundle carries it as a
    //    plain integer per its contract), and the backend creates its own
    //    descriptor pool (DescriptorPoolSize) -- the seam exposes none.
    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = static_cast<VkInstance>(handles.vkInstance);
    info.PhysicalDevice = static_cast<VkPhysicalDevice>(handles.vkPhysicalDevice);
    info.Device = static_cast<VkDevice>(handles.vkDevice);
    info.QueueFamily = handles.queueFamily;
    info.Queue = static_cast<VkQueue>(handles.vkQueue);
    info.PipelineCache = nullptr;       // optional; not exposed by the overlay seam in v1
    info.DescriptorPool = nullptr;      // the backend creates its own pool
    info.DescriptorPoolSize = 16;       // font atlas + panel textures, well inside one device pool
    info.UseDynamicRendering = true;
    // imgui 1.91.9b carries the dynamic-rendering state in
    // PipelineRenderingCreateInfo (one color attachment: the swapchain
    // format the bundle carries as a plain integer). The format value lives
    // in the Impl so the pointer stays valid for the backend's pipeline
    // creation inside Init. The sType MUST be set explicitly: the zero-init
    // struct's sType 0 is VK_STRUCTURE_TYPE_APPLICATION_INFO, and a pNext
    // chain that leads with it makes the pipeline's rendering state
    // unreadable - the backend then creates its pipeline with an empty
    // colorAttachmentCount (undefined attachment format on every driver; the
    // validation layer names the missing pNext chain outright).
    impl_->swapchainFormat = static_cast<VkFormat>(handles.colorFormat);
    info.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &impl_->swapchainFormat;
    info.Subpass = 0;
    info.MinImageCount = handles.minImageCount;
    info.ImageCount = handles.imageCount;
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;  // overlay is sampled at 1x; the renderer resolves
    info.Allocator = nullptr;
    info.CheckVkResultFn = [](VkResult r) {
        if (r != VK_SUCCESS) {
            HARROWMERE_LOG_WARN(HARROWMERE_LOG_CATEGORY, "ImGui Vulkan backend vkResult=%d", static_cast<int>(r));
        }
    };

    if (!ImGui_ImplVulkan_Init(&info)) {
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

void ImGuiBackend::endFrame(void *commandBuffer) {
    if (!impl_->initialized) return;
    ImGui::Render();
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(commandBuffer);
    if (cmd != nullptr) {
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    } else {
        // No overlay command buffer for this frame -- the renderer skipped the
        // overlay slot (e.g. minimized window). The draw data is dropped this
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
