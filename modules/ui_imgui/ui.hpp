#ifndef ENGINE_MODULES_UI_UI_HPP
#define ENGINE_MODULES_UI_UI_HPP

#include "input_capability.hpp"
#include "logging_capability.hpp"
#include "persistence_capability.hpp"
#include "renderer_capability.hpp"
#include "renderer_overlay_capability.hpp"
#include "scene_capability.hpp"
#include "ui_capability.hpp"
#include "window_capability.hpp"
#include "engine/kernel/component_interface.hpp"
#include "engine/kernel/event_bus.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace engine
{

class InputCapability;
class LoggerCapability;
class PersistenceCapability;
class RendererCapability;
class RendererOverlayHostCapability;
class SceneCapability;
class WindowCapability;

// One row of the engine KeyCode -> UI library key table (imgui_backend.cpp).
// The key code int is the UI library's ImGuiKey value; int here keeps the
// ImGui headers out of the module header.
struct UiKeyMapping
{
    int imguiKey          = 0;
    std::uint32_t keyCode = 0;
};

// The mapping table, shared between the frame handler (ui.cpp) and the
// backend unit. Sorted by key code for readability; entries cover every key
// an immediate mode debug UI consumes.
const std::vector<UiKeyMapping> &uiKeyMap();

// Handoff for the Vulkan result guard (imgui_backend.cpp): once the backend
// is up, the UI library's CheckVkResultFn must not throw out of ImGui's
// per-frame calls - unwinding through ImGui internals terminates the run -
// so the guard logs and flips the module's backendFailed flag through these
// pointers instead. Both null during backend initialization: there throwing
// is the correct report, and ensureBackendInitialized's catch produces the
// one precise failure log.
void setVulkanResultGuardTargets(bool *backendFailedFlag, LoggerCapability *logger);

// Module "ui_imgui": the immediate mode debug overlay (family member "ui.imgui").
//
// The module sits on the renderer's overlay seam: it feeds the UI library the
// polled input state once per frame (FrameBeginEvent), builds the widget tree
// and lets the renderer's overlay recorder emit the draw commands into the
// frame after the composite pass. It owns no world state of its own - every
// panel row reads a capability it consumes (frame statistics from the UI
// library, the scene census from the registry, the camera pose from the
// renderer), so the overlay is a VIEW of the run, never a second source of
// truth.
//
// Input handling follows one modal rule (documented on UiCapability): while
// the overlay is visible it claims the pointer. The module drains the cursor
// and wheel accumulators of the input capability every visible frame - the
// scripting bridge additionally gates itself on wantsMouse(), so camera
// scripts stop receiving motion while a debug window is up - and forces the
// cursor capture off for the duration, restoring the previous capture state
// when the overlay hides again.
//
// Vulkan note: the module is UI library agnostic at the capability level, but
// this implementation speaks Vulkan directly for its backend initialization
// (font atlas upload, the pipeline with the swapchain's color format). That
// is why the plugin requires "renderer.overlay": the opaque backend handle
// bundle documented there is the ONLY renderer internals this module touches.
class UiModule final : public ModuleInterface, public UiCapability, public OverlayRecorder
{
public:
    UiModule();
    ~UiModule() override;

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;

    // UiCapability.
    bool overlayVisible() const override
    {
        return overlayVisible_;
    }

    void setOverlayVisible(bool visible) override;
    void toggleOverlay() override;
    bool wantsMouse() const override
    {
        return overlayVisible_;
    }

    // OverlayRecorder: the renderer calls this once per frame, after the
    // composite wrote the swapchain image. Emits the UI library's draw data
    // into the frame's command buffer; a no-op while the overlay is hidden.
    void recordOverlay(const OverlayFrameContext &context) override;

private:
    // Event handlers (the bus delivers on the main thread, in subscription
    // order relative to other FrameBeginEvent consumers).
    void handleFrameBegin(const FrameBeginEvent &event);
    void handleToggleKey(const KeyPressedEvent &event);
    void handleTextInput(const TextInputEvent &event);

    // One-time backend bring-up against the renderer's handle bundle. Runs
    // lazily on the first visible frame (the bundle is only complete after
    // the renderer initialized its device and swapchain). Returns false with
    // one precise log when the renderer cannot host the overlay.
    bool ensureBackendInitialized();

    // The text-input events queued since the last frame (the UI library
    // consumes characters through its io, not through key events).
    void flushQueuedTextInput();

    // The widget tree: one window, sections for frame statistics, the scene
    // census, the camera pose and the run actions (capture, save, load).
    void buildDebugWindow();

    // Tri-state optional capability resolution (the screenshot-capability
    // lesson from the scripting module): the persistence module may be
    // absent from a run; the save/load buttons then fail precisely per
    // press, not at startup.
    PersistenceCapability *ensurePersistenceCapability();

    ComponentManifest componentManifest;
    ComponentContext *boundContext               = nullptr;
    LoggerCapability *logger                     = nullptr;
    WindowCapability *windowCapability           = nullptr;
    InputCapability *inputCapability             = nullptr;
    RendererCapability *rendererCapability       = nullptr;
    RendererOverlayHostCapability *overlayHost   = nullptr;
    SceneCapability *sceneCapability             = nullptr;
    PersistenceCapability *persistenceCapability = nullptr;

    EventChannel<FrameBeginEvent> *frameBeginChannel = nullptr;
    EventChannel<KeyPressedEvent> *keyPressedChannel = nullptr;
    EventChannel<TextInputEvent> *textInputChannel   = nullptr;
    EventSubscription frameBeginSubscription;
    EventSubscription keyPressedSubscription;
    EventSubscription textInputSubscription;

    // Configuration ([ui] section, read once during construct).
    std::uint32_t toggleKeyCode = static_cast<std::uint32_t>(KeyCode::F1);
    float fontScale             = 1.0f;
    // Where the save button writes and the load button reads
    // ([ui].save_path; the default is the historical demo-package location).
    std::string savePath = "mods/test_scene/saves/overlay_save.toml";

    bool overlayVisible_    = false; // the capability's modal state
    bool cursorWasCaptured  = false; // restored when the overlay hides
    bool backendInitialized = false;
    bool backendFailed      = false; // decided once, one precise log
    bool warnedNoBackend    = false; // one warning for device-less runs

    // The main window identifier, resolved on the first visible frame and
    // reused afterwards: the window list is constant for the run (windows
    // are created at start, never re-created - the composition root's frame
    // loop relies on the same contract for its own cached identifier), and
    // windowIdentifiers() builds a fresh heap vector per call, which made
    // every visible overlay frame pay an allocation to read the same front()
    // value.
    WindowIdentifier mainWindowIdentifier = invalidWindowIdentifier;

    // Tri-state persistence resolution: 0 unresolved, 1 available, -1
    // decided unavailable (module absent or permission denied).
    int persistenceResolutionState    = 0;
    bool persistenceWarnedUnavailable = false;

    std::vector<std::uint32_t> pendingTextInput;
    std::int64_t lastFrameBeginTime = 0; // microseconds, for DeltaTime

    // Frame statistics window (the overlay's min/max/avg FPS row). Sampled in
    // handleFrameBegin on EVERY frame - visible or not - so the window stays
    // truthful across overlay toggles: a hidden stretch still happened at
    // whatever pace the frame loop ran, and the first visible frame after a
    // toggle must not inherit a stale timestamp. The ring holds the last
    // frameTimeWindow frame intervals (seconds); min/max/avg derive from it
    // at display time, so no incrementally-updated state can drift from the
    // samples it summarizes.
    static constexpr std::size_t frameTimeWindow = 240;
    std::array<float, frameTimeWindow> frameTimeSamples{};
    std::size_t frameTimeCursor    = 0;
    std::uint32_t frameTimeCount   = 0;
    std::int64_t lastStatFrameTime = 0; // microseconds, statistics clock
};

} // namespace engine

#endif // ENGINE_MODULES_UI_UI_HPP
