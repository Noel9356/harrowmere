#include "hot_reload.hpp"

#include "engine/kernel/filesystem_utilities.hpp"

#include <algorithm>

namespace engine
{

namespace
{

// Kinds for a given path, by convention (the runtime layout is the contract).
std::uint32_t kindForPath(
    const std::filesystem::path &relativePath, std::string &outModId
)
{
    outModId.clear();
    const std::string generic = relativePath.generic_string();
    if (generic.rfind("runtime/shaders/", 0) == 0 && generic.ends_with(".spv"))
    {
        return hot_reload_kind::shader;
    }
    if (generic.rfind("mods/", 0) == 0)
    {
        // mods/<id>/... - the second component is the package id.
        const std::size_t firstSlash  = generic.find('/');
        const std::size_t secondSlash = generic.find('/', firstSlash + 1);
        if (secondSlash != std::string::npos)
        {
            outModId = generic.substr(firstSlash + 1, secondSlash - firstSlash - 1);
        }
        if (generic.ends_with(".lua"))
        {
            return hot_reload_kind::script;
        }
        if (generic.ends_with("mod.toml"))
        {
            return hot_reload_kind::manifest;
        }
        if (generic.ends_with("scene.toml"))
        {
            return hot_reload_kind::scene;
        }
    }
    if (generic.rfind("config/", 0) == 0 && generic.ends_with(".toml"))
    {
        return hot_reload_kind::config;
    }
    return hot_reload_kind::config; // unknown-but-watched: the conservative kind
}

} // namespace

HotReloadModule::HotReloadModule()
{
    componentManifest = ComponentManifest{
        .name                 = "hot_reload",
        .kind                 = ComponentKind::Module,
        .version              = "0.1.0",
        .providedCapabilities = {"hot_reload.watcher"},
        .requiredCapabilities = {"logging"},
        .optionalCapabilities = {},
        .requiredComponents   = {},
        .requiredPermissions  = {"capability.logging", "filesystem.read"},
        .knownConfigKeys      = {
            "hot_reload.watch_interval_ms",
            "hot_reload.watch_paths",
        },
    };
}

HotReloadModule::~HotReloadModule()
{
    stop();
}

void HotReloadModule::construct(
    ComponentContext &context
)
{
    boundContext = &context;
    logger       = &context.acquireCapability<LoggerCapability>();

    const auto interval = context.configurationInteger("watch_interval_ms").value_or(250);
    watchInterval       = std::chrono::milliseconds(std::clamp<std::int64_t>(interval, 50, 5000));

    // The watch roots: an explicit [hot_reload].watch_paths array replaces
    // the default set (engine-relative paths, files or directories); an
    // absent key keeps the runtime layout's three hot spots. Custom roots
    // classify through the same path-kind convention - a path outside the
    // known prefixes reports the conservative "config" kind (log + restart).
    configuredWatchPaths = context.configurationStringArrayInSection("hot_reload", "watch_paths");

    EventBus &bus = context.eventBus();
    // The drain point is the kernel's EngineFrameEvent heartbeat (the
    // composition root's loop clock), not the renderer's FrameBeginEvent: a
    // headless dev loop (a server whose scripts hot-swap) reloads exactly
    // like a rendered one.
    frameTickChannel      = &bus.channel<EngineFrameEvent>();
    frameTickSubscription = EventSubscription(
        &bus,
        typeid(EngineFrameEvent),
        frameTickChannel->subscribe<HotReloadModule, &HotReloadModule::forwardFrameTick>(this)
    );
}

void HotReloadModule::start()
{
    if (!boundContext->isCapabilityDemanded(capabilityName))
    {
        // Demand-driven by contract: nothing should have constructed us
        // without demand, but the guard keeps the log honest if a future
        // assembly changes that.
        logger->logMessage(LogLevel::Warning, "hot reload module started without demand (unexpected)");
    }
    buildDefaultWatchSet();
    // One worker, explicitly: the watcher's contract is ONE poll in flight
    // at a time (forwardFrameTick re-arms the next scan only after the
    // previous one finished), which keeps the cadence even when a scan is
    // slower than the interval. The dispatcher's default start() is sized
    // for independent batch work; a self re-arming poll cycle is the
    // documented reason a caller pins the historical single worker.
    dispatcher.start(1);
    // First scan immediately: baseline mtimes before any change can fire.
    scanOnce();
    logger->logMessage(
        LogLevel::Information,
        "hot reload module started (" + std::to_string(watchedFiles.size()) + " file(s) watched, poll "
            + std::to_string(watchInterval.count()) + " ms)"
    );
    startupLogged = true;
}

void HotReloadModule::stop()
{
    frameTickSubscription.release();
    dispatcher.stop();
    {
        std::lock_guard<std::mutex> lock(watchMutex);
        watchedFiles.clear();
    }
    boundContext = nullptr;
    logger       = nullptr;
}

void *HotReloadModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    if (capabilityName == "hot_reload.watcher")
    {
        return static_cast<HotReloadCapability *>(this);
    }
    return nullptr;
}

