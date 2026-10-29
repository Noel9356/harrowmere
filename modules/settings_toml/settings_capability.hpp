#ifndef ENGINE_MODULES_SETTINGS_SETTINGS_CAPABILITY_HPP
#define ENGINE_MODULES_SETTINGS_SETTINGS_CAPABILITY_HPP

// The published capability surface of the settings module. Every consumer -
// other modules, native mods, the app - includes THIS header, from the
// module that provides the capability: the provider owns and publishes its
// own capability vocabulary, so there is no central contract directory and
// no mirrored copy that could drift. Like every published capability header
// this stays implementation-, platform- and vendor-agnostic; the per-module
// purity guard in the module's CMakeLists enforces that mechanically.
// See mods/README.md, section "Portability".

#include <cstddef>
#include <cstdint>
#include <string>

namespace engine
{

// What kind of value is stored under a name. The typed reads are the
// day-to-day surface; the kind query exists for generic tooling - a settings
// screen that renders one editor per row, or a bridge that must preserve a
// value's type without knowing it.
enum class SettingKind
{
    Missing,
    Number,
    Boolean,
    String
};

// Capability "settings": the run's persisted key-value store.
//
// This is the player-options surface the engine did not have: volumes,
// quality levels, keybind hints, a mod's unlocks - values a game wants to
// survive a restart, addressed by name and stored as one human-readable
// TOML file. The persistence module is deliberately NOT this: it saves the
// scene registry's objects (the world), while settings save the OPTIONS
// around the world - and a script mod had no persistent storage at all
// before this capability, because mod config.toml files are authored
// defaults, not runtime state.
//
// Values are scalars only (numbers, booleans, strings) - the shape a
// hand-edited options file stays flat and diffable. Structure beyond that
// (per-keybind tables, arrays) is the caller's encoding job: spell it into
// the name ("keybind.forward") and the file stays editable.
class SettingsCapability
{
public:
    // The implementation-agnostic family name of this capability. Consumers
    // acquire through it; the module's plugin descriptor carries the
    // implementation-bound member name ("settings.toml").
    static constexpr const char *capabilityName = "settings";

    virtual ~SettingsCapability() = default;

    // Loads one settings file over the current values. Loading MERGES: keys
    // the file defines are written, keys it does not define keep their
    // current value (a reset is the caller's explicit removeValue job) - so
    // a mod can ship a base file and let a user file layer on top. An empty
    // path loads the configured default file ([settings].file). A
    // structurally broken file refuses with a precise message and changes
    // nothing; rows of an unusable type are skipped with one log each and
    // the load continues (the return value is what the caller checks).
    virtual bool load(const std::string &path, std::string &errorReport) = 0;

    // Writes every current value to one TOML file, in deterministic order.
    // An empty path saves the configured default file. Returns false with a
    // precise message when the file cannot be written; a refused save
    // leaves the previous file untouched.
    virtual bool save(const std::string &path, std::string &errorReport) = 0;

    // Typed writes. A write replaces the value under the name regardless of
    // the previous kind; with [settings] autosave enabled the configured
    // default file is re-written after every mutation.
    virtual void setNumber(const std::string &name, double value) = 0;

    virtual void setBoolean(const std::string &name, bool value) = 0;

    virtual void setString(const std::string &name, const std::string &value) = 0;

    // Typed reads with caller-supplied fallbacks. A stored value of a
    // DIFFERENT kind returns the fallback - a read degrades, it never
    // throws, and the module logs the kind mismatch once per name so a
    // hand-edited file cannot silently feed a wrong-typed value to code
    // that expected one.
    virtual double numberValue(const std::string &name, double fallback) const = 0;

    virtual bool booleanValue(const std::string &name, bool fallback) const = 0;

    virtual std::string stringValue(const std::string &name, const std::string &fallback) const = 0;

    // What kind of value is stored under the name (SettingKind).
    virtual SettingKind kindOf(const std::string &name) const = 0;

    // Whether the named value exists (any kind).
    virtual bool contains(const std::string &name) const = 0;

    // Removes one value. Returns false when the name was not set. With
    // [settings] autosave enabled the default file is re-written after a
    // successful removal.
    virtual bool removeValue(const std::string &name) = 0;

    // How many values the store currently holds.
    virtual std::size_t valueCount() const = 0;
};

} // namespace engine

#endif // ENGINE_MODULES_SETTINGS_SETTINGS_CAPABILITY_HPP
