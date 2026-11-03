#include "screenshot.hpp"

#include "input_capability.hpp"

#include <stb_image_write.h>
#include <sol/sol.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace engine
{

namespace
{

// The dedicated Lua bridge extension object for the screenshot module. Owns
// the sol2 wiring that installs engine.request_screenshot into every script
// state; holds a raw pointer back to the ScreenshotCapability the module
// provides (the module IS the capability, so `this` from the module suffices).
// Defined here in the module's translation unit so the concrete type stays
// private: the header holds only a unique_ptr<LuaBridgeExtension>.
class ScreenshotLuaBridge final : public LuaBridgeExtension
{
public:
    explicit ScreenshotLuaBridge(
        ScreenshotCapability *capability
    )
        : capability(capability)
    {}

    const char *extensionName() const override
    {
        return "screenshot";
    }

    void installLuaBridge(
        luaState *lua
    ) override
    {
        // The host (lua_scripting) guarantees the engine table exists when
        // this is called: installEngineBridge ran first and created it. The
        // bridge function calls requestScreenshot() on the module's
        // ScreenshotCapability through the captured pointer - an in-module
        // dispatch, no capability acquisition, no permission check. A script
        // calling engine.request_screenshot() lands here on the same thread
        // the script runs on (the engine's single main thread, by the bus
        // contract), so the queueing into captureQueued stays race-free.
        sol::state_view luaView(lua);
        sol::table engine = luaView["engine"];
        engine.set_function("request_screenshot", [this]() { capability->requestScreenshot(); });
    }

private:
    ScreenshotCapability *capability;
};

} // namespace

ScreenshotModule::ScreenshotModule()
{
    componentManifest = ComponentManifest{
        .name    = "screenshot",
        .kind    = ComponentKind::Module,
        .version = "0.1.0",
        // "screenshot.png" is the implementation-bound member of the
        // "screenshot" family: the PNG-encoder capture implementation.
        .providedCapabilities
        = {"screenshot.png",
            // The Lua bridge extension member: declares this module as a
            // provider of the "scripting.lua_bridge" family (the descriptor's
            // static list carries the same entry - the kernel verifies the
            // manifest and the descriptor agree at startup).
            "scripting.lua_bridge.screenshot"},
        // "input" is deliberately NOT required: the module reacts to key presses
        // through the event bus (input publishes KeyPressedEvent), so a build
        // without the input module loses only the hotkey, not the capability API.
        .requiredCapabilities = {"logging", "renderer"},
        .optionalCapabilities = {},
        .requiredComponents   = {},
        .requiredPermissions  = {"capability.logging", "capability.renderer", "filesystem.write"},
        .knownConfigKeys
        = {"screenshot.key",
            "screenshot.output_directory",
            "screenshot.format",
            "screenshot.jpeg_quality",
            "screenshot.filename_prefix",
            "screenshot.encode_workers"},
    };
}

ScreenshotModule::~ScreenshotModule()
{
    stop();
}

void ScreenshotModule::construct(
    ComponentContext &context
)
{
    boundContext       = &context;
    logger             = &context.acquireCapability<LoggerCapability>();
    rendererCapability = &context.acquireCapability<RendererCapability>();

    triggerKeyName          = context.configurationString("key").value_or("F12");
    const KeyCode parsedKey = keyCodeFromName(triggerKeyName);
    if (parsedKey == KeyCode::Unknown)
    {
        throw std::runtime_error(
            "screenshot module: configuration key '" + triggerKeyName + "' is not a known key name"
        );
    }
    triggerKeyCode = static_cast<std::uint32_t>(parsedKey);

    outputDirectory = context.configurationString("output_directory").value_or(std::string());

    // File format: png (the default, the historical behavior) | jpg | bmp.
    // An unknown spelling is a precise startup error - the extension and the
    // encoder must agree, and a silent fallback would write files a tooling
    // pipeline cannot parse under the name it expected.
    const std::string formatName  = context.configurationString("format").value_or("png");
    bool formatKnown              = false;
    const FileFormat parsedFormat = fileFormatFromName(formatName, formatKnown);
    if (!formatKnown)
    {
        throw std::runtime_error(
            "screenshot module: [screenshot] format '" + formatName + "' is not a known format (png, jpg, bmp)"
        );
    }
    fileFormat = parsedFormat;
    // JPEG quality (1..100, stb_image_write's range; 90 = a high-quality
    // default). Only consulted by the jpg encoder.
    jpegQuality
        = static_cast<int>(std::clamp<std::int64_t>(context.configurationInteger("jpeg_quality").value_or(90), 1, 100));
    // The file name stem: "screenshot_<timestamp>_<counter>.<ext>" by default
    // (the historical pattern); an assembly that pipelines captures into a
    // directory of its own names them accordingly.
    filenamePrefix = context.configurationString("filename_prefix").value_or("screenshot");

    // Encode pool size: 0 is AUTO - the pool starts with the engine-wide
    // hardware-derived worker count (defaultWorkerCount, parallel_for.hpp),
    // the parallel-by-default behavior. 1..8 pins an explicit size (the
    // clamp matches the header's contract - captures are user-triggered and
    // rare, so more than eight workers would be tuning theater); anything
    // else falls back to auto.
    const std::int64_t configuredEncodeWorkers = context.configurationInteger("encode_workers").value_or(0);
    encodeWorkerCount
        = configuredEncodeWorkers > 0 ? static_cast<int>(std::clamp<std::int64_t>(configuredEncodeWorkers, 1, 8)) : 0;

    EventBus &bus                                 = context.eventBus();
    EventChannel<KeyPressedEvent> &pressedChannel = bus.channel<KeyPressedEvent>();
    keyPressedSubscription                        = EventSubscription(
        &bus,
        typeid(KeyPressedEvent),
        pressedChannel.subscribe<ScreenshotModule, &ScreenshotModule::handleKeyPressed>(this)
    );

    EventChannel<FrameBeginEvent> &frameBeginChannel = bus.channel<FrameBeginEvent>();
    frameBeginSubscription                           = EventSubscription(
        &bus,
        typeid(FrameBeginEvent),
        frameBeginChannel.subscribe<ScreenshotModule, &ScreenshotModule::handleFrameBegin>(this)
    );

    EventChannel<FrameCapturedEvent> &capturedChannel = bus.channel<FrameCapturedEvent>();
    frameCapturedSubscription                         = EventSubscription(
        &bus,
        typeid(FrameCapturedEvent),
        capturedChannel.subscribe<ScreenshotModule, &ScreenshotModule::handleFrameCaptured>(this)
    );

    // Live configuration reload (kernel reloadConfiguration) through the
    // configuration watch hub: one watch per output-affecting key, each
    // re-reading exactly its own value. Free when reload is disabled (nobody
    // publishes the event the hub subscribes to).
    configurationWatches.bind(bus);
    formatWatch          = configurationWatches.watch("screenshot.format", [this](const std::string &) {
        reloadFormatFromConfiguration();
    });
    jpegQualityWatch     = configurationWatches.watch("screenshot.jpeg_quality", [this](const std::string &) {
        reloadJpegQualityFromConfiguration();
    });
    filenamePrefixWatch  = configurationWatches.watch("screenshot.filename_prefix", [this](const std::string &) {
        reloadFilenamePrefixFromConfiguration();
    });
    outputDirectoryWatch = configurationWatches.watch("screenshot.output_directory", [this](const std::string &) {
        reloadOutputDirectoryFromConfiguration();
    });

    // Construct the Lua bridge extension object. The module IS the
    // ScreenshotCapability, so `this` is the capability pointer the bridge
    // calls back into. Held as the base LuaBridgeExtension pointer in the
    // member; capabilityPointerFor returns it for the scripting member name.
    luaBridge = std::make_unique<ScreenshotLuaBridge>(this);
}

void ScreenshotModule::start()
{
    // Destination resolution: relative paths resolve against the process
    // working directory, which is the project root for the documented quick
    // start (./build/runtime/engine from the engine tree). The default is a
    // working-directory 'screenshots' folder, so a normal run writes to
    // <project root>/screenshots where the user actually looks for captures.
    // Absolute paths are used as-is.
    if (outputDirectory.empty())
    {
        resolvedOutputDirectory = std::filesystem::current_path() / "screenshots";
    } else
    {
        resolvedOutputDirectory = std::filesystem::path(outputDirectory).is_relative()
                                    ? (std::filesystem::current_path() / outputDirectory)
                                    : std::filesystem::path(outputDirectory);
    }
    // The encode pool starts with the module (the completion drain runs in
    // the frame handler below); 0 = auto sizes it from the machine's
    // hardware threads, an explicit 1..8 from the configuration.
    const std::size_t startedEncodeWorkers
        = encodeWorkerCount > 0 ? static_cast<std::size_t>(encodeWorkerCount) : defaultWorkerCount();
    encoder.start(startedEncodeWorkers);
    logger->logMessage(
        LogLevel::Information,
        "screenshot module started; trigger key is '" + triggerKeyName + "', captures are written to '"
            + resolvedOutputDirectory.string() + "' (format "
            + (fileFormat == FileFormat::Png    ? "png"
                : fileFormat == FileFormat::Jpg ? "jpg"
                                                : "bmp")
            + ", " + std::to_string(startedEncodeWorkers) + " encode worker(s))"
    );
}

void ScreenshotModule::stop()
{
    // The workers stop first: a capture still queued at shutdown is dropped
    // (the run is over; nothing reads it) - logged, not silent, when one was
    // in flight.
    if (encodeTasksInFlight > 0)
    {
        logger->logMessage(
            LogLevel::Warning, "screenshot module: a capture was still encoding at shutdown; it was dropped"
        );
    }
    encoder.stop();
    keyPressedSubscription.release();
    frameBeginSubscription.release();
    frameCapturedSubscription.release();
    // The watch hub releases its reload subscription and every watch here
    // (the assignment drops the member's state); the reload* callbacks must
    // not survive the pointers they read through, which the resets below
    // null out.
    configurationWatches = ConfigurationWatchHub{};
    // Reset the Lua bridge before nulling the capability pointers it captures.
    luaBridge.reset();
    boundContext       = nullptr;
    rendererCapability = nullptr;
    logger             = nullptr;
}

void *ScreenshotModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    // The member name this module's descriptor declares: family requests
    // ("screenshot") resolve through it, so the hook answers the member, never
    // the family spelling.
    if (capabilityName == "screenshot.png")
    {
        return static_cast<ScreenshotCapability *>(this);
    }
    // The "scripting.lua_bridge.screenshot" member: returns the dedicated
    // ScreenshotLuaBridge object this module owns (constructed in construct()).
    // The lua_scripting host acquires every member of the
    // "scripting.lua_bridge" family through ComponentContext::
    // acquireEachCapability and asks each one to install its own bindings into
    // the host's engine table - this is how the screenshot bridge reaches Lua
    // without lua_scripting naming the screenshot module anywhere in its
    // source, manifest, or build files.
    if (capabilityName == "scripting.lua_bridge.screenshot")
    {
        return luaBridge.get();
    }
    return nullptr;
}

