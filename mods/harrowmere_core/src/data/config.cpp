#include "data/config.hpp"

#include <cassert>
#include <filesystem>
#include <limits>
#include <utility>

namespace harrowmere::data
{
namespace
{

// Pinned package defaults - the exact values mods/harrowmere_core/config.toml
// ships (task 0.1). They are configuration knobs, not Appendix A rows
// (common.md Appendix A header: "Config-flavored knobs are not A-rows").
constexpr std::uint32_t kDefaultTickRate          = 10;     // D-TICK-1 (§4.4)
constexpr std::uint32_t kDefaultTimeScale         = 60;     // D-TIME-1 (§4.4)
constexpr std::uint32_t kDefaultAutosaveGameHours = 6;      // task 0.1 config.toml
constexpr std::uint64_t kDefaultSeed              = 0;      // 0 = random, persisted by 7.1
constexpr const char *kFallbackModDirectory       = "mods"; // task 0.4 pinned decision

// A [harrowmere] integer must fit the unsigned field it feeds; anything else
// (negative, oversized) is treated as unset - the pinned default applies and
// the configuration stays safe to hand to the sim.
std::uint32_t u32OrDefault(
    const std::optional<std::int64_t> &value, std::uint32_t fallback
)
{
    if (!value.has_value())
    {
        return fallback;
    }
    if (*value < 0 || *value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()))
    {
        return fallback;
    }
    return static_cast<std::uint32_t>(*value);
}

} // namespace

bool isDeclaredConfigKey(
    std::string_view dottedKey
)
{
    for (const char *declared : kDeclaredConfigKeys)
    {
        if (dottedKey == declared)
        {
            return true;
        }
    }
    return false;
}

GameConfig::GameConfig(
    std::uint32_t tickRate,
    std::uint32_t timeScale,
    std::uint32_t autosaveGameHours,
    std::uint64_t seed,
    std::string modDirectory
)
    : tickRate_(tickRate), timeScale_(timeScale), autosaveGameHours_(autosaveGameHours), seed_(seed),
      modDirectory_(std::move(modDirectory))
{}

GameConfig GameConfig::withValues(
    std::uint32_t tickRate,
    std::uint32_t timeScale,
    std::uint32_t autosaveGameHours,
    std::uint64_t seed,
    std::string modDirectory
)
{
    return GameConfig(tickRate, timeScale, autosaveGameHours, seed, std::move(modDirectory));
}

GameConfig GameConfig::load(
    const ConfigSource &source
)
{
    // The wrapper reads ONLY declared keys (task 0.4 pinned decision; the
    // kernel's startup validation walks the effective table on its side).
    // The asserts pin our own half of that contract; a new key joins
    // kDeclaredConfigKeys AND mod.toml's config_keys in the same change.
    assert(isDeclaredConfigKey("harrowmere.tick_rate"));
    assert(isDeclaredConfigKey("harrowmere.time_scale"));
    assert(isDeclaredConfigKey("harrowmere.autosave_game_hours"));
    assert(isDeclaredConfigKey("harrowmere.seed"));

    const std::uint32_t tickRate  = u32OrDefault(source.integer("harrowmere", "tick_rate"), kDefaultTickRate);
    const std::uint32_t timeScale = u32OrDefault(source.integer("harrowmere", "time_scale"), kDefaultTimeScale);
    const std::uint32_t autosave
        = u32OrDefault(source.integer("harrowmere", "autosave_game_hours"), kDefaultAutosaveGameHours);

    // The seed is u64-wide; the TOML integer reader is i64-wide, so the
    // usable range is [0, INT64_MAX] - and a negative seed is authoring
    // nonsense, so it falls back to the default like any other garbage.
    std::uint64_t seed = kDefaultSeed;
    if (const auto value = source.integer("harrowmere", "seed"))
    {
        if (*value >= 0)
        {
            seed = static_cast<std::uint64_t>(*value);
        }
    }

    // Engine-owned key, read through the same seam (not ours to declare).
    std::string modDirectory = kFallbackModDirectory;
    if (const auto value = source.string("engine", "mod_directory"))
    {
        if (!value->empty())
        {
            modDirectory = *value;
        }
    }

    return GameConfig(tickRate, timeScale, autosave, seed, std::move(modDirectory));
}

std::uint32_t GameConfig::tickRate() const
{
    return tickRate_;
}

std::uint32_t GameConfig::timeScale() const
{
    return timeScale_;
}

std::uint32_t GameConfig::autosaveGameHours() const
{
    return autosaveGameHours_;
}

std::uint64_t GameConfig::seed() const
{
    return seed_;
}

const std::string &GameConfig::modDirectory() const
{
    return modDirectory_;
}

std::string GameConfig::coreDataDirectory(
    std::string_view executableDir
) const
{
    return (std::filesystem::path(executableDir) / modDirectory_ / "harrowmere_core" / "data").string();
}

} // namespace harrowmere::data
