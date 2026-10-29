#ifndef HARROWMERE_CORE_DATA_CONFIG_ENGINE_HPP
#define HARROWMERE_CORE_DATA_CONFIG_ENGINE_HPP

// The engine-side half of the configuration seam (task 0.4 pinned decision:
// config reads are wrapped ONCE, "so the rest of the code never touches the
// kernel type"). This is the ONLY file under src/data/ that includes an
// engine header: it adapts the kernel's ConfigurationStore
// (kernel/include/engine/kernel/configuration.hpp) to the kernel-free
// ConfigSource the data layer and every later consumer see.
//
// Usage, where the kernel is visible (the mod's construct(), the app):
//     const harrowmere::data::EngineConfigSource source(kernel.configuration());
//     const harrowmere::data::GameConfig config = harrowmere::data::GameConfig::load(source);

#include <optional>
#include <string>

#include "engine/kernel/configuration.hpp"

#include "data/config.hpp"

namespace harrowmere::data
{

class EngineConfigSource final : public ConfigSource
{
public:
    explicit EngineConfigSource(
        const engine::ConfigurationStore &store
    )
        : store_(store)
    {}

    std::optional<std::int64_t> integer(
        const char *section, const char *key
    ) const override
    {
        return store_.integerValue(section, key);
    }

    std::optional<std::string> string(
        const char *section, const char *key
    ) const override
    {
        return store_.stringValue(section, key);
    }

private:
    const engine::ConfigurationStore &store_;
};

} // namespace harrowmere::data

#endif // HARROWMERE_CORE_DATA_CONFIG_ENGINE_HPP
