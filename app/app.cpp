// The composition root implementation (engine::App, declared in app.hpp):
// the command line surface, the frame loop, the capability acquisition and
// the shutdown sequence. The process entry point in main.cpp is deliberately
// thin; everything a run does lives here.
#include "app.hpp"

// The app includes the published capability headers of the modules it
// acquires from - the providers themselves own and publish their capability
// types; there is no central contract directory.
#include "input_capability.hpp"
#include "renderer_capability.hpp"
#include "window_capability.hpp"
#include "engine/kernel/configuration_watch.hpp"
#include "engine/kernel/filesystem_utilities.hpp"
#include "engine/kernel/version.hpp"

#if defined(_WIN32)
// The frame pacing's timer-resolution request (timeBeginPeriod) lives in
// winmm. windows.h comes FIRST: timeapi.h expects the win32 base types it
// uses (UINT, DWORD, BYTE, WINAPI, DECLARE_HANDLE) to already be defined and
// does not pull windows.h in itself, so a direct include fails on MinGW with
// "'UINT' does not name a type". windows.h is included with the process-wide
// WIN32_LEAN_AND_MEAN and NOMINMAX definitions the root CMakeLists sets.
#include <windows.h>
#include <timeapi.h>
#endif

#if defined(_MSC_VER)
// The spin-wait hint's intrinsics (_mm_pause on x86, __yield on ARM).
#include <intrin.h>
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace engine
{

namespace
{

// The default no-renderer poll interval: the loop keeps pumping events at
// roughly 60 Hz so a run without a renderer still behaves predictably. The
// [engine].idle_poll_ms configuration key overrides it; this constant is the
// last-resort fallback so the historical behavior survives even a configuration
// that never mentions the key.
constexpr std::chrono::milliseconds defaultNoRendererPollInterval{16};
// The assembly configuration default; --config selects another file.
constexpr const char *defaultConfigurationFile = "config/engine.toml";
// The frame-time percentile ring's default capacity (frames). Bounded memory
// like the bus rings; [diagnostics].frame_history resizes it.
constexpr std::size_t defaultFrameSampleCapacity = 4096;
constexpr std::size_t maxFrameSampleCapacity     = 1u << 20;
// The CPU-side frame cap's last-resort fallback: 0 = uncapped, the
// historical behavior (pacing delegated entirely to the swapchain's vsync).
constexpr std::int64_t defaultMaxFps = 0;

// The sleep-then-spin wait's spin window: the last stretch before a frame
// deadline is waited out by spinning on the clock instead of sleeping,
// because an OS wakeup is never exact (the scheduler wakes a sleeping thread
// up to a timer tick late, and every millisecond of overshoot is frame-time
// jitter a capped run pays on every frame). One millisecond keeps the spin's
// CPU cost (a few percent of one core at a 60 fps cap) far below the pacing
// accuracy it buys; an uncapped run never executes the wait at all.
constexpr std::chrono::steady_clock::duration framePacingSpinWindow{std::chrono::milliseconds(1)};

// Whether the process's standard input is an interactive terminal: the trust
// gate's consent prompt is only askable when a user can actually answer it.
// A piped or redirected stdin (scripts, CI) cannot agree to anything, so the
// handler below declines and the kernel aborts with the trust instructions.
bool standardInputIsInteractive()
{
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

// The composition root's consent surface for the kernel's trust gate: prints
// every flagged-but-untrusted component with its structural reasons and asks
// the user to agree. The answer is per-run and never remembered - a standing
// decision belongs in the configuration's [trust] section, where it is
// visible and reviewable, not buried in a prompt history.
bool askTrustConsent(
    const std::vector<ComponentTrustAdvisory> &untrustedAdvisories
)
{
    if (!standardInputIsInteractive())
    {
        std::cerr << "engine: no interactive terminal is connected, so the agreement cannot be asked for.\n";
        return false;
    }
    std::cerr << "\nThe engine will not use untrusted components that can be harmful without your agreement:\n";
    for (const ComponentTrustAdvisory &advisory : untrustedAdvisories)
    {
        std::cerr << "  - " << advisory.name << " (";
        for (std::size_t index = 0; index < advisory.reasons.size(); ++index)
        {
            if (index != 0)
            {
                std::cerr << "; ";
            }
            std::cerr << advisory.reasons[index];
        }
        std::cerr << ")\n";
    }
    std::cerr << "Review these components before agreeing. To decide permanently, add them to the"
                 " configuration's [trust] section instead.\n";
    std::cerr << "Proceed with these components? [y/N] ";
    std::string answer;
    if (!std::getline(std::cin, answer))
    {
        std::cerr << "\n";
        return false;
    }
    for (char &character : answer)
    {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return answer == "y" || answer == "yes";
}

// One iteration of de-turbo for the pacing spin's tail: the x86 PAUSE /
// ARM YIELD instruction keeps the busy clock spin from burning power and
// from stealing execution resources from the SMT sibling thread, without
// adding the wakeup overshoot a yield to the OS scheduler could introduce
// into the very interval the spin exists to keep exact. A no-op where the
// architecture names no hint.
void spinWaitHint()
{
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#if defined(_MSC_VER)
    _mm_pause();
#else
    __builtin_ia32_pause();
#endif
#elif defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64) || defined(_M_ARM)
#if defined(_MSC_VER)
    __yield();
#else
    __builtin_arm_yield();
#endif
#endif
}

// The OS timer resolution a capped run asks for, on Windows only: the
// default system timer tick (~15.6 ms) makes every sleep wake late by up to
// that much, which no spin window can recover - the wait would end after the
// deadline, not at it. timeBeginPeriod(1) buys 1 ms wakeup granularity for
// the process; the RAII guard releases it on the same failure paths that
// unwind past the frame loop. Other platforms schedule nanosleeps at sub-millisecond
// granularity by default and need no request.
#if defined(_WIN32)
class TimerResolutionRequest final
{
public:
    TimerResolutionRequest()
    {
        ::timeBeginPeriod(1);
    }

    TimerResolutionRequest(const TimerResolutionRequest &)            = delete;
    TimerResolutionRequest &operator=(const TimerResolutionRequest &) = delete;

    ~TimerResolutionRequest()
    {
        ::timeEndPeriod(1);
    }
};
#else
class TimerResolutionRequest final
{
public:
    TimerResolutionRequest()                                          = default;
    TimerResolutionRequest(const TimerResolutionRequest &)            = delete;
    TimerResolutionRequest &operator=(const TimerResolutionRequest &) = delete;
    ~TimerResolutionRequest()                                         = default;
};
#endif

// Sleeps until the deadline in two stages: sleep through the bulk of the
// interval, then spin on the clock across the final spin window. The
// deadline is the CALLER's problem after this returns (the frame loop
// re-derives it from the actual wake time, so a late wake never compresses
// the following frames into a burst).
void sleepUntilWithSpin(
    std::chrono::steady_clock::time_point deadline
)
{
    for (;;)
    {
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            return;
        }
        if (deadline - now > framePacingSpinWindow)
        {
            std::this_thread::sleep_until(deadline - framePacingSpinWindow);
        } else
        {
            // Inside the spin window: hint the core (PAUSE/YIELD), then let
            // the loop's next clock read land at or past the deadline with
            // the clock's own granularity, no sleep overshoot involved.
            spinWaitHint();
        }
    }
}

// The resolved frame-pacing knobs, re-read from the layered configuration
// once at startup and again whenever a configuration watch fires, so a live
// reload retunes pacing without a restart (a module-side concern applies the
// same pattern for its own keys).
struct FramePacing
{
    std::chrono::milliseconds noRendererPollInterval{defaultNoRendererPollInterval};
    double targetFrameSeconds = 0.0;
    // The fixed-timestep update rate for the EngineFixedUpdateEvent loop
    // (0 = the variable heartbeat alone, the default and the historical
    // behavior) and its one-iteration catch-up clamp.
    std::int64_t fixedUpdateHz       = 0;
    std::int64_t maxFixedUpdateSteps = 5;
    // A finite run length for headless and CI runs (0 = run until the window
    // closes, the default and the historical behavior).
    double quitAfterSeconds = 0.0;
};

FramePacing readPacingConfiguration(
    const ConfigurationStore &configuration
)
{
    // The historical constants are the fallback contract: pacing knobs are
    // enhancements, never a startup risk, so a store that refuses to answer
    // leaves the defaults in place.
    FramePacing pacing;
    try
    {
        const std::int64_t idlePollMs
            = configuration.integerValue(ConfigurationStore::engineSectionName, ConfigurationStore::engineIdlePollMsKey)
                  .value_or(defaultNoRendererPollInterval.count());
        pacing.noRendererPollInterval = std::chrono::milliseconds(std::clamp<std::int64_t>(idlePollMs, 1, 1000));

        const std::int64_t maxFps
            = configuration.integerValue(ConfigurationStore::engineSectionName, ConfigurationStore::engineMaxFpsKey)
                  .value_or(defaultMaxFps);
        if (maxFps > 0)
        {
            // A cap below 1 fps is a clamp artifact, not a configuration: the
            // floor keeps the deadline arithmetic inside double precision.
            pacing.targetFrameSeconds = 1.0 / static_cast<double>(std::clamp<std::int64_t>(maxFps, 1, 10000));
        }

        pacing.fixedUpdateHz = std::clamp<std::int64_t>(
            configuration
                .integerValue(ConfigurationStore::engineSectionName, ConfigurationStore::engineFixedUpdateHzKey)
                .value_or(0),
            0,
            10000
        );
        pacing.maxFixedUpdateSteps = std::clamp<std::int64_t>(
            configuration
                .integerValue(ConfigurationStore::engineSectionName, ConfigurationStore::engineMaxFixedUpdateStepsKey)
                .value_or(5),
            1,
            1000
        );
        pacing.quitAfterSeconds = std::clamp(
            configuration
                .floatingValue(ConfigurationStore::engineSectionName, ConfigurationStore::engineQuitAfterSecondsKey)
                .value_or(0.0),
            0.0,
            604800.0
        );
    } catch (const std::exception &)
    {
        pacing = FramePacing{};
    }
    return pacing;
}

void printUsage(
    std::ostream &stream
)
{
    stream << "engine " << engineVersionString << " - modular capability-driven engine kernel\n"
           << "\n"
           << "usage: engine [options]\n"
           << "\n"
           << "options:\n"
           << "  --config <file>       assembly configuration file (default config/engine.toml,\n"
           << "                        relative to the executable directory)\n"
           << "  --set <key>=<value>   stage one configuration override, the highest layer\n"
           << "                        (repeatable; TOML value syntax, e.g. --set renderer.exposure=1.4)\n"
           << "  --scene <file>        replace the world content with the file's [scene] section\n"
           << "  --mods-dir <dir>      scan <dir> for mod packages (repeatable: several\n"
           << "                        directories scanned in the given order; given flags\n"
           << "                        replace [engine].mod_directory and extra_mod_directories)\n"
           << "  --validate            dry run: configuration, manifests, graph, permissions -\n"
           << "                        no backend starts (CI-suitable; exit 0 on success)\n"
           << "  --print-graph         dry run like --validate, then print the component graph:\n"
           << "                        every active component with its capabilities, permissions\n"
           << "                        and start order, plus the event subscription map once a\n"
           << "                        run would have one (mod development; exit 0 on success)\n"
           << "  --version             print the engine version and exit\n"
           << "  --help                print this help and exit\n";
}

} // namespace

int App::run(
    int argumentCount, char *arguments[]
)
{
    // ---- command line ----------------------------------------------------------------
    // The flags map one-to-one onto hooks the kernel already exposed
    // (setConfigurationFile, addCommandLineOverride, setSceneFile,
    // setModsDirectory, validate) - the composition root simply stops being
    // the only caller that could not reach them. A malformed flag is a usage
    // error (exit 2) BEFORE any backend is touched; --validate short-circuits
    // the run after the dry pass.
    std::filesystem::path configurationFile = defaultConfigurationFile;
    std::filesystem::path sceneFile;
    std::vector<std::filesystem::path> modsDirectories;
    bool haveSceneFile = false;
    bool validateOnly  = false;
    bool printGraph    = false;
    std::vector<std::string> stagedOverrides;

    for (int index = 1; index < argumentCount; ++index)
    {
        const std::string argument = arguments[index] != nullptr ? arguments[index] : "";
        // A flag's value must exist AND be a real string: a nullptr argv slot
        // handed to std::filesystem::path is undefined behavior, and a flag
        // at the very end of the command line is a usage error anyway.
        const bool hasValue = index + 1 < argumentCount && arguments[index + 1] != nullptr;
        if (argument == "--help" || argument == "-h")
        {
            printUsage(std::cout);
            return 0;
        } else if (argument == "--version")
        {
            std::cout << "engine " << engineVersionString << "\n";
            return 0;
        } else if (argument == "--config" && hasValue)
        {
            configurationFile = arguments[++index];
        } else if (argument == "--scene" && hasValue)
        {
            sceneFile     = arguments[++index];
            haveSceneFile = true;
        } else if (argument == "--mods-dir" && hasValue)
        {
            // Repeatable: the first flag keeps the historical single-flag
            // meaning (replace [engine].mod_directory), every further flag
            // adds another scan root after it. Given flags replace the
            // configuration's mod_directory AND extra_mod_directories
            // together, so one invocation composes a mod set from several
            // locations without copying packages into one tree.
            modsDirectories.emplace_back(arguments[++index]);
        } else if (argument == "--set" && hasValue)
        {
            stagedOverrides.emplace_back(arguments[++index]);
        } else if (argument == "--validate")
        {
            validateOnly = true;
        } else if (argument == "--print-graph")
        {
            // Implies the dry run: the graph report is computed from the same
            // manifests the validation pass builds, so no backend is ever
            // touched.
            validateOnly = true;
            printGraph   = true;
        } else
        {
            std::cerr << "engine: unknown or incomplete argument '" << argument << "'\n";
            printUsage(std::cerr);
            return 2;
        }
    }

    kernel.setConfigurationFile(configurationFile);
    // The trust gate's consent surface: every real run (construct) that
    // carries flagged-but-untrusted mods or modules asks here before any of
    // their code is used. The validate dry run never enforces the gate, so
    // CI stays non-interactive.
    kernel.setTrustConsentHandler(&askTrustConsent);
    if (haveSceneFile)
    {
        kernel.setSceneFile(sceneFile);
    }
    if (!modsDirectories.empty())
    {
        kernel.setModsDirectory(modsDirectories.front());
        for (std::size_t index = 1; index < modsDirectories.size(); ++index)
        {
            kernel.addModsDirectory(modsDirectories[index]);
        }
    }
    for (const std::string &assignment : stagedOverrides)
    {
        std::string overrideError;
        if (!kernel.addCommandLineOverride(assignment, overrideError))
        {
            std::cerr << "engine: malformed --set assignment '" << assignment << "': " << overrideError << "\n";
            return 2;
        }
    }

    // ---- validate-only mode ------------------------------------------------------------
    // The kernel's dry run: configuration load, manifest build, graph
    // resolution, permission check - no backend is initialized, which is
    // what makes it CI-suitable. The report goes to stdout on success (the
    // warnings a normal run would print) and to stderr with exit 1 on
    // failure, so pipelines can rely on the exit code alone.
    if (validateOnly)
    {
        std::string validationReport;
        const bool valid = kernel.validate(validationReport);
        if (!validationReport.empty())
        {
            std::cout << validationReport;
        }
        // startupWarnings need no second print here: validate() embeds them
        // in the report on every exit path (success, resolution failure and
        // every earlier failure), and the extra accessor print this branch
        // used to add made every advisory appear twice - a double print that
        // reads as two scans having run.
        if (!valid)
        {
            return 1;
        }
        if (printGraph)
        {
            // The dry run built every manifest and resolved the graph; the
            // report walks that resolved order (nothing is constructed yet,
            // and the report says so).
            std::cout << kernel.componentGraphReport();
        }
        std::cout << "validation succeeded (no backend was started)\n";
        return 0;
    }

    // ---- startup -----------------------------------------------------------------------
    std::string report;
    if (!kernel.construct(report))
    {
        std::cerr << report;
        std::cerr << kernel.startupWarnings();
        return 1;
    }
    // Mod trust advisories are part of the user-facing startup output: the
    // engine blocks what it can enforce, and the user decides what to trust.
    if (!kernel.startupWarnings().empty())
    {
        std::cout << kernel.startupWarnings();
    }
    // start() initializes backends and runs mod code; a module or mod that
    // throws there must produce the same diagnostic-and-exit the frame loop
    // guarantees, not an uncaught exception out of main (kernel.start() is
    // documented to throw std::runtime_error for its own failures, and any
    // component's start() may throw too). A failed start leaves the kernel's
    // destructor to release whatever phase completed.
    // exitCode lives here, not inside the frame-loop block: a failed start
    // records 1 below and the run falls straight through to the shared
    // shutdown path (the frame loop itself is gated on engineStarted).
    int exitCode       = 0;
    bool engineStarted = false;
    try
    {
        kernel.start();
        engineStarted = true;
    } catch (const std::exception &error)
    {
        std::cerr << "engine startup failed: " << error.what() << "\n";
        exitCode = 1;
    }

    // The component graph report: opt-in developer output ([diagnostics]
    // graph_report), printed once after start so the "state" lines read
    // "started". A run that never sets the key never builds the report.
    try
    {
        if (kernel.configuration()
                .booleanValue(ConfigurationStore::diagnosticsSectionName, ConfigurationStore::diagnosticsGraphReportKey)
                .value_or(false))
        {
            std::cout << kernel.componentGraphReport();
        }
    } catch (const std::exception &)
    {
        // Diagnostic output; never a run failure.
    }

    // ---- frame pacing knobs --------------------------------------------------------------
    // Read after construct: the layered configuration only exists once the
    // kernel assembled it. All keys default to the historical behavior, so
    // an unconfigured run never reads an extra clock: max_fps = 0 leaves
    // pacing to the swapchain's vsync, and the no-renderer poll keeps its
    // 16 ms interval. The same reader runs for every watch the reload hub
    // fires below, so a live reload retunes pacing without a restart.
    FramePacing pacing = readPacingConfiguration(kernel.configuration());

    // The pacing keys through the configuration watch hub (the kernel
    // utility every component can use the same way): one watch per key, one
    // subscription for all of them, and a callback that fires only when a
    // key the app owns actually changed - the hand-written changedKeys scan,
    // its binding struct and its free-function adapter reduced to their
    // essential statement.
    ConfigurationWatchHub pacingWatches{kernel.eventBus()};
    // The OS timer-resolution request for capped runs, acquired the first
    // time a cap exists (at startup or through a live reload) and held for
    // the rest of the process. The optional keeps the RAII guard alive
    // exactly as long as the run is; an uncapped run never constructs one
    // and never touches the timer.
    std::optional<TimerResolutionRequest> cappedRunTimerResolution;
    if (pacing.targetFrameSeconds > 0.0)
    {
        cappedRunTimerResolution.emplace();
    }
    ConfigurationWatch pacingMaxFpsWatch = pacingWatches.watch(
        std::string(ConfigurationStore::engineSectionName) + "." + ConfigurationStore::engineMaxFpsKey,
        [this, &pacing, &cappedRunTimerResolution](const std::string &) {
            pacing = readPacingConfiguration(kernel.configuration());
            // A cap that arrives through a live reload acquires the same
            // timer-resolution request the startup cap did; a cap removed by
            // a reload keeps the request until shutdown (re-acquiring on the
            // next flip costs nothing, and the guard's release path stays
            // exactly one object).
            if (pacing.targetFrameSeconds > 0.0 && !cappedRunTimerResolution.has_value())
            {
                cappedRunTimerResolution.emplace();
            }
        }
    );
    ConfigurationWatch pacingIdlePollWatch = pacingWatches.watch(
        std::string(ConfigurationStore::engineSectionName) + "." + ConfigurationStore::engineIdlePollMsKey,
        [this, &pacing](const std::string &) { pacing = readPacingConfiguration(kernel.configuration()); }
    );
    ConfigurationWatch pacingFixedUpdateWatch = pacingWatches.watch(
        std::string(ConfigurationStore::engineSectionName) + "." + ConfigurationStore::engineFixedUpdateHzKey,
        [this, &pacing](const std::string &) { pacing = readPacingConfiguration(kernel.configuration()); }
    );
    ConfigurationWatch pacingMaxFixedStepsWatch = pacingWatches.watch(
        std::string(ConfigurationStore::engineSectionName) + "." + ConfigurationStore::engineMaxFixedUpdateStepsKey,
        [this, &pacing](const std::string &) { pacing = readPacingConfiguration(kernel.configuration()); }
    );
    ConfigurationWatch pacingQuitAfterWatch = pacingWatches.watch(
        std::string(ConfigurationStore::engineSectionName) + "." + ConfigurationStore::engineQuitAfterSecondsKey,
        [this, &pacing](const std::string &) { pacing = readPacingConfiguration(kernel.configuration()); }
    );

    // ---- live configuration reload (opt-in) -----------------------------------------------
    // [engine].config_reload_poll_seconds > 0 makes the frame loop stat the
    // assembly file at that interval; a changed mtime triggers the kernel's
    // reload (which publishes ConfigurationReloadedEvent - the pacing re-read
    // above and every subscribing module pick up their keys). The default (0)
    // keeps the frame path exactly as it was: no stat, no extra clock, no
    // feature the run never asked for.
    double configReloadPollSeconds = 0.0;
    try
    {
        configReloadPollSeconds = std::clamp(
            kernel.configuration()
                .floatingValue(
                    ConfigurationStore::engineSectionName, ConfigurationStore::engineConfigReloadPollSecondsKey
                )
                .value_or(0.0),
            0.0,
            3600.0
        );
    } catch (const std::exception &)
    {
        configReloadPollSeconds = 0.0;
    }
    const std::filesystem::path monitoredConfigurationPath = std::filesystem::path(configurationFile).is_relative()
                                                               ? (executableDirectory() / configurationFile)
                                                               : configurationFile;
    std::filesystem::file_time_type lastConfigurationWrite{};
    bool haveConfigurationWrite = false;
    auto nextReloadPoll         = std::chrono::steady_clock::now()
                                + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::duration<double>(configReloadPollSeconds)
                                );
    if (configReloadPollSeconds > 0.0)
    {
        std::error_code statError;
        const auto writeTime = std::filesystem::last_write_time(monitoredConfigurationPath, statError);
        if (!statError)
        {
            // Seed without reloading: the file was just loaded at startup.
            lastConfigurationWrite = writeTime;
            haveConfigurationWrite = true;
        }
    }

    // The frame loop belongs to the composition root, not to the kernel: the
    // kernel only mediates communication, it never owns a window or a frame.
    // The app is a consumer like any other: it acquires the capabilities the
    // active mods have pulled in, and a capability no mod demanded is simply
    // absent - the run degrades instead of assuming a backend exists. The
    // non-throwing acquire answers "is it there" in one walk: no try/catch
    // scaffolding around an availability question.
    std::unique_ptr<ComponentContext> appContext = kernel.createAppContext();

    WindowCapability *window = appContext->tryAcquireCapability<WindowCapability>();
    if (window == nullptr)
    {
        std::cout << "window capability is not available in this run; nothing to drive\n";
    }

    RendererCapability *renderer = appContext->tryAcquireCapability<RendererCapability>();
    if (renderer == nullptr)
    {
        std::cout << "renderer capability is not available in this build; running without frames\n";
    }

    // Read-only borrow of the provider's cached window list (see the
    // capability header): the local copy this binding used to make paid one
    // heap copy at startup to read front().
    static const std::vector<WindowIdentifier> noWindows{};
    const std::vector<WindowIdentifier> &windowIdentifiers
        = window != nullptr ? window->windowIdentifiers() : noWindows;
    const WindowIdentifier mainWindow = windowIdentifiers.empty() ? invalidWindowIdentifier : windowIdentifiers.front();

    // A failure anywhere in the frame loop (a missing shader asset, a backend
    // that refuses a pipeline, a mod that throws during dispatch) must produce
    // a diagnostic and a non-zero exit instead of an uncaught exception.
    // (exitCode itself is declared with engineStarted above - a failed start
    // shares this variable and the shutdown path below.)
    std::uint64_t iterations = 0;
    double totalFrameSeconds = 0.0;
    double worstFrameSeconds = 0.0;
    // Frame-time percentile support, only maintained when the latency report
    // is enabled ([diagnostics] latency_report): a bounded ring (capacity from
    // [diagnostics].frame_history, default 4096 slots) plus a lifetime sum for
    // the mean.
    std::size_t frameSampleCapacity = defaultFrameSampleCapacity;
    std::vector<double> frameSamples;
    try
    {
        const std::int64_t configuredCapacity
            = kernel.configuration()
                  .integerValue(
                      ConfigurationStore::diagnosticsSectionName, ConfigurationStore::diagnosticsFrameHistoryKey
                  )
                  .value_or(static_cast<std::int64_t>(defaultFrameSampleCapacity));
        frameSampleCapacity
            = static_cast<std::size_t>(std::clamp<std::int64_t>(configuredCapacity, 0, maxFrameSampleCapacity));
    } catch (const std::exception &)
    {
        // Same fallback contract as the pacing knobs above.
    }
    frameSamples.reserve(frameSampleCapacity);
    if (kernel.eventBus().latencyRecordingEnabled() && frameSampleCapacity > 0)
    {
        frameSamples.resize(frameSampleCapacity);
    }
    std::size_t frameSampleCount  = 0;
    std::size_t frameSampleNext   = 0;
    const bool diagnosticsEnabled = !frameSamples.empty();
    // The frame clock: the two loop-owned channels the composition root
    // publishes. The heartbeat decouples gameplay cadence from rendering
    // cadence (docs/architecture.md, "The frame clock"): modules tick from
    // it instead of the renderer's FrameBeginEvent, so a headless run - no
    // window, no renderer - keeps ticking timers, physics, behavior trees,
    // tweens, the net pump and script frame handlers. Both publishes are
    // gated on the channel's subscriber count: a run whose components never
    // subscribe pays one subscriberCount() check per iteration and nothing
    // else (no clock read, no event construction).
    EventChannel<EngineFrameEvent> &frameHeartbeatChannel    = kernel.eventBus().channel<EngineFrameEvent>();
    EventChannel<EngineFixedUpdateEvent> &fixedUpdateChannel = kernel.eventBus().channel<EngineFixedUpdateEvent>();
    try
    {
        // The CPU-side deadline of the previous frame; only consulted when a
        // cap is configured (targetFrameSeconds > 0), so an uncapped run pays
        // no clock read for pacing beyond the per-frame measurement it always
        // did.
        std::chrono::steady_clock::time_point nextFrameDeadline = std::chrono::steady_clock::now();
        // The frame clock's timing state: previous-iteration wall time for
        // the heartbeat delta, the accumulated fixed-step debt, and the loop
        // start used by [engine].quit_after_seconds. All live only when their
        // feature has subscribers / is configured.
        std::chrono::steady_clock::time_point lastHeartbeatTime{};
        bool haveHeartbeatTime                                    = false;
        double elapsedHeartbeatSeconds                            = 0.0;
        double fixedUpdateAccumulator                             = 0.0;
        const std::chrono::steady_clock::time_point loopStartTime = std::chrono::steady_clock::now();
        // The quit condition. engineStarted gates a failed start() (the
        // diagnostic is already out; the loop must not pump a half-started
        // engine). window and mainWindow are constant for the loop (created
        // above, never re-created); the close flag itself only changes inside
        // pollEvents, so one check per frame - right after the poll - covers
        // every possible transition. A run with no window (headless, or a
        // renderer without a windowing backend) STAYS in the loop: the
        // scheduler ticks, deferred events flush, the reload poll runs and a
        // window-less renderer keeps rendering - the composition root's
        // per-frame services never silently stop because no window can close.
        // A run that configured [engine].quit_after_seconds instead leaves
        // the loop on its own once the limit is reached (the headless and CI
        // shape: a finite smoke run with the clean-shutdown path and exit 0).
        const bool closeCapable = window != nullptr && mainWindow != invalidWindowIdentifier;
        while (engineStarted)
        {
            if (pacing.targetFrameSeconds > 0.0)
            {
                // Sleep to the deadline (bulk sleep, final spin - see
                // sleepUntilWithSpin), then re-derive it from the actual
                // wake time: sleeping drifts (wakeups overshoot), and
                // anchoring each deadline to the real previous wakeup keeps a
                // slow frame from compressing every later frame into a burst.
                sleepUntilWithSpin(nextFrameDeadline);
                const auto now    = std::chrono::steady_clock::now();
                nextFrameDeadline = now
                                  + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                      std::chrono::duration<double>(pacing.targetFrameSeconds)
                                  );
                // A frame that rendered past its deadline must not wait an
                // additional full interval on top of the overshoot.
                if (nextFrameDeadline < now)
                {
                    nextFrameDeadline = now;
                }
            }
            if (closeCapable)
            {
                window->pollEvents();
                if (window->windowCloseRequested(mainWindow))
                {
                    break;
                }
            }
            // The kernel's two per-frame services, one statement each and
            // both no-ops on an idle frame: due timed callbacks (one
            // empty-vector check when nothing is scheduled) and deferred
            // event delivery (one relaxed atomic load when nothing was
            // deferred). They sit between input polling and rendering, the
            // same window the async completion drains live in.
            kernel.scheduler().tick();
            kernel.eventBus().flushDeferredEvents();
            if (configReloadPollSeconds > 0.0)
            {
                // The opt-in reload poll: one stat at the configured
                // interval, nothing on the other frames. Runs on the main
                // thread between polls and render, so the reload's event
                // handlers see the same single-threaded world a frame
                // handler does.
                const auto now = std::chrono::steady_clock::now();
                if (now >= nextReloadPoll)
                {
                    nextReloadPoll = now
                                   + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                       std::chrono::duration<double>(configReloadPollSeconds)
                                   );
                    std::error_code statError;
                    const auto writeTime = std::filesystem::last_write_time(monitoredConfigurationPath, statError);
                    if (!statError)
                    {
                        if (haveConfigurationWrite && writeTime != lastConfigurationWrite)
                        {
                            std::string reloadReport;
                            if (kernel.reloadConfiguration(reloadReport))
                            {
                                std::cout << reloadReport;
                            } else
                            {
                                // A refused reload keeps the running
                                // configuration; the diagnostic goes to the
                                // error stream and the run continues.
                                std::cerr << reloadReport;
                            }
                        }
                        lastConfigurationWrite = writeTime;
                        haveConfigurationWrite = true;
                    }
                }
            }
            // The frame clock, at the same point the renderer's FrameBegin
            // used to occupy in the ordering: after input polling, the kernel
            // services and the reload poll, before the render. The heartbeat
            // first (the variable wall-clock delta the gameplay consumers
            // keep their seeded-clock contracts against), then zero or more
            // fixed updates from the opt-in accumulator loop.
            if (frameHeartbeatChannel.subscriberCount() > 0)
            {
                const std::chrono::steady_clock::time_point heartbeatNow = std::chrono::steady_clock::now();
                const double heartbeatDelta
                    = haveHeartbeatTime ? std::chrono::duration<double>(heartbeatNow - lastHeartbeatTime).count() : 0.0;
                lastHeartbeatTime = heartbeatNow;
                haveHeartbeatTime = true;
                elapsedHeartbeatSeconds += heartbeatDelta;

                EngineFrameEvent heartbeat;
                heartbeat.frameIndex     = iterations;
                heartbeat.deltaSeconds   = heartbeatDelta;
                heartbeat.elapsedSeconds = elapsedHeartbeatSeconds;
                frameHeartbeatChannel.publish(heartbeat);

                if (pacing.fixedUpdateHz > 0 && fixedUpdateChannel.subscriberCount() > 0)
                {
                    // The classic accumulator loop: advance by whole steps,
                    // clamp the debt so a stalled frame yields a bounded burst
                    // (max_fixed_update_steps), and keep the remainder for the
                    // next iteration instead of dropping it (small steps keep
                    // most of it anyway). stepSeconds derives from the LIVE
                    // pacing value, so a reload retunes the step the same way
                    // it retunes max_fps.
                    const double stepSeconds = 1.0 / static_cast<double>(pacing.fixedUpdateHz);
                    fixedUpdateAccumulator += heartbeatDelta;
                    const double maximumDebt = stepSeconds * static_cast<double>(pacing.maxFixedUpdateSteps);
                    if (fixedUpdateAccumulator > maximumDebt)
                    {
                        fixedUpdateAccumulator = maximumDebt;
                    }
                    std::uint32_t stepsRun = 0;
                    while (fixedUpdateAccumulator >= stepSeconds && stepsRun < pacing.maxFixedUpdateSteps)
                    {
                        EngineFixedUpdateEvent update;
                        update.deltaSeconds = stepSeconds;
                        fixedUpdateChannel.publish(update);
                        fixedUpdateAccumulator -= stepSeconds;
                        stepsRun += 1;
                    }
                }
            }
            if (renderer != nullptr)
            {
                // The two clock reads per frame serve the latency report
                // only ([diagnostics] latency_report); a normal run never
                // reads them, so it never pays for them.
                std::chrono::steady_clock::time_point frameStart;
                if (diagnosticsEnabled)
                {
                    frameStart = std::chrono::steady_clock::now();
                }
                renderer->renderFrame();
                if (diagnosticsEnabled)
                {
                    const double frameSeconds
                        = std::chrono::duration<double>(std::chrono::steady_clock::now() - frameStart).count();
                    totalFrameSeconds += frameSeconds;
                    worstFrameSeconds             = std::max(worstFrameSeconds, frameSeconds);
                    frameSamples[frameSampleNext] = frameSeconds;
                    frameSampleNext               = (frameSampleNext + 1) % frameSampleCapacity;
                    frameSampleCount              = std::min(frameSampleCount + 1, frameSampleCapacity);
                }
            } else
            {
                std::this_thread::sleep_for(pacing.noRendererPollInterval);
            }
            ++iterations;
            if (pacing.quitAfterSeconds > 0.0)
            {
                // The finite-run check: one clock read per iteration, and
                // only while the key is configured. The break lands on the
                // clean-shutdown path (waitUntilIdle, the opt-in reports,
                // stop) exactly like a window close does.
                const std::chrono::steady_clock::time_point loopNow = std::chrono::steady_clock::now();
                if (std::chrono::duration<double>(loopNow - loopStartTime).count() >= pacing.quitAfterSeconds)
                {
                    break;
                }
            }
        }
    } catch (const std::exception &error)
    {
        std::cerr << "engine frame loop failed: " << error.what() << "\n";
        exitCode = 1;
    }

    // The pacing watch hub retires before the kernel stops, in the same
    // discipline the raw reload subscription always had: the bus's channels
    // die inside stop() while the plugin libraries are still loaded, and the
    // reload handler has nothing to say to a shutting-down engine. Releasing
    // the watches and re-assigning the hub un-subscribes everything now.
    pacingMaxFpsWatch.release();
    pacingIdlePollWatch.release();
    pacingFixedUpdateWatch.release();
    pacingMaxFixedStepsWatch.release();
    pacingQuitAfterWatch.release();
    pacingWatches = ConfigurationWatchHub{};

    if (renderer != nullptr)
    {
        renderer->waitUntilIdle();
        if (exitCode == 0 && renderer->validationErrorsObserved())
        {
            std::cerr << "engine observed graphics backend validation errors during this run\n";
            exitCode = 3;
        }
    }

    // Communication and frame latency report: OPT-IN through the
    // configuration ([diagnostics] latency_report). A normal run publishes
    // thousands of events per second and its user does not want a data dump
    // on shutdown, so both measurement (see EventBus::setLatencyRecordingEnabled)
    // and printing are disabled unless the configuration enables them. With
    // it enabled, the bus numbers are the definitive dispatch latencies
    // measured in publish(), and the frame block reports mean plus the
    // p50/p95/p99/max over the most recent ring window.
    try
    {
        if (kernel.eventBus().latencyRecordingEnabled())
        {
            const std::string latencyReport = appContext->eventBus().latencyReport();
            if (!latencyReport.empty())
            {
                std::cout << "communication latency report (event bus, direct dispatch):\n" << latencyReport;
            }
            if (iterations > 0 && renderer != nullptr && frameSampleCapacity > 0)
            {
                std::vector<double> ordered(
                    frameSamples.begin(), frameSamples.begin() + static_cast<std::ptrdiff_t>(frameSampleCount)
                );
                std::sort(ordered.begin(), ordered.end());
                const auto percentile = [&ordered](std::size_t fraction) {
                    const std::size_t index = std::min(ordered.size() - 1, ordered.size() * fraction / 100);
                    return ordered[index];
                };
                std::cout << "frame performance: " << iterations << " frames, mean "
                          << (totalFrameSeconds / static_cast<double>(iterations)) * 1000.0 << " ms, p50 "
                          << percentile(50) * 1000.0 << " ms, p95 " << percentile(95) * 1000.0 << " ms, p99 "
                          << percentile(99) * 1000.0 << " ms, worst " << worstFrameSeconds * 1000.0
                          << " ms (percentiles over the most recent " << frameSampleCount << " frames)\n";
            }
        }
    } catch (const std::exception &)
    {
        // The report is diagnostic; never turn it into a run failure.
    }

    kernel.stop();
    return exitCode;
}

} // namespace engine