bool HotReloadModule::watchPath(
    const std::string &engineRelativePath
)
{
    const std::filesystem::path absolute = std::filesystem::path(engineRelativePath).is_relative()
                                             ? (executableDirectory() / engineRelativePath)
                                             : std::filesystem::path(engineRelativePath);
    std::error_code error;
    if (!std::filesystem::exists(absolute, error))
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning, "hot reload module: watch path '" + engineRelativePath + "' does not exist"
            );
        }
        return false;
    }
    if (std::filesystem::is_regular_file(absolute, error))
    {
        // Classify the ENGINE-RELATIVE path, exactly like the directory
        // branch below: kindForPath matches the runtime layout's prefixes,
        // which an absolute path never carries (every single-file watch
        // would degrade to the conservative "config" kind).
        std::error_code relativeError;
        const std::filesystem::path relative
            = std::filesystem::relative(absolute, executableDirectory(), relativeError);
        std::lock_guard<std::mutex> lock(watchMutex);
        WatchedFile file;
        file.absolutePath  = absolute;
        file.kind          = kindForPath(relative, file.modId);
        file.lastWriteTime = std::filesystem::last_write_time(absolute, error);
        file.lastSeenTime  = file.lastWriteTime;
        watchedFiles.emplace(std::filesystem::path(engineRelativePath).generic_string(), std::move(file));
        return true;
    }
    // Directory: add every file under it (the module's own extension filter
    // keeps binaries out).
    for (const std::filesystem::directory_entry &entry : std::filesystem::recursive_directory_iterator(absolute, error))
    {
        if (!entry.is_regular_file(error))
        {
            continue;
        }
        const std::filesystem::path relative = std::filesystem::relative(entry.path(), executableDirectory(), error);
        std::lock_guard<std::mutex> lock(watchMutex);
        WatchedFile file;
        file.absolutePath  = entry.path();
        file.kind          = kindForPath(relative, file.modId);
        file.lastWriteTime = std::filesystem::last_write_time(entry.path(), error);
        file.lastSeenTime  = file.lastWriteTime;
        watchedFiles.emplace(relative.generic_string(), std::move(file));
    }
    return true;
}

std::uint32_t HotReloadModule::watchedFileCount() const
{
    std::lock_guard<std::mutex> lock(watchMutex);
    return static_cast<std::uint32_t>(watchedFiles.size());
}

void HotReloadModule::buildDefaultWatchSet()
{
    // The runtime layout's hot spots, unless [hot_reload].watch_paths names
    // its own. Missing directories are normal (a build without the renderer
    // has no shaders; an assembly may ship no mods) - absent trees simply
    // contribute nothing, and watchPath already logs them precisely.
    if (!configuredWatchPaths.empty())
    {
        for (const std::string &path : configuredWatchPaths)
        {
            watchPath(path);
        }
        return;
    }
    const std::filesystem::path root = executableDirectory();
    std::error_code error;
    const std::filesystem::path shaders = root / "runtime/shaders";
    if (std::filesystem::is_directory(shaders, error))
    {
        watchPath("runtime/shaders");
    }
    const std::filesystem::path mods = root / "mods";
    if (std::filesystem::is_directory(mods, error))
    {
        watchPath("mods");
    }
    const std::filesystem::path config = root / "config";
    if (std::filesystem::is_directory(config, error))
    {
        watchPath("config");
    }
}