ScreenshotModule::FileFormat ScreenshotModule::fileFormatFromName(
    const std::string &name, bool &ok
)
{
    ok = true;
    if (name == "png")
    {
        return FileFormat::Png;
    }
    if (name == "jpg" || name == "jpeg")
    {
        return FileFormat::Jpg;
    }
    if (name == "bmp")
    {
        return FileFormat::Bmp;
    }
    ok = false;
    return FileFormat::Png;
}

void ScreenshotModule::reloadFormatFromConfiguration()
{
    const std::string formatName  = boundContext->configurationString("format").value_or("png");
    bool formatKnown              = false;
    const FileFormat parsedFormat = fileFormatFromName(formatName, formatKnown);
    if (formatKnown)
    {
        fileFormat = parsedFormat;
        logger->logMessage(LogLevel::Information, "screenshot module: reloaded capture format");
    } else
    {
        logger->logMessage(
            LogLevel::Error,
            "screenshot module: reload carried unknown format '" + formatName
                + "' (known: png, jpg, bmp); keeping the previous format"
        );
    }
}

void ScreenshotModule::reloadJpegQualityFromConfiguration()
{
    jpegQuality = static_cast<int>(
        std::clamp<std::int64_t>(boundContext->configurationInteger("jpeg_quality").value_or(90), 1, 100)
    );
    logger->logMessage(LogLevel::Information, "screenshot module: reloaded jpeg quality");
}

