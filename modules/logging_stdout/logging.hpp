#ifndef ENGINE_MODULES_LOGGING_HPP
#define ENGINE_MODULES_LOGGING_HPP

#include "logging_capability.hpp"
#include "engine/kernel/component_interface.hpp"
#include "engine/kernel/configuration_watch.hpp"

#include <fstream>
#include <mutex>
#include <string>

namespace engine
{

// Module "logging_stdout": provides the LoggerCapability through a console sink and an
// optional file sink. Console output is colored when the stream is a terminal.
class LoggingModule final : public ModuleInterface, public LoggerCapability
{
public:
    LoggingModule();

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;

    void logMessage(LogLevel level, const std::string &message) override;
    // LoggerCapability: the same minimum-level gate logMessage opens with,
    // exposed to callers that format their message before calling (a
    // refusal path's number-to-string concatenation) so they can skip the
    // formatting when this configuration would drop the line anyway. Reads
    // only minimumLevel - const, and safe from any thread logMessage is.
    bool wouldLog(
        LogLevel level
    ) const override
    {
        return static_cast<int>(level) >= static_cast<int>(minimumLevel);
    }

private:
    // When logMessage flushes the sinks ([logging].flush). Always is the
    // historical behavior and the default; the other two trade crash-time
    // durability for fewer write syscalls on chatty runs.
    enum class FlushPolicy
    {
        Always,
        WarningAndAbove,
        Never
    };

    static const char *levelTag(LogLevel level);
    static const char *levelColor(LogLevel level);

    ComponentManifest componentManifest;
    LogLevel minimumLevel   = LogLevel::Information;
    FlushPolicy flushPolicy = FlushPolicy::Always;
    // [logging].timestamp_millis: append ".mmm" to the timestamp (default
    // off = the historical %H:%M:%S form).
    bool timestampMilliseconds = false;
    // Live configuration reload through the configuration watch hub (the
    // same pattern the screenshot module runs): one watch per retunable key
    // (level, flush). Free while reload is disabled - nobody publishes the
    // event the hub subscribes to - and on a reload a changed key re-reads
    // exactly its own value. The file sink is deliberately not retunable
    // live: reopening files mid-run is a resource decision, not a value
    // update (the next run picks the key up).
    void applyLevelFromConfiguration();
    void applyFlushPolicyFromConfiguration();
    static bool flushPolicyFromName(const std::string &name, FlushPolicy &policy);
    ConfigurationWatchHub configurationWatches;
    ConfigurationWatch levelWatch;
    ConfigurationWatch flushWatch;
    // The context the watches re-read through (heap anchored by the kernel;
    // nulled at stop so the callbacks cannot outlive their source).
    ComponentContext *boundContext = nullptr;
    // Terminal detection is answered once: the stdout file description does
    // not change type for a running process, and the check is an isatty
    // syscall per log line otherwise. A redirected run that later attaches a
    // terminal is not a supported workflow (and would only affect color).
    bool outputIsTerminal = false;
    std::ofstream fileSink;
    std::mutex sinkMutex;
};

} // namespace engine

#endif // ENGINE_MODULES_LOGGING_HPP
