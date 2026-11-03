#include "window.hpp"

#include <vulkan/vulkan.h> // window module implementation detail, not part of the window contract
#include <GLFW/glfw3.h>

namespace engine
{

namespace
{

// GLFW callbacks carry no user data for the error path; the active module is
// tracked here for the duration of start() .. stop().
WindowModule *activeWindowModule = nullptr;

std::string describeGlfwError(
    int errorCode
)
{
    switch (errorCode)
    {
    case GLFW_NOT_INITIALIZED:
        return "GLFW_NOT_INITIALIZED";
    case GLFW_NO_CURRENT_CONTEXT:
        return "GLFW_NO_CURRENT_CONTEXT";
    case GLFW_INVALID_ENUM:
        return "GLFW_INVALID_ENUM";
    case GLFW_INVALID_VALUE:
        return "GLFW_INVALID_VALUE";
    case GLFW_OUT_OF_MEMORY:
        return "GLFW_OUT_OF_MEMORY";
    case GLFW_API_UNAVAILABLE:
        return "GLFW_API_UNAVAILABLE";
    case GLFW_VERSION_UNAVAILABLE:
        return "GLFW_VERSION_UNAVAILABLE";
    case GLFW_PLATFORM_ERROR:
        return "GLFW_PLATFORM_ERROR";
    case GLFW_FORMAT_UNAVAILABLE:
        return "GLFW_FORMAT_UNAVAILABLE";
    default:
        return "GLFW_ERROR_" + std::to_string(errorCode);
    }
}

} // namespace

WindowModule::WindowModule()
{
    componentManifest = ComponentManifest{
        .name    = "window",
        .kind    = ComponentKind::Module,
        .version = "0.1.0",
        // The implementation-bound member name: this module is the GLFW
        // implementation of the "window" family. Consumers require the family;
        // a mod that needs this backend names the member exactly.
        .providedCapabilities = {"window.glfw"},
        .requiredCapabilities = {"logging"},
        .optionalCapabilities = {},
        .requiredComponents   = {},
        .requiredPermissions  = {"capability.logging"},
        .knownConfigKeys
        = {"window.title", "window.width", "window.height", "window.resizable", "window.fullscreen", "window.monitor"},
    };
}

WindowModule::~WindowModule()
{
    stop();
}

void WindowModule::construct(
    ComponentContext &context
)
{
    boundContext = &context;
    logger       = &context.acquireCapability<LoggerCapability>();

    // Cache the publish channels once (the bus anchors channels on the heap,
    // so the references outlive construct): the resize/close publishes used
    // to go through EventBus::publish<T>, which re-hashes the event's
    // type_index per event. Resizes arrive in bursts during drag and
    // fullscreen transitions - each now publishes with zero lookups, like
    // every cached-channel module.
    EventBus &bus        = context.eventBus();
    windowClosedChannel  = &bus.channel<WindowClosedEvent>();
    windowResizedChannel = &bus.channel<WindowResizedEvent>();

    const std::string title = context.configurationString("title").value_or("engine demo");
    const auto width        = context.configurationInteger("width").value_or(1280);
    const auto height       = context.configurationInteger("height").value_or(720);
    const bool resizable    = context.configurationBoolean("resizable").value_or(true);

    // The description carries unsigned sizes and GLFW takes ints: a negative
    // config value would wrap static_cast into a huge unsigned size and zero
    // would create a degenerate window. Clamp to the sane [1, 16384] span at
    // the boundary (like monitor_index above) instead of wrapping.
    const std::int64_t clampedWidth  = std::clamp<std::int64_t>(width, 1, 16384);
    const std::int64_t clampedHeight = std::clamp<std::int64_t>(height, 1, 16384);
    if ((clampedWidth != width || clampedHeight != height) && logger != nullptr)
    {
        logger->logMessage(
            LogLevel::Warning,
            "window module: [window] width/height clamped to 1..16384 (got " + std::to_string(width) + "x"
                + std::to_string(height) + ")"
        );
    }

    // Display mode: "off" (the default, the historical windowed behavior),
    // "borderless" (a decoration-less window filling the chosen monitor),
    // or "exclusive" (a real video mode switch). [window].monitor selects
    // the monitor (0 = the primary). Parsed here so a typo fails startup
    // with a precise diagnostic instead of surprising at first frame.
    const std::string fullscreenModeName = context.configurationString("fullscreen").value_or("off");
    WindowDisplayMode fullscreenMode     = WindowDisplayMode::Off;
    if (fullscreenModeName == "off")
    {
        fullscreenMode = WindowDisplayMode::Off;
    } else if (fullscreenModeName == "borderless")
    {
        fullscreenMode = WindowDisplayMode::Borderless;
    } else if (fullscreenModeName == "exclusive")
    {
        fullscreenMode = WindowDisplayMode::Exclusive;
    } else
    {
        throw std::runtime_error(
            "window module: [window] fullscreen '" + fullscreenModeName
            + "' is not a known mode (off, borderless, exclusive)"
        );
    }
    const auto monitorIndex = std::clamp<std::int64_t>(context.configurationInteger("monitor").value_or(0), 0, 7);

    // The main window is created during start so that GLFW initialization
    // happens inside the start phase, never during dry validation.
    pendingDescription.title     = title;
    pendingDescription.width     = static_cast<std::uint32_t>(clampedWidth);
    pendingDescription.height    = static_cast<std::uint32_t>(clampedHeight);
    pendingDescription.resizable = resizable;
    fullscreenMode_              = fullscreenMode;
    monitorIndex_                = static_cast<std::uint32_t>(monitorIndex);
}

void WindowModule::start()
{
    if (!glfwInit())
    {
        // Headless degrade (the game assembly's pinned CI shape, common.md
        // section 3.6 "headless degrades window/renderer, never input"): a
        // display-less machine cannot initialize GLFW, but the run must keep
        // ticking - the app's frame loop stays in the windowless pump and
        // every WindowCapability method below guards on glfwInitialized.
        // Failing the whole startup here would make the pinned headless
        // smoke run (--set engine.quit_after_seconds=1.0, exit 0) impossible.
        // A display-capable environment is unaffected: glfwInit succeeds and
        // the main window is created right below, exactly as before.
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning, "window module: no usable display - running windowless (headless degrade)"
            );
        }
        return;
    }
    glfwInitialized    = true;
    activeWindowModule = this;
    glfwSetErrorCallback(&WindowModule::errorCallback);

    createMainWindow(pendingDescription);
    if (logger != nullptr)
    {
        logger->logMessage(LogLevel::Information, "window module started");
    }
}