void ScreenshotModule::reloadFilenamePrefixFromConfiguration()
{
    filenamePrefix = boundContext->configurationString("filename_prefix").value_or("screenshot");
    logger->logMessage(LogLevel::Information, "screenshot module: reloaded filename prefix");
}

void ScreenshotModule::reloadOutputDirectoryFromConfiguration()
{
    // The same resolution start() applies: relative against the process
    // working directory, absolute as-is.
    outputDirectory         = boundContext->configurationString("output_directory").value_or(std::string());
    resolvedOutputDirectory = outputDirectory.empty() ? (std::filesystem::current_path() / "screenshots")
                                                      : (std::filesystem::path(outputDirectory).is_relative()
                                                                ? (std::filesystem::current_path() / outputDirectory)
                                                                : std::filesystem::path(outputDirectory));
    logger->logMessage(
        LogLevel::Information,
        "screenshot module: reloaded output directory to '" + resolvedOutputDirectory.string() + "'"
    );
}

void ScreenshotModule::requestScreenshot()
{
    captureQueued = true;
}

void ScreenshotModule::handleKeyPressed(
    const KeyPressedEvent &event
)
{
    if (static_cast<std::uint32_t>(event.keyCode) == triggerKeyCode)
    {
        captureQueued = true;
        logger->logMessage(LogLevel::Debug, "screenshot module: capture queued by key press");
    }
}

