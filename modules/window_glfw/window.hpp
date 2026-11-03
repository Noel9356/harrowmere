#ifndef ENGINE_MODULES_WINDOW_HPP
#define ENGINE_MODULES_WINDOW_HPP

#include "logging_capability.hpp"
#include "window_capability.hpp"
#include "engine/kernel/component_interface.hpp"

#include <map>
#include <stdexcept>
#include <string>

struct GLFWwindow;

namespace engine
{

// The window's display mode ([window].fullscreen; Off is the default and
// the historical windowed behavior). Applied after window creation so the
// normal framebuffer-resize event reports it to the renderer.
enum class WindowDisplayMode
{
    Off,        // windowed, decorations on (the historical default)
    Borderless, // decoration-less window filling the chosen monitor
    Exclusive   // a real video-mode switch on the chosen monitor
};

// Module "window_glfw": provides the WindowCapability on top of GLFW. Owns the GLFW
// lifecycle, the main window, the platform event pump and the Vulkan surface
// creation entry point. The renderer and the input module reach GLFW through
// this capability instead of initializing anything themselves.
class WindowModule final : public ModuleInterface, public WindowCapability
{
public:
    WindowModule();
    ~WindowModule() override;

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;

    WindowIdentifier createMainWindow(const WindowDescription &description) override;
    bool windowCloseRequested(WindowIdentifier windowIdentifier) override;
    void pollEvents() override;
    std::uint32_t framebufferWidth(WindowIdentifier windowIdentifier) override;
    std::uint32_t framebufferHeight(WindowIdentifier windowIdentifier) override;
    const std::vector<WindowIdentifier> &windowIdentifiers() const override;
    std::vector<std::string> requiredInstanceExtensions(const std::string &graphicsApi) override;
    void *createPresentationSurface(
        const std::string &graphicsApi, void *graphicsInstance, WindowIdentifier windowIdentifier
    ) override;
    void *nativeWindowHandle(WindowIdentifier windowIdentifier) override;
    WindowIdentifier windowIdentifierFromNativeHandle(void *nativeWindowHandle) override;

private:
    struct WindowRecord
    {
        WindowIdentifier identifier = invalidWindowIdentifier;
        GLFWwindow *window          = nullptr;
        bool closeReported          = false;
        // Framebuffer size, cached at creation and maintained by the resize
        // callback: framebufferWidth/Height are polled per frame (UI, script
        // bridges), and each poll used to be a GLFW call pair.
        std::uint32_t cachedFramebufferWidth  = 0;
        std::uint32_t cachedFramebufferHeight = 0;
    };

    WindowRecord *findRecord(WindowIdentifier windowIdentifier);
    void installCallbacks(GLFWwindow *window);
    // Applies fullscreenMode_ to the window (called from createMainWindow;
    // requires glfwInit). Warns and stays windowed without a usable monitor.
    void applyDisplayMode(GLFWwindow *window);

    static void framebufferResizeCallback(GLFWwindow *window, int width, int height);
    static void errorCallback(int errorCode, const char *description);

    ComponentManifest componentManifest;
    ComponentContext *boundContext = nullptr;
    LoggerCapability *logger       = nullptr;
    // Publish channels cached at construct (see construct() for why): the
    // bursty resize/close publishes run without a per-event type_index
    // lookup. Nulled at stop like the other context-bound state.
    EventChannel<WindowClosedEvent> *windowClosedChannel   = nullptr;
    EventChannel<WindowResizedEvent> *windowResizedChannel = nullptr;
    WindowDescription pendingDescription;
    // [window].fullscreen / [window].monitor: the display mode applied after
    // the main window is created (Off = the historical windowed default).
    WindowDisplayMode fullscreenMode_ = WindowDisplayMode::Off;
    std::uint32_t monitorIndex_       = 0;
    bool glfwInitialized              = false;
    std::map<WindowIdentifier, WindowRecord> windows;
    WindowIdentifier nextWindowIdentifier = 1;
    // The identifier list handed out per frame (input polls, UI display
    // size, script bridges), cached and rebuilt only when a window is
    // created - windows are never removed mid-run (close requests end the
    // run; teardown destroys the module), so the cache cannot go stale.
    mutable std::vector<WindowIdentifier> cachedIdentifiers;
    mutable bool identifiersCacheDirty = true;
};

} // namespace engine

#endif // ENGINE_MODULES_WINDOW_HPP
