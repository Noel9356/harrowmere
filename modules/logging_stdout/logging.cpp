#include "logging.hpp"

#include "engine/kernel/filesystem_utilities.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#if defined(_WIN32)
// WIN32_LEAN_AND_MEAN and NOMINMAX are defined for the whole build through
// the root CMakeLists; windows.h must not be included without them.
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace engine
{

namespace
{

bool standardOutputIsTerminal()
{
#if defined(_WIN32)
    return ::_isatty(::_fileno(stdout)) != 0;
#else
    return ::isatty(::fileno(stdout)) != 0;
#endif
}

#if defined(_WIN32)
// The level colors are ANSI escape sequences, which the classic Windows
// console host (conhost) ignores unless virtual terminal processing is
// enabled for the output handle - the raw escapes would print as garbage.
// Windows Terminal and VT-capable hosts need nothing, and a non-console
// stdout (a pipe, a file) is never touched. Failure is deliberately
// ignored: an uncolored log line is cosmetic degradation, never an error.
void enableAnsiColorsOnLegacyConsole()
{
    const HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == INVALID_HANDLE_VALUE || output == nullptr)
    {
        return;
    }
    DWORD consoleMode = 0;
    if (::GetConsoleMode(output, &consoleMode) != 0)
    {
        ::SetConsoleMode(output, consoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}
#endif

LogLevel parseLevelName(
    const std::string &levelName
)
{
    if (levelName == "trace")
    {
        return LogLevel::Trace;
    }
    if (levelName == "debug")
    {
        return LogLevel::Debug;
    }
    if (levelName == "information")
    {
        return LogLevel::Information;
    }
    if (levelName == "warning")
    {
        return LogLevel::Warning;
    }
    if (levelName == "error")
    {
        return LogLevel::Error;
    }
    return LogLevel::Information;
}

} // namespace

LoggingModule::LoggingModule()
{
    componentManifest = ComponentManifest{
        .name    = "logging",
        .kind    = ComponentKind::Module,
        .version = "0.1.0",
        // The implementation-bound member of the "logging" family: the console
        // (stdout) sink implementation. Consumers demand the family; a run that
        // needs THIS sink names the member exactly.
        .providedCapabilities = {"logging.stdout"},
        .requiredCapabilities = {},
        .optionalCapabilities = {},
        .requiredComponents   = {},
        .requiredPermissions  = {},
        .knownConfigKeys      = {"logging.level", "logging.file", "logging.flush", "logging.timestamp_millis"},
    };
}

void LoggingModule::construct(
    ComponentContext &context
)
{
    const std::string levelName = context.configurationString("level").value_or("information");
    minimumLevel                = parseLevelName(levelName);
    // One terminal probe for the process lifetime (see the member comment).
    // On Windows a classic console host first needs virtual terminal
    // processing enabled or the level colors would print as raw escapes.
#if defined(_WIN32)
    outputIsTerminal = standardOutputIsTerminal();
    if (outputIsTerminal)
    {
        enableAnsiColorsOnLegacyConsole();
    }
#else
    outputIsTerminal = standardOutputIsTerminal();
#endif

    // Flush policy: "always" (the default and the historical behavior)
    // flushes stdout and the file sink after every line - crash-accurate
    // logs, at the cost of a write syscall per message. "warning" flushes
    // only when the message is warning-or-worse, which keeps diagnostics
    // durable while trace/info lines ride the stream buffer (measurably less
    // IO on chatty frames). "never" leaves every flush to the streams
    // themselves; the file sink still flushes once at stop. A run that logs
    // thousands of frame-rate events is the use case; a quiet run cannot
    // measure the difference. An unknown name fails the construct phase
    // (a misconfigured sink must not start silently); the live-reload path
    // keeps the previous policy instead - see applyFlushPolicyFromConfiguration.
    const std::string flushPolicyName = context.configurationString("flush").value_or("always");
    if (!flushPolicyFromName(flushPolicyName, flushPolicy))
    {
        throw std::runtime_error(
            "logging module: [logging] flush '" + flushPolicyName + "' is not a known policy (always, warning, never)"
        );
    }

    // Sub-second timestamps: false (the default) keeps the historical
    // %H:%M:%S form; true appends ".mmm" so frame-loop diagnostics can tell
    // two messages in one second apart.
    timestampMilliseconds = context.configurationBoolean("timestamp_millis").value_or(false);

    const std::string fileName = context.configurationString("file").value_or("");
    if (!fileName.empty())
    {
        const std::filesystem::path filePath = std::filesystem::path(fileName);
        if (filePath.is_relative())
        {
            fileSink.open(executableDirectory() / filePath, std::ios::out | std::ios::app);
        } else
        {
            fileSink.open(filePath, std::ios::out | std::ios::app);
        }
        // A failed file sink must not stay silent - the run would believe
        // its logs are durable when they are not. logMessage is safe here:
        // the sink is not open, so the warning cannot recurse into itself.
        if (!fileSink.is_open())
        {
            logMessage(
                LogLevel::Warning,
                "logging module: cannot open log file '" + filePath.string() + "' - continuing with stdout only"
            );
        }
    }

    // Live configuration reload (kernel reloadConfiguration) through the
    // configuration watch hub: one watch per retunable key, each re-reading
    // exactly its own value. Free while reload is disabled (nobody publishes
    // the event the hub subscribes to); on a reload, a changed key applies
    // without a restart - the classic dev-loop tuning story for verbosity.
    boundContext = &context;
    configurationWatches.bind(context.eventBus());
    levelWatch
        = configurationWatches.watch("logging.level", [this](const std::string &) { applyLevelFromConfiguration(); });
    flushWatch = configurationWatches.watch("logging.flush", [this](const std::string &) {
        applyFlushPolicyFromConfiguration();
    });
}

void LoggingModule::start()
{
    logMessage(LogLevel::Information, "logging module started");
}

bool LoggingModule::flushPolicyFromName(
    const std::string &name, FlushPolicy &policy
)
{
    if (name == "always")
    {
        policy = FlushPolicy::Always;
        return true;
    }
    if (name == "warning")
    {
        policy = FlushPolicy::WarningAndAbove;
        return true;
    }
    if (name == "never")
    {
        policy = FlushPolicy::Never;
        return true;
    }
    return false;
}

void LoggingModule::applyLevelFromConfiguration()
{
    if (boundContext == nullptr)
    {
        return;
    }
    // Same parse contract as construct: an unknown name falls back to
    // information (the default), so a reload behaves exactly like a restart
    // with the same file. The confirmation logs BEFORE the new level
    // applies, under the level the sink was running at - a confirmation the
    // new level would filter must not silently swallow itself.
    const std::string levelName = boundContext->configurationString("level").value_or("information");
    logMessage(LogLevel::Information, "logging module: minimum level reloading to '" + levelName + "'");
    minimumLevel = parseLevelName(levelName);
}

void LoggingModule::applyFlushPolicyFromConfiguration()
{
    if (boundContext == nullptr)
    {
        return;
    }
    // Keep-last-good on an invalid live value: a half-edited file must not
    // take the sink's durability down mid-run (construct still fails the
    // startup for the same typo - the two paths differ on purpose).
    FlushPolicy parsedPolicy          = FlushPolicy::Always;
    const std::string flushPolicyName = boundContext->configurationString("flush").value_or("always");
    if (flushPolicyFromName(flushPolicyName, parsedPolicy))
    {
        flushPolicy = parsedPolicy;
        logMessage(LogLevel::Information, "logging module: flush policy reloaded to '" + flushPolicyName + "'");
    } else
    {
        logMessage(
            LogLevel::Warning,
            "logging module: [logging] flush '" + flushPolicyName
                + "' is not a known policy (always, warning, never) - keeping the previous policy"
        );
    }
}

void LoggingModule::stop()
{
    // The watch hub retires with the component (the assignment releases the
    // reload subscription and every watch); the callbacks must not survive
    // the state they write to.
    configurationWatches = ConfigurationWatchHub{};
    boundContext         = nullptr;
    if (fileSink.is_open())
    {
        fileSink.flush();
        fileSink.close();
    }
}

void *LoggingModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    // The member name this module's descriptor declares: family requests
    // ("logging") resolve through it, so the hook answers the member, never
    // the family spelling.
    if (capabilityName == "logging.stdout")
    {
        return static_cast<LoggerCapability *>(this);
    }
    return nullptr;
}

const char *LoggingModule::levelTag(
    LogLevel level
)
{
    switch (level)
    {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Information:
        return "INFO ";
    case LogLevel::Warning:
        return "WARN ";
    case LogLevel::Error:
        return "ERROR";
    }
    return "LOG  ";
}

const char *LoggingModule::levelColor(
    LogLevel level
)
{
    switch (level)
    {
    case LogLevel::Trace:
        return "\033[90m";
    case LogLevel::Debug:
        return "\033[36m";
    case LogLevel::Information:
        return "\033[32m";
    case LogLevel::Warning:
        return "\033[33m";
    case LogLevel::Error:
        return "\033[31m";
    }
    return "";
}

void LoggingModule::logMessage(
    LogLevel level, const std::string &message
)
{
    if (static_cast<int>(level) < static_cast<int>(minimumLevel))
    {
        return;
    }

    // ONE clock read for the whole line: seconds and milliseconds derive
    // from the same time_point. (The previous shape read the clock twice,
    // and a line stamped .999 by the first read could format .001 - or the
    // next second - from the second read: time running backwards inside
    // one log line.)
    const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    const std::time_t seconds                       = std::chrono::system_clock::to_time_t(now);
    std::tm timeInformation{};
#if defined(_WIN32)
    ::localtime_s(&timeInformation, &seconds);
#else
    ::localtime_r(&seconds, &timeInformation);
#endif
    char timestamp[32] = {};
    std::strftime(timestamp, sizeof(timestamp), "%H:%M:%S", &timeInformation);
    // The optional sub-second suffix comes from the SAME time_point (no
    // extra clock read, no seam): milliseconds since the last whole second.
    char millisecondSuffix[8] = {};
    if (timestampMilliseconds)
    {
        const auto sinceEpoch = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        std::snprintf(millisecondSuffix, sizeof(millisecondSuffix), ".%03d", static_cast<int>(sinceEpoch % 1000));
    }

    const bool flushNow = flushPolicy == FlushPolicy::Always
                       || (flushPolicy == FlushPolicy::WarningAndAbove
                           && static_cast<int>(level) >= static_cast<int>(LogLevel::Warning));

    const std::lock_guard<std::mutex> guard(sinkMutex);
    if (outputIsTerminal)
    {
        std::cout << levelColor(level) << "[" << timestamp << millisecondSuffix << " " << levelTag(level) << "]\033[0m "
                  << message << "\n";
    } else
    {
        std::cout << "[" << timestamp << millisecondSuffix << " " << levelTag(level) << "] " << message << "\n";
    }
    if (flushNow)
    {
        std::cout.flush();
    }

    if (fileSink.is_open())
    {
        fileSink << "[" << timestamp << millisecondSuffix << " " << levelTag(level) << "] " << message << "\n";
        if (flushNow)
        {
            fileSink.flush();
        }
    }
}

} // namespace engine
