#ifndef ENGINE_MODULES_SETTINGS_HPP
#define ENGINE_MODULES_SETTINGS_HPP

#include "logging_capability.hpp"
#include "lua_bridge_extension.hpp"
#include "settings_capability.hpp"
#include "engine/kernel/component_interface.hpp"

#include <map>
#include <memory>
#include <string>

namespace engine
{

// Module "settings": provides the SettingsCapability (family member
// "settings.toml").
//
// One in-memory store of scalar values, loaded from the configured TOML
// file at start (a missing file is a first run, not an error) and written
// back explicitly or on every mutation ([settings] autosave). The store is
// std::map, not unordered: a save is deterministic across runs, which makes
// the file diffable and keeps the shipped-configs ethos of this tree.
//
// The module additionally provides the "scripting.lua_bridge.settings" Lua
// bridge extension: it installs engine.set_setting, engine.get_setting,
// engine.remove_setting, engine.save_settings and engine.load_settings into
// every script state through a dedicated SettingsLuaBridge object owned by
// this module (see settings.cpp). This is a script mod's FIRST persistent
// storage: mod config.toml files are authored defaults, not runtime state,
// and nothing else in the tree persists a mod's own values across runs.
class SettingsModule final : public ModuleInterface, public SettingsCapability
{
public:
    SettingsModule();

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;

    // SettingsCapability.
    bool load(const std::string &path, std::string &errorReport) override;
    bool save(const std::string &path, std::string &errorReport) override;
    void setNumber(const std::string &name, double value) override;
    void setBoolean(const std::string &name, bool value) override;
    void setString(const std::string &name, const std::string &value) override;
    double numberValue(const std::string &name, double fallback) const override;
    bool booleanValue(const std::string &name, bool fallback) const override;
    std::string stringValue(const std::string &name, const std::string &fallback) const override;
    SettingKind kindOf(const std::string &name) const override;
    bool contains(const std::string &name) const override;
    bool removeValue(const std::string &name) override;
    std::size_t valueCount() const override;

    // The stored value type: one plain tagged struct. Public so the
    // single-lookup reader below can hand it out (callers use auto; the
    // type is a data record, not an API surface).
    struct SettingValue
    {
        SettingKind kind = SettingKind::Missing;
        double number    = 0.0;
        bool boolean     = false;
        std::string string;
    };

    // Single-lookup form of the typed readers, for the Lua bridge's
    // get_setting (settings.cpp): the bridge used to pay kindOf() AND a
    // typed read - two O(log n) tree walks per script call - to answer one
    // question the iterator already carries. Module-internal API (the
    // capability interface above is untouched); the module IS the
    // capability, so the bridge reaches this through the identity cast.
    const SettingValue *findValue(
        const std::string &name
    ) const
    {
        const auto found = values.find(name);
        return found != values.end() ? &found->second : nullptr;
    }

private:
    // Resolves an empty path to the configured default file; relative paths
    // resolve against the executable directory (the engine's path convention
    // for state the RUN owns - the same convention the log file keeps).
    std::string resolvePath(const std::string &path) const;

    // The write-after-mutation half of [settings] autosave: saves the
    // configured default file and logs a warning when the write fails (a
    // failed autosave must never throw onto the frame path).
    void autosaveIfEnabled();

    ComponentManifest componentManifest;
    LoggerCapability *logger = nullptr;
    // The store, ordered by name: a save is deterministic across runs.
    std::map<std::string, SettingValue> values;
    // [settings] file / autosave (read in construct).
    std::string defaultFile = "settings.toml";
    bool autosave           = false;
    // Names whose kind mismatch was already logged (one warning per name,
    // not one per read - a per-frame read of a mistyped key must not spam).
    mutable std::map<std::string, bool> mismatchWarned;
    // The Lua bridge extension object owned by this module (constructed in
    // construct(), returned from capabilityPointerFor for the
    // "scripting.lua_bridge.settings" member; the concrete
    // SettingsLuaBridge type stays private to settings.cpp).
    std::unique_ptr<LuaBridgeExtension> luaBridge;
};

} // namespace engine

#endif // ENGINE_MODULES_SETTINGS_HPP