void WindowModule::stop()
{
    for (auto &entry : windows)
    {
        if (entry.second.window != nullptr)
        {
            glfwDestroyWindow(entry.second.window);
            entry.second.window = nullptr;
        }
    }
    windows.clear();
    // The identifier cache must not hand out identifiers of destroyed
    // windows if the module is restarted.
    cachedIdentifiers.clear();
    identifiersCacheDirty = true;

    if (glfwInitialized)
    {
        glfwTerminate();
        glfwInitialized = false;
    }
    // Release the cached publish channels with the rest of the context-bound
    // state (the channels die with the bus during kernel teardown).
    windowClosedChannel  = nullptr;
    windowResizedChannel = nullptr;
    activeWindowModule   = nullptr;
    boundContext         = nullptr;
}

void *WindowModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    if (capabilityName == "window.glfw")
    {
        return static_cast<WindowCapability *>(this);
    }
    return nullptr;
}

WindowIdentifier WindowModule::createMainWindow(
    const WindowDescription &description
)
{
    if (!glfwInitialized)
    {
        throw std::runtime_error("window module: window creation requested before start");
    }

    // No OpenGL or OpenGL ES context: the renderer drives Vulkan exclusively.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, description.resizable ? GLFW_TRUE : GLFW_FALSE);

    GLFWwindow *window = glfwCreateWindow(
        static_cast<int>(description.width),
        static_cast<int>(description.height),
        description.title.c_str(),
        nullptr,
        nullptr
    );
    if (window == nullptr)
    {
        throw std::runtime_error("window module: glfwCreateWindow failed");
    }

    WindowRecord record;
    record.identifier = nextWindowIdentifier++;
    record.window     = window;

    installCallbacks(window);
    int framebufferWidth  = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    record.cachedFramebufferWidth  = static_cast<std::uint32_t>(framebufferWidth);
    record.cachedFramebufferHeight = static_cast<std::uint32_t>(framebufferHeight);
    windows.emplace(record.identifier, record);
    identifiersCacheDirty = true;

    // Display mode: applied after creation so the framebuffer-size callback
    // (installed above) reports the mode change like any other resize - the
    // renderer's swapchain recreation path needs no fullscreen-specific
    // code. Borderless undecorates and fills the monitor's current video
    // mode; exclusive hands the window to the monitor (a real mode switch).
    if (fullscreenMode_ != WindowDisplayMode::Off)
    {
        applyDisplayMode(window);
    }

    if (logger != nullptr)
    {
        logger->logMessage(
            LogLevel::Information,
            "created window '" + description.title + "' (" + std::to_string(description.width) + "x"
                + std::to_string(description.height) + ")"
        );
    }
    return record.identifier;
}

