#include "profiler.hpp"

#include "logging_capability.hpp"

#include <cstdio>
#include <cstdarg>
#include <string>
#include <utility>

namespace engine
{

namespace
{

LogLevel parseReportLevel(
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

// One report row, formatted like snprintf but never truncated. Report names
// are script-provided (any counter/sample name), so a row longer than the
// stack buffer is re-rendered into a std::string sized by vsnprintf's
// return - a clipped name would be a wrong diagnostic.
std::string formatRow(
    const char *format, ...
)
{
    char stackBuffer[512];
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(stackBuffer, sizeof(stackBuffer), format, arguments);
    va_end(arguments);
    if (written < 0 || written < static_cast<int>(sizeof(stackBuffer)))
    {
        return std::string(stackBuffer);
    }
    std::string row(static_cast<std::size_t>(written) + 1, '\0');
    va_start(arguments, format);
    std::vsnprintf(row.data(), row.size(), format, arguments);
    va_end(arguments);
    row.resize(static_cast<std::size_t>(written));
    return row;
}

} // namespace

ProfilerModule::ProfilerModule()
{
    componentManifest = ComponentManifest{
        .name                 = "profiler",
        .kind                 = ComponentKind::Module,
        .version              = "0.1.0",
        // The implementation-bound member of the "profiler" family: the CPU-side
        // instrumentation implementation. Consumers demand the family; a run
        // that needs THIS implementation names the member exactly.
        .providedCapabilities = {"profiler.cpu"},
        .requiredCapabilities = {"logging"},
        .optionalCapabilities = {},
        .requiredComponents   = {},
        .requiredPermissions  = {},
        .knownConfigKeys      = {"profiler.report_level", "profiler.interval_seconds"},
    };
}

void ProfilerModule::construct(
    ComponentContext &context
)
{
    // The one cross-component touch of the construct phase: the report sink.
    // The requirement is declared in the manifest and in the static plugin
    // descriptor, so the grant ("capability.logging") was derived from the
    // requirement and the provider is ordered before this module.
    logger = &context.acquireCapability<LoggerCapability>();

    const std::string levelName = context.configurationString("report_level").value_or("information");
    reportLevel                 = parseReportLevel(levelName);

    // Periodic reporting ([profiler].interval_seconds; 0 = the historical
    // stop-only report, the default). A positive interval schedules one
    // recurring task on the kernel's main-thread scheduler instead of the
    // historical frame-event subscription: no handler joins the FrameBegin
    // dispatch scan, no clock is read on frames whose window has no report
    // due, and renderer-less runs (a headless capture pipeline, a tooling
    // assembly) still see periodic reports, because the composition root
    // ticks the scheduler every frame-loop iteration regardless of who, if
    // anyone, renders. The interval clamps to 3600 s; the task handle is the
    // RAII member that cancels the schedule at teardown.
    const double interval = std::clamp(context.configurationFloating("interval_seconds").value_or(0.0), 0.0, 3600.0);
    if (interval > 0.0)
    {
        periodicReport = context.scheduler().scheduleEvery(interval, [this]() { emitReport(); });
        // Kept as a member only for the startup line below: the schedule
        // itself owns the timing, the interval is what the log reports.
        reportInterval = interval;
    }
}

void ProfilerModule::start()
{
    if (logger != nullptr)
    {
        logger->logMessage(LogLevel::Information, "profiler module started");
        if (reportInterval > 0.0)
        {
            logger->logMessage(
                LogLevel::Information, "profiler module: periodic report every " + std::to_string(reportInterval) + " s"
            );
        }
    }
}

void ProfilerModule::stop()
{
    // Retire the periodic schedule before the final report: a recurring task
    // must not race the shutdown sequence it is being retired by (and the
    // kernel clears the scheduler right after the shutdown event anyway -
    // this release is the component-side half of the same discipline).
    periodicReport.cancel();
    emitReport();
}

void ProfilerModule::addCounterAmount(
    const std::string &name, double amount
)
{
    counters[name] += amount;
}

void ProfilerModule::addSampleSeconds(
    const std::string &name, double seconds
)
{
    SampleStatistics &statistics = samples[name];
    statistics.count += 1;
    statistics.totalSeconds += seconds;
    statistics.maxSeconds = statistics.maxSeconds > seconds ? statistics.maxSeconds : seconds;
}

void ProfilerModule::emitReport()
{
    if (logger == nullptr)
    {
        return; // never constructed: nothing was measured, nothing to report
    }

    logger->logMessage(
        reportLevel, formatRow("profiler report: %zu counter(s), %zu sample group(s)", counters.size(), samples.size())
    );

    // Sorted at report time (see the container comment in profiler.hpp):
    // the stores are unordered, the printed rows keep the name order a
    // diffable report wants. Row pointers only - no copies of the names.
    std::vector<const std::pair<const std::string, double> *> counterRows;
    counterRows.reserve(counters.size());
    for (const auto &entry : counters)
    {
        counterRows.push_back(&entry);
    }
    std::sort(
        counterRows.begin(),
        counterRows.end(),
        [](const std::pair<const std::string, double> *left, const std::pair<const std::string, double> *right) {
            return left->first < right->first;
        }
    );
    for (const auto *entry : counterRows)
    {
        logger->logMessage(reportLevel, formatRow("  counter '%s': total %.3f", entry->first.c_str(), entry->second));
    }

    std::vector<const std::pair<const std::string, SampleStatistics> *> sampleRows;
    sampleRows.reserve(samples.size());
    for (const auto &entry : samples)
    {
        sampleRows.push_back(&entry);
    }
    std::sort(
        sampleRows.begin(),
        sampleRows.end(),
        [](const std::pair<const std::string, SampleStatistics> *left,
            const std::pair<const std::string, SampleStatistics> *right) { return left->first < right->first; }
    );
    for (const auto *entry : sampleRows)
    {
        const SampleStatistics &statistics = entry->second;
        const double meanSeconds
            = statistics.count > 0 ? statistics.totalSeconds / static_cast<double>(statistics.count) : 0.0;
        logger->logMessage(
            reportLevel,
            formatRow(
                "  sample '%s': %llu call(s), total %.4f s, mean %.4f s, max %.4f s",
                entry->first.c_str(),
                static_cast<unsigned long long>(statistics.count),
                statistics.totalSeconds,
                meanSeconds,
                statistics.maxSeconds
            )
        );
    }
}

void *ProfilerModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    // The member name this module's descriptor declares: family requests
    // ("profiler") resolve through it, so the hook answers the member, never
    // the family spelling.
    if (capabilityName == "profiler.cpu")
    {
        return static_cast<ProfilerCapability *>(this);
    }
    return nullptr;
}

} // namespace engine
