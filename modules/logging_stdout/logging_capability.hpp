#ifndef ENGINE_MODULES_LOGGING_LOGGING_CAPABILITY_HPP
#define ENGINE_MODULES_LOGGING_LOGGING_CAPABILITY_HPP

// The published capability surface of the logging module. Every consumer -
// other modules, native mods, the app - includes THIS header, from the
// module that provides the capability: the provider owns and publishes its
// own capability vocabulary, so there is no central contract directory and
// no mirrored copy that could drift. Like every published capability header
// this stays implementation-, platform- and vendor-agnostic; the per-module
// purity guard in the module's CMakeLists enforces that mechanically.
// See mods/README.md, section "Portability".

#include <string>

namespace engine
{

enum class LogLevel
{
    Trace,
    Debug,
    Information,
    Warning,
    Error
};

// Capability "logging": message sink. Every other component depends on it
// first, which makes it the root of the dependency graph; whichever component
// provides it is just the graph's first provider.
class LoggerCapability
{
public:
    // The implementation-agnostic family name of this capability. Consumers
    // acquire through it; the logging_stdout module's plugin descriptor
    // carries the implementation-bound member name ("logging.stdout").
    static constexpr const char *capabilityName = "logging";

    virtual ~LoggerCapability() = default;

    virtual void logMessage(LogLevel level, const std::string &message) = 0;

    // Whether a message at this level would reach a sink right now - the
    // same minimum-level check logMessage applies, exposed so a caller that
    // FORMATS its message first can skip the formatting when the sink would
    // drop it anyway. A per-frame refusal path that concatenates numbers
    // into a warning string pays the concatenation (and its heap round
    // trips) even when the configuration filters that level; guarding the
    // construction with this query answers the identical question without
    // changing what gets logged. The default (true) is the conservative
    // answer: a provider that does not implement the probe never silences
    // a call site. Appended after the established surface, so the existing
    // pure-virtual contract above is untouched.
    virtual bool wouldLog(
        LogLevel level
    ) const
    {
        (void)level;
        return true;
    }
};

} // namespace engine

#endif // ENGINE_MODULES_LOGGING_LOGGING_CAPABILITY_HPP
