#ifndef ENGINE_MODULES_PROFILER_HPP
#define ENGINE_MODULES_PROFILER_HPP

#include "profiler_capability.hpp"
#include "logging_capability.hpp"
#include "engine/kernel/component_interface.hpp"
#include "engine/kernel/main_thread_scheduler.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine
{

// Module "profiler": provides the ProfilerCapability. Accumulates counters
// and named duration samples over the run and emits one aggregated report
// through the logging capability at shutdown - after every consumer has
// stopped, because the kernel stops components in reverse dependency order
// and the profiler, as a provider, stops last among its consumers.
class ProfilerModule final : public ModuleInterface, public ProfilerCapability
{
public:
    ProfilerModule();

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;

    void addCounterAmount(const std::string &name, double amount) override;
    void addSampleSeconds(const std::string &name, double seconds) override;

private:
    struct SampleStatistics
    {
        std::uint64_t count = 0;
        double totalSeconds = 0.0;
        double maxSeconds   = 0.0;
    };

    void emitReport();

    ComponentManifest componentManifest;
    LoggerCapability *logger = nullptr;
    LogLevel reportLevel     = LogLevel::Information;
    // Periodic reporting ([profiler].interval_seconds; 0 = the historical
    // stop-only report, and then nothing below is scheduled). A positive
    // interval schedules one recurring task on the kernel's main-thread
    // scheduler (ComponentContext::scheduler, ticked by the composition
    // root's frame loop): the frame path pays nothing for the feature - no
    // frame subscription, no per-frame clock read - and the report ticks in
    // renderer-less loops too, where frame events never fire.
    ScheduledTask periodicReport;
    // The configured interval, held only for the startup log line.
    double reportInterval = 0.0;
    // Unordered stores, sorted at report time: the counters and samples
    // feed one entry PER instrumented call (per frame per site, script-driven
    // ones even more), and the ordered map re-walked a red-black tree with a
    // string compare per call. The shutdown/periodic REPORT is where
    // determinism matters ("the report is deterministic across runs, which
    // makes before/after comparisons readable") - emitReport sorts the rows
    // by name, so the printed report stays byte-identical to the ordered
    // container's while every recording call pays one hash instead.
    std::unordered_map<std::string, double> counters;
    std::unordered_map<std::string, SampleStatistics> samples;
};

} // namespace engine

#endif // ENGINE_MODULES_PROFILER_HPP
