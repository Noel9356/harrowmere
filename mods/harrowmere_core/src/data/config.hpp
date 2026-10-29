#ifndef HARROWMERE_CORE_DATA_CONFIG_HPP
#define HARROWMERE_CORE_DATA_CONFIG_HPP

// The configuration wrapper (task 0.4 pinned decision): the [harrowmere]
// tunables are read through ONE kernel-free seam, "wrapped once here, so the
// rest of the code never touches the kernel type" (common.md §4.3, D-DATA-1).
// The engine-side half of the seam is data/config_engine.hpp - the only file
// in src/data/ that includes an engine header. Everything downstream
// (clocks in 0.3, autosave in 7.1, worldgen's seed in 1.5) reads a GameConfig.
//
// Semantics (pinned): a missing key falls back to the package default shipped
// in mods/harrowmere_core/config.toml (task 0.1); the wrapper reads ONLY keys
// declared in mod.toml's config_keys - isDeclaredConfigKey() is the guard,
// and the kernel's startup validation catches the engine-side drift
// (undeclared keys draw warnings, ConfigurationStore::provenanceReport()
// names the layer that provided each effective value).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace harrowmere::data
{

// Every [harrowmere] key this wrapper may read - in lockstep with
// `config_keys` in mods/harrowmere_core/mod.toml and the compiled manifest's
// knownConfigKeys (task 0.1). The engine-owned [engine].mod_directory is read
// through the same seam but is not ours to declare.
inline constexpr const char *kDeclaredConfigKeys[] = {
    "harrowmere.tick_rate",
    "harrowmere.time_scale",
    "harrowmere.autosave_game_hours",
    "harrowmere.seed",
};

// Is `dottedKey` one of kDeclaredConfigKeys? ("harrowmere.billing" is a
// programming error, not a data error - this predicate is what the wrapper
// asserts on before reading.)
bool isDeclaredConfigKey(std::string_view dottedKey);

// Kernel-free view of the layered engine configuration: exactly the two
// typed reads the game needs (integerValue / stringValue on the engine's
// ConfigurationStore - see config_engine.hpp for the adapter that implements
// this against the real kernel type). Tests inject a fake.
class ConfigSource
{
public:
    virtual ~ConfigSource() = default;

    virtual std::optional<std::int64_t> integer(const char *section, const char *key) const = 0;
    virtual std::optional<std::string> string(const char *section, const char *key) const = 0;
};

// The wrapped values. Constructed by load() from a ConfigSource, or directly
// (withValues) in tests and defaults-only contexts.
class GameConfig
{
public:
    // Reads every declared [harrowmere] key (missing -> pinned package
    // default) plus the engine-owned [engine].mod_directory (missing ->
    // pinned fallback "mods"). Out-of-range or negative values fall back to
    // the default too: configuration garbage must never reach the sim as a
    // plausible number.
    static GameConfig load(const ConfigSource &source);

    // Direct construction (tests; the shape later tasks cache).
    static GameConfig withValues(std::uint32_t tickRate, std::uint32_t timeScale,
                                 std::uint32_t autosaveGameHours, std::uint64_t seed,
                                 std::string modDirectory);

    // Sim ticks per real second - the fixed-update heartbeat (D-TICK-1,
    // common.md §4.4; default 10 from config.toml, task 0.1).
    std::uint32_t tickRate() const;

    // TIME_SCALE (D-TIME-1): game seconds advanced per real second
    // (default 60: one real second is one game minute).
    std::uint32_t timeScale() const;

    // Autosave cadence in game hours (task 7.1 consumes it; default 6).
    std::uint32_t autosaveGameHours() const;

    // World seed; 0 = a random seed chosen at first launch and then
    // persisted by the save system (task 7.1; default 0).
    std::uint64_t seed() const;

    // The mod scan root ([engine].mod_directory, pinned fallback "mods").
    const std::string &modDirectory() const;

    // Base content directory (task 0.4 pinned decision):
    //   <executableDir>/<modDirectory>/harrowmere_core/data/
    // - the runtime layout places the packaged data/ exactly there
    //   (build/runtime/mods/harrowmere_core/data). Data from OTHER active
    //   packages is applied afterwards in manifest-id order by task 7.5; the
    //   base package is just the first element of that enumeration, hardcoded
    //   harrowmere_core.
    std::string coreDataDirectory(std::string_view executableDir) const;

private:
    GameConfig(std::uint32_t tickRate, std::uint32_t timeScale,
               std::uint32_t autosaveGameHours, std::uint64_t seed, std::string modDirectory);

    std::uint32_t tickRate_;
    std::uint32_t timeScale_;
    std::uint32_t autosaveGameHours_;
    std::uint64_t seed_;
    std::string modDirectory_;
};

} // namespace harrowmere::data

#endif // HARROWMERE_CORE_DATA_CONFIG_HPP