bool WindowModule::windowCloseRequested(
    WindowIdentifier windowIdentifier
)
{
    WindowRecord *record = findRecord(windowIdentifier);
    if (record == nullptr || record->window == nullptr)
    {
        return true;
    }
    return glfwWindowShouldClose(record->window) != 0;
}

void WindowModule::pollEvents()
{
    if (!glfwInitialized)
    {
        return;
    }
    glfwPollEvents();

    // Report close requests exactly once per window.
    for (auto &entry : windows)
    {
        WindowRecord &record = entry.second;
        if (!record.closeReported && record.window != nullptr && glfwWindowShouldClose(record.window))
        {
            record.closeReported = true;
            if (boundContext != nullptr)
            {
                WindowClosedEvent event;
                event.windowIdentifier = record.identifier;
                windowClosedChannel->publish(event);
            }
        }
    }
}

std::uint32_t WindowModule::framebufferWidth(
    WindowIdentifier windowIdentifier
)
{
    WindowRecord *record = findRecord(windowIdentifier);
    if (record == nullptr || record->window == nullptr)
    {
        return 0;
    }
    // Cached at creation, maintained by the resize callback: per-frame polls
    // (UI, scripts) used to pay a GLFW call each.
    return record->cachedFramebufferWidth;
}

std::uint32_t WindowModule::framebufferHeight(
    WindowIdentifier windowIdentifier
)
{
    WindowRecord *record = findRecord(windowIdentifier);
    if (record == nullptr || record->window == nullptr)
    {
        return 0;
    }
    return record->cachedFramebufferHeight;
}

const std::vector<WindowIdentifier> &WindowModule::windowIdentifiers() const
{
    // Rebuilt only when the window set changed (creation); windows are never
    // removed mid-run, so the cache cannot go stale between creations.
    // Returned by reference (see the capability header): the copy-per-call
    // this return used to make bought nothing - every consumer reads the
    // list, none stores it.
    if (identifiersCacheDirty)
    {
        cachedIdentifiers.clear();
        cachedIdentifiers.reserve(windows.size());
        for (const auto &entry : windows)
        {
            cachedIdentifiers.push_back(entry.first);
        }
        identifiersCacheDirty = false;
    }
    return cachedIdentifiers;
}

std::vector<std::string> WindowModule::requiredInstanceExtensions(
    const std::string &graphicsApi
)
{
    if (graphicsApi != "vulkan")
    {
        throw std::invalid_argument("window module: unsupported graphics API family '" + graphicsApi + "'");
    }
    if (!glfwInitialized)
    {
        return {};
    }
    std::uint32_t extensionCount = 0;
    const char **extensionNames  = glfwGetRequiredInstanceExtensions(&extensionCount);
    std::vector<std::string> extensions;
    extensions.reserve(extensionCount);
    for (std::uint32_t index = 0; index < extensionCount; ++index)
    {
        extensions.emplace_back(extensionNames[index]);
    }
    return extensions;
}

void *WindowModule::createPresentationSurface(
    const std::string &graphicsApi, void *graphicsInstance, WindowIdentifier windowIdentifier
)
{
    if (graphicsApi != "vulkan")
    {
        throw std::invalid_argument("window module: unsupported graphics API family '" + graphicsApi + "'");
    }
    WindowRecord *record = findRecord(windowIdentifier);
    if (record == nullptr || record->window == nullptr)
    {
        throw std::runtime_error("window module: surface requested for an unknown window");
    }
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult result
        = glfwCreateWindowSurface(static_cast<VkInstance>(graphicsInstance), record->window, nullptr, &surface);
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error(
            "window module: glfwCreateWindowSurface failed with VkResult " + std::to_string(static_cast<int>(result))
        );
    }
    return surface;
}

void *WindowModule::nativeWindowHandle(
    WindowIdentifier windowIdentifier
)
{
    WindowRecord *record = findRecord(windowIdentifier);
    if (record == nullptr)
    {
        return nullptr;
    }
    return record->window;
}

WindowIdentifier WindowModule::windowIdentifierFromNativeHandle(
    void *nativeWindowHandle
)
{
    for (const auto &entry : windows)
    {
        if (entry.second.window == nativeWindowHandle)
        {
            return entry.first;
        }
    }
    return invalidWindowIdentifier;
}

WindowModule::WindowRecord *WindowModule::findRecord(
    WindowIdentifier windowIdentifier
)
{
    const auto found = windows.find(windowIdentifier);
    if (found == windows.end())
    {
        return nullptr;
    }
    return &found->second;
}