void ScreenshotModule::handleFrameBegin(
    const FrameBeginEvent &event
)
{
    // Drain the encode completions FIRST: a capture from the previous frame
    // publishes ScreenshotSavedEvent here, on the main thread, for a file
    // that is already on disk (the bus stays single threaded).
    encoder.drainCompletedTasks();
    if (captureQueued && rendererCapability != nullptr)
    {
        // The renderer publishes FrameBegin before recording commands, so the
        // capture lands inside the very same frame.
        rendererCapability->requestFrameCapture();
    }
    (void)event;
}

void ScreenshotModule::handleFrameCaptured(
    const FrameCapturedEvent &event
)
{
    if (!captureQueued || boundContext == nullptr)
    {
        return;
    }
    captureQueued = false;

    const std::filesystem::path &directory = resolvedOutputDirectory;
    // The error_code overload: the throwing form can fail on a read-only
    // path, a permissions problem or a race with another creator - an
    // exception out of this event handler would walk straight through the
    // event bus and terminate the run. Log and drop the capture instead.
    std::error_code directoryError;
    std::filesystem::create_directories(directory, directoryError);
    if (directoryError)
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Error,
                "screenshot module: cannot create output directory '" + directory.string()
                    + "': " + directoryError.message()
            );
        }
        return;
    }

    const std::string filePath = (directory / nextCaptureFilePath()).string();
    // Copy the pixels out of the renderer's staging memory NOW: the event's
    // contract bounds their lifetime to this handler, and the encode runs on
    // the worker after the frame has moved on. One bounded allocation per
    // capture (a rare, user-triggered event) - the frame path pays a memcpy,
    // never an encode.
    const std::size_t byteCount = static_cast<std::size_t>(event.width) * static_cast<std::size_t>(event.height) * 4u;
    auto pixels             = std::make_shared<std::vector<std::uint8_t>>(event.pixelData, event.pixelData + byteCount);
    const int strideBytes   = static_cast<int>(event.width) * 4;
    const int width         = static_cast<int>(event.width);
    const int height        = static_cast<int>(event.height);
    const FileFormat format = fileFormat;
    const int quality       = jpegQuality;
    ++encodeTasksInFlight;
    encoder.postWithCompletion(
        [filePath, pixels, width, height, strideBytes, format, quality]() {
            bool written = false;
            switch (format)
            {
            case FileFormat::Jpg:
                written = stbi_write_jpg(filePath.c_str(), width, height, 4, pixels->data(), quality) != 0;
                break;
            case FileFormat::Bmp:
                written = stbi_write_bmp(filePath.c_str(), width, height, 4, pixels->data()) != 0;
                break;
            case FileFormat::Png:
            default:
                written = stbi_write_png(filePath.c_str(), width, height, 4, pixels->data(), strideBytes) != 0;
                break;
            }
            if (!written)
            {
                pixels->clear(); // the completion reports the failure through the empty vector
            }
        },
        [this, filePath, pixels]() {
            --encodeTasksInFlight;
            if (logger == nullptr || boundContext == nullptr)
            {
                return; // the module stopped between the post and this drain
            }
            if (pixels->empty())
            {
                logger->logMessage(LogLevel::Error, "screenshot module: failed to write '" + filePath + "'");
                return;
            }
            logger->logMessage(LogLevel::Information, "screenshot module: saved '" + filePath + "'");

            ScreenshotSavedEvent saved;
            saved.filePath = filePath;
            boundContext->eventBus().publish(saved);
        }
    );
    ++captureCounter;
}

std::string ScreenshotModule::nextCaptureFilePath() const
{
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm timeInformation{};
#if defined(_WIN32)
    ::localtime_s(&timeInformation, &now);
#else
    ::localtime_r(&now, &timeInformation);
#endif
    const char *extension = fileFormat == FileFormat::Jpg ? ".jpg" : fileFormat == FileFormat::Bmp ? ".bmp" : ".png";
    std::ostringstream name;
    name << filenamePrefix << "_" << std::put_time(&timeInformation, "%Y%m%d_%H%M%S") << "_" << std::setw(3)
         << std::setfill('0') << (captureCounter % 1000) << extension;
    return name.str();
}

} // namespace engine