void HotReloadModule::scanOnce()
{
    // Worker thread. Snapshot the watch set (cheap: the module owns few
    // hundred files at most), stat each, and post ONE completion per change -
    // the completions run on the main thread and publish the events.
    // The snapshot takes (path, mtime) PAIRS under the lock instead of
    // copying the whole map: the previous full copy re-allocated every path
    // string (std::map copy = one node clone per file) every scan interval;
    // the pairs are two POD-ish members per file and the strings move once.
    struct WatchSnapshotEntry
    {
        std::string relativePath;
        std::filesystem::path absolutePath;
        std::uint32_t kind = 0;
        std::string modId;
        std::filesystem::file_time_type lastWriteTime{};
        std::filesystem::file_time_type lastSeenTime{};
    };
    std::vector<WatchSnapshotEntry> snapshot;
    {
        std::lock_guard<std::mutex> lock(watchMutex);
        snapshot.reserve(watchedFiles.size());
        for (const auto &[path, watched] : watchedFiles)
        {
            snapshot.push_back(
                WatchSnapshotEntry{
                    path, watched.absolutePath, watched.kind, watched.modId, watched.lastWriteTime, watched.lastSeenTime
                }
            );
        }
    }
    std::error_code error;
    for (auto &entry : snapshot)
    {
        const std::filesystem::file_time_type current = std::filesystem::last_write_time(entry.absolutePath, error);
        if (error || current == entry.lastWriteTime)
        {
            continue;
        }
        // Debounce: an mtime that differs from the PREVIOUS scan's means
        // the write is still in flight (editors and downloaders update a
        // file progressively). Record the mtime and let the next scan
        // confirm it - only a value that survives one scan unchanged fires,
        // so no partial content is ever published.
        if (current != entry.lastSeenTime)
        {
            std::lock_guard<std::mutex> lock(watchMutex);
            const auto live = watchedFiles.find(entry.relativePath);
            if (live != watchedFiles.end())
            {
                live->second.lastSeenTime = current;
            }
            continue;
        }
        // current == lastSeenTime (stable across two consecutive scans) and
        // != lastWriteTime (checked above): the write settled. Commit it as
        // the new baseline in the LIVE map and fire exactly once.
        {
            std::lock_guard<std::mutex> lock(watchMutex);
            const auto live = watchedFiles.find(entry.relativePath);
            if (live != watchedFiles.end())
            {
                live->second.lastWriteTime = current;
                live->second.lastSeenTime  = current;
            }
        }
        WatchedFile changed;
        changed.absolutePath  = entry.absolutePath;
        changed.kind          = entry.kind;
        changed.modId         = entry.modId;
        changed.lastWriteTime = current;
        dispatcher.postWithCompletion(
            [changed]() { /* the stat already happened; the completion carries the result */ },
            [this, changed]() {
                if (logger != nullptr)
                {
                    logger->logMessage(
                        LogLevel::Information,
                        "hot reload: '" + changed.absolutePath.generic_string() + "' changed (kind "
                            + std::to_string(changed.kind) + ")"
                    );
                }
                if (boundContext != nullptr)
                {
                    boundContext->eventBus().publish(
                        HotReloadEvent{
                            .kind         = changed.kind,
                            .relativePath = changed.absolutePath.generic_string(),
                            .modId        = changed.modId
                        }
                    );
                }
            }
        );
    }
}

void HotReloadModule::forwardFrameTick(
    const EngineFrameEvent & /*event*/
)
{
    // The main-thread drain point: completions (event publications) run here.
    // The drain keeps its relaxed atomic fast path, so an idle dispatcher
    // costs one load even on frames this handler does nothing else.
    dispatcher.drainCompletedTasks();

    // Nothing watched, nothing to pace: the clock read and the interval
    // comparison below exist to schedule scans of the watch set, and a run
    // whose watch set never filled (the common shape - an assembly that
    // registers no watchers, the module installed but inert) would otherwise
    // pay a steady_clock::now() every frame for a scan that can never be
    // posted. A watch registered later starts pacing from that frame (the
    // default-constructed lastScanPostTime is an epoch long past, so the
    // first comparison with the interval is true immediately).
    if (watchedFiles.empty())
    {
        return;
    }

    // Re-arm the next scan, paced by [hot_reload].watch_interval_ms - the
    // documented poll cadence, previously parsed but never applied, which
    // made the worker rescan the whole watch set at frame rate (a full map
    // copy under the mutex plus one stat per watched file, 60+ times a
    // second). One poll in flight at a time keeps the cadence even when a
    // scan is slower than the interval. The drain stays per-frame, so a
    // change still surfaces on the frame right after its scan completes.
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (now - lastScanPostTime >= watchInterval)
    {
        lastScanPostTime = now;
        dispatcher.post([this]() { scanOnce(); });
    }
    (void)startupLogged;
}

} // namespace engine
