#ifndef ENGINE_MODULES_HOT_RELOAD_HPP
#define ENGINE_MODULES_HOT_RELOAD_HPP

#include "hot_reload_capability.hpp"
#include "logging_capability.hpp"
#include "engine/kernel/async_dispatcher.hpp"
#include "engine/kernel/component_interface.hpp" // EngineFrameEvent (the drain
                                                 // point: declared in the
                                                 // kernel's component-facing
                                                 // header, so the module needs
                                                 // no renderer capability
                                                 // include and drains in
                                                 // renderer-less runs too)
#include "engine/kernel/event_bus.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace engine
{

// Module "hot_reload_watcher": filesystem watching for the development loop.
//
// Shape: a worker thread (the kernel's AsyncTaskDispatcher) polls the watch
// set's mtimes every [hot_reload].watch_interval_ms; each poll's result
// comes back as MAIN-thread completions (drained on the frame event), which
// is where the HotReloadEvent is published - the bus stays single threaded,
// the frame loop pays one drain call.
//
// What is watched automatically (all engine-relative):
//   runtime/shaders/*.spv   -> kind shader   (the renderer re-creates its pipelines)
//   mods/<id>/*.lua         -> kind script   (the scripting module reloads the mod)
//   mods/<id>/mod.toml      -> kind manifest (logged; restart applies)
//   mods/<id>/scene.toml    -> kind scene    (logged; restart, or re-import)
//   config/**/*.toml        -> kind config   (logged; restart applies)
//
// Consumers: the renderer (shaders) and the scripting module (scripts)
// subscribe to the bus channel themselves - the module publishes, it does
// not know who reacts.
class HotReloadModule final : public ModuleInterface, public HotReloadCapability
{
public:
    HotReloadModule();
    ~HotReloadModule() override;

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;

    // HotReloadCapability.
    bool watchPath(const std::string &engineRelativePath) override;
    std::uint32_t watchedFileCount() const override;

private:
    struct WatchedFile
    {
        std::filesystem::path absolutePath;
        std::uint32_t kind = 0;
        std::string modId;
        std::filesystem::file_time_type lastWriteTime{};
        // The mtime seen by the PREVIOUS scan (the debounce baseline): an
        // event fires only when the current stat matches this AND differs
        // from lastWriteTime, so a progressively written file - a fresh
        // mtime every scan while the write is in flight - settles before
        // anything fires.
        std::filesystem::file_time_type lastSeenTime{};
    };

    void buildDefaultWatchSet();
    void scanOnce(); // runs on the WORKER thread
    void forwardFrameTick(const EngineFrameEvent &event);

    ComponentManifest componentManifest;
    ComponentContext *boundContext = nullptr;
    LoggerCapability *logger       = nullptr;

    AsyncTaskDispatcher dispatcher;
    EventSubscription frameTickSubscription;
    EventChannel<EngineFrameEvent> *frameTickChannel = nullptr;

    // The watch set. Written on the main thread (construct/start/watchPath);
    // read by the worker during scans. The mutex guards exactly this
    // hand-off - the only lock in the module.
    mutable std::mutex watchMutex;
    std::map<std::string, WatchedFile> watchedFiles; // keyed by engine-relative path
    std::chrono::milliseconds watchInterval{250};
    // Main-thread clock of the last scan post: paces forwardFrameBegin's
    // re-arm to the configured interval (the default epoch means the first
    // frame posts immediately).
    std::chrono::steady_clock::time_point lastScanPostTime{};
    // [hot_reload].watch_paths: the assembly's own watch roots when set
    // (engine-relative); empty keeps buildDefaultWatchSet's three roots.
    std::vector<std::string> configuredWatchPaths;

    // One-time diagnostics.
    bool startupLogged = false;
};

} // namespace engine

#endif // ENGINE_MODULES_HOT_RELOAD_HPP
