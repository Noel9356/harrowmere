#ifndef ENGINE_MODULES_SCREENSHOT_HPP
#define ENGINE_MODULES_SCREENSHOT_HPP

#include "input_capability.hpp"
#include "logging_capability.hpp"
#include "lua_bridge_extension.hpp"
#include "renderer_capability.hpp"
#include "screenshot_capability.hpp"
#include "engine/kernel/async_dispatcher.hpp"
#include "engine/kernel/component_interface.hpp"
#include "engine/kernel/configuration_watch.hpp"
#include "engine/kernel/event_bus.hpp"
#include "engine/kernel/parallel_for.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace engine
{

class InputCapability;
class RendererCapability;

// Module "screenshot_png": watches the configured trigger key and queues one frame
// capture per press. The capture itself travels renderer -> FrameCapturedEvent
// -> encoded file, so the module never touches Vulkan and the renderer never
// touches the filesystem: each side stays behind its capability contract.
//
// Encoding runs on the module's worker thread (the kernel's AsyncTaskDispatcher
// pattern): the FrameCapturedEvent handler only copies the pixels out of the
// renderer's staging memory (the event's contract bounds their lifetime to the
// handler) and hands the copy to the worker; the completion runs on the main
// thread at the next frame event, publishing ScreenshotSavedEvent for a file
// that is already on disk. A capture therefore never stalls the frame it
// interrupted. The event bus stays single threaded - the completion is where
// the result re-enters it.
//
// The module also provides the "scripting.lua_bridge.screenshot" Lua bridge
// extension: it installs engine.request_screenshot into every script state
// through a dedicated ScreenshotLuaBridge object owned by this module (see
// screenshot.cpp). The host discovers this extension through the
// multi-provider acquisition of the "scripting.lua_bridge" family, so adding
// this capability to the module never touches the lua_scripting source.
class ScreenshotModule final : public ModuleInterface, public ScreenshotCapability
{
public:
    ScreenshotModule();
    ~ScreenshotModule() override;

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;

    void requestScreenshot() override;

private:
    // The configured file format ([screenshot].format). Png is the default
    // and the historical behavior; the other two exist because a capture
    // pipeline that only previews (jpg) or only feeds a fixed-size compositor
    // (bmp) does not want a lossless encoder's cost.
    enum class FileFormat
    {
        Png,
        Jpg,
        Bmp
    };

    void handleKeyPressed(const KeyPressedEvent &event);
    void handleFrameBegin(const FrameBeginEvent &event);
    void handleFrameCaptured(const FrameCapturedEvent &event);
    // Live configuration reload (kernel reloadConfiguration) through the
    // kernel's configuration watch utility: one watch per output-affecting
    // key (format, quality, prefix, output directory), each re-reading its
    // own value with keep-last-good semantics when the reload carries an
    // invalid one. The trigger key is deliberately not watched (re-binding
    // mid-run could leave a half-pressed capture state) - it keeps the
    // startup binding.
    void reloadFormatFromConfiguration();
    void reloadJpegQualityFromConfiguration();
    void reloadFilenamePrefixFromConfiguration();
    void reloadOutputDirectoryFromConfiguration();
    // The format name -> enum mapping shared by construct (invalid = startup
    // error) and the reload path (invalid = keep-last-good + log).
    static FileFormat fileFormatFromName(const std::string &name, bool &ok);
    std::string nextCaptureFilePath() const;

    ComponentManifest componentManifest;
    ComponentContext *boundContext         = nullptr;
    LoggerCapability *logger               = nullptr;
    RendererCapability *rendererCapability = nullptr;
    EventSubscription keyPressedSubscription;
    EventSubscription frameBeginSubscription;
    EventSubscription frameCapturedSubscription;
    // The reload watches (see the reload*FromConfiguration methods): the hub
    // holds the module's single ConfigurationReloadedEvent subscription and
    // fires exactly the key that changed, replacing the hand-written
    // changedKeys scan the reload handler used to be. The guards below OWN
    // the watches - a watch whose guard is discarded unregisters itself at
    // once (the RAII discipline EventSubscription uses), so keeping them as
    // members is what makes the watches live for the module's lifetime.
    ConfigurationWatchHub configurationWatches;
    ConfigurationWatch formatWatch;
    ConfigurationWatch jpegQualityWatch;
    ConfigurationWatch filenamePrefixWatch;
    ConfigurationWatch outputDirectoryWatch;
    // Default trigger: F12 from the input contract's key enum (one source of
    // the numeric code; the config [screenshot].key replaces it by name).
    std::uint32_t triggerKeyCode = static_cast<std::uint32_t>(KeyCode::F12);
    std::string triggerKeyName   = "F12";
    std::string outputDirectory; // empty = platform user media directory
    std::filesystem::path resolvedOutputDirectory;
    bool captureQueued           = false;
    std::uint64_t captureCounter = 0;
    // [screenshot].format / .jpeg_quality / .filename_prefix.
    FileFormat fileFormat      = FileFormat::Png;
    int jpegQuality            = 90;
    std::string filenamePrefix = "screenshot";
    // [screenshot].encode_workers: the encoder pool size. 0 (the default)
    // is AUTO - the pool starts with defaultWorkerCount(), the engine-wide
    // hardware-derived worker count (parallel_for.hpp), so a burst of
    // captures encodes in parallel out of the box. 1..8 pins an explicit
    // size; stb encodes are CPU-bound and independent, so more workers
    // drain a burst faster, and completions still drain on the frame event,
    // so the bus stays single threaded.
    int encodeWorkerCount = 0;
    // The encode+write worker pool (the kernel's off-main-thread pattern):
    // work runs on the workers, the saved-event completion drains on the
    // frame event. Started in start(), stopped in stop (a capture still
    // queued at stop is dropped with a log - the run is over; nothing reads
    // it). The in-flight COUNTER (not a bool) is what makes several workers
    // correct: concurrent encodes decrement their own completion.
    AsyncTaskDispatcher encoder;
    int encodeTasksInFlight = 0;
    // The Lua bridge extension object owned by this module. Constructed in
    // construct() and returned from capabilityPointerFor for the
    // "scripting.lua_bridge.screenshot" member. Held as the base
    // LuaBridgeExtension pointer: the concrete ScreenshotLuaBridge type
    // (defined in screenshot.cpp) stays private to this module's translation
    // unit, so the header never names it.
    std::unique_ptr<LuaBridgeExtension> luaBridge;
};

} // namespace engine

#endif // ENGINE_MODULES_SCREENSHOT_HPP