void WindowModule::applyDisplayMode(
    GLFWwindow *window
)
{
    // glfwInit succeeded (this is only reachable from createMainWindow), so
    // the monitor list is live. GLFW orders monitors as the platform
    // reports them; index 0 is the primary. An out-of-range index falls
    // back to the primary with a warning - a missing monitor must not
    // refuse a window the run asked for.
    int monitorCount       = 0;
    GLFWmonitor **monitors = glfwGetMonitors(&monitorCount);
    const int index        = static_cast<int>(monitorIndex_);
    GLFWmonitor *monitor   = nullptr;
    if (monitors != nullptr && monitorCount > 0)
    {
        monitor = index < monitorCount ? monitors[index] : monitors[0];
    }
    if (monitor == nullptr)
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning, "window module: no monitor for the fullscreen mode; staying windowed"
            );
        }
        return;
    }
    if (index >= monitorCount && logger != nullptr)
    {
        logger->logMessage(
            LogLevel::Warning,
            "window module: monitor index " + std::to_string(index) + " is out of range ("
                + std::to_string(monitorCount) + " monitor(s)); using the first"
        );
    }
    if (fullscreenMode_ == WindowDisplayMode::Exclusive)
    {
        // A real video-mode switch: the window (and its Vulkan surface) is
        // owned by the monitor until stop. The mode's refresh rate keeps the
        // compositor out of the presentation path.
        const GLFWvidmode *mode = glfwGetVideoMode(monitor);
        if (mode == nullptr)
        {
            // glfwGetVideoMode returns null when the monitor (just) went
            // away - a hot-unplug between glfwGetMonitors and here would
            // otherwise dereference null. Staying windowed is the safe
            // fallback, same as the missing-monitor case above.
            if (logger != nullptr)
            {
                logger->logMessage(LogLevel::Warning, "window module: video mode query failed; staying windowed");
            }
            return;
        }
        glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else
    {
        // Borderless: undecorate, then fill the monitor's current video
        // mode. glfwSetWindowMonitor with a null monitor moves AND resizes
        // the window while keeping it a window - the decoration change
        // before it means no title bar ever flashes over the content.
        const GLFWvidmode *mode = glfwGetVideoMode(monitor);
        if (mode == nullptr)
        {
            if (logger != nullptr)
            {
                logger->logMessage(LogLevel::Warning, "window module: video mode query failed; staying windowed");
            }
            return;
        }
        int x = 0;
        int y = 0;
        glfwGetMonitorPos(monitor, &x, &y);
        glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
        glfwSetWindowMonitor(window, nullptr, x, y, mode->width, mode->height, mode->refreshRate);
    }
    if (logger != nullptr)
    {
        logger->logMessage(
            LogLevel::Information,
            std::string("window module: display mode ")
                + (fullscreenMode_ == WindowDisplayMode::Exclusive ? "exclusive" : "borderless") + " on monitor "
                + std::to_string(index)
        );
    }
}

void WindowModule::installCallbacks(
    GLFWwindow *window
)
{
    glfwSetFramebufferSizeCallback(window, &WindowModule::framebufferResizeCallback);
}

void WindowModule::framebufferResizeCallback(
    GLFWwindow *window, int width, int height
)
{
    if (activeWindowModule == nullptr || activeWindowModule->boundContext == nullptr)
    {
        return;
    }
    const WindowIdentifier identifier = activeWindowModule->windowIdentifierFromNativeHandle(window);
    if (identifier == invalidWindowIdentifier)
    {
        return;
    }
    // Keep the per-record framebuffer cache current: the per-frame polls
    // read it instead of calling into GLFW.
    const auto found = activeWindowModule->windows.find(identifier);
    if (found != activeWindowModule->windows.end())
    {
        found->second.cachedFramebufferWidth  = static_cast<std::uint32_t>(width);
        found->second.cachedFramebufferHeight = static_cast<std::uint32_t>(height);
    }
    WindowResizedEvent event;
    event.windowIdentifier = identifier;
    event.width            = static_cast<std::uint32_t>(width);
    event.height           = static_cast<std::uint32_t>(height);
    activeWindowModule->windowResizedChannel->publish(event);
}

void WindowModule::errorCallback(
    int errorCode, const char *description
)
{
    if (activeWindowModule != nullptr && activeWindowModule->logger != nullptr)
    {
        activeWindowModule->logger->logMessage(
            LogLevel::Error,
            "window module: " + describeGlfwError(errorCode) + " (" + std::to_string(errorCode)
                + "): " + (description != nullptr ? description : "")
        );
    }
}

} // namespace engine
